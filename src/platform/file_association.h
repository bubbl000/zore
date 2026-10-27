#pragma once

#include "platform/registry_scope.h"

#include <filesystem>
#include <string>
#include <vector>

namespace nv::platform {

// ProgID written by RegisterFileAssociations. Every supported image document
// extension gets an OpenWithProgids entry for it, so ZoreView appears in the
// Explorer "Open with" list without stealing the user's current default.
inline constexpr wchar_t kAssociationProgId[] = L"ZoreView.Image";

struct AssociationStatus {
  bool prog_id_registered = false;
  int extension_count = 0;
  int open_with_count = 0;
  int default_count = 0;
};

// Registers the ProgID and an OpenWithProgids entry for every image document
// extension. With `make_default` the extension default values are also pointed
// at the ProgID (previous values are backed up and restored by Unregister).
// Windows "UserChoice" defaults cannot be changed from the registry; those
// extensions are left alone and reported through `warning`.
bool RegisterFileAssociations(const std::filesystem::path& executable,
                              bool make_default, std::wstring* error,
                              std::wstring* warning = nullptr,
                              RegistryScope scope = RegistryScope::kUser);

// Removes the ProgID, the OpenWithProgids entries and restores any default
// values changed by Register(..., make_default=true).
bool UnregisterFileAssociations(
    std::wstring* error,
    RegistryScope scope = RegistryScope::kUser);

bool AreFileAssociationsRegistered(
    const std::filesystem::path& executable,
    AssociationStatus* status = nullptr,
    RegistryScope scope = RegistryScope::kUser);

// Number of extensions covered by the ProgID (icon formats are excluded).
int AssociationExtensionCount();

// Per-extension registration state for the settings page (規劃/22/07 §4.4).
struct AssociationExtensionState {
  std::wstring extension;  // ".jpg"
  bool open_with = false;  // our ProgID is in OpenWithProgids
  bool is_default = false; // the class default points at our ProgID
};

std::vector<AssociationExtensionState> GetAssociationExtensionStates(
    RegistryScope scope = RegistryScope::kUser);

// Registers or removes only the listed extensions' OpenWithProgids entry. The
// ProgID itself is written when enabling; disabling also restores a default
// value that still points at us. Used by the settings page's selection grid.
bool UpdateOpenWithForExtensions(const std::filesystem::path& executable,
                                 const std::vector<std::wstring>& extensions,
                                 bool enable, std::wstring* error,
                                 RegistryScope scope = RegistryScope::kUser);

}  // namespace nv::platform
