#pragma once

// Numeric and textual version constants shared by C++ code
// (src/core/version.h) and the Win32 VERSIONINFO resources
// (src/app/version.rc, src/cli/version.rc, src/shell/version.rc).
//
// Keep NV_VERSION_STRING in MAJOR.MINOR.PATCH form and in sync with the
// numeric fields: Explorer, the installer's DisplayVersion and the release
// scripts all read one of the two representations.
#define NV_VERSION_MAJOR 0
#define NV_VERSION_MINOR 3
#define NV_VERSION_PATCH 0
#define NV_VERSION_BUILD 0
#define NV_VERSION_STRING "0.3.0"

#define NV_VERSION_PRODUCT "ZoreView"
#define NV_VERSION_COMPANY "ZoreView contributors"
#define NV_VERSION_COPYRIGHT "Copyright (c) 2026 ZoreView contributors"
