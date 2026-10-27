#pragma once

#include <windows.h>

// Exports of ZoreViewThumb.dll. Kept in one place so the installer, the
// registration helper and the tests agree on the signatures.
#ifdef ZOREVIEW_THUMBSHELL_EXPORTS
#define NV_THUMB_API
#else
#define NV_THUMB_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Per-user (HKCU) registration of the thumbnail handler for every supported
// extension. The module path is taken from this DLL, so callers never pass a
// path that could get stale.
NV_THUMB_API HRESULT WINAPI ZoreViewThumbnailRegister(void);
NV_THUMB_API HRESULT WINAPI ZoreViewThumbnailUnregister(void);

// True when the CLSID points at this DLL and all registered extensions use it.
NV_THUMB_API BOOL WINAPI ZoreViewThumbnailIsRegistered(void);

// Copies the DLL path (may be called by the installer to show what is
// registered). Returns the required character count including the terminator,
// or 0 on failure.
NV_THUMB_API DWORD WINAPI ZoreViewThumbnailGetPath(LPWSTR buffer, DWORD capacity);

#ifdef __cplusplus
}
#endif
