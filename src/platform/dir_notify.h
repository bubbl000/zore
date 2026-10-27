#pragma once

// V3-05 (規劃/25 §8): local-disk folder notifications.
//
// One watcher thread and one directory handle for the folder on screen —
// never one thread per folder (that design was rejected in §9).
// A burst of file-system events is coalesced with a quiet period: the thread
// keeps a read pending until kMergeMs passes without a new batch, then posts
// kMessage once. The window procedure then runs exactly the same
// TakeFolderSnapshot → FolderWatcher::Update → rescan path the 2 s poll used,
// so the debounce/suppression state machine is shared, not duplicated.
//
// Scope (user decision 2026-10-26): local fixed disks only. UNC paths and
// mapped network drives keep the legacy 2 s polling, and so does any folder
// whose Start() fails. This class never retries on its own; the app decides.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace nv::platform {

class DirNotifier {
 public:
  // Posted, without payload, to the window passed to Start().
  static constexpr UINT kMessage = WM_APP + 7;
  // Quiet period after the last batch before one message is posted.
  static constexpr DWORD kMergeMs = 250;
  // FILE_NOTIFY_CHANGE_FILE_NAME | DIR_NAME | LAST_WRITE | SIZE; not recursive.
  static constexpr DWORD kFilter =
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
  static constexpr DWORD kBufferBytes = 64 * 1024;

  DirNotifier() = default;
  ~DirNotifier();
  DirNotifier(const DirNotifier&) = delete;
  DirNotifier& operator=(const DirNotifier&) = delete;

  // Opens the folder and starts the watcher thread. Returns false — with
  // error_text() explaining why and nothing left running — when the handle or
  // the first overlapped read cannot be created; the caller keeps polling.
  bool Start(HWND window, const std::filesystem::path& folder);

  // Cancels the pending read, closes the handle and joins the thread. Safe when
  // not running; ERROR_OPERATION_ABORTED is a normal exit, not a failure.
  void Stop();

  bool running() const { return running_.load(std::memory_order_acquire); }
  // True when the thread stopped because of an I/O error (the window got a
  // kMessage for it too, so the app can fall back to polling).
  bool failed() const { return failed_.load(std::memory_order_acquire); }
  // Populated by a failed Start(); only read it on the thread that called it.
  const std::wstring& error_text() const { return error_; }
  const std::filesystem::path& folder() const { return folder_; }

 private:
  void Run();
  void CloseHandles();
  void Fail(const wchar_t* what, DWORD error);

  HWND window_ = nullptr;
  std::filesystem::path folder_;
  HANDLE dir_ = INVALID_HANDLE_VALUE;
  HANDLE stop_event_ = nullptr;
  HANDLE read_event_ = nullptr;
  OVERLAPPED overlapped_{};
  std::vector<BYTE> buffer_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};
  std::wstring error_;
};

}  // namespace nv::platform
