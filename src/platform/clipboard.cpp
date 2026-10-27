#include "platform/clipboard.h"

#include "platform/i18n.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace nv::platform {
namespace {

void SetError(std::wstring* error, const wchar_t* step, DWORD code) {
  if (error == nullptr) return;
  wchar_t buffer[256]{};
  swprintf_s(buffer, L"%s failed (error %lu)", step,
             static_cast<unsigned long>(code));
  *error = buffer;
}

bool OpenClipboardRetry(HWND owner, std::wstring* error) {
  for (int attempt = 0; attempt < 6; ++attempt) {
    if (OpenClipboard(owner)) return true;
    Sleep(20);
  }
  SetError(error, L"Open clipboard", GetLastError());
  return false;
}

}  // namespace

bool CopyImageToClipboard(HWND owner, const Image& image, std::wstring* error) {
  if (error != nullptr) error->clear();
  if (image.Empty() || image.info.format != PixelFormat::kBgra8) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorNotBgra8);
    }
    return false;
  }
  const uint32_t width = image.info.width;
  const uint32_t height = image.info.height;
  const uint32_t source_stride = image.info.stride;
  if (source_stride < width * 4u) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorInvalidStride);
    }
    return false;
  }

  const SIZE_T pixel_bytes = static_cast<SIZE_T>(width) * height * 4u;
  if (pixel_bytes == 0 || pixel_bytes > 0x7FFFFFFFull) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorImageTooLarge);
    }
    return false;
  }
  const SIZE_T total = sizeof(BITMAPINFOHEADER) + pixel_bytes;
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, total);
  if (memory == nullptr) {
    SetError(error, L"Allocate clipboard memory", GetLastError());
    return false;
  }

  auto* base = static_cast<uint8_t*>(GlobalLock(memory));
  if (base == nullptr) {
    GlobalFree(memory);
    SetError(error, L"Lock clipboard memory", GetLastError());
    return false;
  }

  auto* header = reinterpret_cast<BITMAPINFOHEADER*>(base);
  *header = {};
  header->biSize = sizeof(BITMAPINFOHEADER);
  header->biWidth = static_cast<LONG>(width);
  header->biHeight = static_cast<LONG>(height);
  header->biPlanes = 1;
  header->biBitCount = 32;
  header->biCompression = BI_RGB;
  header->biSizeImage = static_cast<DWORD>(pixel_bytes);

  // CF_DIB rows are bottom-up; the decoder gives top-down rows.
  uint8_t* destination = base + sizeof(BITMAPINFOHEADER);
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * row_bytes,
                image.pixels.data() +
                    static_cast<size_t>(height - 1 - y) * source_stride,
                row_bytes);
  }
  GlobalUnlock(memory);

  if (!OpenClipboardRetry(owner, error)) {
    GlobalFree(memory);
    return false;
  }
  if (!EmptyClipboard()) {
    SetError(error, L"Clear clipboard", GetLastError());
    CloseClipboard();
    GlobalFree(memory);
    return false;
  }
  if (!SetClipboardData(CF_DIB, memory)) {
    SetError(error, L"Write clipboard data", GetLastError());
    CloseClipboard();
    GlobalFree(memory);
    return false;
  }
  // Ownership of `memory` transferred to the clipboard.
  CloseClipboard();
  return true;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text,
                         std::wstring* error) {
  if (error != nullptr) error->clear();
  const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    SetError(error, L"Allocate clipboard memory", GetLastError());
    return false;
  }
  auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
  if (destination == nullptr) {
    GlobalFree(memory);
    SetError(error, L"Lock clipboard memory", GetLastError());
    return false;
  }
  std::memcpy(destination, text.c_str(), bytes);
  GlobalUnlock(memory);

  if (!OpenClipboardRetry(owner, error)) {
    GlobalFree(memory);
    return false;
  }
  if (!EmptyClipboard()) {
    SetError(error, L"Clear clipboard", GetLastError());
    CloseClipboard();
    GlobalFree(memory);
    return false;
  }
  if (!SetClipboardData(CF_UNICODETEXT, memory)) {
    SetError(error, L"Write clipboard data", GetLastError());
    CloseClipboard();
    GlobalFree(memory);
    return false;
  }
  CloseClipboard();
  return true;
}

}  // namespace nv::platform
