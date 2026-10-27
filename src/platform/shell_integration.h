#pragma once

#include "platform/registry_scope.h"

#include <filesystem>
#include <string>

namespace nv::platform {

// Explorer context-menu verbs for every image document format the application
// can decode. Registered under
// <scope>\Software\Classes\SystemFileAssociations\<ext>\shell\ZoreView.<verb>.
// The verbs are namespaced (ZoreView.*), so uninstall deletes only ours.
bool RegisterShellVerbs(const std::filesystem::path& executable,
                        std::wstring* error,
                        RegistryScope scope = RegistryScope::kUser);
bool UnregisterShellVerbs(std::wstring* error,
                          RegistryScope scope = RegistryScope::kUser);

// True when all three verbs exist for every supported image extension and
// point at `executable`; `extension_count` receives the number checked.
bool AreShellVerbsRegistered(const std::filesystem::path& executable,
                             int* extension_count = nullptr,
                             RegistryScope scope = RegistryScope::kUser);

// Extensions that get context-menu verbs. Icon formats (.ico/.cur) are omitted
// because "set as wallpaper"/"copy image" make little sense for a file icon;
// they still get thumbnails and can be opened with the application.
int ShellVerbExtensionCount();

}  // namespace nv::platform
