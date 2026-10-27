#include "platform/window_capture.h"

#include "platform/screenshot.h"

#include <windows.h>

namespace nv::platform {

bool CaptureClientToPng(HWND hwnd, const std::filesystem::path& path) {
  if (hwnd == nullptr) return false;
  RECT client{};
  GetClientRect(hwnd, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  if (width <= 0 || height <= 0) return false;

  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap == nullptr || bits == nullptr) {
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return false;
  }
  HGDIOBJ old = SelectObject(dc, bitmap);
  const BOOL printed = PrintWindow(hwnd, dc, PW_CLIENTONLY);
  SelectObject(dc, old);
  bool ok = false;
  if (printed) {
    ok = SavePng(path, static_cast<const uint8_t*>(bits),
                 static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                 static_cast<uint32_t>(width) * 4u);
  }
  DeleteObject(bitmap);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return ok;
}

}  // namespace nv::platform
