#include "platform/installer.h"

#include "core/version.h"
#include "platform/file_association.h"
#include "platform/i18n.h"
#include "platform/paths.h"
#include "platform/shell_integration.h"
#include "shell/thumbnail_registry.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace nv::platform {
namespace {

constexpr wchar_t kInstallKey[] = L"Software\\ZoreView\\Install";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZoreView";
constexpr wchar_t kDisplayName[] = L"ZoreView";

std::wstring ErrorText(const wchar_t* action, HRESULT hr) {
  wchar_t buffer[256]{};
  std::swprintf(buffer, std::size(buffer), L"%s failed (HRESULT 0x%08lX)",
                action, static_cast<unsigned long>(hr));
  return buffer;
}

std::wstring Win32ErrorText(const wchar_t* action, DWORD code) {
  wchar_t buffer[256]{};
  std::swprintf(buffer, std::size(buffer), L"%s failed (error %lu)", action,
                static_cast<unsigned long>(code));
  return buffer;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) {
    return std::wstring();
  }
  std::wstring value(raw);
  CoTaskMemFree(raw);
  return value;
}

std::filesystem::path CurrentExecutablePath() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return std::filesystem::path();
    if (length < buffer.size()) {
      buffer.resize(length);
      return std::filesystem::path(buffer);
    }
    if (buffer.size() >= 32768) return std::filesystem::path();
    buffer.resize(buffer.size() * 2);
  }
}

HRESULT SetString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                  const std::wstring& value) {
  HKEY key = nullptr;
  LSTATUS status =
      RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  status = RegSetValueExW(
      key, name, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT SetDword(HKEY root, const std::wstring& subkey, const wchar_t* name,
                 DWORD value) {
  HKEY key = nullptr;
  LSTATUS status =
      RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  status = RegSetValueExW(key, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value),
                          static_cast<DWORD>(sizeof(value)));
  RegCloseKey(key);
  return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT ReadString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                   std::wstring* value, bool* exists) {
  *exists = false;
  DWORD bytes = 0;
  LSTATUS status = RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ,
                                nullptr, nullptr, &bytes);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return S_OK;
  }
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  if (bytes < sizeof(wchar_t)) return S_OK;

  std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
  status = RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr,
                        buffer.data(), &bytes);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  buffer.resize(std::wcslen(buffer.c_str()));
  *value = std::move(buffer);
  *exists = true;
  return S_OK;
}

HRESULT DeleteTree(HKEY root, const std::wstring& subkey) {
  const LSTATUS status = RegDeleteTreeW(root, subkey.c_str());
  if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
      status == ERROR_PATH_NOT_FOUND) {
    return S_OK;
  }
  return HRESULT_FROM_WIN32(status);
}

bool SameFile(const std::filesystem::path& left,
              const std::filesystem::path& right) {
  std::error_code ec;
  if (!std::filesystem::exists(left, ec) ||
      !std::filesystem::exists(right, ec)) {
    return false;
  }
  return std::filesystem::equivalent(left, right, ec) && !ec;
}

bool CopyIntoPlace(const std::filesystem::path& source,
                   const std::filesystem::path& target, std::wstring* error) {
  if (SameFile(source, target)) return true;
  if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
    if (error != nullptr) {
      *error = Win32ErrorText(L"Copy installation file", GetLastError());
      *error += L": ";
      *error += target.wstring();
    }
    return false;
  }
  return true;
}

