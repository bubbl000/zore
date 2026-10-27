#pragma once

#include <algorithm>
#include <cmath>

namespace nv::render {

// Zoom limits from 規劃/22/09 §6. The lower bound is *derived* from the fit
// scale, because an 8K image in a small window fits at ~8% and a fixed 10%
// floor would make "fit to window" impossible (坑 3).
inline constexpr float kZoomFloor = 0.10f;
inline constexpr float kZoomCeiling = 32.0f;
// Keyboard zoom step. 22/07 turns this into a setting (1.05/1.10/1.15/1.25);
// the default lives here so the renderer and the settings file cannot drift.
inline constexpr float kDefaultZoomStep = 1.15f;

inline float FitScaleFor(float content_width, float content_height,
                         float image_width, float image_height) {
  if (image_width <= 0.0f || image_height <= 0.0f) return 1.0f;
  const float w = std::max(1.0f, content_width);
  const float h = std::max(1.0f, content_height);
  return std::min(w / image_width, h / image_height);
}

inline float MinZoomFor(float fit_scale) {
  return std::min(kZoomFloor, fit_scale);
}

inline float ClampZoomFor(float zoom, float fit_scale) {
  return std::clamp(zoom, MinZoomFor(fit_scale), kZoomCeiling);
}

// Percent shown in the title/bottom bar. Rounding (not truncation) keeps 100%
// from rendering as 99% after floating-point stepping (坑 4).
inline int ZoomPercent(float zoom) {
  return static_cast<int>(std::lround(zoom * 100.0f));
}

}  // namespace nv::render
