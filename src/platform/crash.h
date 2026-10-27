#pragma once

namespace nv::platform {

// Installs a process-wide unhandled-exception filter that writes a minidump
// under %LOCALAPPDATA%\ZoreView\crashes.
void InstallCrashHandler();

}  // namespace nv::platform