// Release packages carry the license bundle, documentation and SBOM next to
// the binaries. Copy them when present so an installed copy stays compliant;
// development builds without those files install exactly as before.
bool CopyReleaseDocuments(const std::filesystem::path& source_dir,
                          const std::filesystem::path& install_dir,
                          std::wstring* error) {
  const wchar_t* const files[] = {
      L"LICENSE",       L"NOTICE.md",       L"README.md",
      L"PRIVACY.md",    L"CHANGELOG.md",    L"RELEASE_NOTES.md",
      L"SBOM.cdx.json", L"VERSION.txt",     L"SIGNATURES.txt",
  };
  for (const wchar_t* name : files) {
    const std::filesystem::path source = source_dir / name;
    if (!std::filesystem::is_regular_file(source)) continue;
    if (!CopyIntoPlace(source, install_dir / name, error)) return false;
  }

  const std::filesystem::path licenses_source =
      source_dir / L"THIRD_PARTY_LICENSES";
  if (std::filesystem::is_directory(licenses_source)) {
    const std::filesystem::path licenses_target =
        install_dir / L"THIRD_PARTY_LICENSES";
    std::error_code ec;
    std::filesystem::create_directories(licenses_target, ec);
    if (ec) {
      if (error != nullptr) {
        *error = L"Could not create the license directory: " +
                 licenses_target.wstring();
      }
      return false;
    }
    for (std::filesystem::directory_iterator it(licenses_source, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (!CopyIntoPlace(it->path(), licenses_target / it->path().filename(),
                         error)) {
        return false;
      }
    }
    if (ec) {
      if (error != nullptr) {
        *error = L"Could not read the license directory: " +
                 licenses_source.wstring();
      }
      return false;
    }
  }
  return true;
}

bool DirectoryContains(const std::filesystem::path& directory,
                       const std::filesystem::path& file) {  std::error_code ec;
  std::filesystem::path dir = std::filesystem::weakly_canonical(directory, ec);
  if (ec) dir = std::filesystem::absolute(directory, ec);
  ec.clear();
  std::filesystem::path target = std::filesystem::weakly_canonical(file, ec);
  if (ec) target = std::filesystem::absolute(file, ec);
  if (dir.empty() || target.empty()) return false;

  std::wstring prefix = dir.wstring();
  if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/') {
    prefix.push_back(L'\\');
  }
  const std::wstring text = target.wstring();
  return _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool RemoveDirectoryWithRetry(const std::filesystem::path& directory,
                              std::wstring* error) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    if (!ec) return true;
    if (!std::filesystem::exists(directory, ec)) return true;
    Sleep(200);
  }
  if (error != nullptr) {
    *error = L"Could not remove the installation directory: ";
    *error += directory.wstring();
  }
  return false;
}

