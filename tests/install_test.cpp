// M3-2b: portable mode, optional file associations, per-user installer and the
// matching headless commands.
//
// The harness registers real per-user Shell state, so every section restores
// the previous values it touched; tests/run_install_check.ps1 additionally
// runs --unregister-assoc/--unregister-shell in its finally block.

#include "core/image.h"
#include "core/supported_extensions.h"
#include "core/version.h"
#include "platform/file_association.h"
#include "platform/installer.h"
#include "platform/paths.h"
#include "platform/registry_scope.h"
#include "platform/shell_integration.h"
#include "shell/thumbnail_registry.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

// ASan adds clang_rt.asan_*_dynamic-x86_64.dll next to the build outputs;
// copied executables would not find it. Library-level checks still run, but
// child processes started from temporary copies are skipped in ASan builds.
#if defined(NV_ASAN)
constexpr bool kAsanBuild = true;
#else
constexpr bool kAsanBuild = false;
#endif

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

bool KeyExists(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  RegCloseKey(key);
  return true;
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

bool ReadValue(HKEY root, const std::wstring& subkey, const wchar_t* name,
               std::wstring* value) {
  DWORD bytes = 0;
  LSTATUS status = RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ,
                                nullptr, nullptr, &bytes);
  if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return false;
  std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
  status = RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr,
                        buffer.data(), &bytes);
  if (status != ERROR_SUCCESS) return false;
  buffer.resize(std::wcslen(buffer.c_str()));
  *value = std::move(buffer);
  return true;
}

std::wstring ClassKey(const std::wstring& extension) {
  return L"Software\\Classes\\" + extension;
}

std::wstring OpenWithKey(const std::wstring& extension) {
  return ClassKey(extension) + L"\\OpenWithProgids";
}

std::wstring UserChoiceKey(const std::wstring& extension) {
  return L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
         extension + L"\\UserChoice";
}

struct ExtensionSnapshot {
  std::wstring extension;
  bool had_default = false;
  std::wstring default_value;
  bool had_user_choice = false;
  std::wstring user_choice_prog_id;
  std::wstring user_choice_hash;
};

ExtensionSnapshot Capture(const std::wstring& extension) {
  ExtensionSnapshot snapshot;
  snapshot.extension = extension;
  snapshot.had_default = ReadDefault(HKEY_CURRENT_USER, ClassKey(extension),
                                     &snapshot.default_value);
  snapshot.had_user_choice =
      KeyExists(HKEY_CURRENT_USER, UserChoiceKey(extension));
  if (snapshot.had_user_choice) {
    (void)ReadValue(HKEY_CURRENT_USER, UserChoiceKey(extension), L"ProgId",
                    &snapshot.user_choice_prog_id);
    (void)ReadValue(HKEY_CURRENT_USER, UserChoiceKey(extension), L"Hash",
                    &snapshot.user_choice_hash);
  }
  return snapshot;
}

void WriteDefault(HKEY root, const std::wstring& subkey,
                  const std::wstring& value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, nullptr, 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(value.c_str()),
                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
}

void WriteValue(HKEY root, const std::wstring& subkey, const wchar_t* name,
                const std::wstring& value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(key, name, 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(value.c_str()),
                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
}

void Restore(const ExtensionSnapshot& snapshot) {
  const std::wstring classes = ClassKey(snapshot.extension);
  if (snapshot.had_default) {
    WriteDefault(HKEY_CURRENT_USER, classes, snapshot.default_value);
  } else {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, classes.c_str(), 0, KEY_SET_VALUE,
                      &key) == ERROR_SUCCESS) {
      RegDeleteValueW(key, nullptr);
      RegCloseKey(key);
    }
  }
  if (snapshot.had_user_choice) {
    WriteValue(HKEY_CURRENT_USER, UserChoiceKey(snapshot.extension), L"ProgId",
               snapshot.user_choice_prog_id);
    WriteValue(HKEY_CURRENT_USER, UserChoiceKey(snapshot.extension), L"Hash",
               snapshot.user_choice_hash);
  }
}

