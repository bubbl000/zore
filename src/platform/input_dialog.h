#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace nv::platform {

// Small modal single-line text prompt. Returns nullopt when cancelled.
// Enter confirms, Escape cancels; the owner window is disabled while open.
std::optional<std::wstring> PromptForText(HWND owner, const std::wstring& title,
                                          const std::wstring& label,
                                          const std::wstring& initial);

}  // namespace nv::platform
