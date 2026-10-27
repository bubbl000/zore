#include "decoders/freeimage_decoder.h"

#include "core/image_transform.h"
#include "platform/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef NV_WITH_FREEIMAGE
#include <FreeImage.h>
#endif

namespace nv::freeimage {
namespace {

constexpr int kDecodeErrorBase = 2100;
// Planning 22/16 §3.4: refuse absurd float images before conversion buffers
// multiply the memory. 16 bytes/pixel approximates RGBA float.
constexpr uint64_t kMaxPixels = 200ull * 1000ull * 1000ull;
constexpr uint64_t kMaxFloatBytes = 1500ull * 1000ull * 1000ull;
constexpr double kToneMapGamma = 2.2;

#ifdef NV_WITH_FREEIMAGE
void FreeImageOutput(FREE_IMAGE_FORMAT /*fif*/, const char* message) {
  if (message != nullptr) {
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
  }
}

void EnsureInitialized() {
  static std::once_flag once;
  std::call_once(once, [] {
    FreeImage_Initialise(FALSE);
    FreeImage_SetOutputMessage(FreeImageOutput);
  });
}

uint16_t ReadOrientation(FIBITMAP* dib) {
  FITAG* tag = nullptr;
  if (dib == nullptr ||
      FreeImage_GetMetadata(FIMD_EXIF_MAIN, dib, "Orientation", &tag) != TRUE ||
      tag == nullptr || FreeImage_GetTagType(tag) != FIDT_SHORT) {
    return 1;
  }
  const auto* value = static_cast<const uint16_t*>(FreeImage_GetTagValue(tag));
  if (value == nullptr) return 1;
  const uint16_t orientation = *value;
  return (orientation >= 1 && orientation <= 8) ? orientation : 1;
}

std::wstring LowerExtension(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  for (wchar_t& ch : extension) {
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
  }
  return extension;
}

// V3-02: label a file that FreeImage identified without magic/extension. The
// router only calls Probe with kUnknown for that case, so a known hint stays
// authoritative.
ImageFormat FormatFromFif(FREE_IMAGE_FORMAT fif) {
  switch (fif) {
    case FIF_BMP: return ImageFormat::kBmp;
    case FIF_ICO: return ImageFormat::kIco;
    case FIF_JPEG: return ImageFormat::kJpeg;
    case FIF_JNG: return ImageFormat::kMng;
    case FIF_MNG: return ImageFormat::kMng;
    case FIF_PBM: return ImageFormat::kPnm;
    case FIF_PGM: return ImageFormat::kPnm;
    case FIF_PNG: return ImageFormat::kPng;
    case FIF_PPM: return ImageFormat::kPnm;
    case FIF_TARGA: return ImageFormat::kTga;
    case FIF_TIFF: return ImageFormat::kTiff;
    case FIF_WBMP: return ImageFormat::kWbmp;
    case FIF_PCX: return ImageFormat::kPcx;
    case FIF_XBM: return ImageFormat::kXbm;
    case FIF_XPM: return ImageFormat::kXpm;
    case FIF_GIF: return ImageFormat::kGif;
    case FIF_HDR: return ImageFormat::kHdr;
    case FIF_EXR: return ImageFormat::kExr;
    case FIF_J2K: return ImageFormat::kJp2;
    case FIF_JP2: return ImageFormat::kJp2;
    case FIF_JXR: return ImageFormat::kJxr;
    case FIF_PFM: return ImageFormat::kPfm;
    default: return ImageFormat::kUnknown;
  }
}

FREE_IMAGE_FORMAT FifForHint(ImageFormat hint,
                             const std::filesystem::path& path) {
  switch (hint) {
    case ImageFormat::kHdr: return FIF_HDR;
    case ImageFormat::kExr: return FIF_EXR;
    case ImageFormat::kPfm: return FIF_PFM;
    case ImageFormat::kWbmp: return FIF_WBMP;
    case ImageFormat::kJxr: return FIF_JXR;
    case ImageFormat::kJp2: {
      const std::wstring extension = LowerExtension(path);
      return extension == L".j2k" || extension == L".jpc" ? FIF_J2K : FIF_JP2;
    }
    case ImageFormat::kMng:
      return LowerExtension(path) == L".jng" ? FIF_JNG : FIF_MNG;
    default:
      return FIF_UNKNOWN;
  }
}

FREE_IMAGE_FORMAT ResolveFif(const std::filesystem::path& path,
                             ImageFormat hint) {
  const FREE_IMAGE_FORMAT hinted = FifForHint(hint, path);
  if (hinted != FIF_UNKNOWN && FreeImage_FIFSupportsReading(hinted)) {
    return hinted;
  }
  return FreeImage_GetFileTypeU(path.c_str(), 0);
}

bool IsFloatType(FREE_IMAGE_TYPE type) {
  return type == FIT_FLOAT || type == FIT_RGBF || type == FIT_RGBAF ||
         type == FIT_DOUBLE;
}

// --- Lightweight header dimension probes for the float formats -------------
// FreeImage has no dimension-only API; loading a malicious/huge EXR first would
// allocate before the 200 MP / 1.5 GB guard can run.
bool ReadPfmSize(std::istream& in, uint32_t* width, uint32_t* height) {
  std::string magic;
  if (!(in >> magic) || (magic != "PF" && magic != "Pf")) return false;
  long w = 0;
  long h = 0;
  if (!(in >> w >> h) || w <= 0 || h <= 0) return false;
  *width = static_cast<uint32_t>(w);
  *height = static_cast<uint32_t>(h);
  return true;
}

bool ReadHdrSize(std::istream& in, uint32_t* width, uint32_t* height) {
  std::string line;
  for (int i = 0; i < 64 && std::getline(in, line); ++i) {
    if (line.size() < 4 || line[0] != '-' || line[1] != 'Y') continue;
    const char* plus = std::strstr(line.c_str(), "+X");
    if (plus == nullptr) continue;
    const long h = std::strtol(line.c_str() + 2, nullptr, 10);
    const long w = std::strtol(plus + 2, nullptr, 10);
    if (w <= 0 || h <= 0) return false;
    *width = static_cast<uint32_t>(w);
    *height = static_cast<uint32_t>(h);
    return true;
  }
  return false;
}

bool ReadExrSize(std::istream& in, uint32_t* width, uint32_t* height) {
  char magic[4]{};
  if (!in.read(magic, 4)) return false;
  if (static_cast<uint8_t>(magic[0]) != 0x76 ||
      static_cast<uint8_t>(magic[1]) != 0x2F ||
      static_cast<uint8_t>(magic[2]) != 0x31 ||
      static_cast<uint8_t>(magic[3]) != 0x01) {
    return false;
  }
  char version[4]{};
  if (!in.read(version, 4)) return false;

  const auto read_cstring = [&in](std::string* out) {
    out->clear();
    for (int i = 0; i < 256; ++i) {
      char ch = 0;
      if (!in.get(ch)) return false;
      if (ch == '\0') return true;
      out->push_back(ch);
    }
    return false;
  };

  for (int attribute = 0; attribute < 512; ++attribute) {
    std::string name;
    if (!read_cstring(&name)) return false;
    if (name.empty()) break;  // End of the header.
    std::string type;
    if (!read_cstring(&type)) return false;
    uint8_t size_bytes[4]{};
    if (!in.read(reinterpret_cast<char*>(size_bytes), 4)) return false;
    const uint32_t size = static_cast<uint32_t>(size_bytes[0]) |
                          (static_cast<uint32_t>(size_bytes[1]) << 8) |
                          (static_cast<uint32_t>(size_bytes[2]) << 16) |
                          (static_cast<uint32_t>(size_bytes[3]) << 24);
    if (size > 1024 * 1024) return false;
    if (name == "dataWindow" && size >= 16) {
      uint8_t box[16]{};
      if (!in.read(reinterpret_cast<char*>(box), sizeof(box))) return false;
      const auto int32_at = [&box](int offset) {
        return static_cast<int32_t>(
            static_cast<uint32_t>(box[offset]) |
            (static_cast<uint32_t>(box[offset + 1]) << 8) |
            (static_cast<uint32_t>(box[offset + 2]) << 16) |
            (static_cast<uint32_t>(box[offset + 3]) << 24));
      };
      const int32_t x0 = int32_at(0);
      const int32_t y0 = int32_at(4);
      const int32_t x1 = int32_at(8);
      const int32_t y1 = int32_at(12);
      if (x1 < x0 || y1 < y0) return false;
      *width = static_cast<uint32_t>(x1 - x0 + 1);
      *height = static_cast<uint32_t>(y1 - y0 + 1);
      return true;
    }
    in.seekg(size, std::ios::cur);
    if (!in) return false;
  }
  return false;
}

bool ReadFloatDimensions(const std::filesystem::path& path, ImageFormat hint,
                         uint32_t* width, uint32_t* height) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  switch (hint) {
    case ImageFormat::kPfm: return ReadPfmSize(in, width, height);
    case ImageFormat::kHdr: return ReadHdrSize(in, width, height);
    case ImageFormat::kExr: return ReadExrSize(in, width, height);
    default: return false;
  }
}

bool TooLarge(uint64_t width, uint64_t height, bool floating,
              std::string* detail) {
  const uint64_t pixels = width * height;
  if (pixels > kMaxPixels || (floating && pixels * 16ull > kMaxFloatBytes)) {
    *detail = "freeimage: image too large (" + std::to_string(width) + "x" +
              std::to_string(height) + ")";
    return true;
  }
  return false;
}

std::array<uint8_t, 4097> MakeGammaLut() {
  std::array<uint8_t, 4097> lut{};
  for (size_t i = 0; i < lut.size(); ++i) {
    const double linear = static_cast<double>(i) / 4096.0;
    lut[i] = static_cast<uint8_t>(
        std::lround(std::pow(linear, 1.0 / kToneMapGamma) * 255.0));
  }
  return lut;
}

// Fixed Reinhard luminance mapping + sRGB-ish gamma. The parameters are
// constants so the output is reproducible (規劃/22/16 §3.3/坑 1/14).
bool ToneMapToImage(FIBITMAP* rgbf, Image* image) {
  static const std::array<uint8_t, 4097> lut = MakeGammaLut();
  const uint32_t width = FreeImage_GetWidth(rgbf);
  const uint32_t height = FreeImage_GetHeight(rgbf);
  if (width == 0 || height == 0) return false;

  image->info.width = width;
  image->info.height = height;
  image->info.stride = width * 4u;
  image->info.format = PixelFormat::kBgra8;
  image->info.has_alpha = false;
  image->pixels.resize(static_cast<size_t>(image->info.stride) * height);

  const auto& table = lut;
  for (uint32_t y = 0; y < height; ++y) {
    const float* line = reinterpret_cast<const float*>(
        FreeImage_GetScanLine(rgbf, static_cast<int>(height - 1 - y)));
    uint8_t* out = image->pixels.data() + image->info.stride * y;
    for (uint32_t x = 0; x < width; ++x) {
      // FreeImage's RGBF scanline is memory-ordered BGR (same channel
      // constants as BGRA); reading it as RGB swaps red and blue.
      const double blue =
          std::isfinite(line[x * 3 + FI_RGBA_BLUE])
              ? std::max(0.0, static_cast<double>(line[x * 3 + FI_RGBA_BLUE]))
              : 0.0;
      const double green =
          std::isfinite(line[x * 3 + FI_RGBA_GREEN])
              ? std::max(0.0, static_cast<double>(line[x * 3 + FI_RGBA_GREEN]))
              : 0.0;
      const double red =
          std::isfinite(line[x * 3 + FI_RGBA_RED])
              ? std::max(0.0, static_cast<double>(line[x * 3 + FI_RGBA_RED]))
              : 0.0;
      const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
      const double mapped = luminance / (1.0 + luminance);
      const double scale = luminance > 1e-9 ? mapped / luminance : 0.0;
      const double channels[3] = {blue, green, red};
      for (int c = 0; c < 3; ++c) {
        const double linear = std::clamp(channels[c] * scale, 0.0, 1.0);
        const size_t index =
            static_cast<size_t>(linear * 4096.0 + 0.5);
        out[x * 4 + c] = table[std::min<size_t>(index, table.size() - 1)];
      }
      out[x * 4 + 3] = 255;
    }
  }
  return true;
}

// Box-average downscale for tone-mapped HDR/EXR/PFM thumbnails. The raw float
// data is already released at this point, so only the small output is kept.
void Downscale(Image* image, uint32_t max_edge) {
  const uint32_t longest = std::max(image->info.width, image->info.height);
  if (max_edge == 0 || longest <= max_edge || image->info.width == 0 ||
      image->info.height == 0) {
    return;
  }
  const double scale = static_cast<double>(max_edge) / longest;
  const uint32_t target_width = std::max<uint32_t>(
      1, static_cast<uint32_t>(image->info.width * scale + 0.5));
  const uint32_t target_height = std::max<uint32_t>(
      1, static_cast<uint32_t>(image->info.height * scale + 0.5));
  if (target_width == image->info.width && target_height == image->info.height) {
    return;
  }

  std::vector<uint8_t> pixels(static_cast<size_t>(target_width) * target_height * 4);
  const uint32_t source_width = image->info.width;
  const uint32_t source_height = image->info.height;
  for (uint32_t ty = 0; ty < target_height; ++ty) {
    const uint32_t y0 = static_cast<uint32_t>(
        static_cast<uint64_t>(ty) * source_height / target_height);
    uint32_t y1 = static_cast<uint32_t>(
        static_cast<uint64_t>(ty + 1) * source_height / target_height);
    if (y1 <= y0) y1 = y0 + 1;
    for (uint32_t tx = 0; tx < target_width; ++tx) {
      const uint32_t x0 = static_cast<uint32_t>(
          static_cast<uint64_t>(tx) * source_width / target_width);
      uint32_t x1 = static_cast<uint32_t>(
          static_cast<uint64_t>(tx + 1) * source_width / target_width);
      if (x1 <= x0) x1 = x0 + 1;
      uint64_t sums[4]{};
      uint64_t count = 0;
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const uint8_t* row = image->pixels.data() + image->info.stride * sy;
        for (uint32_t sx = x0; sx < x1; ++sx) {
          for (int c = 0; c < 4; ++c) sums[c] += row[sx * 4 + c];
          ++count;
        }
      }
      uint8_t* out = pixels.data() +
                     (static_cast<size_t>(ty) * target_width + tx) * 4;
      for (int c = 0; c < 4; ++c) {
        out[c] = static_cast<uint8_t>(sums[c] / std::max<uint64_t>(count, 1));
      }
    }
  }
  image->info.width = target_width;
  image->info.height = target_height;
  image->info.stride = target_width * 4u;
  image->pixels = std::move(pixels);
}

