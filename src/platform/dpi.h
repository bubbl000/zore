#pragma once

#include <windows.h>

namespace nv::platform {

// Effective UI scale for a window: 1.0 at 96 DPI, 1.25 at 125%, ...
// Falls back to the system DPI when the window is not valid yet or the process
// is DPI-unaware.
float DpiScaleForWindow(HWND hwnd);

// Converts a DIP (logical pixel at 96 DPI) to physical pixels.
int DpiPixels(float dip, float scale);

// Test hook (--force-dpi-scale): overrides DpiScaleForWindow until reset with 0.
// This is how the automated checks exercise 125/150/200% on one desktop.
void SetForcedDpiScale(float scale);
float ForcedDpiScale();

}  // namespace nv::platform
