#include "core/folder_watcher.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace nv {
namespace {

std::wstring LowerKey(const std::filesystem::path& path) {
  std::wstring text = path.lexically_normal().wstring();
  std::ranges::transform(text, text.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  return text;
}

uint64_t FileTimeValue(const FILETIME& time) {
  ULARGE_INTEGER value{};
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return value.QuadPart;
}

bool ReadAttributes(const std::filesystem::path& path,
                    WIN32_FILE_ATTRIBUTE_DATA* data) {
  if (path.empty() || data == nullptr) return false;
  return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, data) != FALSE;
}

FileSnapshot ReadFile(const std::filesystem::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!ReadAttributes(path, &data)) return {};
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return {};
  FileSnapshot snapshot;
  snapshot.exists = true;
  snapshot.size =
      (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  snapshot.write_time = FileTimeValue(data.ftLastWriteTime);
  return snapshot;
}

}  // namespace

FolderSnapshot TakeFolderSnapshot(const std::filesystem::path& folder,
                                  const std::filesystem::path& current,
                                  bool include_file_count) {
  FolderSnapshot snapshot;
  snapshot.folder_key = LowerKey(folder);
  snapshot.current_key = current.empty() ? std::wstring() : LowerKey(current);

  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (ReadAttributes(folder, &data) &&
      (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    snapshot.dir_write_time = FileTimeValue(data.ftLastWriteTime);
  }
  if (!current.empty()) snapshot.current = ReadFile(current);

  if (include_file_count) {
    std::error_code ec;
    std::filesystem::directory_iterator it(
        folder, std::filesystem::directory_options::skip_permission_denied, ec);
    if (!ec) {
      size_t count = 0;
      for (const auto& entry : it) {
        std::error_code entry_ec;
        if (entry.is_regular_file(entry_ec)) ++count;
      }
      snapshot.file_count = count;
    }
  }
  return snapshot;
}

bool IsRemotePath(const std::filesystem::path& folder) {
  if (folder.empty()) return false;
  const std::wstring& text = folder.native();
  if (text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\') return true;
  const std::wstring root = folder.root_name().wstring();
  if (root.size() != 2 || root[1] != L':') return false;
  return GetDriveTypeW((root + L"\\").c_str()) == DRIVE_REMOTE;
}

void FolderWatcher::Reset(const FolderSnapshot& snapshot, double now_ms) {
  (void)now_ms;
  last_ = snapshot;
  initialized_ = true;
  rescan_deferred_ = false;
  // A baseline is not a rescan: the first genuine change after it must be
  // reported immediately, even when it happens inside the 2 s debounce window.
  last_rescan_ms_ = -1.0e9;
}

void FolderWatcher::Suppress(double now_ms, double duration_ms) {
  suppress_until_ms_ = std::max(suppress_until_ms_, now_ms + duration_ms);
}

WatchEvent FolderWatcher::Update(const FolderSnapshot& snapshot,
                                 double now_ms) {
  rescan_deferred_ = false;
  if (!initialized_) {
    Reset(snapshot, now_ms);
    return {};
  }

  // Navigation (or the app rescanning on its own) changes these keys; adopt the
  // new baseline silently instead of reporting the new file as "modified".
  if (snapshot.folder_key != last_.folder_key ||
      snapshot.current_key != last_.current_key) {
    last_ = snapshot;
    return {};
  }

  if (suppressed(now_ms)) {
    last_ = snapshot;
    return {};
  }

  bool dir_changed = snapshot.dir_write_time != last_.dir_write_time;
  if (snapshot.file_count.has_value() && last_.file_count.has_value() &&
      *snapshot.file_count != *last_.file_count) {
    dir_changed = true;
  }

  const bool current_removed =
      last_.current.exists && !snapshot.current.exists;
  const bool current_modified =
      snapshot.current.exists && last_.current.exists &&
      snapshot.current != last_.current;

  if (current_removed || dir_changed) {
    // Antivirus/sync clients can touch a directory several times in a row: a
    // 2 s minimum between rescans keeps the list stable (坑 6). The snapshot is
    // not adopted here so the change is retried on the next poll instead of
    // being silently lost.
    if (now_ms - last_rescan_ms_ < kMinRescanMs) {
      rescan_deferred_ = true;
      return {};
    }
    last_rescan_ms_ = now_ms;
    last_ = snapshot;
    return {current_removed ? WatchEventType::kCurrentRemoved
                            : WatchEventType::kFolderChanged};
  }

  if (current_modified) {
    // Content edits do not change the directory timestamp, so this branch is
    // the only way the "file changed, press F5" toast can ever fire (坑 3).
    last_ = snapshot;
    return {WatchEventType::kCurrentModified};
  }

  last_ = snapshot;
  return {};
}

}  // namespace nv
