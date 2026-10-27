#include "shell/thumbnail_registry.h"

#include "core/supported_extensions.h"
#include "shell/thumbnail_provider.h"

#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <string.h>
#include <string>
#include <vector>

namespace nv::shell {
namespace {

constexpr wchar_t kClassesRoot[] = L"Software\\Classes\\";
constexpr wchar_t kBackupRoot[] =
    L"Software\\ZoreView\\ShellThumbnail\\Backup\\";
constexpr wchar_t kBackupOwner[] = L"Software\\ZoreView\\ShellThumbnail";
constexpr wchar_t kZoreViewRoot[] = L"Software\\ZoreView";
constexpr wchar_t kApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";

constexpr wchar_t kHadPreviousValue[] = L"HadPrevious";
constexpr wchar_t kCreatedKeyValue[] = L"CreatedKey";
constexpr wchar_t kPreviousValue[] = L"Previous";

std::wstring ClassesKey(const std::wstring& extension) {
  return std::wstring(kClassesRoot) + extension;
}

std::wstring ShellExKey(const std::wstring& extension) {
  return ClassesKey(extension) + L"\\ShellEx\\" +
         kThumbnailHandlerShellExGuidText;
}

std::wstring BackupKey(const std::wstring& extension) {
  return std::wstring(kBackupRoot) + extension;
}

std::wstring ClsidKey() {
  return std::wstring(kClassesRoot) + L"CLSID\\" +
         kThumbnailProviderClsidText;
}

HRESULT ReadStringValue(HKEY root, const std::wstring& subkey,
                        const wchar_t* name, std::wstring* value,
                        bool* exists) {
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

HRESULT WriteStringValue(HKEY root, const std::wstring& subkey,
                         const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  LSTATUS status =
      RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  const DWORD bytes =
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  status = RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), bytes);
  RegCloseKey(key);
  return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT WriteDwordValue(HKEY root, const std::wstring& subkey,
                        const wchar_t* name, DWORD value) {
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

HRESULT ReadDwordValue(HKEY root, const std::wstring& subkey,
                       const wchar_t* name, DWORD* value, bool* exists) {
  *exists = false;
  DWORD data = 0;
  DWORD bytes = static_cast<DWORD>(sizeof(data));
  const LSTATUS status =
      RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_DWORD, nullptr,
                   &data, &bytes);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return S_OK;
  }
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  *value = data;
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

HRESULT DeleteValue(HKEY root, const std::wstring& subkey,
                    const wchar_t* name) {
  HKEY key = nullptr;
  LSTATUS status =
      RegOpenKeyExW(root, subkey.c_str(), 0, KEY_SET_VALUE, &key);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return S_OK;
  }
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  status = RegDeleteValueW(key, name);
  RegCloseKey(key);
  if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) return S_OK;
  return HRESULT_FROM_WIN32(status);
}

bool KeyExists(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  const LSTATUS status =
      RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key);
  if (status != ERROR_SUCCESS) return false;
  RegCloseKey(key);
  return true;
}

bool KeyIsEmpty(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD subkeys = 0;
  DWORD values = 0;
  const LSTATUS status =
      RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                       &values, nullptr, nullptr, nullptr, nullptr);
  RegCloseKey(key);
  return status == ERROR_SUCCESS && subkeys == 0 && values == 0;
}

void DeleteEmptyKey(HKEY root, const std::wstring& subkey) {
  if (KeyIsEmpty(root, subkey)) {
    RegDeleteKeyW(root, subkey.c_str());
  }
}

void NotifyShell() {
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

HRESULT WriteClsid(HKEY root, const std::wstring& module_path,
                   nv::platform::RegistryScope scope) {
  HRESULT hr = WriteStringValue(root, ClsidKey(), nullptr,
                                kThumbnailProviderName);
  if (FAILED(hr)) return hr;
  hr = WriteStringValue(root, ClsidKey() + L"\\InprocServer32", nullptr,
                        module_path);
  if (FAILED(hr)) return hr;
  hr = WriteStringValue(root, ClsidKey() + L"\\InprocServer32",
                        L"ThreadingModel", L"Both");
  if (FAILED(hr)) return hr;
  if (scope == nv::platform::RegistryScope::kUser) {
    // Windows 11's isolated thumbnail host (dllhost.exe) does not resolve
    // per-user (HKCU) in-proc server CLSIDs; without this flag Explorer shows
    // the generic icon even though the registration is present. Running the
    // handler in-process keeps the portable/per-user install working. The
    // machine-wide installer registers the CLSID under HKLM and must NOT set
    // this value (isolation stays on there).
    hr = WriteDwordValue(root, ClsidKey(), L"DisableProcessIsolation", 1);
    if (FAILED(hr)) return hr;
  } else {
    // A stale value from an earlier per-user registration would otherwise be
    // inherited by the machine-wide CLSID when the same key is reused.
    (void)DeleteValue(root, ClsidKey(), L"DisableProcessIsolation");
  }
  // Some Windows builds only load shell extensions that are listed as
  // approved. The per-user entry is ignored on builds that do not need it.
  return WriteStringValue(root, kApprovedKey, kThumbnailProviderClsidText,
                          kThumbnailProviderName);
}

// Saves the handler that was registered before us so uninstall can restore it
// instead of leaving the user with a dead CLSID.
HRESULT BackupExistingHandler(HKEY root, const std::wstring& extension,
                              const std::wstring& backup,
                              bool extension_key_existed) {
  std::wstring previous;
  bool has_previous = false;
  HRESULT hr = ReadStringValue(root, ShellExKey(extension), nullptr, &previous,
                               &has_previous);
  if (FAILED(hr)) return hr;

  const bool ours = has_previous && previous == kThumbnailProviderClsidText;
  const bool restore = has_previous && !ours && !previous.empty();

  hr = WriteDwordValue(root, backup, kCreatedKeyValue,
                       extension_key_existed ? 0u : 1u);
  if (FAILED(hr)) return hr;
  hr = WriteDwordValue(root, backup, kHadPreviousValue, restore ? 1u : 0u);
  if (FAILED(hr)) return hr;
  if (restore) {
    hr = WriteStringValue(root, backup, kPreviousValue, previous);
  }
  return hr;
}

HRESULT RestoreExtension(HKEY root, const std::wstring& extension) {
  const std::wstring backup = BackupKey(extension);
  DWORD had_previous = 0;
  DWORD created_key = 0;
  bool exists = false;
  (void)ReadDwordValue(root, backup, kHadPreviousValue, &had_previous, &exists);
  exists = false;
  (void)ReadDwordValue(root, backup, kCreatedKeyValue, &created_key, &exists);

  std::wstring previous;
  bool has_previous = false;
  (void)ReadStringValue(root, backup, kPreviousValue, &previous, &has_previous);

  HRESULT first_error = S_OK;
  HRESULT hr = S_OK;
  if (had_previous != 0 && has_previous) {
    hr = WriteStringValue(root, ShellExKey(extension), nullptr, previous);
  } else {
    // Deleting the {handler} subkey removes the ShellEx value and nothing else.
    hr = DeleteTree(root, ShellExKey(extension));
  }
  if (FAILED(hr)) first_error = hr;

  // Remove the now-empty ShellEx key even when the file type already existed:
  // leaving an empty ShellEx adds noise to the association tree. Deleting an
  // empty .ext key is harmless because it holds neither values nor subkeys.
  (void)created_key;
  const std::wstring shell_ex = ClassesKey(extension) + L"\\ShellEx";
  DeleteEmptyKey(root, shell_ex);
  DeleteEmptyKey(root, ClassesKey(extension));
  (void)DeleteTree(root, backup);
  return first_error;
}

}  // namespace

