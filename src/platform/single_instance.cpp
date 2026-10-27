#include "platform/single_instance.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace nv::platform {
namespace {

constexpr size_t kMaxPathChars = 32768;  // Windows path limit, including NUL.
constexpr size_t kHeaderSize = 12;

struct PayloadHeader {
  DWORD magic;
  DWORD version;
  DWORD path_chars;
};
static_assert(sizeof(PayloadHeader) == kHeaderSize, "payload header is packed");

// FNV-1a 64 over the lower-cased path. Wide characters are hashed as two
// little-endian bytes so the value is stable across compilers.
uint64_t Fnv1a(const std::wstring& text) {
  uint64_t hash = 1469598103934665603ull;
  for (const wchar_t ch : text) {
    const wchar_t lower = static_cast<wchar_t>(std::towlower(ch));
    const uint16_t value = static_cast<uint16_t>(lower);
    hash ^= static_cast<uint8_t>(value & 0xFF);
    hash *= 1099511628211ull;
    hash ^= static_cast<uint8_t>((value >> 8) & 0xFF);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::wstring DataDirHash(const std::filesystem::path& data_dir) {
  std::wstring text = data_dir.lexically_normal().wstring();
  while (!text.empty() && (text.back() == L'\\' || text.back() == L'/')) {
    text.pop_back();
  }
  const uint64_t hash = Fnv1a(text);
  std::wostringstream out;
  out << std::hex << std::setw(16) << std::setfill(L'0') << hash;
  return out.str();
}

bool RegisterReceiverClass(const std::wstring& class_name, HINSTANCE instance) {
  WNDCLASSEXW existing{};
  existing.cbSize = sizeof(existing);
  if (GetClassInfoExW(instance, class_name.c_str(), &existing) != 0) {
    return true;
  }
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &InstanceReceiver::WndProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = class_name.c_str();
  return RegisterClassExW(&window_class) != 0;
}

}  // namespace

std::wstring InstanceMutexName(const std::filesystem::path& data_dir) {
  return L"Local\\ZoreView.Single." + DataDirHash(data_dir);
}

std::wstring InstanceReceiverClass(const std::filesystem::path& data_dir) {
  return L"ZoreView.InstanceReceiver." + DataDirHash(data_dir);
}

std::vector<char> EncodeInstancePayload(const std::wstring& path) {
  if (path.size() + 1 > kMaxPathChars) return {};
  const size_t chars = path.size() + 1;
  PayloadHeader header{kInstancePayloadMagic, kInstancePayloadVersion,
                       static_cast<DWORD>(chars)};
  std::vector<char> payload(sizeof(header) + chars * sizeof(wchar_t));
  std::memcpy(payload.data(), &header, sizeof(header));
  wchar_t* text = reinterpret_cast<wchar_t*>(payload.data() + sizeof(header));
  if (!path.empty()) {
    std::memcpy(text, path.data(), path.size() * sizeof(wchar_t));
  }
  text[path.size()] = L'\0';
  return payload;
}

bool DecodeInstancePayload(const void* data, DWORD size, std::wstring* path) {
  if (data == nullptr || path == nullptr || size < sizeof(PayloadHeader)) {
    return false;
  }
  PayloadHeader header{};
  std::memcpy(&header, data, sizeof(header));
  if (header.magic != kInstancePayloadMagic ||
      header.version != kInstancePayloadVersion) {
    return false;
  }
  if (header.path_chars == 0 || header.path_chars > kMaxPathChars) return false;
  const uint64_t expected =
      sizeof(PayloadHeader) +
      static_cast<uint64_t>(header.path_chars) * sizeof(wchar_t);
  if (size != expected) return false;
  const wchar_t* text =
      reinterpret_cast<const wchar_t*>(static_cast<const char*>(data) +
                                       sizeof(PayloadHeader));
  size_t length = header.path_chars;
  if (length > 0 && text[length - 1] == L'\0') --length;
  path->assign(text, length);
  return true;
}

SingleInstanceLock::~SingleInstanceLock() { Release(); }

bool SingleInstanceLock::Acquire(const std::wstring& name,
                                 bool* already_running) {
  if (already_running != nullptr) *already_running = false;
  Release();
  if (name.empty()) return false;
  mutex_ = CreateMutexW(nullptr, TRUE, name.c_str());
  if (mutex_ == nullptr) return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (already_running != nullptr) *already_running = true;
  }
  return true;
}

void SingleInstanceLock::Release() {
  if (mutex_ != nullptr) {
    // CloseHandle also releases our ownership; the kernel drops the mutex once
    // the last handle closes, so a crashed process never leaves a stale lock.
    CloseHandle(mutex_);
    mutex_ = nullptr;
  }
}

InstanceReceiver::~InstanceReceiver() { Destroy(); }

bool InstanceReceiver::Create(const std::wstring& class_name) {
  if (hwnd_ != nullptr) return true;
  if (class_name.empty()) return false;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  if (!RegisterReceiverClass(class_name, instance)) return false;
  hwnd_ = CreateWindowExW(0, class_name.c_str(), L"", 0, 0, 0, 0, 0,
                          HWND_MESSAGE, nullptr, instance, this);
  return hwnd_ != nullptr;
}

void InstanceReceiver::SetHandler(Handler handler) {
  handler_ = std::move(handler);
  if (handler_ && pending_) {
    pending_ = false;
    const std::wstring path = std::move(pending_path_);
    pending_path_.clear();
    handler_(path);
  }
}

void InstanceReceiver::Destroy() {
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  pending_ = false;
  pending_path_.clear();
  handler_ = nullptr;
}

HWND InstanceReceiver::Find(const std::wstring& class_name) {
  if (class_name.empty()) return nullptr;
  return FindWindowExW(HWND_MESSAGE, nullptr, class_name.c_str(), nullptr);
}

LRESULT CALLBACK InstanceReceiver::WndProc(HWND hwnd, UINT message,
                                           WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto* self = reinterpret_cast<InstanceReceiver*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_COPYDATA) {
    const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
    std::wstring path;
    if (self != nullptr && data != nullptr &&
        data->dwData == kInstancePayloadMagic &&
        DecodeInstancePayload(data->lpData, data->cbData, &path)) {
      self->OnPayload(path);
      return TRUE;
    }
    return FALSE;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void InstanceReceiver::OnPayload(const std::wstring& path) {
  if (handler_) {
    handler_(path);
    return;
  }
  // The main window is still being created; keep the newest request. A path
  // is preferred over a bare activation.
  if (path.empty() && pending_) return;
  pending_ = true;
  pending_path_ = path;
}

bool AllowForegroundFromWindow(HWND target) {
  if (target == nullptr) return false;
  DWORD process_id = 0;
  GetWindowThreadProcessId(target, &process_id);
  if (process_id == 0) return false;
  return AllowSetForegroundWindow(process_id) != FALSE;
}

bool ForwardInstanceMessage(HWND target, const std::wstring& path,
                            DWORD timeout_ms, std::wstring* error) {
  if (error != nullptr) error->clear();
  if (target == nullptr) {
    if (error != nullptr) *error = L"no receiver window";
    return false;
  }
  const std::vector<char> payload = EncodeInstancePayload(path);
  if (payload.empty()) {
    if (error != nullptr) *error = L"path too long";
    return false;
  }
  COPYDATASTRUCT data{};
  data.dwData = kInstancePayloadMagic;
  data.cbData = static_cast<DWORD>(payload.size());
  data.lpData = const_cast<char*>(payload.data());
  DWORD_PTR result = 0;
  const LRESULT sent =
      SendMessageTimeoutW(target, WM_COPYDATA, 0,
                          reinterpret_cast<LPARAM>(&data),
                          SMTO_ABORTIFHUNG | SMTO_BLOCK, timeout_ms, &result);
  if (sent == 0) {
    const DWORD code = GetLastError();
    if (error != nullptr) {
      *error = L"SendMessageTimeout failed (" + std::to_wstring(code) + L")";
    }
    return false;
  }
  if (result == 0) {
    if (error != nullptr) *error = L"receiver rejected the payload";
    return false;
  }
  return true;
}

}  // namespace nv::platform
