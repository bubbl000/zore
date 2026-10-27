// M3-1: exercises ZoreViewThumb.dll directly (COM) and through the Shell
// thumbnail cache (IShellItemImageFactory), so registration and Explorer
// integration are both verified without opening a window.
//
// Run from the new/ directory: build\Release\zoreview_shelltests.exe
// The test registers the DLL under HKCU, runs the checks and unregisters again
// (unless --keep-registered is passed).

#include "core/format_router.h"
#include "core/image.h"
#include "shell/thumbnail_api.h"
#include "shell/thumbnail_provider.h"
#include "shell/thumbnail_registry.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;
int g_checks = 0;

void Fail(const std::wstring& message) {
  std::wcout << L"FAIL: " << message << L"\n";
  ++g_failures;
}

void Note(const std::wstring& message) {
  std::wcout << L"     " << message << L"\n";
}

std::wstring HResultText(HRESULT hr) {
  wchar_t buffer[32]{};
  std::swprintf(buffer, std::size(buffer), L"0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
  std::filesystem::path path(left);
  path /= right;
  return path.wstring();
}

struct BitmapStats {
  int width = 0;
  int height = 0;
  bool has_transparent_pixels = false;
  bool has_visible_pixels = false;
};

bool InspectBitmap(HBITMAP bitmap, BitmapStats* stats) {
  if (bitmap == nullptr) return false;
  DIBSECTION section{};
  if (GetObjectW(bitmap, static_cast<int>(sizeof(section)), &section) !=
      static_cast<int>(sizeof(section))) {
    return false;
  }
  if (section.dsBm.bmBits == nullptr || section.dsBm.bmBitsPixel != 32) {
    return false;
  }
  stats->width = section.dsBm.bmWidth;
  stats->height = std::abs(section.dsBm.bmHeight);
  if (stats->width <= 0 || stats->height <= 0) return false;

  const auto* pixels = static_cast<const uint8_t*>(section.dsBm.bmBits);
  const size_t count = static_cast<size_t>(stats->width) * stats->height;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* pixel = pixels + i * 4;
    if (pixel[3] < 250) stats->has_transparent_pixels = true;
    if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
      stats->has_visible_pixels = true;
    }
  }
  return true;
}

HRESULT CreateProvider(ComPtr<IThumbnailProvider>* provider) {
  return CoCreateInstance(nv::shell::kThumbnailProviderClsid, nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(provider->GetAddressOf()));
}

// Calls the provider the way Shell does: IInitializeWithFile for most paths,
// IInitializeWithItem when requested.
HRESULT ThumbnailDirect(const std::wstring& path, UINT cx, bool use_item,
                        HBITMAP* bitmap, WTS_ALPHATYPE* alpha_type,
                        double* elapsed_ms) {
  *bitmap = nullptr;
  *alpha_type = WTSAT_UNKNOWN;

  ComPtr<IThumbnailProvider> provider;
  HRESULT hr = CreateProvider(&provider);
  if (FAILED(hr)) return hr;

  std::error_code ec;
  const std::wstring absolute =
      std::filesystem::absolute(path, ec).wstring();

  if (use_item) {
    ComPtr<IShellItem> item;
    hr = SHCreateItemFromParsingName(absolute.c_str(), nullptr,
                                     IID_PPV_ARGS(item.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ComPtr<IInitializeWithItem> initialize;
    hr = provider.As(&initialize);
    if (FAILED(hr)) return hr;
    hr = initialize->Initialize(item.Get(), STGM_READ);
  } else {
    ComPtr<IInitializeWithFile> initialize;
    hr = provider.As(&initialize);
    if (FAILED(hr)) return hr;
    hr = initialize->Initialize(absolute.c_str(), STGM_READ);
  }
  if (FAILED(hr)) return hr;

  const auto start = std::chrono::steady_clock::now();
  hr = provider->GetThumbnail(cx, bitmap, alpha_type);
  if (elapsed_ms != nullptr) {
    const auto end = std::chrono::steady_clock::now();
    *elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
  }
  return hr;
}

HRESULT ThumbnailViaShell(const std::wstring& path, int cx, int cy,
                          HBITMAP* bitmap) {
  *bitmap = nullptr;
  std::error_code ec;
  const std::wstring absolute =
      std::filesystem::absolute(path, ec).wstring();
  ComPtr<IShellItemImageFactory> factory;
  HRESULT hr = SHCreateItemFromParsingName(
      absolute.c_str(), nullptr, IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr)) return hr;
  const SIZE size{cx, cy};
  return factory->GetImage(size, SIIGBF_THUMBNAILONLY, bitmap);
}

std::wstring CreateWorkDirectory() {
  wchar_t temp[MAX_PATH]{};
  if (GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) == 0) {
    return std::wstring();
  }
  wchar_t suffix[64]{};
  std::swprintf(suffix, std::size(suffix), L"nv-shell-thumb-%lu-%llu",
                GetCurrentProcessId(),
                static_cast<unsigned long long>(GetTickCount64()));
  const std::wstring directory = JoinPath(temp, suffix);
  if (!CreateDirectoryW(directory.c_str(), nullptr)) return std::wstring();
  return directory;
}

