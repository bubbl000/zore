#include "decoders/wic_decoder.h"

#include "core/icc_profile.h"
#include "core/image_transform.h"
#include "platform/log.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <shlwapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv::wic {
namespace {

constexpr int kDecodeErrorBase = 2000;
constexpr size_t kMaxIccProfileBytes = 4ull * 1024 * 1024;

Error HResultError(const char* what, HRESULT hr) {
  return Error::Make(kDecodeErrorBase, what, hr);
}

bool IsAlphaFormat(const WICPixelFormatGUID& format) {
  return IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) ||
         IsEqualGUID(format, GUID_WICPixelFormat32bppPBGRA) ||
         IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA) ||
         IsEqualGUID(format, GUID_WICPixelFormat32bppPRGBA) ||
         IsEqualGUID(format, GUID_WICPixelFormat64bppRGBA) ||
         IsEqualGUID(format, GUID_WICPixelFormat64bppPRGBA);
}

// Container name for the V3-02 "unknown extension" probe. The router normally
// knows the format from magic/extension; this only labels files that WIC can
// open without either.
ImageFormat FormatFromContainer(const GUID& container) {
  if (IsEqualGUID(container, GUID_ContainerFormatJpeg)) return ImageFormat::kJpeg;
  if (IsEqualGUID(container, GUID_ContainerFormatPng)) return ImageFormat::kPng;
  if (IsEqualGUID(container, GUID_ContainerFormatBmp)) return ImageFormat::kBmp;
  if (IsEqualGUID(container, GUID_ContainerFormatGif)) return ImageFormat::kGif;
  if (IsEqualGUID(container, GUID_ContainerFormatTiff)) return ImageFormat::kTiff;
  if (IsEqualGUID(container, GUID_ContainerFormatIco)) return ImageFormat::kIco;
  if (IsEqualGUID(container, GUID_ContainerFormatWmp)) return ImageFormat::kJxr;
  return ImageFormat::kUnknown;
}

uint16_t ReadOrientation(IWICBitmapFrameDecode* frame) {
  ComPtr<IWICMetadataQueryReader> reader;
  if (frame == nullptr || FAILED(frame->GetMetadataQueryReader(reader.GetAddressOf()))) {
    return 1;
  }

  constexpr const wchar_t* kQueries[] = {
      L"/ifd/{ushort=274}",
      L"/app1/ifd/{ushort=274}",
  };
  for (const wchar_t* query : kQueries) {
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT hr = reader->GetMetadataByName(query, &value);
    if (SUCCEEDED(hr)) {
      uint16_t orientation = 1;
      if (value.vt == VT_UI2) {
        orientation = value.uiVal;
      } else if (value.vt == VT_UI4) {
        orientation = static_cast<uint16_t>(value.ulVal);
      }
      PropVariantClear(&value);
      if (orientation >= 1 && orientation <= 8) return orientation;
    } else {
      PropVariantClear(&value);
    }
  }
  return 1;
}

// Returns the first embedded ICC profile exposed by the frame. Empty when the
// file has no profile (or only an EXIF color-space hint).
//
// Per the WIC contract the caller's array must already contain valid
// IWICColorContext objects created by IWICImagingFactory::CreateColorContext;
// passing null pointers makes GetColorContexts fail with E_INVALIDARG.
std::vector<uint8_t> ReadIccProfile(IWICImagingFactory* factory,
                                    IWICBitmapFrameDecode* frame) {
  std::vector<uint8_t> profile;
  if (factory == nullptr || frame == nullptr) return profile;

  UINT count = 0;
  if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) {
    return profile;
  }
  count = std::min<UINT>(count, 8);

  std::vector<ComPtr<IWICColorContext>> contexts(count);
  std::vector<IWICColorContext*> raw(count, nullptr);
  for (UINT i = 0; i < count; ++i) {
    if (FAILED(factory->CreateColorContext(contexts[i].GetAddressOf()))) {
      return profile;
    }
    raw[i] = contexts[i].Get();
  }

  UINT actual = 0;
  if (FAILED(frame->GetColorContexts(count, raw.data(), &actual))) {
    return profile;
  }
  actual = std::min(actual, count);

  for (UINT i = 0; i < actual && profile.empty(); ++i) {
    UINT size = 0;
    if (FAILED(contexts[i]->GetProfileBytes(0, nullptr, &size)) || size == 0 ||
        size > kMaxIccProfileBytes) {
      continue;
    }
    profile.resize(size);
    UINT written = 0;
    if (FAILED(contexts[i]->GetProfileBytes(size, profile.data(), &written))) {
      profile.clear();
      continue;
    }
    profile.resize(std::min<UINT>(written, size));
  }
  return profile;
}

