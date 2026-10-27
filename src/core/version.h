#pragma once

#include "version_numbers.h"

namespace nv {

inline constexpr wchar_t kAppName[] = L"Zore";
// Derived from the shared macro so the VERSIONINFO resource and the strings
// shown by the UI/CLI cannot drift apart.
inline constexpr wchar_t kVersion[] = L"" NV_VERSION_STRING;

}  // namespace nv
