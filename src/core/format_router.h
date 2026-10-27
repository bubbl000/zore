#pragma once

#include "core/error.h"
#include "core/image.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace nv {

enum class ImageFormat {
  kUnknown = 0,
  kJpeg,
  kPng,
  kApng,  // PNG with acTL before IDAT (animated; frames are composited).
  kBmp,
  kGif,
  kWebp,
  kTiff,
  kIco,
  kPsd,   // PSD/PSB merged image.
  kHeif,  // HEIC/HEIF (ISO BMFF, decoded through the system codec).
  kAvif,  // AVIF (ISO BMFF, decoded through the system codec).
  kSvg,   // SVG (rasterized with Direct2D).
  kTga,   // Targa (FreeImage).
  kPcx,   // PCX (FreeImage).
  kPnm,   // PBM/PGM/PPM (FreeImage).
  kXbm,   // X BitMap (FreeImage).
  kXpm,   // X PixMap (FreeImage).
  kWbmp,  // Wireless Bitmap (FreeImage; extension only, no magic).
  kHdr,   // Radiance HDR (FreeImage + fixed tone mapping).
  kExr,   // OpenEXR (FreeImage + fixed tone mapping).
  kPfm,   // Portable Float Map (FreeImage + fixed tone mapping).
  kJp2,   // JPEG 2000 (WIC first, FreeImage fallback).
  kJxr,   // JPEG XR (WIC first, FreeImage fallback; .jxr/.wdp).
  kMng,   // MNG/JNG (FreeImage; first frame only).
  kRaw,   // detected, but decoding is M2.
};

const wchar_t* FormatName(ImageFormat format);
const char* FormatNameUtf8(ImageFormat format);

struct ProbeResult {
  ImageFormat format = ImageFormat::kUnknown;
  uint32_t width = 0;
  uint32_t height = 0;
  bool has_alpha = false;
  uint16_t orientation = 1;         // EXIF tag; width/height are upright.
  bool orientation_applied = false;  // True when display applies the rotation.
  bool has_icc = false;
  bool icc_is_srgb = false;
  uint32_t frame_count = 1;  // GIF/APNG frames; TIFF reports its page count.
  bool animated = false;
  std::string decoder;  // "wic", "freeimage", ...
  Error error;

  bool ok() const { return error.ok() && width > 0 && height > 0; }
};

struct DecodeOptions {
  uint32_t max_edge = 0;  // 0 = original size.
  bool animation = true;  // GIF/APNG: decode all frames when true, no scaling.
};

struct DecodeResult {
  std::shared_ptr<Image> image;
  Error error;

  bool ok() const { return error.ok() && image && !image->Empty(); }
};

class FormatRouter {
 public:
  FormatRouter() = delete;

  // Detect by magic first, extension as fallback.
  static ImageFormat Detect(const std::filesystem::path& path);

  // Cheap metadata probe; may decode only headers.
  static ProbeResult Probe(const std::filesystem::path& path);

  // Same probe with the format the caller already resolved (規劃/25 §7.13-3).
  // One open computes "which decoder" and the UI needs that answer for the
  // animation/tier/preview decisions anyway, so re-running Detect() inside the
  // probe would read the same header a second time on every open. `format`
  // must be the result of Detect() for this path; kUnknown means "detect it
  // yourself" and behaves exactly like the overload above.
  static ProbeResult Probe(const std::filesystem::path& path,
                           ImageFormat known_format);

  // Decode to BGRA8; orientation is reported in ImageInfo, not baked yet.
  static DecodeResult Decode(const std::filesystem::path& path,
                             const DecodeOptions& options = {});

  static bool IsSupportedExtension(const std::wstring& extension_lower);

  // The format an extension (lowercase, leading dot) routes to, or kUnknown.
  // The empty-state format summary groups SupportedExtensions() through this,
  // so the advertised families can never drift from the actual routing table
  // (規劃/22/03 §3.3 rule 1).
  static ImageFormat FormatForExtension(const std::wstring& extension_lower);
};

}  // namespace nv
