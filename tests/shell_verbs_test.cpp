// M3-2: Shell context-menu verbs and the headless commands they invoke.
//
// Registers the three verbs for every supported image extension, verifies the
// registry layout and command lines, runs the real executable's
// --register-shell/--unregister-shell/--copy-image paths, and (optionally with
// --wallpaper) --set-wallpaper with the previous wallpaper restored.

#include "core/format_router.h"
#include "core/image.h"
#include "platform/file_ops.h"
#include "platform/i18n.h"
#include "platform/paths.h"
#include "platform/shell_integration.h"

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (ok) {
    std::printf("ok   %s\n", what.c_str());
  } else {
    std::printf("FAIL %s\n", what.c_str());
    ++g_failures;
  }
  std::fflush(stdout);
}

constexpr wchar_t kOpenVerb[] = L"ZoreView.Open";
constexpr wchar_t kWallpaperVerb[] = L"ZoreView.SetWallpaper";
constexpr wchar_t kCopyVerb[] = L"ZoreView.CopyImage";

// {3981E225-F559-11D3-8E3A-00C04F6837D5}
constexpr GUID kSfuiObjectHandler = {
    0x3981e225, 0xf559, 0x11d3,
    {0x8e, 0x3a, 0x00, 0xc0, 0x4f, 0x68, 0x37, 0xd5}};

std::wstring VerbKey(const std::wstring& extension, const wchar_t* verb) {
  return L"Software\\Classes\\SystemFileAssociations\\" + extension +
         L"\\shell\\" + verb;
}

bool ReadDefault(HKEY root, const std::wstring& subkey, std::wstring* value) {
  DWORD bytes = 0;
  LSTATUS status = RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ,
                                nullptr, nullptr, &bytes);
  if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return false;
  std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
  status = RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                        buffer.data(), &bytes);
  if (status != ERROR_SUCCESS) return false;
  buffer.resize(std::wcslen(buffer.c_str()));
  *value = std::move(buffer);
  return true;
}

bool KeyExists(const std::wstring& subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  RegCloseKey(key);
  return true;
}

std::wstring QuoteCommand(const std::filesystem::path& exe,
                          const wchar_t* arguments) {
  std::wstring command = L"\"";
  command += exe.wstring();
  command += L"\" ";
  command += arguments;
  return command;
}

bool RunProcess(const std::filesystem::path& executable,
                const std::wstring& arguments, DWORD* exit_code,
                DWORD timeout_ms = 30000) {
  std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) {
    return false;
  }
  const DWORD wait = WaitForSingleObject(process.hProcess, timeout_ms);
  DWORD code = 1;
  if (wait == WAIT_OBJECT_0) {
    GetExitCodeProcess(process.hProcess, &code);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exit_code != nullptr) *exit_code = code;
  return wait == WAIT_OBJECT_0;
}

bool CheckVerbLayout(const std::filesystem::path& exe) {
  const std::wstring extensions[] = {L".jpg", L".psd", L".tga"};
  for (const std::wstring& extension : extensions) {
    std::wstring label;
    std::wstring command;
    if (!ReadDefault(HKEY_CURRENT_USER, VerbKey(extension, kOpenVerb),
                     &label) ||
        !ReadDefault(HKEY_CURRENT_USER,
                     VerbKey(extension, kOpenVerb) + L"\\command", &command)) {
      return false;
    }
    if (label != L"用 ZoreView 打开") return false;
    if (_wcsicmp(command.c_str(), QuoteCommand(exe, L"\"%1\"").c_str()) != 0) {
      return false;
    }

    if (!ReadDefault(HKEY_CURRENT_USER,
                     VerbKey(extension, kWallpaperVerb) + L"\\command",
                     &command) ||
        _wcsicmp(command.c_str(),
                 QuoteCommand(exe, L"--set-wallpaper \"%1\"").c_str()) != 0) {
      return false;
    }

    if (!ReadDefault(HKEY_CURRENT_USER,
                     VerbKey(extension, kCopyVerb) + L"\\command", &command) ||
        _wcsicmp(command.c_str(),
                 QuoteCommand(exe, L"--copy-image \"%1\"").c_str()) != 0) {
      return false;
    }
  }
  // Icon formats deliberately get no verbs.
  return !KeyExists(VerbKey(L".ico", kOpenVerb)) &&
         !KeyExists(VerbKey(L".cur", kOpenVerb));
}

