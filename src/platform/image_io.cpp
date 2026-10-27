#include "platform/image_io.h"

#include "platform/gif_encoder.h"
#include "platform/i18n.h"

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <vector>

#ifdef NV_WITH_FREEIMAGE
// libwebp's simple encoding API is compiled into the vendored freeimage.lib.
// Declaring the three entry points here keeps a separate libwebp dependency out
// of the build while giving WebP a real quality parameter (規劃/22/14 §4.1).
extern "C" {
size_t WebPEncodeBGRA(const uint8_t* bgra, int width, int height, int stride,
                      float quality_factor, uint8_t** output);
void WebPFree(void* pointer);
}
#endif

using Microsoft::WRL::ComPtr;

namespace nv::platform {
namespace {

void SetError(std::wstring* error, const wchar_t* step, HRESULT hr) {
  if (error == nullptr) return;
  wchar_t buffer[256]{};
  swprintf_s(buffer, L"%s failed (HRESULT=0x%08lX)", step,
             static_cast<unsigned long>(hr));
  *error = buffer;
}

void SetWin32Error(std::wstring* error, const wchar_t* step, DWORD code) {
  if (error == nullptr) return;
  wchar_t buffer[256]{};
  swprintf_s(buffer, L"%s failed (error %lu)", step,
             static_cast<unsigned long>(code));
  *error = buffer;
}

std::wstring LowerExtension(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  std::ranges::transform(extension, extension.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return extension;
}

GUID ContainerFormat(ImageSaveFormat format) {
  switch (format) {
    case ImageSaveFormat::kJpeg: return GUID_ContainerFormatJpeg;
    case ImageSaveFormat::kBmp: return GUID_ContainerFormatBmp;
    case ImageSaveFormat::kTiff: return GUID_ContainerFormatTiff;
    case ImageSaveFormat::kWebp: return GUID_ContainerFormatWebp;
    case ImageSaveFormat::kPng:
    default: return GUID_ContainerFormatPng;
  }
}

bool WritePackedBgr(IWICBitmapFrameEncode* frame, const uint8_t* bgra,
                    uint32_t width, uint32_t height, uint32_t stride) {
  const uint32_t row_bytes = width * 3u;
  if (row_bytes / 3u != width) return false;
  std::vector<uint8_t> packed(static_cast<size_t>(row_bytes) * height);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* source = bgra + static_cast<size_t>(y) * stride;
    uint8_t* destination = packed.data() + static_cast<size_t>(y) * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      destination[x * 3u + 0] = source[x * 4u + 0];
      destination[x * 3u + 1] = source[x * 4u + 1];
      destination[x * 3u + 2] = source[x * 4u + 2];
    }
  }
  const uint64_t total = static_cast<uint64_t>(row_bytes) * height;
  if (total > 0xFFFFFFFFull) return false;
  return SUCCEEDED(frame->WritePixels(height, row_bytes,
                                      static_cast<UINT>(total),
                                      packed.data()));
}

// WIC path: PNG / JPEG / BMP / TIFF (and WebP when a system encoder exists).
bool EncodeWic(IStream* output, const uint8_t* bgra, uint32_t width,
               uint32_t height, uint32_t stride, ImageSaveFormat format,
               int quality, std::wstring* error) {
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr)) {
    SetError(error, L"Create WIC factory", hr);
    return false;
  }

  ComPtr<IWICStream> stream;
  hr = factory->CreateStream(stream.GetAddressOf());
  if (SUCCEEDED(hr)) hr = stream->InitializeFromIStream(output);
  if (FAILED(hr)) {
    SetError(error, L"Create encoder stream", hr);
    return false;
  }

  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(ContainerFormat(format), nullptr,
                              encoder.GetAddressOf());
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  }
  if (FAILED(hr)) {
    SetError(error, L"Create encoder", hr);
    return false;
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  hr = encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf());
  if (SUCCEEDED(hr)) {
    if (SaveFormatHasQuality(format) && options) {
      PROPBAG2 option{};
      option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
      VARIANT value;
      VariantInit(&value);
      value.vt = VT_R4;
      value.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.0f;
      options->Write(1, &option, &value);
      VariantClear(&value);
    }
    hr = frame->Initialize(options.Get());
  }
  if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
  if (FAILED(hr)) {
    SetError(error, L"Initialize output frame", hr);
    return false;
  }

  // PNG and TIFF keep alpha; JPEG/BMP are 24bpp. WebP accepts BGRA.
  const bool wants_alpha = format == ImageSaveFormat::kPng ||
                           format == ImageSaveFormat::kTiff ||
                           format == ImageSaveFormat::kWebp;
  WICPixelFormatGUID pixel_format =
      wants_alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
  hr = frame->SetPixelFormat(&pixel_format);
  if (FAILED(hr)) {
    SetError(error, L"Set output pixel format", hr);
    return false;
  }

  if (wants_alpha) {
    if (!IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA)) {
      if (error != nullptr) *error = L"Encoder does not support 32bpp BGRA.";
      return false;
    }
    const uint64_t total = static_cast<uint64_t>(stride) * height;
    if (total > 0xFFFFFFFFull ||
        FAILED(frame->WritePixels(height, stride, static_cast<UINT>(total),
                                  const_cast<BYTE*>(bgra)))) {
      if (error != nullptr) *error = L"Write pixels failed.";
      return false;
    }
  } else {
    if (!IsEqualGUID(pixel_format, GUID_WICPixelFormat24bppBGR)) {
      if (error != nullptr) *error = L"Encoder does not support 24bpp BGR.";
      return false;
    }
    if (!WritePackedBgr(frame.Get(), bgra, width, height, stride)) {
      if (error != nullptr) *error = L"Write pixels failed.";
      return false;
    }
  }

  hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  if (FAILED(hr)) {
    SetError(error, L"Commit image", hr);
    return false;
  }

  // Release the COM objects while their stream is still alive so the caller can
  // read or replace the file (規劃/22/14 坑 4).
  frame.Reset();
  encoder.Reset();
  options.Reset();
  stream.Reset();
  factory.Reset();
  return true;
}

