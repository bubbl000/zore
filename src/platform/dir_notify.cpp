#include "platform/dir_notify.h"

#include "platform/log.h"

#include <string>

namespace nv::platform {
namespace {

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

}  // namespace

DirNotifier::~DirNotifier() { Stop(); }

bool DirNotifier::Start(HWND window, const std::filesystem::path& folder) {
  // Re-targeting (navigation, or a retry after a fallback) always tears the old
  // watch down first: there is at most one open directory handle at a time.
  Stop();
  window_ = window;
  folder_ = folder;
  error_.clear();
  failed_.store(false, std::memory_order_release);
  if (window == nullptr || folder.empty()) {
    error_ = L"invalid window or folder";
    return false;
  }

  // FILE_FLAG_BACKUP_SEMANTICS is what lets CreateFile open a directory at all;
  // the share modes match Explorer so the user can keep renaming/deleting.
  dir_ = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     nullptr, OPEN_EXISTING,
                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (dir_ == INVALID_HANDLE_VALUE) {
    error_ = L"CreateFileW failed, error=" +
             std::to_wstring(static_cast<unsigned long>(GetLastError()));
    return false;
  }

  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop_event_ == nullptr || read_event_ == nullptr) {
    error_ = L"CreateEventW failed, error=" +
             std::to_wstring(static_cast<unsigned long>(GetLastError()));
    CloseHandles();
    return false;
  }

  buffer_.assign(kBufferBytes, 0);
  overlapped_ = OVERLAPPED{};
  overlapped_.hEvent = read_event_;
  // The first read is issued here so Start() fails loudly (and the caller keeps
  // polling) instead of a thread silently dying a moment later.
  if (ReadDirectoryChangesW(dir_, buffer_.data(), kBufferBytes, FALSE, kFilter,
                            nullptr, &overlapped_, nullptr) == FALSE) {
    error_ = L"ReadDirectoryChangesW failed, error=" +
             std::to_wstring(static_cast<unsigned long>(GetLastError()));
    CloseHandles();
    return false;
  }

  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this] { Run(); });
  return true;
}

void DirNotifier::Stop() {
  if (stop_event_ != nullptr) SetEvent(stop_event_);
  if (dir_ != INVALID_HANDLE_VALUE) CancelIoEx(dir_, &overlapped_);
  if (thread_.joinable()) thread_.join();
  CloseHandles();
  running_.store(false, std::memory_order_release);
  window_ = nullptr;
  folder_.clear();
}

void DirNotifier::CloseHandles() {
  if (read_event_ != nullptr) {
    CloseHandle(read_event_);
    read_event_ = nullptr;
  }
  if (stop_event_ != nullptr) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  if (dir_ != INVALID_HANDLE_VALUE) {
    CloseHandle(dir_);
    dir_ = INVALID_HANDLE_VALUE;
  }
  overlapped_ = OVERLAPPED{};
}

void DirNotifier::Fail(const wchar_t* what, DWORD error) {
  failed_.store(true, std::memory_order_release);
  LogInfo("dir_notify: " + Narrow(what) + " error=" + std::to_string(error) +
          " folder=" + Narrow(folder_.wstring()));
}

void DirNotifier::Run() {
  const HANDLE waits[2] = {stop_event_, read_event_};
  for (;;) {
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait != WAIT_OBJECT_0 + 1) {
      if (wait != WAIT_OBJECT_0) Fail(L"wait failed", GetLastError());
      break;
    }

    DWORD bytes = 0;
    if (GetOverlappedResult(dir_, &overlapped_, &bytes, FALSE) == FALSE) {
      const DWORD error = GetLastError();
      // Stop() cancelled the pending read: a normal exit, not a failure.
      if (error != ERROR_OPERATION_ABORTED) Fail(L"read failed", error);
      break;
    }
    // bytes == 0 on success means the kernel buffer overflowed and events were
    // lost; the snapshot diff still finds the change, so just wake the app.

    // Quiet-period merge: re-arm immediately, then wait up to kMergeMs for the
    // next batch. Every batch restarts the window, so 100 rapid writes produce
    // exactly one message (規劃/25 §8 验收：rescan ≤2).
    ULONGLONG deadline = GetTickCount64() + kMergeMs;
    bool stopped = false;
    bool read_failed = false;
    for (;;) {
      ResetEvent(read_event_);
      if (ReadDirectoryChangesW(dir_, buffer_.data(), kBufferBytes, FALSE,
                                kFilter, nullptr, &overlapped_, nullptr) ==
          FALSE) {
        read_failed = true;
        break;
      }
      const ULONGLONG now = GetTickCount64();
      const DWORD remaining =
          now >= deadline ? 0u : static_cast<DWORD>(deadline - now);
      const DWORD merged = WaitForMultipleObjects(2, waits, FALSE, remaining);
      if (merged == WAIT_OBJECT_0) {
        stopped = true;
        break;
      }
      if (merged == WAIT_OBJECT_0 + 1) {
        DWORD more = 0;
        if (GetOverlappedResult(dir_, &overlapped_, &more, FALSE) == FALSE) {
          const DWORD error = GetLastError();
          if (error != ERROR_OPERATION_ABORTED) Fail(L"read failed", error);
          read_failed = true;
          break;
        }
        // A new batch arrived inside the quiet window: extend it.
        deadline = GetTickCount64() + kMergeMs;
        continue;
      }
      if (merged == WAIT_TIMEOUT) break;
      Fail(L"wait failed", GetLastError());
      read_failed = true;
      break;
    }

    if (!stopped && !read_failed) {
      // One message per quiet period. The window may already be gone during
      // shutdown; PostMessage then simply returns FALSE.
      PostMessageW(window_, kMessage, 0, 0);
    }
    if (stopped) break;
    if (read_failed) break;
    // A read is still pending from the last inner iteration; wait for it.
  }

  running_.store(false, std::memory_order_release);
  if (failed()) {
    // Tell the app the watch died (share violation, handle revoked, ...) so it
    // can fall back to the 2 s poll instead of silently not watching.
    PostMessageW(window_, kMessage, 0, 0);
  }
}

}  // namespace nv::platform