const std::vector<std::wstring>& ThumbnailExtensions() {
  // One list for every Shell integration surface; see core/supported_extensions.
  return nv::SupportedExtensions();
}

HRESULT RegisterThumbnailProvider(
    const std::wstring& module_path, nv::platform::RegistryScope scope) {
  if (module_path.empty()) return E_INVALIDARG;

  const HKEY root = nv::platform::RootKey(scope);
  HRESULT first_error = WriteClsid(root, module_path, scope);
  bool wrote_any = SUCCEEDED(first_error);

  for (const std::wstring& extension : ThumbnailExtensions()) {
    const bool extension_key_existed = KeyExists(root, ClassesKey(extension));
    HRESULT hr = BackupExistingHandler(root, extension,
                                       BackupKey(extension),
                                       extension_key_existed);
    if (SUCCEEDED(hr)) {
      hr = WriteStringValue(root, ShellExKey(extension), nullptr,
                            kThumbnailProviderClsidText);
    }
    if (FAILED(hr)) {
      if (SUCCEEDED(first_error)) first_error = hr;
    } else {
      wrote_any = true;
    }
  }

  if (wrote_any) NotifyShell();
  return first_error;
}

HRESULT UnregisterThumbnailProvider(nv::platform::RegistryScope scope) {
  const HKEY root = nv::platform::RootKey(scope);
  HRESULT first_error = S_OK;
  for (const std::wstring& extension : ThumbnailExtensions()) {
    const HRESULT hr = RestoreExtension(root, extension);
    if (FAILED(hr) && SUCCEEDED(first_error)) first_error = hr;
  }

  (void)DeleteTree(root, kBackupRoot);
  DeleteEmptyKey(root, kBackupOwner);
  DeleteEmptyKey(root, kZoreViewRoot);

  const HRESULT hr = DeleteTree(root, ClsidKey());
  if (FAILED(hr) && SUCCEEDED(first_error)) first_error = hr;
  const HRESULT approved =
      DeleteValue(root, kApprovedKey, kThumbnailProviderClsidText);
  if (FAILED(approved) && SUCCEEDED(first_error)) first_error = approved;

  NotifyShell();
  return first_error;
}

bool IsThumbnailProviderRegistered(const std::wstring& module_path,
                                   nv::platform::RegistryScope scope) {
  const HKEY root = nv::platform::RootKey(scope);
  std::wstring registered;
  bool exists = false;
  if (FAILED(ReadStringValue(root, ClsidKey() + L"\\InprocServer32", nullptr,
                             &registered, &exists)) ||
      !exists) {
    return false;
  }
  if (_wcsicmp(registered.c_str(), module_path.c_str()) != 0) return false;

  DWORD isolated = 0;
  bool has_flag = false;
  if (FAILED(ReadDwordValue(root, ClsidKey(), L"DisableProcessIsolation",
                            &isolated, &has_flag))) {
    return false;
  }
  if (scope == nv::platform::RegistryScope::kUser) {
    // Per-user installs need in-process activation on Windows 11; a missing
    // flag means Explorer will silently fall back to the generic icon.
    if (!has_flag || isolated != 1) return false;
  } else if (has_flag && isolated == 1) {
    // Machine-wide installs must keep the isolated thumbnail host.
    return false;
  }

  for (const std::wstring& extension : ThumbnailExtensions()) {
    std::wstring handler;
    bool has_handler = false;
    if (FAILED(ReadStringValue(root, ShellExKey(extension), nullptr, &handler,
                               &has_handler)) ||
        !has_handler) {
      return false;
    }
    if (handler != kThumbnailProviderClsidText) return false;
  }
  return true;
}

}  // namespace nv::shell
