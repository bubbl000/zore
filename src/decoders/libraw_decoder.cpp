#include "decoders/libraw_decoder.h"

#include "core/image_transform.h"
#include "decoders/wic_decoder.h"
#include "platform/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef NV_WITH_LIBRAW
#pragma warning(push)
#pragma warning(disable : 4251)
#include <libraw/libraw.h>
#pragma warning(pop)
#endif

namespace nv::libraw {
namespace {

constexpr int kDecodeErrorBase = 2200;

// LibRaw dcraw flip mapping is independent of the build option so the
// unit tests can exercise it in Debug builds as well.
uint16_t ExifFromFlip(int flip) {
  // LibRaw dcraw flip codes: 0 none, 1 mirror-H, 2 mirror-V, 3 rotate 180,
  // 4 transpose, 5 rotate 270 CW, 6 rotate 90 CW, 7 transverse.
  switch (flip) {
    case 1: return 2;
    case 2: return 4;
    case 3: return 3;
    case 4: return 5;
    case 5: return 8;
    case 6: return 6;
    case 7: return 7;
    case 90: return 6;
    case 180: return 3;
    case 270: return 8;
    default: return 1;
  }
}

bool FlipSwapsAxes(int flip) {
  return (flip >= 4 && flip <= 7) || flip == 90 || flip == 270;
}

int NormalizedFlip(int flip) {
  if (flip == 90 || flip == 180 || flip == 270) return flip;
  if (flip >= 0 && flip <= 7) return flip;
  return 0;
}


#ifdef NV_WITH_LIBRAW

// LibRaw keeps process-global tables; serializing whole RAW decodes is the
// simplest way to avoid races between the two decode workers and the
// thumbnail worker. Ordinary formats stay fully parallel.
std::mutex& RawMutex() {
  static std::mutex mutex;
  return mutex;
}

struct RawHandle {
  ::libraw_data_t* raw = nullptr;
  RawHandle() { raw = ::libraw_init(0); }
  ~RawHandle() {
    if (raw != nullptr) ::libraw_close(raw);
  }
  RawHandle(const RawHandle&) = delete;
  RawHandle& operator=(const RawHandle&) = delete;
};

Error RawError(const char* what, int code) {
  std::string message = std::string(what) + ": ";
  const char* text = ::libraw_strerror(code);
  message += text == nullptr ? "unknown LibRaw error" : text;
  return Error::Make(kDecodeErrorBase, std::move(message), static_cast<HRESULT>(code));
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>* bytes,
              std::wstring* error) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > 512ull * 1024 * 1024) {
    if (error != nullptr) *error = L"无法读取 RAW 文件";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error != nullptr) *error = L"无法打开 RAW 文件";
    return false;
  }
  bytes->resize(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
  if (!file) {
    if (error != nullptr) *error = L"RAW 文件读取不完整";
    return false;
  }
  return true;
}

uint32_t OrientedWidth(const ::libraw_data_t* raw) {
  const int flip = NormalizedFlip(raw->sizes.flip);
  const uint32_t width = raw->sizes.width != 0 ? raw->sizes.width
                                               : raw->sizes.iwidth;
  const uint32_t height = raw->sizes.height != 0 ? raw->sizes.height
                                                 : raw->sizes.iheight;
  return FlipSwapsAxes(flip) ? height : width;
}

uint32_t OrientedHeight(const ::libraw_data_t* raw) {
  const int flip = NormalizedFlip(raw->sizes.flip);
  const uint32_t width = raw->sizes.width != 0 ? raw->sizes.width
                                               : raw->sizes.iwidth;
  const uint32_t height = raw->sizes.height != 0 ? raw->sizes.height
                                                 : raw->sizes.iheight;
  return FlipSwapsAxes(flip) ? width : height;
}

int OpenBuffer(::libraw_data_t* raw, const std::vector<uint8_t>& bytes) {
  const int rc = ::libraw_open_buffer(raw, bytes.data(), bytes.size());
  // LIBRAW_NO_THUMBNAIL means the RAW opened fine but has no preview image.
  if (rc == LIBRAW_NO_THUMBNAIL) return LIBRAW_SUCCESS;
  return rc;
}