#ifdef NV_WITH_FREEIMAGE
bool EncodeWebp(const uint8_t* bgra, uint32_t width, uint32_t height,
                uint32_t stride, int quality, std::vector<uint8_t>* output,
                std::wstring* error) {
  uint8_t* encoded = nullptr;
  const float factor =
      static_cast<float>(std::clamp(quality, 1, 100));
  const size_t size = WebPEncodeBGRA(bgra, static_cast<int>(width),
                                     static_cast<int>(height),
                                     static_cast<int>(stride), factor,
                                     &encoded);
  if (size == 0 || encoded == nullptr) {
    if (error != nullptr) *error = L"WebP encoder failed.";
    if (encoded != nullptr) WebPFree(encoded);
    return false;
  }
  output->assign(encoded, encoded + size);
  WebPFree(encoded);
  return true;
}
#endif

// Copies the encoded bytes into the sibling temporary file and replaces the
// target. Shared by the GIF/WebP memory path and by the WIC file path.
bool WriteAtomically(const std::filesystem::path& path,
                     const std::vector<uint8_t>& bytes,
                     std::wstring* error) {
  std::filesystem::path temporary = path;
  temporary += L".nvtmp";
  DeleteFileW(temporary.c_str());

  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    SetWin32Error(error, L"Create output file", GetLastError());
    return false;
  }
  uint64_t written = 0;
  bool ok = true;
  while (written < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<size_t>(bytes.size() - written, 1u << 20));
    DWORD actual = 0;
    if (!WriteFile(file, bytes.data() + written, chunk, &actual, nullptr) ||
        actual == 0) {
      SetWin32Error(error, L"Write output file", GetLastError());
      ok = false;
      break;
    }
    written += actual;
  }
  if (ok) FlushFileBuffers(file);
  CloseHandle(file);
  if (!ok) {
    DeleteFileW(temporary.c_str());
    return false;
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    SetWin32Error(error, L"Replace target file", GetLastError());
    DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

bool EncodeToMemoryBytes(const uint8_t* bgra, uint32_t width, uint32_t height,
                         uint32_t stride, ImageSaveFormat format, int quality,
                         std::vector<uint8_t>* output, std::wstring* error) {
  output->clear();
  if (format == ImageSaveFormat::kGif) {
    return EncodeGif(bgra, width, height, stride, output, error);
  }
  if (format == ImageSaveFormat::kWebp) {
#ifdef NV_WITH_FREEIMAGE
    return EncodeWebp(bgra, width, height, stride, quality, output, error);
#else
    if (error != nullptr) *error = L"WebP encoder is not available.";
    return false;
#endif
  }

  ComPtr<IStream> memory;
  memory.Attach(SHCreateMemStream(nullptr, 0));
  if (!memory) {
    if (error != nullptr) *error = L"Could not create the output stream.";
    return false;
  }
  if (!EncodeWic(memory.Get(), bgra, width, height, stride, format, quality,
                 error)) {
    return false;
  }
  // Read the size after the encoder released its own reference to the stream.
  STATSTG stat{};
  if (FAILED(memory->Stat(&stat, STATFLAG_NONAME))) {
    if (error != nullptr) *error = L"Could not measure the encoded stream.";
    return false;
  }
  output->resize(static_cast<size_t>(stat.cbSize.QuadPart));
  if (!output->empty()) {
    LARGE_INTEGER start{};
    ULONG read = 0;
    if (FAILED(memory->Seek(start, STREAM_SEEK_SET, nullptr)) ||
        FAILED(memory->Read(output->data(),
                            static_cast<ULONG>(output->size()), &read)) ||
        read != output->size()) {
      output->clear();
      if (error != nullptr) *error = L"Could not read the encoded stream.";
      return false;
    }
  }
  return true;
}

}  // namespace