bool RunProcess(const std::filesystem::path& executable,
                const std::wstring& arguments, DWORD* exit_code,
                DWORD timeout_ms = 60000) {  std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) {
    return false;
  }
  const DWORD wait = WaitForSingleObject(process.hProcess, timeout_ms);
  DWORD code = 1;
  if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exit_code != nullptr) *exit_code = code;
  return wait == WAIT_OBJECT_0;
}

std::filesystem::path UniqueTempDirectory(const wchar_t* name) {
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  const std::filesystem::path directory =
      base / (std::wstring(name) + L"-" +
              std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
  std::filesystem::create_directories(directory, ec);
  return directory;
}

bool CopyTreeFile(const std::filesystem::path& source,
                  const std::filesystem::path& target) {
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  return CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE;
}

std::string ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return std::string();
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

std::wstring LineValue(const std::string& text, const std::string& key) {
  const std::string prefix = key + "=";
  const size_t start = text.find(prefix);
  if (start == std::string::npos) return std::wstring();
  const size_t end = text.find('\n', start);
  const std::string value =
      text.substr(start + prefix.size(),
                  end == std::string::npos ? std::string::npos
                                           : end - start - prefix.size());
  const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
  if (length <= 0) return std::wstring();
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(),
                      static_cast<int>(value.size()), wide.data(), length);
  return wide;
}

void CheckPortable(const std::filesystem::path& exe,
                   const std::filesystem::path& dll,
                   const std::filesystem::path& temp) {
  const std::filesystem::path marker = temp / L"ZoreView.portable";
  Check(!nv::platform::HasPortableMarker(temp),
        "portable marker is absent before creation");
  std::ofstream(marker, std::ios::binary) << "marker\n";
  Check(nv::platform::HasPortableMarker(temp), "portable marker is detected");

  nv::platform::SetPortableMode(true);
  Check(nv::platform::IsPortableMode(), "--portable forces portable mode");
  Check(nv::platform::DataDir() == nv::platform::ExecutableDirectory(),
        "portable DataDir is the executable directory");
  nv::platform::SetPortableMode(false);
  Check(!nv::platform::IsPortableMode(), "portable override can be cleared");
  Check(nv::platform::DataDir() == nv::platform::AppDataDir(),
        "installed DataDir is %LOCALAPPDATA%\\ZoreView");

  // End to end: a copied package with the marker writes settings/logs next to
  // itself and reports the portable data directory through --print-paths.
  if (kAsanBuild) {
    std::printf("skip copied-executable runs (ASan runtime DLL)\n");
    nv::platform::SetPortableMode(false);
    return;
  }
  const std::filesystem::path package = temp / L"package";
  std::error_code ec;
  std::filesystem::create_directories(package, ec);
  Check(CopyTreeFile(exe, package / L"zoreview.exe"),
        "copy the executable into the portable package");
  (void)CopyTreeFile(dll, package / L"ZoreViewThumb.dll");
  std::ofstream(package / L"ZoreView.portable", std::ios::binary) << "marker\n";

  const std::filesystem::path report = temp / L"paths.txt";
  DWORD exit_code = 1;
  Check(RunProcess(package / L"zoreview.exe",
                   L"--print-paths \"" + report.wstring() + L"\"", &exit_code),
        "portable executable runs --print-paths");
  const std::string text = ReadFileBytes(report);
  Check(exit_code == 0 && !text.empty(), "--print-paths exits successfully");
  Check(LineValue(text, "portable") == L"1",
        "marker file enables portable mode without --portable");
  Check(LineValue(text, "data") == package.wstring(),
        "--print-paths reports the package directory as data dir");
  Check(std::filesystem::is_directory(package / L"logs", ec),
        "portable mode writes logs next to the executable");

  // --portable works even without the marker.
  const std::filesystem::path forced = temp / L"forced";
  std::filesystem::create_directories(forced, ec);
  Check(CopyTreeFile(exe, forced / L"zoreview.exe"),
        "copy the executable for the --portable case");
  const std::filesystem::path forced_report = temp / L"forced-paths.txt";
  Check(RunProcess(forced / L"zoreview.exe",
                   L"--portable --print-paths \"" + forced_report.wstring() +
                       L"\"",
                   &exit_code) &&
            exit_code == 0,
        "--portable runs without a marker");
  const std::string forced_text = ReadFileBytes(forced_report);
  Check(LineValue(forced_text, "portable") == L"1",
        "--portable reports portable=1");
  Check(LineValue(forced_text, "data") == forced.wstring(),
        "--portable uses the executable directory");

  nv::platform::SetPortableMode(false);
}