std::vector<uint8_t> ResizeBgra(const uint8_t* source, uint32_t width,
                                uint32_t height, uint32_t stride,
                                uint32_t target_width, uint32_t target_height) {
  std::vector<uint8_t> destination(
      static_cast<size_t>(target_width) * target_height * 4u);
  for (uint32_t y = 0; y < target_height; ++y) {
    const uint32_t y0 = static_cast<uint32_t>(
        static_cast<uint64_t>(y) * height / target_height);
    uint32_t y1 = static_cast<uint32_t>(
        static_cast<uint64_t>(y + 1) * height / target_height);
    if (y1 <= y0) y1 = y0 + 1;
    for (uint32_t x = 0; x < target_width; ++x) {
      const uint32_t x0 = static_cast<uint32_t>(
          static_cast<uint64_t>(x) * width / target_width);
      uint32_t x1 = static_cast<uint32_t>(
          static_cast<uint64_t>(x + 1) * width / target_width);
      if (x1 <= x0) x1 = x0 + 1;

      uint64_t sum_b = 0;
      uint64_t sum_g = 0;
      uint64_t sum_r = 0;
      uint64_t count = 0;
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const uint8_t* row = source + static_cast<size_t>(sy) * stride;
        for (uint32_t sx = x0; sx < x1; ++sx) {
          const uint8_t* pixel = row + static_cast<size_t>(sx) * 4u;
          sum_b += pixel[0];
          sum_g += pixel[1];
          sum_r += pixel[2];
          ++count;
        }
      }
      uint8_t* out = destination.data() +
                     (static_cast<size_t>(y) * target_width + x) * 4u;
      out[0] = static_cast<uint8_t>(sum_b / count);
      out[1] = static_cast<uint8_t>(sum_g / count);
      out[2] = static_cast<uint8_t>(sum_r / count);
      out[3] = 255;
    }
  }
  return destination;
}

// Fixed-size LibRaw char arrays: stop at the first NUL, then convert as UTF-8
// with an ANSI fallback (the same rule the EXIF parser uses).
std::wstring WideFromBytes(const char* data, size_t size) {
  if (data == nullptr) return std::wstring();
  size_t length = 0;
  while (length < size && data[length] != '\0') ++length;
  if (length == 0) return std::wstring();
  const int input = static_cast<int>(length);
  int wide_length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, input, nullptr, 0);
  UINT code_page = CP_UTF8;
  if (wide_length <= 0) {
    code_page = CP_ACP;
    wide_length = MultiByteToWideChar(code_page, 0, data, input, nullptr, 0);
  }
  if (wide_length <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(wide_length), L'\0');
  MultiByteToWideChar(code_page, code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                      data, input, out.data(), wide_length);
  return out;
}

// LibRaw reports shutter/aperture/focal length/GPS as floats; the formatting
// helpers take exact rationals, so approximate with 1/1000 precision (the
// exposure helper reduces the fraction back to "1/125" style text).
ExifRational RationalFromFloat(float value) {
  ExifRational rational;
  if (!(value > 0.0f)) return rational;
  if (value < 1.0f) {
    rational.numerator = 1;
    rational.denominator = static_cast<int64_t>(1.0 / value + 0.5);
  } else {
    rational.numerator = static_cast<int64_t>(value * 1000.0f + 0.5f);
    rational.denominator = 1000;
  }
  return rational;
}

#endif  // NV_WITH_LIBRAW

}  // namespace

bool Available() {
#ifdef NV_WITH_LIBRAW
  return true;
#else
  return false;
#endif
}

uint16_t ExifOrientationFromFlip(int flip) {
  return ExifFromFlip(NormalizedFlip(flip));
}

bool FlipSwapsImageAxes(int flip) {
  return FlipSwapsAxes(NormalizedFlip(flip));
}