bool CheckClipboardImage(const std::filesystem::path& source) {
  const nv::DecodeResult decoded = nv::FormatRouter::Decode(source);
  if (!decoded.ok()) return false;

  bool ok = false;
  for (int attempt = 0; attempt < 10 && !ok; ++attempt) {
    if (!OpenClipboard(nullptr)) {
      Sleep(20);
      continue;
    }
    HANDLE data = GetClipboardData(CF_DIB);
    if (data != nullptr) {
      const auto* header =
          static_cast<const BITMAPINFOHEADER*>(GlobalLock(data));
      if (header != nullptr) {
        ok = header->biSize == sizeof(BITMAPINFOHEADER) &&
             header->biBitCount == 32 &&
             header->biWidth == static_cast<LONG>(decoded.image->info.width) &&
             header->biHeight ==
                 static_cast<LONG>(decoded.image->info.height);
        GlobalUnlock(data);
      }
    }
    CloseClipboard();
    if (!ok) Sleep(20);
  }
  return ok;
}

bool ShellContextMenuHas(const std::filesystem::path& file,
                         const wchar_t* expected) {
  std::error_code ec;
  const std::filesystem::path absolute =
      std::filesystem::absolute(file, ec);
  ComPtr<IShellItem> item;
  if (FAILED(SHCreateItemFromParsingName(
          absolute.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf())))) {
    return false;
  }
  ComPtr<IContextMenu> menu;
  if (FAILED(item->BindToHandler(nullptr, kSfuiObjectHandler,
                                 IID_PPV_ARGS(menu.GetAddressOf())))) {
    return false;
  }
  HMENU popup = CreatePopupMenu();
  if (popup == nullptr) return false;

  bool found = false;
  if (SUCCEEDED(menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_NORMAL))) {
    const int count = GetMenuItemCount(popup);
    for (int i = 0; i < count && !found; ++i) {
      wchar_t text[256]{};
      if (GetMenuStringW(popup, static_cast<UINT>(i), text,
                         static_cast<int>(std::size(text)), MF_BYPOSITION) > 0 &&
          std::wcsstr(text, expected) != nullptr) {
        found = true;
      }
    }
  }
  DestroyMenu(popup);
  return found;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::filesystem::path exe = L"build\\Release\\zoreview.exe";
  std::filesystem::path images = L"..\\测试文件夹\\images";
  std::filesystem::path wallpaper_sample = L"..\\测试文件夹\\images\\tga\\photo.tga";
  bool test_wallpaper = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--exe" && i + 1 < argc) {
      exe = argv[++i];
    } else if (arg == L"--images" && i + 1 < argc) {
      images = argv[++i];
    } else if (arg == L"--wallpaper") {
      test_wallpaper = true;
    } else {
      std::printf("usage: shell_verbs_test [--exe <zoreview.exe>] "
                  "[--images <dir>] [--wallpaper]\n");
      return 2;
    }
  }
  exe = std::filesystem::absolute(exe);
  if (!std::filesystem::is_regular_file(exe)) {
    std::printf("executable not found: %ls\n", exe.c_str());
    return 2;
  }
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    std::printf("CoInitializeEx failed\n");
    return 2;
  }
  // The verb labels follow the UI language; pin it so the expected strings do
  // not depend on the machine's display language.
  nv::i18n::SetLanguage(nv::i18n::Language::kChinese);

  nv::platform::UnregisterShellVerbs(nullptr);
  Check(!nv::platform::AreShellVerbsRegistered(exe),
        "verbs are absent before registration");

  std::wstring error;
  Check(nv::platform::RegisterShellVerbs(exe, &error),
        "register shell verbs");
  int extension_count = 0;
  Check(nv::platform::AreShellVerbsRegistered(exe, &extension_count),
        "all verbs point at this executable");
  Check(extension_count == nv::platform::ShellVerbExtensionCount(),
        "verb extension count matches the supported list");
  Check(extension_count > 30, "verbs cover the P0+P1+P2 formats");
  Check(CheckVerbLayout(exe), "verb labels and command lines");

  // Ask the Shell itself for a file's context menu, the same path Explorer
  // uses. This catches registration locations that the registry layout check
  // alone would miss. The merged HKCR view can lag a HKCU write by 1-2 s.
  Sleep(2500);
  const std::filesystem::path menu_sample = images / L"tga" / L"photo.tga";
  if (std::filesystem::is_regular_file(menu_sample)) {
    Check(ShellContextMenuHas(menu_sample, L"用 ZoreView 打开"),
          "Shell menu contains the open verb (tga)");
    Check(ShellContextMenuHas(menu_sample, L"设为桌面壁纸"),
          "Shell menu contains the wallpaper verb (tga)");
    Check(ShellContextMenuHas(menu_sample, L"复制图像"),
          "Shell menu contains the copy verb (tga)");
  } else {
    std::printf("skip Shell menu check (missing %ls)\n", menu_sample.c_str());
  }
  const std::filesystem::path jpg_sample = images / L"jpeg" / L"photo_1600.jpg";
  if (std::filesystem::is_regular_file(jpg_sample)) {
    Check(ShellContextMenuHas(jpg_sample, L"用 ZoreView 打开"),
          "Shell menu contains the open verb (jpg)");
  }

  Check(nv::platform::UnregisterShellVerbs(&error),
        "unregister shell verbs");
  Check(!nv::platform::AreShellVerbsRegistered(exe),
        "verbs are gone after unregister");
  Check(!KeyExists(VerbKey(L".jpg", kOpenVerb)),
        "no ZoreView.Open key remains");

  // The installed executable must be able to do the same thing itself.
  DWORD exit_code = 1;
  Check(RunProcess(exe, L"--register-shell", &exit_code) && exit_code == 0,
        "zoreview.exe --register-shell");
  Check(nv::platform::AreShellVerbsRegistered(exe),
        "app registration is visible");
  Check(RunProcess(exe, L"--unregister-shell", &exit_code) && exit_code == 0,
        "zoreview.exe --unregister-shell");
  Check(!nv::platform::AreShellVerbsRegistered(exe),
        "app unregistration is complete");

  // Headless copy-image: the process must exit and leave a 32bpp CF_DIB.
  const std::filesystem::path copy_sample = images / L"png" / L"photo_rgba.png";
  if (std::filesystem::is_regular_file(copy_sample)) {
    Check(RunProcess(exe, L"--copy-image \"" + copy_sample.wstring() + L"\"",
                     &exit_code) &&
              exit_code == 0,
          "zoreview.exe --copy-image");
    Check(CheckClipboardImage(copy_sample), "clipboard has the decoded image");
  } else {
    std::printf("skip --copy-image (missing %ls)\n", copy_sample.c_str());
  }

  if (test_wallpaper && std::filesystem::is_regular_file(wallpaper_sample)) {
    wchar_t previous[32768]{};
    const bool had_previous =
        SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                              static_cast<UINT>(std::size(previous)), previous,
                              0) != FALSE;
    const std::filesystem::path target =
        nv::platform::AppDataDir() / L"wallpaper" / L"wallpaper.bmp";
    std::wstring arguments = L"--set-wallpaper \"";
    arguments += wallpaper_sample.wstring();
    arguments += L"\"";
    const bool ran = RunProcess(exe, arguments, &exit_code) && exit_code == 0;
    bool current_ok = false;
    if (ran) {
      wchar_t current[32768]{};
      current_ok = SystemParametersInfoW(
                       SPI_GETDESKWALLPAPER,
                       static_cast<UINT>(std::size(current)), current, 0) !=
                       FALSE &&
                   _wcsicmp(current, target.c_str()) == 0;
    }
    if (had_previous && previous[0] != L'\0') {
      nv::platform::SetDesktopWallpaper(previous, &error);
    }
    Check(ran && current_ok, "--set-wallpaper (previous wallpaper restored)");
  }

  nv::platform::UnregisterShellVerbs(nullptr);
  if (g_failures != 0) {
    std::printf("%d shell-verb check(s) failed\n", g_failures);
    std::fflush(stdout);
    CoUninitialize();
    return 1;
  }
  std::printf("all %d shell-verb checks passed\n", g_checks);
  std::fflush(stdout);
  CoUninitialize();
  return 0;
}