void CheckAssociations(const std::filesystem::path& exe,
                       std::vector<ExtensionSnapshot>* snapshots) {
  const wchar_t* extensions[] = {L".psd", L".xpm", L".cr2"};
  snapshots->clear();
  for (const wchar_t* extension : extensions) {
    snapshots->push_back(Capture(extension));
  }

  std::wstring error;
  (void)nv::platform::UnregisterFileAssociations(&error);
  Check(!nv::platform::AreFileAssociationsRegistered(exe),
        "associations are absent before registration");

  Check(nv::platform::RegisterFileAssociations(exe, false, &error),
        "register file associations (OpenWith only)");
  nv::platform::AssociationStatus status;
  Check(nv::platform::AreFileAssociationsRegistered(exe, &status),
        "the ProgID and OpenWith entries point at this executable");
  Check(status.extension_count == nv::platform::AssociationExtensionCount(),
        "association extension count matches the supported list");
  Check(status.extension_count ==
            static_cast<int>(nv::ImageDocumentExtensions().size()),
        "associations cover every image document extension (icons excluded)");
  Check(status.default_count == 0, "registering did not take over defaults");

  std::wstring command;
  Check(ReadDefault(HKEY_CURRENT_USER,
                    L"Software\\Classes\\ZoreView.Image\\shell\\open\\command",
                    &command) &&
            _wcsicmp(command.c_str(),
                     (L"\"" + exe.wstring() + L"\" \"%1\"").c_str()) == 0,
        "ProgID open command quotes the executable and %1");
  std::wstring friendly;
  Check(ReadValue(HKEY_CURRENT_USER, L"Software\\Classes\\ZoreView.Image",
                  L"FriendlyTypeName", &friendly) &&
            !friendly.empty(),
        "ProgID has a friendly type name");

  bool open_with_ok = true;
  for (const ExtensionSnapshot& snapshot : *snapshots) {
    if (!KeyExists(HKEY_CURRENT_USER, OpenWithKey(snapshot.extension))) {
      open_with_ok = false;
      break;
    }
  }
  Check(open_with_ok, "OpenWithProgids exists for sampled extensions");

  // Registering must not change the current default.
  bool defaults_untouched = true;
  for (const ExtensionSnapshot& snapshot : *snapshots) {
    std::wstring current;
    const bool has_current =
        ReadDefault(HKEY_CURRENT_USER, ClassKey(snapshot.extension), &current);
    if (has_current != snapshot.had_default ||
        (has_current && current != snapshot.default_value)) {
      defaults_untouched = false;
    }
  }
  Check(defaults_untouched, "OpenWith registration leaves the default alone");

  Check(nv::platform::RegisterFileAssociations(exe, true, &error),
        "take over defaults explicitly (--set-default-assoc)");
  Check(nv::platform::AreFileAssociationsRegistered(exe, &status),
        "defaults still resolve to this executable");
  Check(status.default_count == status.extension_count,
        "every supported extension now defaults to ZoreView");
  std::wstring psd_default;
  Check(ReadDefault(HKEY_CURRENT_USER, ClassKey(L".psd"), &psd_default) &&
            psd_default == L"ZoreView.Image",
        ".psd class default points at the ZoreView ProgID");
  Check(!KeyExists(HKEY_CURRENT_USER, UserChoiceKey(L".psd")),
        "takeover removes the hashed UserChoice pick");
  Check(KeyExists(HKEY_CURRENT_USER,
                  L"Software\\ZoreView\\Associations\\Backup\\.psd"),
        "the previous default and UserChoice are backed up");

  Check(nv::platform::UnregisterFileAssociations(&error),
        "unregister file associations");
  Check(!nv::platform::AreFileAssociationsRegistered(exe),
        "associations are gone after unregister");
  Check(!KeyExists(HKEY_CURRENT_USER, L"Software\\Classes\\ZoreView.Image"),
        "the ProgID key is removed");
  Check(!KeyExists(HKEY_CURRENT_USER,
                   L"Software\\ZoreView\\Associations\\Backup"),
        "the association backup is removed");

  bool restored = true;
  for (const ExtensionSnapshot& snapshot : *snapshots) {
    std::wstring current;
    const bool has_current =
        ReadDefault(HKEY_CURRENT_USER, ClassKey(snapshot.extension), &current);
    const bool default_ok =
        has_current == snapshot.had_default &&
        (!has_current || current == snapshot.default_value);

    bool user_choice_ok = true;
    if (snapshot.had_user_choice) {
      std::wstring prog_id;
      std::wstring hash;
      user_choice_ok =
          ReadValue(HKEY_CURRENT_USER, UserChoiceKey(snapshot.extension),
                    L"ProgId", &prog_id) &&
          prog_id == snapshot.user_choice_prog_id;
      if (snapshot.user_choice_hash.empty()) {
        user_choice_ok = user_choice_ok &&
                         !ReadValue(HKEY_CURRENT_USER,
                                    UserChoiceKey(snapshot.extension), L"Hash",
                                    &hash);
      } else {
        user_choice_ok = user_choice_ok &&
                         ReadValue(HKEY_CURRENT_USER,
                                   UserChoiceKey(snapshot.extension), L"Hash",
                                   &hash) &&
                         hash == snapshot.user_choice_hash;
      }
    }
    if (!default_ok || !user_choice_ok) restored = false;
  }
  Check(restored, "unregister restores class defaults and UserChoice values");
}

