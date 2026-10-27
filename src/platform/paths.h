#pragma once

#include <filesystem>

namespace nv::platform {

// Directory that contains the running executable. Empty when Windows does not
// report a path (in that case portable mode is unavailable).
const std::filesystem::path& ExecutableDirectory();

// %LOCALAPPDATA%\ZoreView; falls back to ".\ZoreView" when the variable is
// missing. Always the installed-mode location, regardless of portable mode.
std::filesystem::path AppDataDir();

// Portable mode keeps settings.ini, cache\, logs\, crashes\ and wallpaper\
// next to the executable instead of %LOCALAPPDATA%\ZoreView, so a portable
// package never mixes state with an installed copy.
//
// Detection order:
//   1. An explicit --portable switch (SetPortableMode(true));
//   2. a "ZoreView.portable" marker file next to the executable.
// The marker is checked lazily, so packaging only has to ship the file.
bool IsPortableMode();
void SetPortableMode(bool portable);

// True when `directory` contains the portable marker file.
bool HasPortableMarker(const std::filesystem::path& directory);

// Root for settings.ini, cache\, logs\, crashes\ and wallpaper\:
// the executable directory in portable mode, AppDataDir() otherwise.
std::filesystem::path DataDir();

}  // namespace nv::platform