struct LoadedImage {
  std::unique_ptr<FIBITMAP, decltype(&FreeImage_Unload)> dib{nullptr,
                                                             &FreeImage_Unload};
  FREE_IMAGE_FORMAT fif = FIF_UNKNOWN;
};

bool LoadImage(const std::filesystem::path& path, ImageFormat hint,
               LoadedImage* loaded, Error* error) {
  // FreeImage's EXR plugin can spin on a truncated header; the float formats
  // have cheap, well-defined header probes, so refuse malformed ones before
  // the plugin sees them (規劃/22/16 §3.4).
  if (hint == ImageFormat::kHdr || hint == ImageFormat::kExr ||
      hint == ImageFormat::kPfm) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (!ReadFloatDimensions(path, hint, &width, &height)) {
      *error = Error::Make(kDecodeErrorBase,
                           std::string("FreeImage: invalid ") +
                               FormatNameUtf8(hint) + " header");
      return false;
    }
    std::string detail;
    if (TooLarge(width, height, true, &detail)) {
      *error = Error::Make(kDecodeErrorBase, detail);
      return false;
    }
  }

  const FREE_IMAGE_FORMAT fif = ResolveFif(path, hint);
  if (fif == FIF_UNKNOWN) {
    *error = Error::Make(kDecodeErrorBase, "FreeImage: unknown format");
    return false;
  }
  FIBITMAP* raw = FreeImage_LoadU(fif, path.c_str(), 0);
  if (raw == nullptr) {
    *error = Error::Make(kDecodeErrorBase, "FreeImage: LoadU failed");
    return false;
  }
  loaded->dib.reset(raw);
  loaded->fif = fif;
  return true;
}
#endif

}  // namespace

