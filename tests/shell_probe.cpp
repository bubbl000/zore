// M3-1: asks the Shell for one thumbnail without linking ZoreViewThumb.dll.
// This is the closest automated equivalent to Explorer: the handler has to be
// discovered from the registry and loaded on demand. Used by
// tests/run_shell_thumb_check.ps1 after a fresh registration.

#include <windows.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::wprintf(L"usage: zoreview_shellprobe <file> [size]\n");
    return 2;
  }
  int size = 256;
  if (argc >= 3) size = _wtoi(argv[2]);
  if (size <= 0) size = 256;

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    std::wprintf(L"CoInitializeEx failed 0x%08lX\n",
                 static_cast<unsigned long>(com));
    return 2;
  }

  ComPtr<IShellItemImageFactory> factory;
  HRESULT hr = SHCreateItemFromParsingName(
      argv[1], nullptr, IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr)) {
    std::wprintf(L"SHCreateItemFromParsingName failed 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    CoUninitialize();
    return 1;
  }

  HBITMAP bitmap = nullptr;
  const SIZE requested{size, size};
  hr = factory->GetImage(requested, SIIGBF_THUMBNAILONLY, &bitmap);
  if (FAILED(hr) || bitmap == nullptr) {
    std::wprintf(L"GetImage failed 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    CoUninitialize();
    return 1;
  }

  DIBSECTION section{};
  const int got = GetObjectW(bitmap, static_cast<int>(sizeof(section)),
                             &section);
  if (got != static_cast<int>(sizeof(section)) ||
      section.dsBm.bmBits == nullptr) {
    std::wprintf(L"GetImage returned a non-DIB bitmap\n");
    DeleteObject(bitmap);
    CoUninitialize();
    return 1;
  }

  std::wprintf(L"shell thumbnail %dx%d\n", section.dsBm.bmWidth,
               std::abs(section.dsBm.bmHeight));
  DeleteObject(bitmap);
  CoUninitialize();
  return 0;
}