const char* SaveFormatToken(ImageSaveFormat format) {
  switch (format) {
    case ImageSaveFormat::kJpeg: return "jpeg";
    case ImageSaveFormat::kBmp: return "bmp";
    case ImageSaveFormat::kTiff: return "tiff";
    case ImageSaveFormat::kWebp: return "webp";
    case ImageSaveFormat::kGif: return "gif";
    case ImageSaveFormat::kPng:
    default: return "png";
  }
}

const wchar_t* SaveFormatLabel(ImageSaveFormat format) {
  switch (format) {
    case ImageSaveFormat::kJpeg: return L"JPEG";
    case ImageSaveFormat::kBmp: return L"BMP";
    case ImageSaveFormat::kTiff: return L"TIFF";
    case ImageSaveFormat::kWebp: return L"WebP";
    case ImageSaveFormat::kGif: return L"GIF";
    case ImageSaveFormat::kPng:
    default: return L"PNG";
  }
}

const wchar_t* SaveFormatPatterns(ImageSaveFormat format) {
  switch (format) {
    case ImageSaveFormat::kJpeg: return L"*.jpg;*.jpeg;*.jpe;*.jfif";
    case ImageSaveFormat::kBmp: return L"*.bmp;*.dib";
    case ImageSaveFormat::kTiff: return L"*.tif;*.tiff";
    case ImageSaveFormat::kWebp: return L"*.webp";
    case ImageSaveFormat::kGif: return L"*.gif";
    case ImageSaveFormat::kPng:
    default: return L"*.png";
  }
}

const wchar_t* SaveFormatExtension(ImageSaveFormat format) {
  switch (format) {
    case ImageSaveFormat::kJpeg: return L".jpg";
    case ImageSaveFormat::kBmp: return L".bmp";
    case ImageSaveFormat::kTiff: return L".tif";
    case ImageSaveFormat::kWebp: return L".webp";
    case ImageSaveFormat::kGif: return L".gif";
    case ImageSaveFormat::kPng:
    default: return L".png";
  }
}

bool SaveFormatHasQuality(ImageSaveFormat format) {
  return format == ImageSaveFormat::kJpeg || format == ImageSaveFormat::kWebp;
}

ImageSaveFormat ImageFormatFromPath(const std::filesystem::path& path,
                                    ImageSaveFormat fallback) {
  ImageSaveFormat format = fallback;
  if (TryImageFormatFromPath(path, &format)) return format;
  return fallback;
}