bool Available() {
#ifdef NV_WITH_FREEIMAGE
  return true;
#else
  return false;
#endif
}

ProbeResult Probe(const std::filesystem::path& path, ImageFormat hint) {
  ProbeResult result;
  result.format = hint;
#ifdef NV_WITH_FREEIMAGE
  EnsureInitialized();

  if (hint == ImageFormat::kHdr || hint == ImageFormat::kExr ||
      hint == ImageFormat::kPfm) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (ReadFloatDimensions(path, hint, &width, &height)) {
      std::string detail;
      if (TooLarge(width, height, true, &detail)) {
        result.error = Error::Make(kDecodeErrorBase, detail);
        return result;
      }
      result.width = width;
      result.height = height;
      result.decoder = "freeimage";
      return result;
    }
    result.error =
        Error::Make(kDecodeErrorBase, std::string("FreeImage: invalid ") +
                                          FormatNameUtf8(hint) + " header");
    return result;
  }

  LoadedImage loaded;
  if (!LoadImage(path, hint, &loaded, &result.error)) return result;
  if (hint == ImageFormat::kUnknown) {
    result.format = FormatFromFif(loaded.fif);
  }

  const uint32_t width = FreeImage_GetWidth(loaded.dib.get());
  const uint32_t height = FreeImage_GetHeight(loaded.dib.get());
  std::string detail;
  if (TooLarge(width, height, IsFloatType(FreeImage_GetImageType(loaded.dib.get())),
               &detail)) {
    result.error = Error::Make(kDecodeErrorBase, detail);
    return result;
  }
  result.width = width;
  result.height = height;
  result.has_alpha = FreeImage_IsTransparent(loaded.dib.get()) != FALSE;
  result.orientation = ReadOrientation(loaded.dib.get());
  if (result.orientation >= 5) std::swap(result.width, result.height);
  result.orientation_applied = result.orientation != 1;
  result.decoder = "freeimage";
