// V3.1-05 spike: libjpeg-turbo DCT-scaled JPEG decode vs the product's WIC
// path (full entropy decode + IWICBitmapScaler Fant).
//
// This tool is NOT part of the product build. It has its own CMake project so
// that the libjpeg-turbo static library never collides with the libjpeg that
// FreeImage already links into zoreview_core (duplicate `jpeg_*` symbols).
//
// Arms:
//   wic        - replicate zoreview's wic_decoder path for a JPEG with no ICC
//                profile and no EXIF orientation: CreateDecoderFromFilename ->
//                GetFrame(0) -> IWICBitmapScaler(Fant, max_edge) ->
//                IWICFormatConverter(32bppBGRA) -> CopyPixels.
//   turbo      - libjpeg-turbo decode with DCT scaling (scale_num/scale_denom),
//                JCS_EXT_BGR output -> IWICBitmap(24bppBGR) -> the same
//                IWICBitmapScaler(Fant, max_edge) -> the same converter ->
//                CopyPixels. Only the decoder differs.
//
// Both arms produce BGRA8 at the requested edge, so the tool also reports the
// mean/max absolute pixel difference between them (sanity, not a quality gate).
//
// Build/run: see run_jpeg_spike.ps1 in this directory.

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "jpeglib.h"
}

using Microsoft::WRL::ComPtr;

namespace {

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> bgra;
  bool ok = false;
  std::string error;
};

struct Timer {
  LARGE_INTEGER start;
  Timer() { QueryPerformanceCounter(&start); }
  double Ms() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
  }
};

bool ReadFileBytes(const wchar_t* path, std::vector<uint8_t>* out,
                   std::string* error) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = "open failed";
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
    CloseHandle(file);
    *error = "size failed";
    return false;
  }
  out->resize(static_cast<size_t>(size.QuadPart));
  size_t done = 0;
  while (done < out->size()) {
    DWORD read = 0;
    const DWORD want = static_cast<DWORD>(
        std::min<size_t>(out->size() - done, 1u << 20));
    if (!ReadFile(file, out->data() + done, want, &read, nullptr) || read == 0) {
      CloseHandle(file);
      *error = "read failed";
      return false;
    }
    done += read;
  }
  CloseHandle(file);
  return true;
}

