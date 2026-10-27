#pragma once

#include "platform/registry_scope.h"

#include <windows.h>

#include <string>
#include <vector>

namespace nv::shell {

// Extensions registered under <scope>\Software\Classes\<ext>\ShellEx. Only
// formats the application can decode are listed; unregistered formats keep
// their previous handler.
const std::vector<std::wstring>& ThumbnailExtensions();

// Writes the CLSID, InprocServer32 and the ShellEx entries. Existing handlers
// at the same scope are backed up so Unregister can restore them.
HRESULT RegisterThumbnailProvider(
    const std::wstring& module_path,
    nv::platform::RegistryScope scope = nv::platform::RegistryScope::kUser);

// Removes every value written by Register and restores the backed-up handlers.
HRESULT UnregisterThumbnailProvider(
    nv::platform::RegistryScope scope = nv::platform::RegistryScope::kUser);

// True when the CLSID path matches module_path and every supported extension
// points at our CLSID. Per-user registration also requires the
// DisableProcessIsolation flag; machine-wide registration must not have it.
bool IsThumbnailProviderRegistered(
    const std::wstring& module_path,
    nv::platform::RegistryScope scope = nv::platform::RegistryScope::kUser);

}  // namespace nv::shell