// The uninstaller is usually running from inside the directory it has to
// delete, and Windows will not delete a running image. A detached command
// waits for this process to exit and then removes the tree.
bool ScheduleDirectoryRemoval(const std::filesystem::path& directory,
                              std::wstring* error) {
  wchar_t comspec[MAX_PATH * 2]{};
  const DWORD length = GetEnvironmentVariableW(
      L"ComSpec", comspec, static_cast<DWORD>(std::size(comspec)));
  std::wstring shell = (length > 0 && length < std::size(comspec))
                           ? std::wstring(comspec)
                           : std::wstring(L"C:\\Windows\\System32\\cmd.exe");

  std::wstring command = L"\"" + shell +
                         L"\" /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" +
                         directory.wstring() + L"\"";

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                      &startup, &process)) {
    if (error != nullptr) {
      *error = Win32ErrorText(L"Schedule installation directory removal",
                              GetLastError());
    }
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool WriteInstallMetadata(HKEY root, const InstallLayout& layout,
                          RegistryScope scope) {
  HRESULT hr = SetString(root, kInstallKey, L"InstallDir",
                         layout.install_dir.wstring());
  if (FAILED(hr)) return false;
  hr = SetString(root, kInstallKey, L"Version", nv::kVersion);
  if (FAILED(hr)) return false;
  return SUCCEEDED(SetString(root, kInstallKey, L"Scope", ScopeName(scope)));
}

bool WriteUninstallEntry(HKEY root, const InstallLayout& layout,
                         RegistryScope scope) {
  const std::wstring exe = layout.executable.wstring();
  std::wstring uninstall = L"\"" + exe + L"\" --uninstall";
  std::wstring quiet = uninstall + L" --quiet";
  if (scope == RegistryScope::kMachine) {
    uninstall += L" --machine";
    quiet += L" --machine";
  }

  HRESULT hr = SetString(root, kUninstallKey, L"DisplayName", kDisplayName);
  if (FAILED(hr)) return false;
  hr = SetString(root, kUninstallKey, L"DisplayVersion", nv::kVersion);
  if (FAILED(hr)) return false;
  hr = SetString(root, kUninstallKey, L"Publisher", kDisplayName);
  if (FAILED(hr)) return false;
  hr = SetString(root, kUninstallKey, L"DisplayIcon", exe);
  if (FAILED(hr)) return false;
  hr = SetString(root, kUninstallKey, L"InstallLocation",
                 layout.install_dir.wstring());
  if (FAILED(hr)) return false;
  hr = SetString(root, kUninstallKey, L"UninstallString", uninstall);
  if (FAILED(hr)) return false;
  hr = SetString(root, kUninstallKey, L"QuietUninstallString", quiet);
  if (FAILED(hr)) return false;
  hr = SetDword(root, kUninstallKey, L"NoModify", 1);
  if (FAILED(hr)) return false;
  return SUCCEEDED(SetDword(root, kUninstallKey, L"NoRepair", 1));
}

void RemoveInstallMetadata(HKEY root) {
  (void)DeleteTree(root, kUninstallKey);
  (void)DeleteTree(root, kInstallKey);
}

// Machine-wide installs must not be shadowed by a stale per-user registration
// (HKCU wins over HKLM for the current user and would re-enable in-process
// thumbnail loading without DisableProcessIsolation).
void RemovePerUserShadowing() {
  std::wstring ignored;
  (void)nv::shell::UnregisterThumbnailProvider(RegistryScope::kUser);
  (void)UnregisterShellVerbs(&ignored, RegistryScope::kUser);
  (void)UnregisterFileAssociations(&ignored, RegistryScope::kUser);
}

}  // namespace

bool IsProcessElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD bytes = 0;
  const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                      sizeof(elevation), &bytes);
  CloseHandle(token);
  return ok != FALSE && elevation.TokenIsElevated != 0;
}

InstallLayout DefaultInstallLayout(RegistryScope scope) {
  InstallLayout layout;
  if (scope == RegistryScope::kMachine) {
    const std::wstring program_files = KnownFolder(FOLDERID_ProgramFiles);
    layout.install_dir = std::filesystem::path(program_files) / L"ZoreView";
  } else {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    layout.install_dir =
        std::filesystem::path(local) / L"Programs" / L"ZoreView";
  }
  layout.executable = layout.install_dir / L"zoreview.exe";
  layout.thumbnail_dll = layout.install_dir / L"ZoreViewThumb.dll";
  return layout;
}

std::filesystem::path StartMenuShortcutPath(RegistryScope scope) {
  const std::wstring programs =
      scope == RegistryScope::kMachine ? KnownFolder(FOLDERID_CommonPrograms)
                                       : KnownFolder(FOLDERID_Programs);
  if (programs.empty()) return std::filesystem::path();
  return std::filesystem::path(programs) / L"ZoreView.lnk";
}

bool CreateStartMenuShortcut(const InstallLayout& layout, std::wstring* error,
                             RegistryScope scope) {
  if (error != nullptr) error->clear();
  const std::filesystem::path shortcut = StartMenuShortcutPath(scope);
  if (shortcut.empty()) {
    if (error != nullptr) *error = L"Could not locate the Start menu folder.";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(shortcut.parent_path(), ec);

  ComPtr<IShellLinkW> link;
  HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.GetAddressOf()));
  if (FAILED(hr)) {
    if (error != nullptr) *error = ErrorText(L"Create Start menu shortcut", hr);
    return false;
  }
  (void)link->SetPath(layout.executable.c_str());
  (void)link->SetWorkingDirectory(layout.install_dir.c_str());
  (void)link->SetDescription(
      nv::i18n::Text(nv::i18n::StringId::kShortcutDescription));
  (void)link->SetIconLocation(layout.executable.c_str(), 0);

  ComPtr<IPersistFile> file;
  hr = link.As(&file);
  if (SUCCEEDED(hr)) hr = file->Save(shortcut.c_str(), TRUE);
  if (FAILED(hr)) {
    if (error != nullptr) *error = ErrorText(L"Save Start menu shortcut", hr);
    return false;
  }
  return true;
}