ProbeResult Probe(const std::filesystem::path& path) {
  ProbeResult result;
#ifdef NV_WITH_LIBRAW
  std::lock_guard lock(RawMutex());
  RawHandle handle;
  if (handle.raw == nullptr) {
    result.error = Error::Make(kDecodeErrorBase, "LibRaw: init failed");
    return result;
  }
  std::vector<uint8_t> bytes;
  std::wstring read_error;
  if (!ReadFile(path, &bytes, &read_error)) {
    result.error = Error::Make(kDecodeErrorBase, "LibRaw: cannot read file");
    return result;
  }
  const int rc = OpenBuffer(handle.raw, bytes);
  if (rc != LIBRAW_SUCCESS) {
    result.error = RawError("LibRaw: open failed", rc);
    return result;
  }
  const uint32_t width = OrientedWidth(handle.raw);
  const uint32_t height = OrientedHeight(handle.raw);
  if (width == 0 || height == 0) {
    result.error = Error::Make(kDecodeErrorBase, "LibRaw: invalid dimensions");
    return result;
  }
  result.width = width;
  result.height = height;
  result.has_alpha = false;
  result.orientation = ExifFromFlip(NormalizedFlip(handle.raw->sizes.flip));
  result.orientation_applied = result.orientation != 1;
  result.decoder = "libraw";
#else
  (void)path;
  result.error = Error::Make(kDecodeErrorBase, "LibRaw: not built");
#endif
  return result;
}

DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options) {
  DecodeResult result;
#ifdef NV_WITH_LIBRAW
  std::lock_guard lock(RawMutex());
  RawHandle handle;
  if (handle.raw == nullptr) {
    result.error = Error::Make(kDecodeErrorBase, "LibRaw: init failed");
    return result;
  }
  std::vector<uint8_t> bytes;
  std::wstring read_error;
  if (!ReadFile(path, &bytes, &read_error)) {
    result.error = Error::Make(kDecodeErrorBase, "LibRaw: cannot read file");
    return result;
  }

  // Parameters must be set before open; half_size is only used for previews.
  ::libraw_data_t* raw = handle.raw;
  raw->params.half_size = options.max_edge > 0 ? 1 : 0;
  raw->params.user_qual = options.max_edge > 0 ? 0 : 2;  // bilinear / PPG
  raw->params.use_camera_wb = 1;
  // V3.1-10: auto-brighten like a camera JPEG does. With `no_auto_bright = 1`
  // the full decode came out at ~59 % of the embedded preview's level (measured
  // on a CR3: mean RGB 63/55/47 vs 106/93/82, per-channel ratio 0.57-0.59 - i.e.
  // the white balance matched and only the exposure was off), so the fit tier
  // and the full frame looked like two different pictures. LibRaw's own
  // auto-brighten scales so the brightest channel fills the histogram, which is
  // the same intent the in-camera JPEG uses; the camera JPEG's contrast curve
  // still differs, but the level now matches.
  raw->params.no_auto_bright = 0;
  // V3.1-10: a small explicit lift on top of the auto-brighten. Settings:
  // `no_auto_bright = 0`, `bright = 1.10f`, `use_camera_matrix = 1`. Measured on
  // three CR3 files against the camera's embedded preview, the mean level went
  // from 0.84-0.87 (auto-brighten alone) to ~0.89 with this lift, and clipping
  // stays at ~1.3 % (bright=1.2 would reach 0.92 but clip 3 %).
  raw->params.bright = 1.10f;
  raw->params.use_camera_matrix = 1;
  raw->params.output_bps = 8;
  raw->params.user_flip = -1;  // use sizes.flip; pixels come out upright

  const int open_rc = OpenBuffer(raw, bytes);
  if (open_rc != LIBRAW_SUCCESS) {
    result.error = RawError("LibRaw: open failed", open_rc);
    return result;
  }

  const uint32_t full_width = OrientedWidth(raw);
  const uint32_t full_height = OrientedHeight(raw);
  const uint16_t orientation = ExifFromFlip(NormalizedFlip(raw->sizes.flip));

  int rc = ::libraw_unpack(raw);
  if (rc != LIBRAW_SUCCESS) {
    result.error = RawError("LibRaw: unpack failed", rc);
    return result;
  }
  rc = ::libraw_dcraw_process(raw);
  if (rc != LIBRAW_SUCCESS) {
    result.error = RawError("LibRaw: process failed", rc);
    return result;
  }
  int make_error = LIBRAW_SUCCESS;
  ::libraw_processed_image_t* image =
      ::libraw_dcraw_make_mem_image(raw, &make_error);
  if (image == nullptr || make_error != LIBRAW_SUCCESS ||
      image->type != LIBRAW_IMAGE_BITMAP || image->colors != 3 ||
      image->bits != 8) {
    if (image != nullptr) ::libraw_dcraw_clear_mem(image);
    result.error = Error::Make(kDecodeErrorBase,
                               "LibRaw: unexpected processed image format");
    return result;
  }
  std::unique_ptr<::libraw_processed_image_t, void (*)(::libraw_processed_image_t*)>
      holder(image, [](::libraw_processed_image_t* value) {
        ::libraw_dcraw_clear_mem(value);
      });

  const uint32_t source_width = image->width;
  const uint32_t source_height = image->height;
  if (source_width == 0 || source_height == 0) {
    result.error = Error::Make(kDecodeErrorBase, "LibRaw: empty image");
    return result;
  }

  uint32_t output_width = source_width;
  uint32_t output_height = source_height;
  if (options.max_edge > 0 &&
      std::max(source_width, source_height) > options.max_edge) {
    const double scale = static_cast<double>(options.max_edge) /
                         static_cast<double>(std::max(source_width, source_height));
    output_width =
        std::max<uint32_t>(1, static_cast<uint32_t>(source_width * scale + 0.5));
    output_height =
        std::max<uint32_t>(1, static_cast<uint32_t>(source_height * scale + 0.5));
  }

  Image decoded;
  decoded.info.width = output_width;
  decoded.info.height = output_height;
  decoded.info.stride = output_width * 4u;
  decoded.info.format = PixelFormat::kBgra8;
  decoded.info.has_alpha = false;
  decoded.info.orientation = orientation;
  decoded.info.orientation_applied = orientation != 1;
  decoded.info.full_width = full_width;
  decoded.info.full_height = full_height;
  decoded.info.source = options.max_edge > 0 ? "libraw_half" : "libraw";

  const uint8_t* source = image->data;
  if (output_width != source_width || output_height != source_height) {
    std::vector<uint8_t> bgra(static_cast<size_t>(source_width) *
                              source_height * 4u);
    uint8_t* destination = bgra.data();
    const size_t count = static_cast<size_t>(source_width) * source_height;
    for (size_t i = 0; i < count; ++i) {
      destination[0] = source[2];
      destination[1] = source[1];
      destination[2] = source[0];
      destination[3] = 255;
      source += 3;
      destination += 4;
    }
    decoded.pixels = ResizeBgra(bgra.data(), source_width, source_height,
                                source_width * 4u, output_width, output_height);
  } else {
    decoded.pixels.resize(static_cast<size_t>(output_width) * output_height * 4u);
    uint8_t* destination = decoded.pixels.data();
    const size_t count = static_cast<size_t>(source_width) * source_height;
    for (size_t i = 0; i < count; ++i) {
      destination[0] = source[2];
      destination[1] = source[1];
      destination[2] = source[0];
      destination[3] = 255;
      source += 3;
      destination += 4;
    }
  }

  result.image = std::make_shared<Image>(std::move(decoded));
#else
  (void)path;
  (void)options;
  result.error = Error::Make(kDecodeErrorBase, "LibRaw: not built");
#endif
  return result;
}

