#include "platform/file_ops.h"

#include "platform/i18n.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace nv::platform {
namespace {

void SetWin32Error(std::wstring* error, const wchar_t* step, DWORD code) {
  if (error == nullptr) return;
  wchar_t buffer[256]{};
  swprintf_s(buffer, L"%s failed (error %lu)", step,
             static_cast<unsigned long>(code));
  *error = buffer;
}

void SetShellError(std::wstring* error, const wchar_t* step, int code) {
  if (error == nullptr) return;
  wchar_t buffer[256]{};
  swprintf_s(buffer, L"%s failed (Shell error %d)", step, code);
  *error = buffer;
}

}  // namespace

bool DeleteToRecycleBin(const std::filesystem::path& path, std::wstring* error) {
  if (error != nullptr) error->clear();
  if (path.empty()) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorPathEmpty);
    }
    return false;
  }

  std::wstring from = path.wstring();
  from.push_back(L'\0');
  from.push_back(L'\0');

  // FOF_ALLOWUNDO is not a guarantee on network shares; refuse instead of
  // risking a permanent delete outside the user's expectations.
  const std::wstring root = path.root_path().wstring();
  if (!root.empty()) {
    const UINT drive_type = GetDriveTypeW(root.c_str());
    if (drive_type == DRIVE_REMOTE || drive_type == DRIVE_NO_ROOT_DIR) {
      if (error != nullptr) {
        *error = nv::i18n::Text(nv::i18n::StringId::kErrorLocalDiskOnly);
      }
      return false;
    }
  }

  SHFILEOPSTRUCTW operation{};
  operation.wFunc = FO_DELETE;
  operation.pFrom = from.c_str();
  operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

  const int result = SHFileOperationW(&operation);
  if (result != 0 || operation.fAnyOperationsAborted) {
    SetShellError(error, L"Delete to Recycle Bin", result);
    return false;
  }
  return true;
}

bool RenamePath(const std::filesystem::path& from, const std::filesystem::path& to,
                std::wstring* error) {
  if (error != nullptr) error->clear();
  if (from.empty() || to.empty()) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorPathEmpty);
    }
    return false;
  }

  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  const DWORD first_error = GetLastError();

  // Windows cannot rename only the casing of a path in one step. Go through a
  // temporary sibling so "photo.jpg" -> "Photo.jpg" still works.
  if (_wcsicmp(from.c_str(), to.c_str()) == 0) {
    std::filesystem::path temporary = from;
    temporary += L".renaming";
    if (MoveFileExW(from.c_str(), temporary.c_str(), 0)) {
      if (MoveFileExW(temporary.c_str(), to.c_str(), 0)) return true;
      const DWORD second_error = GetLastError();
      MoveFileExW(temporary.c_str(), from.c_str(), 0);
      SetWin32Error(error, L"Rename", second_error);
      return false;
    }
  }

  SetWin32Error(error, L"Rename", first_error);
  return false;
}

bool SetDesktopWallpaper(const std::filesystem::path& path, std::wstring* error) {
  if (error != nullptr) error->clear();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    if (error != nullptr) {
      *error = nv::i18n::Text(nv::i18n::StringId::kErrorWallpaperMissing);
    }
    return false;
  }
  if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0,
                             const_cast<wchar_t*>(path.c_str()),
                             SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
    SetWin32Error(error, L"Set desktop wallpaper", GetLastError());
    return false;
  }
  return true;
}

bool RevealInExplorer(const std::filesystem::path& path, std::wstring* error) {
  if (error != nullptr) error->clear();
  std::wstring arguments = L"/select,\"";
  arguments += path.wstring();
  arguments += L"\"";
  const HINSTANCE result =
      ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(),
                    nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    SetWin32Error(error, L"Open File Explorer", GetLastError());
    return false;
  }
  return true;
}

}  // namespace nv::platform