bool RemoveStartMenuShortcut(std::wstring* error, RegistryScope scope) {
  if (error != nullptr) error->clear();
  const std::filesystem::path shortcut = StartMenuShortcutPath(scope);
  if (shortcut.empty()) return true;
  if (!DeleteFileW(shortcut.c_str())) {
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
      if (error != nullptr) *error = Win32ErrorText(L"Delete shortcut", code);
      return false;
    }
  }
  return true;
}

bool InstallApplication(const std::filesystem::path& source_executable,
                        RegistryScope scope,
                        const std::filesystem::path& custom_dir,
                        bool create_shortcut, bool associate_default,
                        std::wstring* error, std::wstring* warning) {
  if (error != nullptr) error->clear();
  if (warning != nullptr) warning->clear();
  if (scope == RegistryScope::kMachine && !IsProcessElevated()) {
    if (error != nullptr) {
      *error = L"Installing for all users requires administrator rights. "
               L"Run the installer as administrator, or use the per-user "
               L"installation, which needs no elevation.";
    }
    return false;
  }
  if (!std::filesystem::is_regular_file(source_executable)) {
    if (error != nullptr) *error = L"The executable to install was not found.";
    return false;
  }

  const std::filesystem::path source_dir = source_executable.parent_path();
  const std::filesystem::path source_dll = source_dir / L"ZoreViewThumb.dll";
  if (!std::filesystem::is_regular_file(source_dll)) {
    if (error != nullptr) {
      *error =
          L"The package is incomplete: ZoreViewThumb.dll must be in the same "
          L"folder as zoreview.exe.";
    }
    return false;
  }

  InstallLayout layout = DefaultInstallLayout(scope);
  if (!custom_dir.empty()) {
    layout.install_dir = custom_dir;
    layout.executable = layout.install_dir / L"zoreview.exe";
    layout.thumbnail_dll = layout.install_dir / L"ZoreViewThumb.dll";
  }

  std::error_code ec;
  const bool directory_existed =
      std::filesystem::exists(layout.install_dir, ec);
  std::filesystem::create_directories(layout.install_dir, ec);
  if (ec) {
    if (error != nullptr) {
      *error = L"Could not create the installation directory: " +
               layout.install_dir.wstring();
    }
    return false;
  }
  const bool source_inside_target = SameFile(source_executable,
                                             layout.executable);

  std::wstring failure;
  bool files_ok = CopyIntoPlace(source_executable, layout.executable, &failure);
  if (files_ok) {
    files_ok = CopyIntoPlace(source_dll, layout.thumbnail_dll, &failure);
  }
  const std::filesystem::path notice = source_dir / L"NOTICE.md";
  if (files_ok && std::filesystem::is_regular_file(notice)) {
    files_ok =
        CopyIntoPlace(notice, layout.install_dir / L"NOTICE.md", &failure);
  }
  if (files_ok) {
    files_ok = CopyReleaseDocuments(source_dir, layout.install_dir, &failure);
  }
  if (!files_ok) {
    if (error != nullptr) *error = failure;
    return false;
  }

  if (scope == RegistryScope::kMachine) RemovePerUserShadowing();

  auto rollback = [&] {
    std::wstring ignored;
    (void)nv::shell::UnregisterThumbnailProvider(scope);
    (void)UnregisterShellVerbs(&ignored, scope);
    (void)UnregisterFileAssociations(&ignored, scope);
    (void)RemoveStartMenuShortcut(&ignored, scope);
    RemoveInstallMetadata(RootKey(scope));
    // Only remove files when this attempt created the directory: an upgrade
    // over an existing installation must not delete the previous copy.
    if (!directory_existed && !source_inside_target) {
      std::error_code remove_ec;
      std::filesystem::remove_all(layout.install_dir, remove_ec);
    }
  };

  const HRESULT thumb = nv::shell::RegisterThumbnailProvider(
      layout.thumbnail_dll.wstring(), scope);
  if (FAILED(thumb)) {
    rollback();
    if (error != nullptr) *error = ErrorText(L"Register thumbnail provider", thumb);
    return false;
  }

  std::wstring register_error;
  if (!RegisterShellVerbs(layout.executable, &register_error, scope)) {
    rollback();
    if (error != nullptr) *error = register_error;
    return false;
  }
  if (!RegisterFileAssociations(layout.executable, associate_default,
                                &register_error, warning, scope)) {
    rollback();
    if (error != nullptr) *error = register_error;
    return false;
  }

  const HKEY root = RootKey(scope);
  if (!WriteInstallMetadata(root, layout, scope) ||
      !WriteUninstallEntry(root, layout, scope)) {
    rollback();
    if (error != nullptr) {
      *error = L"Could not write installation metadata (registry write denied).";
    }
    return false;
  }

  if (create_shortcut && !CreateStartMenuShortcut(layout, &failure, scope)) {
    rollback();
    if (error != nullptr) *error = failure;
    return false;
  }

  return true;
}