bool TryImageFormatFromPath(const std::filesystem::path& path,
                            ImageSaveFormat* format) {
  if (format == nullptr) return false;
  const std::wstring extension = LowerExtension(path);
  if (extension == L".png") {
    *format = ImageSaveFormat::kPng;
  } else if (extension == L".jpg" || extension == L".jpeg" ||
             extension == L".jpe" || extension == L".jfif") {
    *format = ImageSaveFormat::kJpeg;
  } else if (extension == L".bmp" || extension == L".dib") {
    *format = ImageSaveFormat::kBmp;
  } else if (extension == L".tif" || extension == L".tiff") {
    *format = ImageSaveFormat::kTiff;
  } else if (extension == L".webp") {
    *format = ImageSaveFormat::kWebp;
  } else if (extension == L".gif") {
    *format = ImageSaveFormat::kGif;
  } else {
    return false;
  }
  return true;
}

bool SaveBgra(const std::filesystem::path& path, const uint8_t* bgra,
              uint32_t width, uint32_t height, uint32_t stride,
              ImageSaveFormat format, int quality, std::wstring* error) {
  if (error != nullptr) error->clear();
  if (bgra == nullptr || width == 0 || height == 0 || stride < width * 4u ||
      path.empty()) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorSaveArguments);
    }
    return false;
  }

  // GIF and WebP always go through the memory encoder; their files are small
  // enough that the extra copy is cheaper than a second code path.
  if (format == ImageSaveFormat::kGif || format == ImageSaveFormat::kWebp) {
    std::vector<uint8_t> bytes;
    if (!EncodeToMemoryBytes(bgra, width, height, stride, format, quality,
                             &bytes, error)) {
      return false;
    }
    return WriteAtomically(path, bytes, error);
  }

  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
    SetError(error, L"Initialize COM", com_hr);
    return false;
  }

  std::filesystem::path temporary = path;
  temporary += L".nvtmp";
  DeleteFileW(temporary.c_str());

  // A growable file stream avoids the WIC stream's fixed-buffer semantics and
  // lets the encoder write straight to disk (no whole-file buffer in memory).
  ComPtr<IStream> file_stream;
  IStream* raw_stream = nullptr;
  if (FAILED(SHCreateStreamOnFileEx(
          temporary.c_str(),
          STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE | STGM_FAILIFTHERE,
          FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &raw_stream))) {
    SetWin32Error(error, L"Create output file",
                  GetLastError() == 0 ? ERROR_ACCESS_DENIED : GetLastError());
    return false;
  }
  file_stream.Attach(raw_stream);
  const bool ok =
      EncodeWic(file_stream.Get(), bgra, width, height, stride, format, quality,
                error);
  file_stream.Reset();
  if (!ok) {
    DeleteFileW(temporary.c_str());
    return false;
  }

  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    SetWin32Error(error, L"Replace target file", GetLastError());
    DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

bool SaveImage(const std::filesystem::path& path, const Image& image,
               ImageSaveFormat format, int quality, std::wstring* error) {
  if (image.Empty() || image.info.format != PixelFormat::kBgra8) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorNotBgra8);
    }
    return false;
  }
  return SaveBgra(path, image.pixels.data(), image.info.width,
                  image.info.height, image.info.stride, format, quality,
                  error);
}

bool EncodeBgraToMemory(const uint8_t* bgra, uint32_t width, uint32_t height,
                        uint32_t stride, ImageSaveFormat format, int quality,
                        std::vector<uint8_t>* output, std::wstring* error) {
  if (error != nullptr) error->clear();
  if (output == nullptr || bgra == nullptr || width == 0 || height == 0 ||
      stride < width * 4u) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorSaveArguments);
    }
    return false;
  }
  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // Only balance our own initialisation: S_FALSE means the calling thread
  // already had COM (the UI thread's OLE STA) and must stay initialised.
  const bool uninit = com_hr == S_OK;
  const bool ok = EncodeToMemoryBytes(bgra, width, height, stride, format,
                                      quality, output, error);
  if (uninit) CoUninitialize();
  return ok;
}

}  // namespace nv::platform
