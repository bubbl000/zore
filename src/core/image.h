#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nv {

enum class PixelFormat {
  kUnknown = 0,
  kBgra8 = 1,
  kRgba16 = 2,
  kRgba32f = 3,
};

struct ImageInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  PixelFormat format = PixelFormat::kUnknown;
  bool has_alpha = false;
  // EXIF orientation tag (1..8). width/height/full_width/full_height always
  // describe the upright image; orientation only records the source tag.
  uint16_t orientation = 1;
  bool orientation_applied = false;  // Pixels were rotated/flipped to upright.
  bool icc_applied = false;          // Pixels were converted to sRGB.
  uint32_t frame_count = 1;  // GIF/APNG frames; TIFF reports its page count.
  bool animated = false;     // Multi-frame source with a playable sequence.
  // The decoder stopped early at the 512-frame / 128 MB limit. The pill shows
  // "Truncated" instead of pretending the animation is complete (22/12 §3.4).
  bool frames_truncated = false;
  uint32_t full_width = 0;   // Real file dimensions when this is a scaled preview.
  uint32_t full_height = 0;
  double dpi_x = 96.0;
  double dpi_y = 96.0;
  std::vector<uint8_t> icc_profile;  // Embedded profile, if any (diagnostics).
  std::string source;  // "wic", "freeimage", ...
};

struct ImageFrame {
  std::vector<uint8_t> pixels;
  uint32_t delay_ms = 100;
};

struct Image {
  ImageInfo info;
  std::vector<uint8_t> pixels;
  // Animated GIF/APNG only. `pixels` is frame 0; `frames` is non-empty only
  // when animation playback is available (thumbnails/prefetch keep it empty).
  std::vector<ImageFrame> frames;
  uint32_t loop_count = 0;  // 0 = loop forever.

  bool Empty() const {
    return pixels.empty() || info.width == 0 || info.height == 0 ||
           info.format == PixelFormat::kUnknown;
  }

  uint64_t ByteSize() const {
    uint64_t total = pixels.size();
    for (const ImageFrame& frame : frames) total += frame.pixels.size();
    return total;
  }
};

}  // namespace nv