#else
  (void)path;
  result.error = Error::Make(kDecodeErrorBase, "FreeImage: not built");
#endif
  return result;
}

DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options, ImageFormat hint) {
  DecodeResult result;
#ifdef NV_WITH_FREEIMAGE
  EnsureInitialized();
  LoadedImage loaded;
  if (!LoadImage(path, hint, &loaded, &result.error)) return result;

  const uint32_t source_width = FreeImage_GetWidth(loaded.dib.get());
  const uint32_t source_height = FreeImage_GetHeight(loaded.dib.get());
  const FREE_IMAGE_TYPE type = FreeImage_GetImageType(loaded.dib.get());
  const bool floating = IsFloatType(type);
  std::string detail;
  if (TooLarge(source_width, source_height, floating, &detail)) {
    result.error = Error::Make(kDecodeErrorBase, detail);
    return result;
  }

  Image image;
  image.info.full_width = source_width;
  image.info.full_height = source_height;
  image.info.orientation = ReadOrientation(loaded.dib.get());
  image.info.has_alpha =
      !floating && FreeImage_IsTransparent(loaded.dib.get()) != FALSE;

  if (floating) {
    FIBITMAP* rgbf = FreeImage_ConvertToRGBF(loaded.dib.get());
    if (rgbf == nullptr) {
      result.error = Error::Make(kDecodeErrorBase,
                                 "FreeImage: float conversion failed");
      return result;
    }
    std::unique_ptr<FIBITMAP, decltype(&FreeImage_Unload)> floats(rgbf,
                                                                  &FreeImage_Unload);
    if (!ToneMapToImage(floats.get(), &image)) {
      result.error = Error::Make(kDecodeErrorBase,
                                 "FreeImage: tone mapping failed");
      return result;
    }
    image.info.source = "freeimage-tonemap";
    char line[160]{};
    std::snprintf(line, sizeof(line),
                  "freeimage: format=%s tonemap=reinhard gamma=%.1f exposure=0",
                  FormatNameUtf8(hint), kToneMapGamma);
    nv::platform::LogInfo(line);

    Downscale(&image, options.max_edge);

    ApplyOrientation(&image, image.info.orientation);
    result.image = std::make_shared<Image>(std::move(image));
    return result;
  }

  FIBITMAP* converted = FreeImage_ConvertTo32Bits(loaded.dib.get());
  if (converted == nullptr) {
    result.error = Error::Make(kDecodeErrorBase,
                               "FreeImage: ConvertTo32Bits failed");
    return result;
  }
  std::unique_ptr<FIBITMAP, decltype(&FreeImage_Unload)> dib(converted,
                                                             &FreeImage_Unload);

  if (options.max_edge > 0) {
    const uint32_t longest = std::max(FreeImage_GetWidth(dib.get()),
                                      FreeImage_GetHeight(dib.get()));
    if (longest > options.max_edge) {
      const double scale = static_cast<double>(options.max_edge) /
                           static_cast<double>(longest);
      const int target_width = std::max<int>(
          1, static_cast<int>(FreeImage_GetWidth(dib.get()) * scale + 0.5));
      const int target_height = std::max<int>(
          1, static_cast<int>(FreeImage_GetHeight(dib.get()) * scale + 0.5));
      FIBITMAP* scaled = FreeImage_Rescale(dib.get(), target_width, target_height,
                                           FILTER_BILINEAR);
      if (scaled != nullptr) {
        dib.reset(scaled);
      }
    }
  }

  const uint32_t width = FreeImage_GetWidth(dib.get());
  const uint32_t height = FreeImage_GetHeight(dib.get());
  if (width == 0 || height == 0) {
    result.error = Error::Make(kDecodeErrorBase, "FreeImage: empty image");
    return result;
  }

  image.info.width = width;
  image.info.height = height;
  image.info.stride = width * 4u;
  image.info.format = PixelFormat::kBgra8;
  image.info.source = "freeimage";
  image.pixels.resize(static_cast<size_t>(image.info.stride) * height);

  for (uint32_t y = 0; y < height; ++y) {
    const BYTE* source = FreeImage_GetScanLine(dib.get(),
                                               static_cast<int>(height - 1 - y));
    std::copy_n(source, image.info.stride,
                image.pixels.data() + image.info.stride * y);
  }

  // FreeImage reports the EXIF tag but never applies it; bake it here so the
  // renderer, thumbnails and CLI all see upright pixels (same as the WIC path).
  ApplyOrientation(&image, image.info.orientation);

  result.image = std::make_shared<Image>(std::move(image));
#else
  (void)path;
  (void)options;
  (void)hint;
  result.error = Error::Make(kDecodeErrorBase, "FreeImage: not built");
#endif
  return result;
}

}  // namespace nv::freeimage