uint16_t ReadOrientation(IWICBitmapFrameDecode* frame) {
  ComPtr<IWICMetadataQueryReader> reader;
  if (FAILED(frame->GetMetadataQueryReader(reader.GetAddressOf())) ||
      !reader) {
    return 1;
  }
  static const wchar_t* kQueries[] = {
      L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
  for (const wchar_t* query : kQueries) {
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
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

uint32_t ReadIccSize(IWICImagingFactory* factory,
                     IWICBitmapFrameDecode* frame) {
  UINT count = 0;
  if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) {
    return 0;
  }
  std::vector<ComPtr<IWICColorContext>> contexts(count);
  std::vector<IWICColorContext*> raw(count, nullptr);
  for (UINT i = 0; i < count; ++i) {
    if (FAILED(factory->CreateColorContext(contexts[i].GetAddressOf()))) {
      return 0;
    }
    raw[i] = contexts[i].Get();
  }
  UINT actual = 0;
  if (FAILED(frame->GetColorContexts(count, raw.data(), &actual))) return 0;
  uint32_t total = 0;
  for (UINT i = 0; i < actual; ++i) {
    UINT size = 0;
    if (SUCCEEDED(raw[i]->GetProfileBytes(0, nullptr, &size)) && size > 0) {
      total += size;
      break;
    }
  }
  return total;
}

// Shared tail of both arms: scale (if any) with WIC Fant, convert to BGRA8.
// `fill` scales up as well, so a 1/8 decode can be compared with the WIC arm at
// exactly the same output size (the product itself never upscales a tier).
bool FinishThroughWic(IWICImagingFactory* factory, IWICBitmapSource* source,
                      uint32_t target_edge, bool fill, Image* out) {
  UINT width = 0;
  UINT height = 0;
  if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0) {
    out->error = "source size";
    return false;
  }
  ComPtr<IWICBitmapSource> current = source;
  const bool need_scale =
      target_edge > 0 &&
      ((!fill && std::max(width, height) > target_edge) ||
       (fill && std::max(width, height) != target_edge));
  if (need_scale) {
    const double scale = static_cast<double>(target_edge) /
                         static_cast<double>(std::max(width, height));
    const UINT tw = std::max<UINT>(1, static_cast<UINT>(width * scale + 0.5));
    const UINT th = std::max<UINT>(1, static_cast<UINT>(height * scale + 0.5));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(source, tw, th, WICBitmapInterpolationModeFant))) {
      out->error = "scaler";
      return false;
    }
    current = scaler;
    width = tw;
    height = th;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
      FAILED(converter->Initialize(current.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    out->error = "converter";
    return false;
  }
  out->width = width;
  out->height = height;
  out->bgra.assign(static_cast<size_t>(width) * height * 4, 0);
  const UINT stride = width * 4;
  const UINT bytes = stride * height;
  if (FAILED(converter->CopyPixels(nullptr, stride, bytes, out->bgra.data()))) {
    out->error = "CopyPixels";
    return false;
  }
  out->ok = true;
  return true;
}

WICBitmapTransformOptions OrientationTransform(uint16_t orientation) {
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

Image DecodeWic(IWICImagingFactory* factory, const wchar_t* path,
                uint32_t target_edge, bool fill, uint16_t* orientation_out,
                uint32_t* icc_out) {
  Image image;
  ComPtr<IWICBitmapDecoder> decoder;
  HRESULT hr = factory->CreateDecoderFromFilename(
      path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      decoder.GetAddressOf());
  if (FAILED(hr)) {
    image.error = "CreateDecoderFromFilename";
    return image;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
    image.error = "GetFrame";
    return image;
  }
  const uint16_t orientation = ReadOrientation(frame.Get());
  if (orientation_out) *orientation_out = orientation;
  if (icc_out) *icc_out = ReadIccSize(factory, frame.Get());
  UINT width = 0;
  UINT height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
    image.error = "frame size";
    return image;
  }
  // Mirrors zoreview's wic_decoder: scale the raw frame (swapped dims for
  // 90/270 EXIF orientations), convert to BGRA8, then rotate the BGRA surface.
  const UINT upright_w = orientation >= 5 ? height : width;
  const UINT upright_h = orientation >= 5 ? width : height;
  UINT out_w = upright_w;
  UINT out_h = upright_h;
  ComPtr<IWICBitmapSource> source = frame;
  const bool need_scale =
      target_edge > 0 &&
      ((!fill && std::max(upright_w, upright_h) > target_edge) ||
       (fill && std::max(upright_w, upright_h) != target_edge));
  if (need_scale) {
    const double scale = static_cast<double>(target_edge) /
                         static_cast<double>(std::max(upright_w, upright_h));
    const UINT tw = std::max<UINT>(1, static_cast<UINT>(upright_w * scale + 0.5));
    const UINT th = std::max<UINT>(1, static_cast<UINT>(upright_h * scale + 0.5));
    UINT sw = tw;
    UINT sh = th;
    if (orientation >= 5) std::swap(sw, sh);
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(frame.Get(), sw, sh,
                                  WICBitmapInterpolationModeFant))) {
      image.error = "scaler";
      return image;
    }
    source = scaler;
    out_w = tw;
    out_h = th;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
      FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    image.error = "converter";
    return image;
  }
  ComPtr<IWICBitmapSource> final_source = converter;
  if (orientation != 1) {
    ComPtr<IWICBitmapFlipRotator> rotator;
    if (FAILED(factory->CreateBitmapFlipRotator(rotator.GetAddressOf())) ||
        FAILED(rotator->Initialize(converter.Get(),
                                   OrientationTransform(orientation)))) {
      image.error = "rotator";
      return image;
    }
    final_source = rotator;
  }
  image.width = out_w;
  image.height = out_h;
  image.bgra.assign(static_cast<size_t>(out_w) * out_h * 4, 0);
  const UINT stride = out_w * 4;
  if (FAILED(final_source->CopyPixels(nullptr, stride, stride * out_h,
                                      image.bgra.data()))) {
    image.error = "CopyPixels";
    return image;
  }
  image.ok = true;
  return image;
}

struct TurboError {
  jpeg_error_mgr pub;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};

void TurboErrorExit(j_common_ptr cinfo) {
  TurboError* err = reinterpret_cast<TurboError*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->jump, 1);
}

// Picks the denominator N of the 1/N DCT decode whose long edge is still >=
// target_edge (i.e. the cheapest decode that does not upscale). N in {1,2,4,8};
// N=1 means a full decode.
int ChooseScaleDenom(uint32_t width, uint32_t height, uint32_t target_edge) {
  if (target_edge == 0) return 1;
  const uint32_t long_edge = std::max(width, height);
  if (long_edge <= target_edge) return 1;
  for (int denom : {8, 4, 2}) {
    if (long_edge / static_cast<uint32_t>(denom) >= target_edge) return denom;
  }
  return 1;
}