WICBitmapTransformOptions OrientationTransform(uint16_t orientation) {
  // WIC applies the flip before the rotation. These combinations are the
  // standard EXIF 1..8 mapping; tests/color_tests.cpp pins the pixel layout.
  switch (orientation) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5:
      return static_cast<WICBitmapTransformOptions>(
          WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7:
      return static_cast<WICBitmapTransformOptions>(
          WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
  }
}

// Converts `source` (already in BGRA8) from the embedded profile to sRGB.
// Returns false when ICM cannot use the profile; the caller then keeps the
// untransformed pixels, exactly like the pre-ICC behavior.
bool CreateSrgbColorTransform(IWICImagingFactory* factory,
                              IWICBitmapSource* source,
                              const std::vector<uint8_t>& profile,
                              ComPtr<IWICColorTransform>* transform) {
  if (factory == nullptr || source == nullptr || profile.empty()) return false;

  ComPtr<IWICColorContext> from;
  if (FAILED(factory->CreateColorContext(from.GetAddressOf()))) return false;
  if (FAILED(from->InitializeFromMemory(
          const_cast<BYTE*>(profile.data()), static_cast<UINT>(profile.size())))) {
    return false;
  }

  ComPtr<IWICColorContext> to;
  if (FAILED(factory->CreateColorContext(to.GetAddressOf()))) return false;
  // EXIF ColorSpace 1 is sRGB.
  if (FAILED(to->InitializeFromExifColorSpace(1))) return false;

  ComPtr<IWICColorTransform> created;
  if (FAILED(factory->CreateColorTransformer(created.GetAddressOf()))) {
    return false;
  }
  if (FAILED(created->Initialize(source, from.Get(), to.Get(),
                                 GUID_WICPixelFormat32bppBGRA))) {
    return false;
  }
  *transform = std::move(created);
  return true;
}

ComPtr<IWICImagingFactory> CreateFactory() {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.GetAddressOf())))) {
    return nullptr;
  }
  return factory;
}

ComPtr<IWICBitmapDecoder> OpenDecoder(IWICImagingFactory* factory,
                                      const std::filesystem::path& path,
                                      Error* error) {
  ComPtr<IWICBitmapDecoder> decoder;
  const HRESULT hr = factory->CreateDecoderFromFilename(
      path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      decoder.GetAddressOf());
  if (FAILED(hr)) {
    *error = HResultError("WIC: CreateDecoderFromFilename failed", hr);
    return nullptr;
  }
  return decoder;
}

}  // namespace

bool EnsureComInitialized() {
  thread_local const bool initialized = [] {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means the thread is already STA; WIC still works.
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  }();
  return initialized;
}

bool DecoderSupportsExtension(const std::wstring& extension) {
  if (extension.empty()) return false;
  if (!EnsureComInitialized()) return false;

  ComPtr<IWICImagingFactory> factory = CreateFactory();
  if (factory == nullptr) return false;

  // Windows 8+: enumerate the registered decoder components. The HEIF/AVIF
  // codecs come from Store packages, so "the file exists and decodes" is not
  // testable from here; the component's declared file extensions are.
  ComPtr<IEnumUnknown> components;
  if (FAILED(factory->CreateComponentEnumerator(
          WICDecoder, WICComponentEnumerateDefault,
          components.GetAddressOf()))) {
    return false;
  }

  std::wstring wanted(extension);
  std::ranges::transform(wanted, wanted.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });

  ComPtr<IUnknown> component;
  ULONG fetched = 0;
  while (components->Next(1, component.ReleaseAndGetAddressOf(), &fetched) ==
             S_OK &&
         fetched == 1) {
    ComPtr<IWICBitmapCodecInfo> info;
    if (FAILED(component.As(&info))) continue;

    UINT length = 0;
    if (FAILED(info->GetFileExtensions(0, nullptr, &length)) || length == 0) {
      continue;
    }
    std::wstring text(length, L'\0');
    if (FAILED(info->GetFileExtensions(length, text.data(), &length))) continue;
    text.resize(std::min<size_t>(text.size(), length));

    size_t start = 0;
    while (start <= text.size()) {
      const size_t end = text.find_first_of(L",; ", start);
      std::wstring token = text.substr(
          start, end == std::wstring::npos ? std::wstring::npos : end - start);
      std::ranges::transform(token, token.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
      });
      if (token == wanted) return true;
      if (end == std::wstring::npos) break;
      start = end + 1;
    }
  }
  return false;
}