DecodeResult DecodeEmbeddedPreview(const std::filesystem::path& path) {
  DecodeResult result;
#ifdef NV_WITH_LIBRAW
  const ULONGLONG preview_started = GetTickCount64();
  std::lock_guard lock(RawMutex());
  RawHandle handle;
  if (handle.raw == nullptr) {
    result.error = Error::Make(kDecodeErrorBase, "LibRaw: init failed");
    return result;
  }
  std::vector<uint8_t> bytes;
  std::wstring read_error;
  if (!ReadFile(path, &bytes, &read_error)) {
    result.error = Error::Make(kDecodeErrorBase, "LibRaw: cannot read file");
    return result;
  }

  ::libraw_data_t* raw = handle.raw;
  const int open_rc = OpenBuffer(raw, bytes);
  if (open_rc != LIBRAW_SUCCESS) {
    result.error = RawError("LibRaw: open failed", open_rc);
    return result;
  }

  const uint32_t full_width = OrientedWidth(raw);
  const uint32_t full_height = OrientedHeight(raw);
  const uint16_t orientation = ExifFromFlip(NormalizedFlip(raw->sizes.flip));

  const int rc = ::libraw_unpack_thumb(raw);
  if (rc != LIBRAW_SUCCESS) {
    result.error = RawError("LibRaw: unpack thumb failed", rc);
    return result;
  }

  const ::libraw_thumbnail_t& thumb = raw->thumbnail;
  {
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "libraw: thumb format=%d %ux%u bytes=%u",
                  static_cast<int>(thumb.tformat),
                  static_cast<unsigned>(thumb.twidth),
                  static_cast<unsigned>(thumb.theight),
                  static_cast<unsigned>(thumb.tlength));
    nv::platform::LogInfo(line);
  }
  // Only JPEG previews go through WIC here; BITMAP/RGB previews are rare and
  // would need a separate conversion path (規劃/22/13 §3.3 scope).
  if (thumb.tformat != LIBRAW_THUMBNAIL_JPEG || thumb.thumb == nullptr ||
      thumb.tlength == 0) {
    // CR3 previews are often H.265 (LIBRAW_THUMBNAIL_H265), which WIC cannot
    // open from an elementary stream; those files fall back to the cached
    // thumbnail on later opens (規劃/22/13 §3.3 scope).
    result.error = Error::Make(
        kDecodeErrorBase,
        "LibRaw: no embedded JPEG preview (format=" +
            std::to_string(static_cast<int>(thumb.tformat)) + ")");
    return result;
  }
  // Camera thumbnails can be as small as 160x120; scaling one up looks worse
  // than waiting for the full frame (規劃/22/13 坑 7). The size is checked after
  // decoding because some cameras report 0x0 for a valid JPEG preview.

  // Cap the preview at 1920 px on the long edge: the embedded JPEG is often
  // full-resolution, and the preview only ever needs to fill a window. WIC's
  // scaler is pull-based, so this also keeps the decode cheap.
  DecodeOptions preview_options;
  preview_options.max_edge = 1920;
  preview_options.animation = false;
  DecodeResult decoded = wic::DecodeMemory(
      reinterpret_cast<const uint8_t*>(thumb.thumb),
      static_cast<size_t>(thumb.tlength), preview_options);
  if (!decoded.ok()) {
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "libraw: embedded preview decode failed (%s)",
                  decoded.error.message.c_str());
    nv::platform::LogInfo(line);
    return decoded;
  }
  if (decoded.image->info.width < 320 || decoded.image->info.height < 320) {
    result.error = Error::Make(kDecodeErrorBase,
                               "LibRaw: embedded preview too small");
    return result;
  }

  // The embedded JPEG usually has no orientation tag of its own. Bake the main
  // image's flip so the preview is upright exactly like the final frame
  // (規劃/22/13 坑 2); if the payload already carried its own orientation, keep
  // it — WIC has applied it and the two normally agree.
  if (decoded.image->info.orientation == 1 && orientation != 1) {
    nv::ApplyOrientation(decoded.image.get(), orientation);
  }
  decoded.image->info.orientation = orientation;
  decoded.image->info.orientation_applied = orientation != 1;
  decoded.image->info.full_width = full_width;
  decoded.image->info.full_height = full_height;
  decoded.image->info.source = "libraw_thumb";

  {
    char line[192]{};
    std::snprintf(line, sizeof(line),
                  "libraw: preview %ux%u full=%ux%u orient=%u bytes=%u in %llums",
                  decoded.image->info.width, decoded.image->info.height,
                  full_width, full_height,
                  static_cast<unsigned>(orientation),
                  static_cast<unsigned>(thumb.tlength),
                  static_cast<unsigned long long>(GetTickCount64() -
                                                  preview_started));
    nv::platform::LogInfo(line);
  }

  result.image = std::move(decoded.image);
  return result;