Image DecodeTurbo(IWICImagingFactory* factory, const wchar_t* path,
                  uint32_t target_edge, bool fill, bool no_tail,
                  int forced_scale) {
  Image image;
  std::vector<uint8_t> file;
  if (!ReadFileBytes(path, &file, &image.error)) return image;

  jpeg_decompress_struct cinfo{};
  TurboError err{};
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = TurboErrorExit;
  if (setjmp(err.jump)) {
    jpeg_destroy_decompress(&cinfo);
    image.error = std::string("turbo: ") + err.message;
    return image;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, file.data(), static_cast<unsigned long>(file.size()));
  jpeg_read_header(&cinfo, TRUE);

  cinfo.out_color_space = JCS_EXT_BGR;  // matches WIC 24bppBGR byte order
  const int denom =
      forced_scale > 0
          ? forced_scale
          : ChooseScaleDenom(cinfo.image_width, cinfo.image_height,
                             target_edge);
  cinfo.scale_num = 1;
  cinfo.scale_denom = denom;

  jpeg_start_decompress(&cinfo);
  const uint32_t width = cinfo.output_width;
  const uint32_t height = cinfo.output_height;
  const uint32_t components = cinfo.output_components;
  std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * components);
  const uint32_t row_stride = width * components;
  while (cinfo.output_scanline < height) {
    JSAMPROW row = bgr.data() + static_cast<size_t>(cinfo.output_scanline) * row_stride;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  if (no_tail) {
    image.width = width;
    image.height = height;
    image.ok = true;
    return image;
  }

  ComPtr<IWICBitmap> bitmap;
  if (components != 3) {
    image.error = "turbo: unexpected component count";
    return image;
  }
  HRESULT hr = factory->CreateBitmapFromMemory(
      width, height, GUID_WICPixelFormat24bppBGR, row_stride,
      static_cast<UINT>(bgr.size()), bgr.data(), bitmap.GetAddressOf());
  if (FAILED(hr)) {
    image.error = "turbo: CreateBitmapFromMemory";
    return image;
  }
  FinishThroughWic(factory, bitmap.Get(), target_edge, fill, &image);
  return image;
}

struct Options {
  std::wstring file;
  uint32_t edge = 0;
  std::wstring mode = L"wic";  // wic | turbo
  int reps = 5;
  int scale = 0;  // 0 = auto; otherwise 1, 2, 4 or 8 (denominator)
  bool compare = true;
  bool fill = false;   // scale up to the exact edge (pixel-comparable output)
  bool no_tail = false;  // turbo: measure the decoder alone (no WIC tail)
  bool verbose = false;
  std::wstring out;    // optional raw BGRA8 dump (edge-major, no header)
};

void PrintPixelDiff(const Image& a, const Image& b) {
  if (!a.ok || !b.ok || a.width != b.width || a.height != b.height) {
    std::wprintf(L"compare skipped (size mismatch or failure)\n");
    return;
  }
  double sum = 0;
  int max_diff = 0;
  size_t count = 0;
  // Sample every 7th pixel to keep this cheap on 24 MP frames.
  for (size_t i = 0; i + 3 < a.bgra.size(); i += 4 * 7) {
    for (int c = 0; c < 3; ++c) {
      const int d = std::abs(static_cast<int>(a.bgra[i + c]) -
                             static_cast<int>(b.bgra[i + c]));
      sum += d;
      max_diff = std::max(max_diff, d);
      ++count;
    }
  }
  const double mean = count ? sum / static_cast<double>(count) : 0.0;
  std::wprintf(L"compare wic=%ux%u turbo=%ux%u mean_abs=%.3f max_abs=%d\n",
               a.width, a.height, b.width, b.height, mean, max_diff);
}