bool CopySample(const std::wstring& source, const std::wstring& destination) {
  return CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE;
}

struct ExpectedSize {
  int width = 0;
  int height = 0;
};

ExpectedSize ExpectedThumbnailSize(const nv::ProbeResult& probe, UINT cx) {
  ExpectedSize expected;
  if (probe.width == 0 || probe.height == 0) return expected;
  const uint32_t width = std::min<uint32_t>(cx, probe.width);
  expected.width = static_cast<int>(width);
  expected.height = static_cast<int>(
      std::max<uint64_t>(1, static_cast<uint64_t>(probe.height) * width /
                                probe.width));
  return expected;
}

void CheckThumbnail(const std::wstring& label, const std::wstring& path,
                    UINT cx, bool require_success) {
  ++g_checks;
  const int failures_before = g_failures;
  const nv::ProbeResult probe = nv::FormatRouter::Probe(path);
  if (!probe.ok()) {
    Fail(label + L": probe failed (" +
         std::wstring(probe.error.message.begin(), probe.error.message.end()) +
         L")");
    return;
  }

  HBITMAP bitmap = nullptr;
  WTS_ALPHATYPE alpha_type = WTSAT_UNKNOWN;
  double elapsed_ms = 0.0;
  const HRESULT hr =
      ThumbnailDirect(path, cx, false, &bitmap, &alpha_type, &elapsed_ms);
  if (FAILED(hr)) {
    if (require_success) {
      Fail(label + L": GetThumbnail failed " + HResultText(hr));
    } else {
      Note(label + L": GetThumbnail failed " + HResultText(hr) + L" (allowed)");
    }
    return;
  }
  if (bitmap == nullptr) {
    Fail(label + L": GetThumbnail returned a null bitmap");
    return;
  }

  BitmapStats stats;
  const bool inspected = InspectBitmap(bitmap, &stats);
  if (!inspected) {
    Fail(label + L": bitmap is not a 32bpp DIB section");
  } else {
    const ExpectedSize expected = ExpectedThumbnailSize(probe, cx);
    if (expected.width == 0) {
      Fail(label + L": probe has no usable size");
    } else if (std::abs(stats.width - expected.width) > 1 ||
               std::abs(stats.height - expected.height) > 1) {
      Fail(label + L": size " + std::to_wstring(stats.width) + L"x" +
           std::to_wstring(stats.height) + L" expected " +
           std::to_wstring(expected.width) + L"x" +
           std::to_wstring(expected.height));
    }
    if (!stats.has_visible_pixels) {
      Fail(label + L": thumbnail is completely black/transparent");
    }
    const bool expect_alpha =
        probe.has_alpha || probe.format == nv::ImageFormat::kSvg;
    const bool got_alpha = alpha_type == WTSAT_ARGB;
    if (expect_alpha != got_alpha) {
      Fail(label + L": alpha type " +
           std::to_wstring(static_cast<int>(alpha_type)) + L" expected " +
           (expect_alpha ? L"ARGB" : L"RGB"));
    }
    if (expect_alpha != stats.has_transparent_pixels && expect_alpha) {
      Note(label + L": ARGB image has no pixel below alpha 250");
    }
    std::wcout << (g_failures == failures_before ? L"OK   " : L"FAIL ")
               << label << L": " << stats.width << L"x" << stats.height
               << L" alpha=" << (alpha_type == WTSAT_ARGB ? L"argb" : L"rgb")
               << L" " << static_cast<int>(elapsed_ms) << L" ms\n";
  }
  DeleteObject(bitmap);
}

