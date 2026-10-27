#pragma once

#include <cstdint>

namespace nv::render {

// Interpolation policy for the image layer (規劃/22/18 §3.1).
//
// kFast   LINEAR: interactive pan/zoom and animation playback. Cheap enough to
//         keep a 60 Hz repaint while the picture is moving.
// kGood   HIGH_QUALITY_CUBIC: everything that is standing still (fit, shrink,
//         zoom menu, paused animation). This is what the user actually judges.
// kPixel  NEAREST_NEIGHBOR: pixel_mode magnification at >= 100 %. Pixel-art
//         inspection needs hard blocks; shrinking stays cubic (坑 4).
enum class DrawQuality {
  kFast = 0,
  kGood = 1,
  kPixel = 2,
};

// How long after the last interaction the picture stays in kFast. 150 ms is
// short enough to feel immediate and long enough to skip the final repaint of
// a fast wheel burst (規劃/22/18 §3.1/§3.2).
inline constexpr int kQualityUpgradeMs = 150;

struct DrawQualityInputs {
  // Settings `view.pixel_mode`: nearest-neighbour magnification.
  bool pixel_mode = false;
  // Now < last interaction + kQualityUpgradeMs.
  bool interactive = false;
  // Animation playback (not paused) owns the frame timer.
  bool animating = false;
  // Physical pixels per source pixel (renderer's effective scale).
  float scale = 1.0f;
};

// Pure selection used by the renderer; unit-tested without a device.
inline DrawQuality SelectDrawQuality(const DrawQualityInputs& in) {
  // Pixel mode wins even while panning: at >= 100 % there is nothing to
  // interpolate, so downgrading to linear would only blur the blocks the user
  // asked to see.
  if (in.pixel_mode && in.scale >= 1.0f) return DrawQuality::kPixel;
  if (in.interactive || in.animating) return DrawQuality::kFast;
  return DrawQuality::kGood;
}

// --- Display proxy (規劃/22/18 §3.3) ---------------------------------------
//
// One 1/4-size GPU copy of a very large frame. The plan forbids 1/2 (33 MB for
// 8K): the 8K consecutive-navigation peak is already ~460 MB of a 500 MB
// budget. A 1/4 proxy is ~8 MB, and it is only ever a second copy of the
// *scaled* content, never of the full frame.
inline constexpr uint32_t kProxyFactor = 4;

inline constexpr uint32_t ProxyEdge(uint32_t source) {
  return source / kProxyFactor + (source % kProxyFactor != 0 ? 1u : 0u);
}

struct ProxyInputs {
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  uint32_t content_width = 0;
  uint32_t content_height = 0;
  // Physical pixels per source pixel.
  float scale = 1.0f;
  bool pixel_mode = false;
  // Global switch (--proxy=off / a future setting).
  bool enabled = true;
  // The bitmap on screen is a display tier, not the file's full frame
  // (規劃/25 §7.7). A tier is already at most ~1280 px and is drawn at or near
  // 1:1, so a second 1/4 copy would be a copy of an upscaled copy: it costs
  // VRAM and makes the interactive frames blurrier than the tier itself.
  bool source_is_tier = false;
};

// Only worth a proxy when the image is at least 4x the content area (i.e. it
// is drawn at < 50 %) and pixel mode is off. The proxy keeps the same dest
// rectangle as the source, so switching between them cannot jump (坑 6).
inline bool ShouldUseProxy(const ProxyInputs& in) {
  if (!in.enabled || in.pixel_mode) return false;
  // 規劃/25 §7.7: never bake a proxy from a display tier.
  if (in.source_is_tier) return false;
  if (in.scale <= 0.0f || in.scale >= 0.5f) return false;
  if (in.image_width == 0 || in.image_height == 0) return false;
  if (in.content_width == 0 || in.content_height == 0) return false;
  const uint64_t image_pixels = static_cast<uint64_t>(in.image_width) *
                                static_cast<uint64_t>(in.image_height);
  const uint64_t content_pixels = static_cast<uint64_t>(in.content_width) *
                                  static_cast<uint64_t>(in.content_height);
  return image_pixels > 4ull * content_pixels;
}

inline const char* DrawQualityName(DrawQuality quality) {
  switch (quality) {
    case DrawQuality::kFast:
      return "fast";
    case DrawQuality::kGood:
      return "good";
    case DrawQuality::kPixel:
      return "pixel";
  }
  return "fast";
}

// -1 = automatic (the selection above). Any other value forces the quality,
// used by --force-quality for deterministic screenshots and A/B comparison.
inline constexpr int kQualityAuto = -1;

inline int DrawQualityValue(DrawQuality quality) {
  return static_cast<int>(quality);
}

inline DrawQuality DrawQualityFromValue(int value) {
  if (value == DrawQualityValue(DrawQuality::kGood)) return DrawQuality::kGood;
  if (value == DrawQualityValue(DrawQuality::kPixel)) {
    return DrawQuality::kPixel;
  }
  return DrawQuality::kFast;
}

}  // namespace nv::render
