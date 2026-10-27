#pragma once

#include <windows.h>
#include <d2d1.h>

namespace nv::render {

// COLORREF is 0x00BBGGRR; D2D1_COLOR_F is straight RGBA in 0..1.
inline D2D1_COLOR_F ColorFromColorRef(COLORREF color) {
  return D2D1::ColorF(static_cast<float>(GetRValue(color)) / 255.0f,
                      static_cast<float>(GetGValue(color)) / 255.0f,
                      static_cast<float>(GetBValue(color)) / 255.0f, 1.0f);
}

inline COLORREF ColorRefFromFloat(float red, float green, float blue) {
  const auto channel = [](float value) -> BYTE {
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 255;
    return static_cast<BYTE>(value * 255.0f + 0.5f);
  };
  return RGB(channel(red), channel(green), channel(blue));
}

}  // namespace nv::render
