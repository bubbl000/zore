#include "platform/paths.h"

#include <windows.h>

#include <iterator>
#include <string>

namespace nv::platform {
namespace {

std::filesystem::path DetermineExecutableDirectory() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return std::filesystem::path();
    // GetModuleFileNameW truncates by returning the buffer size; grow until
    // the terminating null fits (long paths up to 32767 characters).
    if (length < buffer.size()) {
      buffer.resize(length);
      std::filesystem::path path(buffer);
      if (path.has_filename()) path = path.parent_path();
      return path;
    }
    if (buffer.size() >= 32768) return std::filesystem::path();
    buffer.resize(buffer.size() * 2);
  }
}

const std::filesystem::path& CachedExecutableDirectory() {
  static const std::filesystem::path directory =
      DetermineExecutableDirectory();
  return directory;
}

bool g_portable_forced = false;
bool g_portable_marker_resolved = false;
bool g_portable_marker = false;

}  // namespace

const std::filesystem::path& ExecutableDirectory() {
  return CachedExecutableDirectory();
}

std::filesystem::path AppDataDir() {
  wchar_t buffer[MAX_PATH * 2]{};
  const DWORD length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
  if (length > 0 && length < std::size(buffer)) {
    return std::filesystem::path(buffer) / L"ZoreView";
  }
  return std::filesystem::path(L".") / L"ZoreView";
}

bool HasPortableMarker(const std::filesystem::path& directory) {
  if (directory.empty()) return false;
  std::error_code ec;
  const std::filesystem::path marker = directory / L"ZoreView.portable";
  return std::filesystem::is_regular_file(marker, ec);
}

bool IsPortableMode() {
  if (g_portable_forced) return true;
  if (!g_portable_marker_resolved) {
    g_portable_marker = HasPortableMarker(ExecutableDirectory());
    g_portable_marker_resolved = true;
  }
  return g_portable_marker;
}

void SetPortableMode(bool portable) {
  g_portable_forced = portable;
  // Re-detect the marker after clearing the override so tests (and a caller
  // that switches back) observe the real filesystem state.
  g_portable_marker_resolved = false;
}

std::filesystem::path DataDir() {
  if (IsPortableMode()) {
    const std::filesystem::path& directory = ExecutableDirectory();
    if (!directory.empty()) return directory;
  }
  return AppDataDir();
}

}  // namespace nv::platform
