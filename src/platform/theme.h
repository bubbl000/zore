#pragma once

#include <windows.h>

#include <cstdint>

namespace nv::platform {

// Windows high-contrast mode. When it is on, the renderer follows the system
// window/selection colors instead of the dark/light theme tokens, and the
// transparency checkerboard is suppressed.
bool IsHighContrastEnabled();

// Test hook: -1 = follow the system, 0 = force off, 1 = force on.
void SetForcedHighContrast(int value);
int ForcedHighContrast();

// Converts a GetSysColor() index to a D2D-compatible 0x00BBGGRR value.
uint32_t SystemColorRef(int system_color_index);

}  // namespace nv::platform
