#pragma once

#include <windows.h>

namespace nv::shell {

// {7B2E5C1A-9F3D-4E8A-8C21-5D6F0A1B2C3D}
inline constexpr GUID kThumbnailProviderClsid = {
    0x7b2e5c1a, 0x9f3d, 0x4e8a,
    {0x8c, 0x21, 0x5d, 0x6f, 0x0a, 0x1b, 0x2c, 0x3d}};

inline constexpr wchar_t kThumbnailProviderClsidText[] =
    L"{7B2E5C1A-9F3D-4E8A-8C21-5D6F0A1B2C3D}";

// Shell's thumbnail handler interface id. Every supported extension points at
// our CLSID through this value.
// {E357FCCD-A995-4576-B01F-234630154E96}
inline constexpr wchar_t kThumbnailHandlerShellExGuidText[] =
    L"{E357FCCD-A995-4576-B01F-234630154E96}";

inline constexpr wchar_t kThumbnailProviderName[] =
    L"ZoreView Thumbnail Provider";

// Creates one provider object. Used by the class factory; exposed so the test
// harness can exercise the implementation without the registration path.
HRESULT CreateThumbnailProvider(REFIID riid, void** object);

}  // namespace nv::shell
