#pragma once

#include <windows.h>

#include <filesystem>

namespace nv::platform {

// Captures one window's client area into a PNG with PrintWindow(PW_CLIENTONLY)
// (規劃/26 §2.1). Shared by the settings and info windows so the --*-screenshot
// test hooks of both use the same code path.
//
// PrintWindow asks the window to paint itself, so the result contains what the
// window draws (D2D included) instead of whatever happens to be on the desktop.
// Returns false when the window has no client area, the DIB cannot be created or
// the window refuses to print.
bool CaptureClientToPng(HWND hwnd, const std::filesystem::path& path);

}  // namespace nv::platform