ProbeResult Probe(const std::filesystem::path& path) {
  ProbeResult result;
  if (!EnsureComInitialized()) {
    result.error = Error::Make(kDecodeErrorBase, "WIC: COM initialization failed");
    return result;
  }

  ComPtr<IWICImagingFactory> factory = CreateFactory();
  if (factory == nullptr) {
    result.error = Error::Make(kDecodeErrorBase, "WIC: factory creation failed");
    return result;
  }

  ComPtr<IWICBitmapDecoder> decoder = OpenDecoder(factory.Get(), path, &result.error);
  if (decoder == nullptr) return result;

  GUID container{};
  if (SUCCEEDED(decoder->GetContainerFormat(&container))) {
    result.format = FormatFromContainer(container);
  }

  ComPtr<IWICBitmapFrameDecode> frame;
  HRESULT hr = decoder->GetFrame(0, frame.GetAddressOf());
  if (FAILED(hr)) {
    result.error = HResultError("WIC: GetFrame(0) failed", hr);
    return result;
  }

  UINT width = 0;
  UINT height = 0;
  hr = frame->GetSize(&width, &height);
  if (FAILED(hr)) {
    result.error = HResultError("WIC: GetSize failed", hr);
    return result;
  }

  result.orientation = ReadOrientation(frame.Get());
  if (result.orientation >= 5) std::swap(width, height);
  result.orientation_applied = result.orientation != 1;
  result.width = width;
  result.height = height;

  WICPixelFormatGUID format{};
  if (SUCCEEDED(frame->GetPixelFormat(&format))) {
    result.has_alpha = IsAlphaFormat(format);
  }

  const std::vector<uint8_t> profile = ReadIccProfile(factory.Get(), frame.Get());
  result.has_icc = !profile.empty();
  result.icc_is_srgb = result.has_icc && nv::IsSrgbProfile(profile.data(), profile.size());
  UINT frame_count = 1;
  if (SUCCEEDED(decoder->GetFrameCount(&frame_count)) && frame_count > 0) {
    result.frame_count = frame_count;
    // Probe reports the container metadata ("frames: 2, animated: yes" means
    // the file holds several images). DecodeOpened deliberately reports
    // animated=false: the WIC path only ever produces frame 0, so HEIC
    // live-photo pairs and multi-page TIFF are static for the UI (cache, pill).
    result.animated = frame_count > 1;
  }
  result.decoder = "wic";
  return result;
}

namespace {

// Shared decode body: `factory` and `decoder` are already open. Keeping one
// implementation means an in-memory payload (the RAW embedded JPEG preview,
// 規劃/22/13 §3.3) goes through exactly the same orientation/ICC/alpha pipeline
// as a file.
DecodeResult DecodeOpened(const ComPtr<IWICImagingFactory>& factory,
                          const ComPtr<IWICBitmapDecoder>& decoder,
                          const DecodeOptions& options) {
  DecodeResult result;

  // Multi-page TIFF/GIF and animation containers intentionally use frame 0:
  // the product shows the first page/frame only (no paging UI).
  ComPtr<IWICBitmapFrameDecode> frame;
  HRESULT hr = decoder->GetFrame(0, frame.GetAddressOf());
  if (FAILED(hr)) {
    result.error = HResultError("WIC: GetFrame(0) failed", hr);
    return result;
  }

  UINT file_width = 0;
  UINT file_height = 0;
  hr = frame->GetSize(&file_width, &file_height);
  if (FAILED(hr) || file_width == 0 || file_height == 0) {
    result.error = HResultError("WIC: invalid image size", hr);
    return result;
  }

  WICPixelFormatGUID source_format{};
  if (FAILED(frame->GetPixelFormat(&source_format))) {
    source_format = GUID_WICPixelFormat32bppBGRA;
  }

  const uint16_t orientation = ReadOrientation(frame.Get());
  // Upright full-size dimensions: 90/270-degree EXIF orientations swap them.
  const UINT full_width = orientation >= 5 ? file_height : file_width;
  const UINT full_height = orientation >= 5 ? file_width : file_height;

  UINT width = full_width;
  UINT height = full_height;
  ComPtr<IWICBitmapSource> source = frame;

  if (options.max_edge > 0 &&
      std::max(full_width, full_height) > options.max_edge) {
    const double scale = static_cast<double>(options.max_edge) /
                         static_cast<double>(std::max(full_width, full_height));
    const UINT target_width =
        std::max<UINT>(1, static_cast<UINT>(full_width * scale + 0.5));
    const UINT target_height =
        std::max<UINT>(1, static_cast<UINT>(full_height * scale + 0.5));

    // Scale before rotating: the scaler then stays pull-based over the source
    // rows (important for 8K thumbnails) and the rotation is a small copy.
    ComPtr<IWICBitmapScaler> scaler;
    UINT scaler_width = target_width;
    UINT scaler_height = target_height;
    if (orientation >= 5) std::swap(scaler_width, scaler_height);
    hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
    if (SUCCEEDED(hr)) {
      hr = scaler->Initialize(frame.Get(), scaler_width, scaler_height,
                              WICBitmapInterpolationModeFant);
    }
    if (SUCCEEDED(hr)) {
      source = scaler;
      width = target_width;
      height = target_height;
    }
  }

  ComPtr<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(converter.GetAddressOf());
  if (FAILED(hr)) {
    result.error = HResultError("WIC: CreateFormatConverter failed", hr);
    return result;
  }
  hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                             WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    result.error = HResultError("WIC: pixel format conversion failed", hr);
    return result;
  }

