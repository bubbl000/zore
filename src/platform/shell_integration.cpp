#include "platform/shell_integration.h"

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

constexpr wchar_t kSystemFileAssociations[] =
    L"Software\\Classes\\SystemFileAssociations\\";
constexpr wchar_t kShell[] = L"\\shell\\";

constexpr wchar_t kOpenVerb[] = L"ZoreView.Open";
constexpr wchar_t kWallpaperVerb[] = L"ZoreView.SetWallpaper";
constexpr wchar_t kCopyVerb[] = L"ZoreView.CopyImage";

std::wstring VerbKey(const std::wstring& extension, const wchar_t* verb) {
  return std::wstring(kSystemFileAssociations) + extension + kShell + verb;
}

std::wstring CommandLine(const std::filesystem::path& executable,
                         const wchar_t* arguments) {
  std::wstring command = L"\"";
  command += executable.wstring();
  command += L"\" ";
  command += arguments;
  return command;
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

HRESULT DeleteTree(HKEY root, const std::wstring& subkey) {
  const LSTATUS status = RegDeleteTreeW(root, subkey.c_str());
  if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
      status == ERROR_PATH_NOT_FOUND) {
    return S_OK;
  }
  return HRESULT_FROM_WIN32(status);
}

bool KeyIsEmpty(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD subkeys = 0;
  DWORD values = 0;
  const LSTATUS status =
      RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr,
                       nullptr, &values, nullptr, nullptr, nullptr, nullptr);
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

struct VerbDefinition {
  const wchar_t* key;
  nv::i18n::StringId label;
  const wchar_t* arguments;
};

const VerbDefinition kVerbs[] = {
    {kOpenVerb, nv::i18n::StringId::kShellVerbOpen, L"\"%1\""},
    {kWallpaperVerb, nv::i18n::StringId::kShellVerbWallpaper,
     L"--set-wallpaper \"%1\""},
    {kCopyVerb, nv::i18n::StringId::kShellVerbCopy, L"--copy-image \"%1\""},
};

}  // namespace

int ShellVerbExtensionCount() {
  return static_cast<int>(nv::ImageDocumentExtensions().size());
}

bool RegisterShellVerbs(const std::filesystem::path& executable,
                        std::wstring* error, RegistryScope scope) {
  if (error != nullptr) error->clear();
  if (executable.empty()) {
    if (error != nullptr) *error = L"The executable path is empty.";
    return false;
  }

  const HKEY root = RootKey(scope);
  bool wrote_any = false;
  std::wstring first_error;
  for (const std::wstring& extension : nv::ImageDocumentExtensions()) {
    for (const VerbDefinition& verb : kVerbs) {
      const std::wstring key = VerbKey(extension, verb.key);
      HRESULT hr = SetDefaultString(root, key, nv::i18n::Text(verb.label));
      if (SUCCEEDED(hr)) {
        hr = SetDefaultString(root, key + L"\\command",
                              CommandLine(executable, verb.arguments));
      }
      if (SUCCEEDED(hr)) {
        hr = SetString(root, key, L"MultiSelectModel", L"Single");
      }
      if (FAILED(hr)) {
        if (first_error.empty()) {
          first_error = ErrorText(L"Write context-menu verb", hr);
        }
      } else {
        wrote_any = true;
      }
    }
  }

  if (wrote_any) NotifyShell();
  if (!first_error.empty()) {
    if (error != nullptr) *error = std::move(first_error);
    return false;
  }
  return true;
}

bool UnregisterShellVerbs(std::wstring* error, RegistryScope scope) {
  if (error != nullptr) error->clear();
  const HKEY root = RootKey(scope);
  std::wstring first_error;

  for (const std::wstring& extension : nv::ImageDocumentExtensions()) {
    for (const VerbDefinition& verb : kVerbs) {
      const std::wstring key = VerbKey(extension, verb.key);
      const HRESULT hr = DeleteTree(root, key);
      if (FAILED(hr) && first_error.empty()) {
        first_error = ErrorText(L"Delete context-menu verb", hr);
      }
    }
    const std::wstring shell = std::wstring(kSystemFileAssociations) +
                               extension + L"\\shell";
    DeleteEmptyKey(root, shell);
    DeleteEmptyKey(root, std::wstring(kSystemFileAssociations) + extension);
  }

  NotifyShell();
  if (!first_error.empty()) {
    if (error != nullptr) *error = std::move(first_error);
    return false;
  }
  return true;
}

bool AreShellVerbsRegistered(const std::filesystem::path& executable,
                             int* extension_count, RegistryScope scope) {
  if (extension_count != nullptr) *extension_count = 0;
  if (executable.empty()) return false;

  const HKEY root = RootKey(scope);
  int checked = 0;
  for (const std::wstring& extension : nv::ImageDocumentExtensions()) {
    for (const VerbDefinition& verb : kVerbs) {
      std::wstring command;
      bool exists = false;
      if (FAILED(ReadDefaultString(root, VerbKey(extension, verb.key) +
                                              L"\\command",
                                   &command, &exists)) ||
          !exists) {
        return false;
      }
      const std::wstring expected = CommandLine(executable, verb.arguments);
      if (_wcsicmp(command.c_str(), expected.c_str()) != 0) return false;
    }
    ++checked;
  }
  if (extension_count != nullptr) *extension_count = checked;
  return checked > 0;
}

}  // namespace nv::platform
