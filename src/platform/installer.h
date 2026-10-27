#pragma once

#include "platform/registry_scope.h"

#include <filesystem>
#include <string>

namespace nv::platform {

struct InstallLayout {
  std::filesystem::path install_dir;
  std::filesystem::path executable;
  std::filesystem::path thumbnail_dll;
};

struct InstallInfo {
  bool installed = false;
  RegistryScope scope = RegistryScope::kUser;
  std::filesystem::path install_dir;
  std::wstring version;
};

// True when the current process has administrator rights (used to reject
// machine-wide install/uninstall without elevation instead of failing halfway).
bool IsProcessElevated();

// %LOCALAPPDATA%\Programs\ZoreView for per-user installs and
// %ProgramFiles%\ZoreView for machine-wide installs.
InstallLayout DefaultInstallLayout(RegistryScope scope);

// Copies this executable and ZoreViewThumb.dll into `custom_dir` (or the
// default location), registers thumbnails/verbs/OpenWith at `scope`, writes the
// Apps & Features entry and creates the Start menu shortcut.
//
// `associate_default` additionally points the registry default at ZoreView;
// Windows UserChoice defaults are reported through `warning` instead of being
// touched.
bool InstallApplication(const std::filesystem::path& source_executable,
                        RegistryScope scope,
                        const std::filesystem::path& custom_dir,
                        bool create_shortcut, bool associate_default,
                        std::wstring* error, std::wstring* warning = nullptr);

// Removes Shell integration, shortcut, uninstall entry and the installation
// directory. When the running executable lives inside that directory the
// removal is scheduled for after this process exits.
bool UninstallApplication(RegistryScope scope,
                          const std::filesystem::path& install_dir,
                          std::wstring* error);

bool QueryInstallation(RegistryScope scope, InstallInfo* info);

// Per-user Start menu shortcut for kUser; the common (all users) Start menu
// for kMachine.
std::filesystem::path StartMenuShortcutPath(
    RegistryScope scope = RegistryScope::kUser);
bool CreateStartMenuShortcut(const InstallLayout& layout,
                             std::wstring* error,
                             RegistryScope scope = RegistryScope::kUser);
bool RemoveStartMenuShortcut(std::wstring* error,
                             RegistryScope scope = RegistryScope::kUser);

}  // namespace nv::platform