void CheckCorrupt(const std::wstring& path) {
  ++g_checks;
  HBITMAP bitmap = nullptr;
  WTS_ALPHATYPE alpha_type = WTSAT_UNKNOWN;
  const HRESULT hr =
      ThumbnailDirect(path, 256, false, &bitmap, &alpha_type, nullptr);
  if (SUCCEEDED(hr) || bitmap != nullptr) {
    Fail(L"corrupt sample returned a thumbnail (hr=" + HResultText(hr) + L")");
    if (bitmap != nullptr) DeleteObject(bitmap);
    return;
  }
  std::wcout << L"OK   corrupt sample rejected: " << HResultText(hr) << L"\n";
}

void CheckShell(const std::wstring& label, const std::wstring& path) {
  ++g_checks;
  HBITMAP bitmap = nullptr;
  const HRESULT hr = ThumbnailViaShell(path, 256, 256, &bitmap);
  if (FAILED(hr) || bitmap == nullptr) {
    Fail(L"Shell " + label + L": GetImage failed " + HResultText(hr));
    return;
  }
  BitmapStats stats;
  if (!InspectBitmap(bitmap, &stats)) {
    Fail(L"Shell " + label + L": GetImage returned a non-DIB bitmap");
  } else if (!stats.has_visible_pixels) {
    Fail(L"Shell " + label + L": thumbnail is blank");
  } else {
    std::wcout << L"OK   Shell " << label << L": " << stats.width << L"x"
               << stats.height << L"\n";
  }
  DeleteObject(bitmap);
}

// The merged HKCR view is refreshed asynchronously after a HKCU write. Shell
// reads that view, so the integration checks wait until it points at us; a real
// installer registers once and Explorer picks it up later.
bool MergedHandlerIsOurs(const wchar_t* extension) {
  const std::wstring subkey = std::wstring(extension) + L"\\ShellEx\\" +
                              nv::shell::kThumbnailHandlerShellExGuidText;
  wchar_t buffer[64]{};
  DWORD bytes = static_cast<DWORD>(sizeof(buffer));
  if (RegGetValueW(HKEY_CLASSES_ROOT, subkey.c_str(), nullptr, RRF_RT_REG_SZ,
                   nullptr, buffer, &bytes) != ERROR_SUCCESS) {
    return false;
  }
  return _wcsicmp(buffer, nv::shell::kThumbnailProviderClsidText) == 0;
}

bool WaitForShellRegistration(DWORD timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (MergedHandlerIsOurs(L".psd") && MergedHandlerIsOurs(L".svg") &&
        MergedHandlerIsOurs(L".tga")) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    Sleep(50);
  }
}