void CheckAssociationCommands(const std::filesystem::path& exe) {
  DWORD exit_code = 1;
  std::wstring error;
  Check(RunProcess(exe, L"--register-assoc", &exit_code) && exit_code == 0,
        "zoreview.exe --register-assoc");
  Check(nv::platform::AreFileAssociationsRegistered(exe),
        "--register-assoc is visible in the registry");
  Check(RunProcess(exe, L"--set-default-assoc", &exit_code) &&
            exit_code == 0,
        "zoreview.exe --set-default-assoc");
  nv::platform::AssociationStatus status;
  Check(nv::platform::AreFileAssociationsRegistered(exe, &status) &&
            status.default_count == status.extension_count,
        "--set-default-assoc makes ZoreView the class default");
  Check(RunProcess(exe, L"--unregister-assoc", &exit_code) && exit_code == 0,
        "zoreview.exe --unregister-assoc");
  Check(!nv::platform::AreFileAssociationsRegistered(exe),
        "--unregister-assoc removes the association");
}

bool WaitForDirectoryRemoval(const std::filesystem::path& directory,
                             int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited += 250) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return true;
    Sleep(250);
  }
  return false;
}

bool FileExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

void CheckInstaller(const std::filesystem::path& exe,
                    const std::filesystem::path& dll,
                    const std::filesystem::path& temp,
                    const std::vector<ExtensionSnapshot>& snapshots) {
  const std::filesystem::path target = temp / L"install";
  std::error_code ec;
  std::filesystem::remove_all(target, ec);
  const std::filesystem::path installed_exe = target / L"zoreview.exe";
  const std::filesystem::path installed_dll = target / L"ZoreViewThumb.dll";
  const std::filesystem::path shortcut = nv::platform::StartMenuShortcutPath();

  DWORD exit_code = 1;
  Check(RunProcess(exe, L"--install --install-dir \"" + target.wstring() +
                            L"\"",
                   &exit_code) &&
            exit_code == 0,
        "zoreview.exe --install (per-user, custom directory)");
  Check(std::filesystem::is_regular_file(installed_exe) &&
            std::filesystem::is_regular_file(installed_dll),
        "the installer copied the executable and the thumbnail DLL");
  std::error_code size_ec;
  Check(std::filesystem::file_size(installed_dll, size_ec) ==
            std::filesystem::file_size(dll, size_ec) &&
            !size_ec,
        "the installed thumbnail DLL matches the source size");
  Check(FileExists(shortcut), "the installer created the Start menu shortcut");

  nv::platform::InstallInfo info;
  Check(nv::platform::QueryInstallation(nv::platform::RegistryScope::kUser,
                                        &info) &&
            info.installed && info.install_dir == target &&
            info.version == nv::kVersion,
        "install metadata records directory and version");
  std::wstring uninstall_string;
  Check(ReadValue(HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                  L"ZoreView",
                  L"UninstallString", &uninstall_string) &&
            uninstall_string.find(installed_exe.wstring()) != std::wstring::npos,
        "Apps & Features has an uninstall command for the installed copy");
  Check(nv::shell::IsThumbnailProviderRegistered(installed_dll.wstring()),
        "the installed thumbnail handler is registered");
  Check(nv::platform::AreShellVerbsRegistered(installed_exe),
        "context-menu verbs point at the installed executable");
  Check(nv::platform::AreFileAssociationsRegistered(installed_exe),
        "file associations point at the installed executable");

  if (kAsanBuild) {
    // The installed copy lacks the ASan runtime DLL; uninstall in-process.
    std::wstring uninstall_error;
    Check(nv::platform::UninstallApplication(
              nv::platform::RegistryScope::kUser, target, &uninstall_error),
          "uninstall the installation in-process (ASan build)");
  } else {
    Check(RunProcess(installed_exe, L"--uninstall", &exit_code) &&
              exit_code == 0,
          "the installed executable uninstalls itself");
  }
  Check(WaitForDirectoryRemoval(target, 20000),
        "the installation directory is removed after the process exits");
  Check(!nv::platform::QueryInstallation(nv::platform::RegistryScope::kUser,
                                         &info),
        "install metadata is removed");
  Check(!std::filesystem::exists(shortcut),
        "the Start menu shortcut is removed");
  Check(!nv::shell::IsThumbnailProviderRegistered(installed_dll.wstring()),
        "the thumbnail handler is unregistered");
  Check(!nv::platform::AreShellVerbsRegistered(installed_exe),
        "context-menu verbs are removed");
  Check(!nv::platform::AreFileAssociationsRegistered(installed_exe),
        "file associations are removed");

  bool defaults_restored = true;
  for (const ExtensionSnapshot& snapshot : snapshots) {
    std::wstring current;
    const bool has_current =
        ReadDefault(HKEY_CURRENT_USER, ClassKey(snapshot.extension), &current);
    if (has_current != snapshot.had_default ||
        (has_current && current != snapshot.default_value)) {
      defaults_restored = false;
    }
  }
  Check(defaults_restored,
        "installing without --associate-default leaves file defaults alone");

  // Second cycle: --associate-default takes over the defaults and uninstall
  // restores the original class defaults / UserChoice values.
  Check(RunProcess(exe, L"--install --associate-default --no-shortcut "
                            L"--install-dir \"" +
                            target.wstring() + L"\"",
                   &exit_code) &&
            exit_code == 0,
        "zoreview.exe --install --associate-default --no-shortcut");
  nv::platform::AssociationStatus assoc_status;
  Check(nv::platform::AreFileAssociationsRegistered(installed_exe,
                                                    &assoc_status) &&
            assoc_status.default_count == assoc_status.extension_count,
        "install --associate-default takes over the image defaults");
  Check(!std::filesystem::exists(shortcut),
        "--no-shortcut skips the Start menu shortcut");
  if (kAsanBuild) {
    std::wstring uninstall_error;
    Check(nv::platform::UninstallApplication(
              nv::platform::RegistryScope::kUser, target, &uninstall_error),
          "uninstall the --associate-default installation (ASan build)");
  } else {
    Check(RunProcess(installed_exe, L"--uninstall", &exit_code) &&
              exit_code == 0,
          "uninstall the --associate-default installation");
  }
  Check(WaitForDirectoryRemoval(target, 20000),
        "the second installation directory is removed");
  bool defaults_restored_after_takeover = true;
  for (const ExtensionSnapshot& snapshot : snapshots) {
    std::wstring current;
    const bool has_current =
        ReadDefault(HKEY_CURRENT_USER, ClassKey(snapshot.extension), &current);
    if (has_current != snapshot.had_default ||
        (has_current && current != snapshot.default_value)) {
      defaults_restored_after_takeover = false;
    }
  }
  Check(defaults_restored_after_takeover,
        "uninstall restores the defaults taken by --associate-default");

  if (!nv::platform::IsProcessElevated()) {
    const std::filesystem::path machine = temp / L"machine";
    Check(RunProcess(exe, L"--install --machine --install-dir \"" +
                              machine.wstring() + L"\"",
                     &exit_code) &&
              exit_code != 0,
          "--install --machine without elevation fails cleanly");
    Check(!std::filesystem::exists(machine),
          "the rejected machine install does not create a directory");
    Check(RunProcess(exe, L"--uninstall --machine", &exit_code) &&
              exit_code != 0,
          "--uninstall --machine without elevation fails cleanly");
  } else {
    std::printf("skip machine-scope rejection (already elevated)\n");
  }

  Check(RunProcess(exe, L"--uninstall", &exit_code) && exit_code != 0,
        "--uninstall without an installation reports an error");
}

