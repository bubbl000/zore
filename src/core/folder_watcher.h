#pragma once

// v2.0 S2 / 22/10: foreground folder polling (no ReadDirectoryChangesW).
//
// The state machine is deliberately free of window/timer code: the app takes a
// snapshot every 2 s and feeds it to FolderWatcher, which decides whether the
// change is new and worth a rescan. That keeps the debounce/suppression rules
// unit-testable without sleeping or touching a real window.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace nv {

struct FileSnapshot {
  bool exists = false;
  uint64_t size = 0;
  // 100 ns units from FILETIME, so two writes in the same second still differ.
  uint64_t write_time = 0;

  bool operator==(const FileSnapshot& other) const {
    return exists == other.exists && size == other.size &&
           write_time == other.write_time;
  }
};

struct FolderSnapshot {
  // Lower-case normalised paths; a change means the user navigated, which must
  // silently re-baseline instead of looking like an external modification.
  std::wstring folder_key;
  std::wstring current_key;
  uint64_t dir_write_time = 0;
  // Nullopt means "not sampled this poll" (network drives and the count
  // down-sampling described in 22/10 §3.1).
  std::optional<size_t> file_count;
  FileSnapshot current;
};

// Reads the folder's directory timestamp plus the open file's size/mtime.
// Missing entries are reported as `exists = false`, never as a hard failure.
FolderSnapshot TakeFolderSnapshot(const std::filesystem::path& folder,
                                  const std::filesystem::path& current,
                                  bool include_file_count);

// True for UNC paths and mapped network drives: those skip the directory count
// because enumerating a large remote folder every poll is too slow (坑 5).
bool IsRemotePath(const std::filesystem::path& folder);

enum class WatchEventType {
  kNone = 0,
  kFolderChanged,
  kCurrentModified,
  kCurrentRemoved,
};

struct WatchEvent {
  WatchEventType type = WatchEventType::kNone;

  explicit operator bool() const { return type != WatchEventType::kNone; }
};

class FolderWatcher {
 public:
  // Poll cadence and the two guards from 22/10 §3.1 / 坑 2, 6.
  static constexpr double kDefaultIntervalMs = 2000.0;
  static constexpr double kSuppressMs = 2500.0;
  static constexpr double kMinRescanMs = 2000.0;

  // Installs a baseline without reporting anything (first poll, navigation,
  // or after the app's own rescan).
  void Reset(const FolderSnapshot& snapshot, double now_ms);

  // Returns the single most important change since the last poll. At most one
  // event per call; the caller rescans, which re-baselines on the next poll.
  WatchEvent Update(const FolderSnapshot& snapshot, double now_ms);

  // The app's own delete/rename/save-as: adopt every change until the window
  // expires so the transient files never trigger a rescan (坑 1).
  void Suppress(double now_ms, double duration_ms = kSuppressMs);
  bool suppressed(double now_ms) const { return now_ms < suppress_until_ms_; }

  bool initialized() const { return initialized_; }
  const FolderSnapshot& last() const { return last_; }
  // True when the last Update saw a folder change that the 2 s minimum rescan
  // interval suppressed. The snapshot was deliberately not adopted, so the
  // caller must look again later; the notify path uses this to keep its retry
  // timer armed (a single notification has no second chance).
  bool rescan_deferred() const { return rescan_deferred_; }

 private:
  FolderSnapshot last_{};
  bool initialized_ = false;
  bool rescan_deferred_ = false;
  double suppress_until_ms_ = -1.0;
  double last_rescan_ms_ = -1.0e9;
};

}  // namespace nv