  ComPtr<IWICBitmapSource> working_source = converter;
  const std::vector<uint8_t> profile = ReadIccProfile(factory.Get(), frame.Get());
  bool icc_applied = false;
  bool merge_alpha = false;
  if (!profile.empty() && !nv::IsSrgbProfile(profile.data(), profile.size())) {
    ComPtr<IWICColorTransform> transform;
    const bool alpha_source = IsAlphaFormat(source_format);
    bool converted = false;
    if (alpha_source) {
      // IWICColorTransform leaves 32-bit BGRA sources untouched on this
      // platform. Drop alpha, convert 32bppBGR, then merge the original alpha
      // plane back after CopyPixels.
      ComPtr<IWICFormatConverter> opaque;
      if (SUCCEEDED(factory->CreateFormatConverter(opaque.GetAddressOf())) &&
          SUCCEEDED(opaque->Initialize(converter.Get(),
                                       GUID_WICPixelFormat32bppBGR,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
        converted = CreateSrgbColorTransform(factory.Get(), opaque.Get(),
                                             profile, &transform);
        merge_alpha = converted;
      }
    }
    if (!converted) {
      // JPEG only applies the explicit source profile when the transform sits
      // directly above the frame; other codecs prefer the BGRA output.
      converted =
          CreateSrgbColorTransform(factory.Get(), source.Get(), profile,
                                   &transform) ||
          CreateSrgbColorTransform(factory.Get(), converter.Get(), profile,
                                   &transform);
    }
    if (converted) {
      working_source = transform;
      icc_applied = true;
    } else {
      const std::string description =
          nv::IccProfileDescription(profile.data(), profile.size());
      nv::platform::LogInfo("wic: ICC conversion failed, keeping source pixels (" +
                            (description.empty() ? std::string("unnamed") : description) +
                            ")");
    }
  }

  // Rotate only after the frame is BGRA. Rotating the JPEG frame directly is
  // pathologically slow (several seconds for 1600x1000) because the rotator
  // pulls columns through the codec; rotating a decoded BGRA surface is a
  // plain copy.
  ComPtr<IWICBitmapSource> final_source = working_source;
  if (orientation != 1) {
    ComPtr<IWICBitmapFlipRotator> rotator;
    hr = factory->CreateBitmapFlipRotator(rotator.GetAddressOf());
    if (SUCCEEDED(hr)) {
      hr = rotator->Initialize(working_source.Get(),
                               OrientationTransform(orientation));
    }
    if (FAILED(hr)) {
      result.error = HResultError("WIC: EXIF orientation transform failed", hr);
      return result;
    }
    final_source = rotator;
  }

  const uint32_t stride = static_cast<uint32_t>(width) * 4u;
  Image image;
  image.info.width = width;
  image.info.height = height;
  image.info.stride = stride;
  image.info.format = PixelFormat::kBgra8;
  image.info.has_alpha = IsAlphaFormat(source_format);
  image.info.full_width = full_width;
  image.info.full_height = full_height;
  image.info.orientation = orientation;
  image.info.orientation_applied = orientation != 1;
  image.info.icc_profile = profile;
  image.info.icc_applied = icc_applied;
  UINT frame_count = 1;
  if (SUCCEEDED(decoder->GetFrameCount(&frame_count)) && frame_count > 0) {
    image.info.frame_count = frame_count;
    // The WIC path always decodes frame 0 only. A container with more than one
    // image (HEIC live-photo pairs, multi-page TIFF) is *not* an animation the
    // product can play, so `animated` must stay false: it gates the animation
    // pill and the image caches. Before this, HEIC/multi-page TIFF were never
    // cached and were re-decoded on every visit (each HEIC decode also leaves
    // semaphores behind in the system HEIF codec; soak measured ~1.5 handles
    // per decode). GIF/APNG/WebP fallbacks re-assert `animated` themselves.
    image.info.animated = false;
  }
  image.info.source = "wic";
  image.pixels.resize(static_cast<size_t>(stride) * height);

  hr = final_source->CopyPixels(nullptr, stride,
                                static_cast<UINT>(image.pixels.size()),
                                image.pixels.data());
  if (FAILED(hr)) {
    result.error = HResultError("WIC: CopyPixels failed", hr);
    return result;
  }

  if (merge_alpha) {
    const uint32_t raw_width = OrientationSwapsAxes(orientation) ? height : width;
    const uint32_t raw_height = OrientationSwapsAxes(orientation) ? width : height;
    const uint32_t raw_stride = raw_width * 4u;
    std::vector<uint8_t> original(static_cast<size_t>(raw_stride) * raw_height);
    if (SUCCEEDED(converter->CopyPixels(nullptr, raw_stride,
                                        static_cast<UINT>(original.size()),
                                        original.data()))) {
      if (orientation == 1) {
        for (size_t i = 3; i < original.size(); i += 4) {
          image.pixels[i] = original[i];
        }
      } else {
        uint32_t rotated_width = 0;
        uint32_t rotated_height = 0;
        const std::vector<uint8_t> rotated = nv::TransformBgra(
            original.data(), raw_width, raw_height, raw_stride, orientation,
            &rotated_width, &rotated_height);
        if (rotated.size() == image.pixels.size()) {
          for (size_t i = 3; i < rotated.size(); i += 4) {
            image.pixels[i] = rotated[i];
          }
        }
      }
    }
  }

  if (orientation != 1 || icc_applied) {
    char line[192]{};
    std::snprintf(line, sizeof(line),
                  "wic: orientation=%u applied=%d icc=%s profile=\"%s\"",
                  static_cast<unsigned>(orientation), orientation != 1 ? 1 : 0,
                  icc_applied ? "converted" : (profile.empty() ? "none" : "kept"),
                  nv::IccProfileDescription(profile.data(), profile.size()).c_str());
    nv::platform::LogInfo(line);
  }

  if (image.info.animated) {
    char line[128]{};
    std::snprintf(line, sizeof(line),
                  "wic: animated source with %u frames; first frame only",
                  static_cast<unsigned>(image.info.frame_count));
    nv::platform::LogInfo(line);
  }

  result.image = std::make_shared<Image>(std::move(image));
  return result;
}

}  // namespace

DecodeResult Decode(const std::filesystem::path& path, const DecodeOptions& options) {
  DecodeResult result;
  if (!EnsureComInitialized()) {
    result.error = Error::Make(kDecodeErrorBase, "WIC: COM initialization failed");
    return result;
  }

  ComPtr<IWICImagingFactory> factory = CreateFactory();
  if (factory == nullptr) {
    result.error = Error::Make(kDecodeErrorBase, "WIC: factory creation failed");
    return result;
  }

  ComPtr<IWICBitmapDecoder> decoder =
      OpenDecoder(factory.Get(), path, &result.error);
  if (decoder == nullptr) return result;
  return DecodeOpened(factory, decoder, options);
}

DecodeResult DecodeMemory(const uint8_t* data, size_t size,
                          const DecodeOptions& options) {
  DecodeResult result;
  if (data == nullptr || size == 0) {
    result.error = Error::Make(kDecodeErrorBase, "WIC: empty memory payload");
    return result;
  }
  if (!EnsureComInitialized()) {
    result.error = Error::Make(kDecodeErrorBase, "WIC: COM initialization failed");
    return result;
  }

  ComPtr<IWICImagingFactory> factory = CreateFactory();
  if (factory == nullptr) {
    result.error = Error::Make(kDecodeErrorBase, "WIC: factory creation failed");
    return result;
  }

  // SHCreateMemStream copies the bytes into an owned IStream, so the caller's
  // buffer may be freed as soon as this function returns.
  ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(data, static_cast<UINT>(size)));
  if (stream == nullptr) {
    result.error = Error::Make(kDecodeErrorBase,
                               "WIC: memory stream creation failed");
    return result;
  }

  ComPtr<IWICBitmapDecoder> decoder;
  const HRESULT hr = factory->CreateDecoderFromStream(
      stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
      decoder.GetAddressOf());
  if (FAILED(hr) || decoder == nullptr) {
    result.error = HResultError("WIC: CreateDecoderFromStream failed", hr);
    return result;
  }
  return DecodeOpened(factory, decoder, options);
}

}  // namespace nv::wic
