#pragma once

#include "core/image.h"

#include <windows.h>

#include <string>

namespace nv::platform {

bool CopyImageToClipboard(HWND owner, const Image& image, std::wstring* error);
bool CopyTextToClipboard(HWND owner, const std::wstring& text,
                         std::wstring* error);

}  // namespace nv::platform
