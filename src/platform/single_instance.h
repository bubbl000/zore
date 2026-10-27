#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nv::platform {

// --- Single instance (規劃/22/15 §5) ---------------------------------------
// A named mutex decides who owns the instance and a message-only window
// receives the path from later launches. Both names include a hash of DataDir
// so a portable copy next to the exe and an installed copy in %LOCALAPPDATA%
// never forward into each other (規劃/22/15 坑 10).

// "Local\\ZoreView.Single.<FNV1a-64 of the normalised DataDir>"
std::wstring InstanceMutexName(const std::filesystem::path& data_dir);
// "ZoreView.InstanceReceiver.<same hash>"
std::wstring InstanceReceiverClass(const std::filesystem::path& data_dir);

// WM_COPYDATA payload: a versioned header followed by a UTF-16 path. An empty
// path means "activate the existing window, do not open a file".
inline constexpr DWORD kInstancePayloadMagic = 0x4E5631;  // 'NV1'
inline constexpr DWORD kInstancePayloadVersion = 1;

// Encodes a payload. Returns an empty vector when the path cannot be
// represented (longer than a Windows path).
std::vector<char> EncodeInstancePayload(const std::wstring& path);
// Validates magic/version/size and copies the path. Unknown versions are
// rejected so a newer launcher never feeds a path an older build cannot read
// (規劃/22/15 §5.2).
bool DecodeInstancePayload(const void* data, DWORD size, std::wstring* path);

// Owns the mutex handle. `primary` is true when this process created it; when
// `already_running` is true the mutex existed and the process should forward
// and exit (or, when forwarding fails, continue as a normal second window).
class SingleInstanceLock {
 public:
  SingleInstanceLock() = default;
  ~SingleInstanceLock();
  SingleInstanceLock(const SingleInstanceLock&) = delete;
  SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;

  bool Acquire(const std::wstring& name, bool* already_running);
  void Release();
  bool acquired() const { return mutex_ != nullptr; }

 private:
  HANDLE mutex_ = nullptr;
};

// Message-only receiver. It exists before the main window so a second launch
// during device initialisation is not lost (規劃/22/15 §5.3/坑 7). Until a
// handler is installed the last path is kept and delivered by SetHandler().
class InstanceReceiver {
 public:
  using Handler = std::function<void(const std::wstring& path)>;

  InstanceReceiver() = default;
  ~InstanceReceiver();
  InstanceReceiver(const InstanceReceiver&) = delete;
  InstanceReceiver& operator=(const InstanceReceiver&) = delete;

  bool Create(const std::wstring& class_name);
  void SetHandler(Handler handler);
  void Destroy();
  HWND hwnd() const { return hwnd_; }

  // Finds the receiver of the instance that owns `class_name`. Message-only
  // windows are children of HWND_MESSAGE, so a plain FindWindow() finds
  // nothing.
  static HWND Find(const std::wstring& class_name);

  // Public so the class-registration helper can reference it; it is not part
  // of the usable API.
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam);

 private:
  void OnPayload(const std::wstring& path);

  HWND hwnd_ = nullptr;
  Handler handler_;
  bool pending_ = false;
  std::wstring pending_path_;
};

// Best-effort foreground permission for the existing instance (規劃/22/15
// §5.2). Failure is not fatal: the receiver still flashes its window.
bool AllowForegroundFromWindow(HWND target);

// Sends the payload with a bounded timeout. Returns false on timeout, a
// rejecting receiver, or a window that is not there any more; callers then
// open a normal window instead of swallowing the launch.
bool ForwardInstanceMessage(HWND target, const std::wstring& path,
                            DWORD timeout_ms, std::wstring* error);

}  // namespace nv::platform
