#include "platform/dpi.h"

#include <cmath>

namespace nv::platform {
namespace {

float g_forced_scale = 0.0f;

}  // namespace

float DpiScaleForWindow(HWND hwnd) {
  if (g_forced_scale > 0.0f) return g_forced_scale;
  if (hwnd != nullptr) {
    const UINT dpi = GetDpiForWindow(hwnd);
    if (dpi != 0) return static_cast<float>(dpi) / 96.0f;
  }
  return 1.0f;
}

int DpiPixels(float dip, float scale) {
  if (scale <= 0.0f) scale = 1.0f;
  const int pixels = static_cast<int>(std::lround(dip * scale));
  return pixels > 0 ? pixels : 1;
}

void SetForcedDpiScale(float scale) {
  g_forced_scale = scale > 0.0f ? scale : 0.0f;
}

float ForcedDpiScale() { return g_forced_scale; }

}  // namespace nv::platform