bool UninstallApplication(RegistryScope scope,
                          const std::filesystem::path& install_dir,
                          std::wstring* error) {
  if (error != nullptr) error->clear();
  if (scope == RegistryScope::kMachine && !IsProcessElevated()) {
    if (error != nullptr) {
      *error = L"Uninstalling for all users requires administrator rights. "
               L"Run the uninstaller as administrator.";
    }
    return false;
  }

  std::wstring ignored;
  (void)nv::shell::UnregisterThumbnailProvider(scope);
  (void)UnregisterShellVerbs(&ignored, scope);
  (void)UnregisterFileAssociations(&ignored, scope);
  (void)RemoveStartMenuShortcut(&ignored, scope);

  // Resolve the directory before deleting the metadata that records it.
  std::filesystem::path directory = install_dir;
  if (directory.empty()) {
    InstallInfo info;
    if (QueryInstallation(scope, &info)) directory = info.install_dir;
  }
  RemoveInstallMetadata(RootKey(scope));
  // Complete uninstall: drop the whole per-app key tree (associations backup
  // included) and the settings/cache/logs directory as well.
  (void)DeleteTree(RootKey(scope), L"Software\\ZoreView");
  const std::filesystem::path data = AppDataDir();
  if (!data.empty() && (directory.empty() || !DirectoryContains(directory, data))) {
    std::error_code data_ec;
    std::filesystem::remove_all(data, data_ec);
  }
  if (directory.empty()) {
    if (error != nullptr) *error = L"ZoreView is not installed.";
    return false;
  }

  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

  if (DirectoryContains(directory, CurrentExecutablePath())) {
    return ScheduleDirectoryRemoval(directory, error);
  }
  return RemoveDirectoryWithRetry(directory, error);
}

bool QueryInstallation(RegistryScope scope, InstallInfo* info) {
  if (info == nullptr) return false;
  *info = InstallInfo{};
  info->scope = scope;

  const HKEY root = RootKey(scope);
  std::wstring directory;
  bool exists = false;
  if (FAILED(ReadString(root, kInstallKey, L"InstallDir", &directory,
                        &exists)) ||
      !exists || directory.empty()) {
    return false;
  }
  info->install_dir = directory;
  const std::filesystem::path executable =
      std::filesystem::path(directory) / L"zoreview.exe";
  info->installed = std::filesystem::is_regular_file(executable);
  std::wstring version;
  if (SUCCEEDED(ReadString(root, kInstallKey, L"Version", &version, &exists)) &&
      exists) {
    info->version = version;
  }
  return true;
}

}  // namespace nv::platform