void CheckShellSizes(const std::wstring& label, const std::wstring& path) {
  // Explorer asks for these four thumbnail cache sizes (see "Thumbnail Cache
  // and Sizing" in the Win32 docs).
  const int sizes[] = {32, 96, 256, 1024};
  for (int size : sizes) {
    ++g_checks;
    HBITMAP bitmap = nullptr;
    const HRESULT hr = ThumbnailViaShell(path, size, size, &bitmap);
    BitmapStats stats;
    if (FAILED(hr) || !InspectBitmap(bitmap, &stats)) {
      Fail(L"Shell " + label + L" @" + std::to_wstring(size) + L": failed " +
           HResultText(hr));
    } else if (stats.width <= 0 || stats.width > size) {
      Fail(L"Shell " + label + L" @" + std::to_wstring(size) + L": width " +
           std::to_wstring(stats.width));
    } else if (size == 1024 && stats.width != 1024) {
      // The sample is wider than 1024, so the handler must fill the request.
      Fail(L"Shell " + label + L" @1024: width " +
           std::to_wstring(stats.width) + L" expected 1024");
    } else {
      std::wcout << L"OK   Shell " << label << L" @" << size << L": "
                 << stats.width << L"x" << stats.height << L"\n";
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
  }
}

void CheckUnregistered() {
  ++g_checks;
  if (ZoreViewThumbnailIsRegistered()) {
    Fail(L"registration still present after unregister");
  }

  // COM keeps a per-process class-object cache, and this process has already
  // activated the handler. Only a fresh process proves that the CLSID is gone.
  wchar_t executable[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, executable,
                         static_cast<DWORD>(std::size(executable))) == 0) {
    Fail(L"could not locate the test executable for the unregister check");
    return;
  }
  std::wstring command = L"\"" + std::wstring(executable) +
                         L"\" --check-unregistered";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) {
    Fail(L"could not start the unregister check process");
    return;
  }
  WaitForSingleObject(process.hProcess, 15000);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  if (exit_code != 0) {
    Fail(L"a fresh process could still create the thumbnail handler");
  } else {
    std::wcout << L"OK   unregistered: a fresh process gets "
                  L"REGDB_E_CLASSNOTREG\n";
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Native output is UTF-8 so Chinese paths survive a redirected pipe.
  _setmode(_fileno(stdout), _O_U8TEXT);
  std::wcout.setf(std::ios::unitbuf);

  std::wstring image_dir;
  bool keep_registered = false;
  bool unregister_only = false;
  bool shell_only = false;
  bool check_unregistered = false;
  bool register_only = false;

  for (int i = 1; i < argc; ++i) {
    const std::wstring argument = argv[i];
    if (argument == L"--images" && i + 1 < argc) {
      image_dir = argv[++i];
    } else if (argument == L"--keep-registered") {
      keep_registered = true;
    } else if (argument == L"--unregister-only") {
      unregister_only = true;
    } else if (argument == L"--shell-only") {
      shell_only = true;
    } else if (argument == L"--check-unregistered") {
      check_unregistered = true;
    } else if (argument == L"--register-only") {
      register_only = true;
    } else {
      std::wcout << L"usage: zoreview_shelltests [--images <dir>] "
                    L"[--keep-registered] [--unregister-only] "
                    L"[--shell-only] [--check-unregistered] "
                    L"[--register-only]\n";
      return 2;
    }
  }

  if (register_only) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 2;
    const HRESULT hr = ZoreViewThumbnailRegister();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
  }

  if (check_unregistered) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 2;
    ComPtr<IThumbnailProvider> provider;
    const HRESULT hr = CreateProvider(&provider);
    CoUninitialize();
    if (SUCCEEDED(hr)) {
      std::wcout << L"FAIL: CoCreateInstance still succeeds after unregister\n";
      return 1;
    }
    std::wcout << L"OK   fresh process: CoCreateInstance " << HResultText(hr)
               << L"\n";
    return 0;
  }

  if (unregister_only) {
    return SUCCEEDED(ZoreViewThumbnailUnregister()) ? 0 : 1;
  }

  if (image_dir.empty()) {
    image_dir = L"..\\测试文件夹\\images";
  }
  if (!std::filesystem::is_directory(image_dir)) {
    std::wcout << L"images directory not found: " << image_dir << L"\n";
    return 2;
  }

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    std::wcout << L"CoInitializeEx failed " << HResultText(com) << L"\n";
    return 2;
  }

  if (shell_only) {
    // Fresh-process check for a handler that was registered before this
    // process started (the normal Explorer case).
    const std::wstring work = CreateWorkDirectory();
    if (work.empty()) {
      Fail(L"could not create the temporary work directory");
    } else {
      const std::wstring psd = JoinPath(work, L"shell-only.psd");
      const std::wstring svg = JoinPath(work, L"shell-only.svg");
      const std::wstring tga = JoinPath(work, L"shell-only.tga");
      const std::wstring jpg = JoinPath(work, L"shell-only.jpg");
      const bool copied =
          CopySample(JoinPath(image_dir, L"psd\\psd_rgba_rle.psd"), psd) &&
          CopySample(JoinPath(image_dir, L"svg\\basic.svg"), svg) &&
          CopySample(JoinPath(image_dir, L"tga\\photo.tga"), tga) &&
          CopySample(JoinPath(image_dir, L"jpeg\\photo_1600.jpg"), jpg);
      if (!copied) {
        Fail(L"could not copy the Shell samples");
      } else {
        CheckShell(L"psd", psd);
        CheckShell(L"svg", svg);
        CheckShell(L"tga", tga);
        CheckShell(L"jpeg", jpg);
        if (g_failures == 0) {
          std::error_code ec;
          std::filesystem::remove_all(work, ec);
        }
      }
    }
    CoUninitialize();
    if (g_failures > 0) {
      std::wcout << L"FAIL: " << g_failures << L" of " << g_checks
                 << L" checks failed\n";
      return 1;
    }
    std::wcout << L"OK: " << g_checks << L" checks passed\n";
    return 0;
  }

  // A previous crashed run may have left the registration behind.
  ZoreViewThumbnailUnregister();

  HRESULT hr = ZoreViewThumbnailRegister();
  if (FAILED(hr) || !ZoreViewThumbnailIsRegistered()) {
    std::wcout << L"FAIL: registration failed " << HResultText(hr) << L"\n";
    CoUninitialize();
    return 1;
  }
  std::wcout << L"registered: HKCU thumbnail handler active\n";
  if (!WaitForShellRegistration(3000)) {
    Note(L"merged HKCR still shows another handler; Shell checks may use it");
  }

  struct DirectCase {
    const wchar_t* relative;
    const wchar_t* label;
    UINT cx;
  };
  const DirectCase cases[] = {
      {L"jpeg\\photo_1600.jpg", L"jpeg", 256},
      {L"jpeg\\orient_6.jpg", L"jpeg orient_6", 256},
      {L"png\\photo_rgba.png", L"png rgba", 256},
      {L"png\\palette.png", L"png palette", 256},
      {L"bmp\\photo24.bmp", L"bmp", 256},
      {L"gif\\anim.gif", L"gif first frame", 256},
      {L"webp\\alpha_lossless.webp", L"webp alpha", 256},
      {L"tiff\\multipage.tif", L"tiff first page", 256},
      {L"ico\\multi.ico", L"ico", 256},
      {L"psd\\psd_rgba_rle.psd", L"psd rgba", 256},
      {L"psd\\psb_rgba_rle.psb", L"psb rgba", 256},
      {L"svg\\basic.svg", L"svg", 256},
      {L"apng\\rgba_full.png", L"apng first frame", 256},
      {L"tga\\photo.tga", L"tga", 256},
      {L"pcx\\photo.pcx", L"pcx", 256},
      {L"pnm\\photo.ppm", L"pnm", 256},
      {L"xbm\\tiny.xbm", L"xbm", 256},
      {L"xpm\\tiny.xpm", L"xpm", 256},
      {L"heic\\example.heic", L"heic", 256},
      {L"avif\\photo.avif", L"avif", 256},
      {L"..\\raw\\Canon\\EOS 5D Mark IV\\B13A0729.CR2", L"raw cr2", 256},
      {L"..\\raw\\Canon\\Canon EOS R5m2\\RAW.CR3", L"raw cr3", 256},
  };
  for (const DirectCase& item : cases) {
    const std::wstring path = JoinPath(image_dir, item.relative);
    if (!std::filesystem::exists(path)) {
      Fail(std::wstring(item.label) + L": sample missing: " + path);
      continue;
    }
    CheckThumbnail(item.label, path, item.cx, true);
  }

  // Large requests are clamped to 1024 pixels.
  ++g_checks;
  {
    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha_type = WTSAT_UNKNOWN;
    const std::wstring path = JoinPath(image_dir, L"jpeg\\photo_1600.jpg");
    const HRESULT large =
        ThumbnailDirect(path, 4096, false, &bitmap, &alpha_type, nullptr);
    BitmapStats stats;
    if (FAILED(large) || !InspectBitmap(bitmap, &stats)) {
      Fail(L"cx=4096 clamp: no bitmap " + HResultText(large));
    } else if (stats.width != 1024) {
      Fail(L"cx=4096 clamp: width " + std::to_wstring(stats.width) +
           L" expected 1024");
    } else {
      std::wcout << L"OK   cx=4096 clamped to " << stats.width << L"x"
                 << stats.height << L"\n";
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
  }

  // IInitializeWithItem is the path Explorer prefers.
  ++g_checks;
  {
    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha_type = WTSAT_UNKNOWN;
    const std::wstring path = JoinPath(image_dir, L"png\\photo_rgba.png");
    const HRESULT item = ThumbnailDirect(path, 128, true, &bitmap, &alpha_type,
                                         nullptr);
    BitmapStats stats;
    if (FAILED(item) || !InspectBitmap(bitmap, &stats)) {
      Fail(L"IInitializeWithItem failed " + HResultText(item));
    } else {
      std::wcout << L"OK   IInitializeWithItem: " << stats.width << L"x"
                 << stats.height << L"\n";
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
  }

  CheckCorrupt(JoinPath(image_dir, L"corrupt\\truncated.tif"));

  // Chinese/space path handling for both direct COM and the Shell path.
  const std::wstring work_dir = CreateWorkDirectory();
  if (work_dir.empty()) {
    Fail(L"could not create the temporary work directory");
  } else {
    const std::wstring source = JoinPath(image_dir, L"jpeg\\photo_1600.jpg");
    const std::wstring chinese = JoinPath(work_dir, L"中文 空格 图片.jpg");
    if (!CopySample(source, chinese)) {
      Fail(L"could not copy the Chinese-path sample");
    } else {
      CheckThumbnail(L"chinese path", chinese, 256, true);
    }

    const std::wstring psd = JoinPath(work_dir, L"shell-provider.psd");
    const std::wstring svg = JoinPath(work_dir, L"shell-provider.svg");
    const std::wstring jpg = JoinPath(work_dir, L"shell-provider.jpg");
    const std::wstring tga = JoinPath(work_dir, L"shell-provider.tga");
    const bool copied =
        CopySample(JoinPath(image_dir, L"psd\\psd_rgba_rle.psd"), psd) &&
        CopySample(JoinPath(image_dir, L"svg\\basic.svg"), svg) &&
        CopySample(source, jpg) &&
        CopySample(JoinPath(image_dir, L"tga\\photo.tga"), tga);
    if (!copied) {
      Fail(L"could not copy the Shell integration samples");
    } else {
      // PSD/SVG have no thumbnail handler on a stock Windows install, so a
      // successful thumbnail proves our registration is being used.
      CheckShell(L"psd", psd);
      CheckShell(L"svg", svg);
      CheckShell(L"jpeg", jpg);
      CheckShell(L"tga", tga);
      CheckShellSizes(L"tga", tga);
      if (g_failures == 0) {
        std::error_code ec;
        std::filesystem::remove_all(work_dir, ec);
      }
    }
  }

  if (!keep_registered) {
    ZoreViewThumbnailUnregister();
    CheckUnregistered();
  } else {
    std::wcout << L"registration kept (--keep-registered)\n";
  }

  CoUninitialize();

  if (g_failures > 0) {
    std::wcout << L"FAIL: " << g_failures << L" of " << g_checks
               << L" checks failed\n";
    return 1;
  }
  std::wcout << L"OK: " << g_checks << L" checks passed\n";
  return 0;
}