// Runs the whole spike. Kept separate from wmain so that every ComPtr (the WIC
// factory included) is destroyed before CoUninitialize(); releasing a WIC
// object after COM teardown faults at exit.
int RunSpike(IWICImagingFactory* factory, const Options& options) {
  // Facts about the sample (so the A/B claim is explicit).
  uint16_t orientation = 1;
  uint32_t icc_bytes = 0;
  {
    Image probe = DecodeWic(factory, options.file.c_str(), 64, false,
                            &orientation, &icc_bytes);
    if (!probe.ok) {
      std::wprintf(L"probe failed: %hs\n", probe.error.c_str());
      return 2;
    }
    std::wprintf(L"sample file=%s edge_probe=64 orientation=%u icc_bytes=%u\n",
                 options.file.c_str(), static_cast<unsigned>(orientation),
                 icc_bytes);
  }

  std::vector<double> times;
  Image first;
  for (int rep = 0; rep < options.reps; ++rep) {
    Timer timer;
    Image image;
    if (options.mode == L"io") {
      // I/O-only floor: just the file read the turbo arm performs.
      std::vector<uint8_t> bytes;
      std::string io_error;
      image.ok = ReadFileBytes(options.file.c_str(), &bytes, &io_error);
      image.width = 1;
      image.height = 1;
      image.error = io_error;
    } else if (options.mode == L"wic") {
      image = DecodeWic(factory, options.file.c_str(), options.edge,
                        options.fill, nullptr, nullptr);
    } else {
      image = DecodeTurbo(factory, options.file.c_str(), options.edge,
                          options.fill, options.no_tail, options.scale);
    }
    const double ms = timer.Ms();
    if (!image.ok) {
      std::wprintf(L"run mode=%s edge=%u rep=%d FAILED %hs\n",
                   options.mode.c_str(), options.edge, rep + 1,
                   image.error.c_str());
      return 3;
    }
    times.push_back(ms);
    if (rep == 0) first = std::move(image);
    std::wprintf(L"run mode=%s edge=%u rep=%d ms=%.1f out=%ux%u\n",
                 options.mode.c_str(), options.edge, rep + 1, ms, first.width,
                 first.height);
  }
  std::vector<double> sorted = times;
  std::sort(sorted.begin(), sorted.end());
  const double median =
      sorted.size() % 2 ? sorted[sorted.size() / 2]
                        : 0.5 * (sorted[sorted.size() / 2 - 1] +
                                 sorted[sorted.size() / 2]);
  std::wprintf(
      L"summary mode=%s edge=%u scale=%d n=%zu min=%.1f median=%.1f max=%.1f\n",
      options.mode.c_str(), options.edge, options.scale, times.size(),
      sorted.front(), median, sorted.back());

  if (!options.out.empty()) {
    HANDLE file = CreateFileW(options.out.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      std::wprintf(L"dump failed: %s\n", options.out.c_str());
      return 4;
    }
    DWORD written = 0;
    WriteFile(file, first.bgra.data(),
              static_cast<DWORD>(first.bgra.size()), &written, nullptr);
    CloseHandle(file);
    std::wprintf(L"dump file=%s bytes=%u %ux%u\n", options.out.c_str(),
                 written, first.width, first.height);
  }

  if (options.compare) {
    Image wic = DecodeWic(factory, options.file.c_str(), options.edge,
                          options.fill, nullptr, nullptr);
    PrintPixelDiff(wic, first);
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    const bool has_next = (i + 1) < argc;
    if (arg == L"--file" && has_next) {
      options.file = argv[++i];
    } else if (arg == L"--edge" && has_next) {
      options.edge = static_cast<uint32_t>(_wtoi(argv[++i]));
    } else if (arg == L"--mode" && has_next) {
      options.mode = argv[++i];
    } else if (arg == L"--reps" && has_next) {
      options.reps = std::max(1, _wtoi(argv[++i]));
    } else if (arg == L"--scale" && has_next) {
      options.scale = _wtoi(argv[++i]);
    } else if (arg == L"--fill") {
      options.fill = true;
    } else if (arg == L"--no-tail") {
      options.no_tail = true;
    } else if (arg == L"--out" && has_next) {
      options.out = argv[++i];
    } else if (arg == L"--no-compare") {
      options.compare = false;
    } else if (arg == L"--verbose") {
      options.verbose = true;
    } else {
      std::fwprintf(stderr, L"unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }
  if (options.file.empty()) {
    std::fwprintf(stderr,
                  L"usage: jpeg_spike --file <x.jpg> --edge <px|0> "
                  L"--mode wic|turbo [--reps N] [--scale 1|2|4|8]\n");
    return 2;
  }

  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int result = 3;
  {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf())))) {
      std::fwprintf(stderr, L"WIC factory failed\n");
      result = 2;
    } else {
      result = RunSpike(factory.Get(), options);
    }
  }
  if (com == S_OK || com == S_FALSE) CoUninitialize();
  return result;
}