#else
  (void)path;
  result.error = Error::Make(kDecodeErrorBase, "LibRaw: not built");
  return result;
#endif
}

ExifData ReadExif(const std::filesystem::path& path) {  ExifData data;
#ifdef NV_WITH_LIBRAW
  std::lock_guard lock(RawMutex());
  RawHandle handle;
  if (handle.raw == nullptr) return data;
  std::vector<uint8_t> bytes;
  std::wstring read_error;
  if (!ReadFile(path, &bytes, &read_error)) return data;
  // Opening parses the TIFF/EXIF block; unpack (raw pixels) is not needed and
  // is what makes a RAW decode slow.
  if (OpenBuffer(handle.raw, bytes) != LIBRAW_SUCCESS) return data;

  const ::libraw_data_t* raw = handle.raw;
  data.Set(ExifId::kMake, WideFromBytes(raw->idata.make, sizeof(raw->idata.make)));
  data.Set(ExifId::kModel,
           WideFromBytes(raw->idata.model, sizeof(raw->idata.model)));
  data.Set(ExifId::kSoftware,
           WideFromBytes(raw->idata.software, sizeof(raw->idata.software)));

  std::wstring lens = WideFromBytes(raw->lens.Lens, sizeof(raw->lens.Lens));
  if (lens.empty()) {
    lens = WideFromBytes(raw->lens.LensMake, sizeof(raw->lens.LensMake));
  }
  data.Set(ExifId::kLens, std::move(lens));

  const ::libraw_imgother_t& other = raw->other;
  if (other.timestamp > 0) {
    struct tm parts {};
    if (localtime_s(&parts, &other.timestamp) == 0) {
      wchar_t buffer[64]{};
      swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d", parts.tm_year + 1900,
                 parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                 parts.tm_sec);
      data.Set(ExifId::kDateTimeOriginal, buffer);
    }
  }
  if (other.shutter > 0.0f) {
    data.Set(ExifId::kExposureTime,
             FormatExposureTime(RationalFromFloat(other.shutter)));
  }
  if (other.aperture > 0.0f) {
    data.Set(ExifId::kFNumber,
             FormatAperture(RationalFromFloat(other.aperture)));
  }
  if (other.iso_speed > 0.0f) {
    data.Set(ExifId::kIso, FormatIso(static_cast<int>(other.iso_speed + 0.5f)));
  }
  if (other.focal_len > 0.0f) {
    data.Set(ExifId::kFocalLength,
             FormatFocalLength(RationalFromFloat(other.focal_len)));
  }

  const ::libraw_gps_info_t& gps = other.parsed_gps;
  if (gps.gpsparsed != 0) {
    const ExifRational lat[3] = {RationalFromFloat(gps.latitude[0]),
                                 RationalFromFloat(gps.latitude[1]),
                                 RationalFromFloat(gps.latitude[2])};
    const ExifRational lon[3] = {RationalFromFloat(gps.longitude[0]),
                                 RationalFromFloat(gps.longitude[1]),
                                 RationalFromFloat(gps.longitude[2])};
    data.Set(ExifId::kGpsLatitude,
             FormatGpsCoordinate(lat[0], lat[1], lat[2],
                                 gps.latref != 0 ? gps.latref : 'N'));
    data.Set(ExifId::kGpsLongitude,
             FormatGpsCoordinate(lon[0], lon[1], lon[2],
                                 gps.longref != 0 ? gps.longref : 'E'));
    if (gps.altitude != 0.0f) {
      const bool below = gps.altref != 0;
      data.Set(ExifId::kGpsAltitude,
               FormatGpsAltitude(RationalFromFloat(gps.altitude), below));
    }
  }
  data.ok = data.Any();
#else
  (void)path;
#endif
  return data;
}

}  // namespace nv::libraw