void FinalCleanup(const std::vector<ExtensionSnapshot>& snapshots) {
  std::wstring ignored;
  (void)nv::platform::UnregisterFileAssociations(&ignored);
  (void)nv::platform::UnregisterShellVerbs(&ignored);
  (void)nv::shell::UnregisterThumbnailProvider();
  (void)nv::platform::RemoveStartMenuShortcut(&ignored);
  for (const ExtensionSnapshot& snapshot : snapshots) Restore(snapshot);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::filesystem::path exe = L"build\\Release\\zoreview.exe";
  std::filesystem::path dll = L"build\\Release\\ZoreViewThumb.dll";
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--exe" && i + 1 < argc) {
      exe = argv[++i];
    } else if (arg == L"--dll" && i + 1 < argc) {
      dll = argv[++i];
    } else {
      std::printf("usage: install_test [--exe <zoreview.exe>] "
                  "[--dll <ZoreViewThumb.dll>]\n");
      return 2;
    }
  }
  exe = std::filesystem::absolute(exe);
  dll = std::filesystem::absolute(dll);
  if (!std::filesystem::is_regular_file(exe) ||
      !std::filesystem::is_regular_file(dll)) {
    std::printf("missing executable or thumbnail DLL\n");
    return 2;
  }

  const std::filesystem::path temp = UniqueTempDirectory(L"zoreview-installtest");
  Check(!temp.empty(), "create the temporary working directory");

  CheckPortable(exe, dll, temp);
  std::vector<ExtensionSnapshot> snapshots;
  CheckAssociations(exe, &snapshots);
  CheckAssociationCommands(exe);
  CheckInstaller(exe, dll, temp, snapshots);

  FinalCleanup(snapshots);
  std::error_code ec;
  std::filesystem::remove_all(temp, ec);

  if (g_failures != 0) {
    std::printf("%d install/association check(s) failed\n", g_failures);
    std::fflush(stdout);
    return 1;
  }
  std::printf("all %d install/association checks passed\n", g_checks);
  std::fflush(stdout);
  return 0;
}
