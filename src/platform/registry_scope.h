#pragma once

#include <windows.h>

namespace nv::platform {

// Where a Shell integration surface is registered.
//
// kUser    -> HKEY_CURRENT_USER\Software\Classes (no elevation, no lock-in)
// kMachine -> HKEY_LOCAL_MACHINE\Software\Classes (requires elevation)
//
// The per-user and per-machine layouts are otherwise identical. Thumbnail
// handlers registered per-user additionally need DisableProcessIsolation when
// running in Explorer on Windows 11 (see platform/shell/thumbnail_registry.cpp).
enum class RegistryScope {
  kUser,
  kMachine,
};

inline HKEY RootKey(RegistryScope scope) {
  return scope == RegistryScope::kMachine ? HKEY_LOCAL_MACHINE
                                          : HKEY_CURRENT_USER;
}

inline const wchar_t* ScopeName(RegistryScope scope) {
  return scope == RegistryScope::kMachine ? L"machine" : L"user";
}

}  // namespace nv::platform
