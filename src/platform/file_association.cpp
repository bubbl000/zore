#include "platform/file_association.h"

#include "core/supported_extensions.h"
#include "platform/i18n.h"

#include <windows.h>
#include <shlobj.h>

#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

namespace nv::platform {
namespace {

constexpr wchar_t kClassesRoot[] = L"Software\\Classes\\";
constexpr wchar_t kBackupRoot[] = L"Software\\ZoreView\\Associations\\Backup\\";
constexpr wchar_t kAssociationsRoot[] = L"Software\\ZoreView\\Associations";

constexpr wchar_t kHadDefaultValue[] = L"HadDefault";
constexpr wchar_t kPreviousValue[] = L"Previous";
constexpr wchar_t kTakeoverValue[] = L"Takeover";
constexpr wchar_t kHadUserChoiceValue[] = L"HadUserChoice";
constexpr wchar_t kPreviousProgIdValue[] = L"PreviousProgId";
constexpr wchar_t kPreviousHashValue[] = L"PreviousHash";

std::wstring ClassesKey(const std::wstring& extension) {
  return std::wstring(kClassesRoot) + extension;
}

std::wstring ProgIdKey() {
  return std::wstring(kClassesRoot) + kAssociationProgId;
}

std::wstring OpenWithKey(const std::wstring& extension) {
  return ClassesKey(extension) + L"\\OpenWithProgids";
}

std::wstring BackupKey(const std::wstring& extension) {
  return std::wstring(kBackupRoot) + extension;
}

std::wstring UserChoiceKey(const std::wstring& extension) {
  return L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
         extension + L"\\UserChoice";
}

std::wstring CommandLine(const std::filesystem::path& executable) {
  std::wstring command = L"\"";
  command += executable.wstring();
  command += L"\" \"%1\"";
  return command;
}

std::wstring IconPath(const std::filesystem::path& executable) {
  std::wstring value = L"\"";
  value += executable.wstring();
  value += L"\",0";
  return value;
}

std::wstring ErrorText(const wchar_t* action, HRESULT hr) {
  wchar_t buffer[256]{};
  std::swprintf(buffer, std::size(buffer), L"%s failed (HRESULT 0x%08lX)",
                action, static_cast<unsigned long>(hr));
  return buffer;
}

HRESULT SetDefaultString(HKEY root, const std::wstring& subkey,
                         const std::wstring& value) {
  HKEY key = nullptr;
  LSTATUS status =
      RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  status = RegSetValueExW(
      key, nullptr, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
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

// OpenWithProgids values are conventionally REG_NONE with zero bytes.
HRESULT SetEmptyValue(HKEY root, const std::wstring& subkey,
                      const wchar_t* name) {
  HKEY key = nullptr;
  LSTATUS status =
      RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  status = RegSetValueExW(key, name, 0, REG_NONE, nullptr, 0);
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

HRESULT ReadDefaultString(HKEY root, const std::wstring& subkey,
                          std::wstring* value, bool* exists) {
  *exists = false;
  DWORD bytes = 0;
  LSTATUS status = RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ,
                                nullptr, nullptr, &bytes);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return S_OK;
  }
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  if (bytes < sizeof(wchar_t)) return S_OK;

  std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
  status = RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                        buffer.data(), &bytes);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  buffer.resize(std::wcslen(buffer.c_str()));
  *value = std::move(buffer);
  *exists = true;
  return S_OK;
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

HRESULT ReadDword(HKEY root, const std::wstring& subkey, const wchar_t* name,
                  DWORD* value, bool* exists) {
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

bool ValueExists(HKEY root, const std::wstring& subkey, const wchar_t* name) {
  DWORD bytes = 0;
  const LSTATUS status =
      RegGetValueW(root, subkey.c_str(), name, RRF_RT_ANY, nullptr, nullptr,
                   &bytes);
  return status == ERROR_SUCCESS;
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

HRESULT DeleteDefaultValue(HKEY root, const std::wstring& subkey) {
  return DeleteValue(root, subkey, nullptr);
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

bool IsAssociationExtension(const std::wstring& extension) {
  for (const std::wstring& known : nv::ImageDocumentExtensions()) {
    if (_wcsicmp(known.c_str(), extension.c_str()) == 0) return true;
  }
  return false;
}

HRESULT WriteProgId(HKEY root, const std::filesystem::path& executable) {
  const wchar_t* friendly =
      nv::i18n::Text(nv::i18n::StringId::kAssociationFriendlyName);
  HRESULT hr = SetDefaultString(root, ProgIdKey(), friendly);
  if (FAILED(hr)) return hr;
  hr = SetString(root, ProgIdKey(), L"FriendlyTypeName", friendly);
  if (FAILED(hr)) return hr;
  hr = SetDefaultString(root, ProgIdKey() + L"\\DefaultIcon",
                        IconPath(executable));
  if (FAILED(hr)) return hr;
  hr = SetDefaultString(
      root, ProgIdKey() + L"\\shell\\open",
      nv::i18n::Text(nv::i18n::StringId::kShellVerbOpen));
  if (FAILED(hr)) return hr;
  return SetDefaultString(root, ProgIdKey() + L"\\shell\\open\\command",
                          CommandLine(executable));
}

// Records the current default before taking it over. Keeping the first backup
// matters: register/unregister cycles must not replace the original value with
// our own ProgID.
//
// Windows 10/11 keep the effective pick in Explorer\FileExts\<ext>\UserChoice
// (a hashed ProgID). "Set as default" is an explicit user action, so the
// original ProgId/Hash pair is backed up and the UserChoice key is removed;
// the class default then decides again. Unregister puts the pair back.
HRESULT BackupDefault(HKEY root, const std::wstring& extension) {
  const std::wstring backup = BackupKey(extension);
  if (KeyExists(root, backup)) return S_OK;

  std::wstring previous;
  bool had_default = false;
  HRESULT hr = ReadDefaultString(root, ClassesKey(extension), &previous,
                                 &had_default);
  if (FAILED(hr)) return hr;

  hr = SetDword(root, backup, kHadDefaultValue, had_default ? 1u : 0u);
  if (FAILED(hr)) return hr;
  hr = SetDword(root, backup, kTakeoverValue, 1u);
  if (FAILED(hr)) return hr;
  if (had_default) {
    hr = SetString(root, backup, kPreviousValue, previous);
    if (FAILED(hr)) return hr;
  }

  if (!KeyExists(HKEY_CURRENT_USER, UserChoiceKey(extension))) return S_OK;
  hr = SetDword(root, backup, kHadUserChoiceValue, 1u);
  if (FAILED(hr)) return hr;

  std::wstring prog_id;
  std::wstring hash;
  bool has_prog_id = false;
  bool has_hash = false;
  (void)ReadStringValue(HKEY_CURRENT_USER, UserChoiceKey(extension),
                        L"ProgId", &prog_id, &has_prog_id);
  (void)ReadStringValue(HKEY_CURRENT_USER, UserChoiceKey(extension), L"Hash",
                        &hash, &has_hash);
  if (has_prog_id) {
    hr = SetString(root, backup, kPreviousProgIdValue, prog_id);
    if (FAILED(hr)) return hr;
  }
  if (has_hash) {
    hr = SetString(root, backup, kPreviousHashValue, hash);
  }
  return hr;
}

void RestoreDefault(HKEY root, const std::wstring& extension) {
  const std::wstring backup = BackupKey(extension);
  DWORD had_default = 0;
  DWORD takeover = 0;
  DWORD had_user_choice = 0;
  bool exists = false;
  (void)ReadDword(root, backup, kTakeoverValue, &takeover, &exists);
  if (!exists || takeover == 0) {
    // No takeover record. Still clear a default that points at us, otherwise
    // deleting the ProgID would leave a dangling association behind.
    if (!KeyExists(root, backup)) {
      std::wstring current;
      bool has_current = false;
      (void)ReadDefaultString(root, ClassesKey(extension), &current,
                              &has_current);
      if (has_current && current == kAssociationProgId) {
        (void)DeleteDefaultValue(root, ClassesKey(extension));
      }
    }
    (void)DeleteTree(root, backup);
    return;
  }
  exists = false;
  (void)ReadDword(root, backup, kHadDefaultValue, &had_default, &exists);
  exists = false;
  (void)ReadDword(root, backup, kHadUserChoiceValue, &had_user_choice,
                  &exists);

  std::wstring previous;
  bool has_previous = false;
  (void)ReadStringValue(root, backup, kPreviousValue, &previous, &has_previous);

  // Only undo the takeover when the current default is still ours: another
  // application (or the user) may have changed it afterwards.
  std::wstring current;
  bool has_current = false;
  (void)ReadDefaultString(root, ClassesKey(extension), &current, &has_current);
  if (has_current && current == kAssociationProgId) {
    if (had_default != 0 && has_previous && !previous.empty()) {
      (void)SetDefaultString(root, ClassesKey(extension), previous);
    } else {
      (void)DeleteDefaultValue(root, ClassesKey(extension));
    }
  }

  // Recreate the previous UserChoice only when nothing else has claimed it.
  if (had_user_choice != 0 &&
      !KeyExists(HKEY_CURRENT_USER, UserChoiceKey(extension))) {
    std::wstring prog_id;
    std::wstring hash;
    bool has_prog_id = false;
    bool has_hash = false;
    (void)ReadStringValue(root, backup, kPreviousProgIdValue, &prog_id,
                          &has_prog_id);
    (void)ReadStringValue(root, backup, kPreviousHashValue, &hash, &has_hash);
    if (has_prog_id) {
      (void)SetString(HKEY_CURRENT_USER, UserChoiceKey(extension), L"ProgId",
                      prog_id);
    }
    if (has_hash) {
      (void)SetString(HKEY_CURRENT_USER, UserChoiceKey(extension), L"Hash",
                      hash);
    }
  }
  (void)DeleteTree(root, backup);
}

}  // namespace

int AssociationExtensionCount() {
  return static_cast<int>(nv::ImageDocumentExtensions().size());
}

std::vector<AssociationExtensionState> GetAssociationExtensionStates(
    RegistryScope scope) {
  const HKEY root = RootKey(scope);
  std::vector<AssociationExtensionState> states;
  for (const std::wstring& extension : nv::ImageDocumentExtensions()) {
    AssociationExtensionState state;
    state.extension = extension;
    state.open_with = ValueExists(root, OpenWithKey(extension),
                                  kAssociationProgId);
    std::wstring current;
    bool has_current = false;
    (void)ReadDefaultString(root, ClassesKey(extension), &current,
                            &has_current);
    state.is_default = has_current && current == kAssociationProgId;
    states.push_back(std::move(state));
  }
  return states;
}

bool UpdateOpenWithForExtensions(const std::filesystem::path& executable,
                                 const std::vector<std::wstring>& extensions,
                                 bool enable, std::wstring* error,
                                 RegistryScope scope) {
  if (error != nullptr) error->clear();
  if (executable.empty()) {
    if (error != nullptr) *error = L"The executable path is empty.";
    return false;
  }

  const HKEY root = RootKey(scope);
  std::wstring first_error;
  if (enable) {
    const HRESULT prog_id = WriteProgId(root, executable);
    if (FAILED(prog_id)) {
      if (error != nullptr) *error = ErrorText(L"Write association ProgID", prog_id);
      return false;
    }
  }

  for (const std::wstring& extension : extensions) {
    if (!IsAssociationExtension(extension)) continue;
    HRESULT hr = S_OK;
    if (enable) {
      hr = SetEmptyValue(root, OpenWithKey(extension), kAssociationProgId);
    } else {
      RestoreDefault(root, extension);
      hr = DeleteValue(root, OpenWithKey(extension), kAssociationProgId);
      DeleteEmptyKey(root, OpenWithKey(extension));
    }
    if (FAILED(hr) && first_error.empty()) {
      first_error = ErrorText(enable ? L"Write OpenWith entry"
                                     : L"Delete OpenWith entry",
                              hr);
    }
  }

  NotifyShell();
  if (!first_error.empty()) {
    if (error != nullptr) *error = std::move(first_error);
    return false;
  }
  return true;
}

bool RegisterFileAssociations(const std::filesystem::path& executable,
                              bool make_default, std::wstring* error,
                              std::wstring* warning, RegistryScope scope) {
  if (error != nullptr) error->clear();
  if (warning != nullptr) warning->clear();
  if (executable.empty()) {
    if (error != nullptr) *error = L"The executable path is empty.";
    return false;
  }

  const HKEY root = RootKey(scope);
  const HRESULT prog_id = WriteProgId(root, executable);

  std::wstring first_error;
  if (FAILED(prog_id)) {
    first_error = ErrorText(L"Write association ProgID", prog_id);
  }

  int skipped_defaults = 0;
  bool wrote_any = SUCCEEDED(prog_id);
  for (const std::wstring& extension : nv::ImageDocumentExtensions()) {
    HRESULT hr = SetEmptyValue(root, OpenWithKey(extension), kAssociationProgId);
    if (FAILED(hr)) {
      if (first_error.empty()) {
        first_error = ErrorText(L"Write OpenWith entry", hr);
      }
      continue;
    }
    wrote_any = true;

    if (!make_default) continue;
    hr = BackupDefault(root, extension);
    if (SUCCEEDED(hr)) {
      // Windows 10/11 keep the effective pick in a hashed UserChoice value.
      // Removing it lets the class default decide again; the previous
      // ProgId/Hash pair was recorded by BackupDefault and is restored by
      // UnregisterFileAssociations.
      const HRESULT removed =
          DeleteTree(HKEY_CURRENT_USER, UserChoiceKey(extension));
      if (FAILED(removed)) {
        ++skipped_defaults;
        continue;
      }
      hr = SetDefaultString(root, ClassesKey(extension), kAssociationProgId);
    }
    if (FAILED(hr) && first_error.empty()) {
      first_error = ErrorText(L"Write default association", hr);
    }
  }

  if (make_default && skipped_defaults > 0 && warning != nullptr) {
    wchar_t buffer[512]{};
    std::swprintf(
        buffer, std::size(buffer),
        nv::i18n::Text(nv::i18n::StringId::kWarningUserChoiceFormat),
        static_cast<size_t>(skipped_defaults));
    *warning = buffer;
  }

  if (wrote_any) NotifyShell();
  if (!first_error.empty()) {
    if (error != nullptr) *error = std::move(first_error);
    return false;
  }
  return true;
}

bool UnregisterFileAssociations(std::wstring* error, RegistryScope scope) {
  if (error != nullptr) error->clear();
  const HKEY root = RootKey(scope);
  std::wstring first_error;

  for (const std::wstring& extension : nv::ImageDocumentExtensions()) {
    RestoreDefault(root, extension);
    const HRESULT hr =
        DeleteValue(root, OpenWithKey(extension), kAssociationProgId);
    if (FAILED(hr) && first_error.empty()) {
      first_error = ErrorText(L"Delete OpenWith entry", hr);
    }
    DeleteEmptyKey(root, OpenWithKey(extension));
    DeleteEmptyKey(root, ClassesKey(extension));
  }

  const HRESULT prog_id = DeleteTree(root, ProgIdKey());
  if (FAILED(prog_id) && first_error.empty()) {
    first_error = ErrorText(L"Delete association ProgID", prog_id);
  }
  (void)DeleteTree(root, kBackupRoot);
  DeleteEmptyKey(root, kAssociationsRoot);

  NotifyShell();
  if (!first_error.empty()) {
    if (error != nullptr) *error = std::move(first_error);
    return false;
  }
  return true;
}

bool AreFileAssociationsRegistered(const std::filesystem::path& executable,
                                   AssociationStatus* status,
                                   RegistryScope scope) {
  AssociationStatus local;
  if (status == nullptr) status = &local;
  *status = AssociationStatus{};
  if (executable.empty()) return false;

  const HKEY root = RootKey(scope);
  std::wstring command;
  bool has_command = false;
  if (FAILED(ReadDefaultString(root, ProgIdKey() + L"\\shell\\open\\command",
                               &command, &has_command)) ||
      !has_command) {
    return false;
  }
  if (_wcsicmp(command.c_str(), CommandLine(executable).c_str()) != 0) {
    return false;
  }
  status->prog_id_registered = true;

  for (const std::wstring& extension : nv::ImageDocumentExtensions()) {
    if (!ValueExists(root, OpenWithKey(extension), kAssociationProgId)) {
      return false;
    }
    ++status->open_with_count;

    std::wstring current;
    bool has_current = false;
    if (SUCCEEDED(ReadDefaultString(root, ClassesKey(extension), &current,
                                    &has_current)) &&
        has_current && current == kAssociationProgId) {
      ++status->default_count;
    }
  }
  status->extension_count = AssociationExtensionCount();
  return status->open_with_count == status->extension_count;
}

}  // namespace nv::platform
