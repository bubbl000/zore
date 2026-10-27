#include "core/exif_service.h"

#include "platform/log.h"

#include <objbase.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace nv {
namespace {

// The mtime is part of the key: a re-saved file must not show stale tags.
std::wstring KeyFor(const std::filesystem::path& path) {
  std::wstring key = path.wstring();
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    key += L"|";
    key += std::to_wstring(
        static_cast<long long>(stamp.time_since_epoch().count()));
  }
  return key;
}

}  // namespace

ExifService::ExifService(HWND completion_window, UINT completion_message,
                         size_t cache_entries)
    : completion_window_(completion_window),
      completion_message_(completion_message),
      cache_entries_(std::max<size_t>(1, cache_entries)) {
  worker_ = std::thread([this] { WorkerLoop(); });
}

ExifService::~ExifService() { Shutdown(); }

std::wstring ExifService::MakeKey(const std::filesystem::path& path) {
  return KeyFor(path);
}

void ExifService::Post(ExifCompletion* completion) const {
  if (completion == nullptr) return;
  if (completion_window_ == nullptr || completion_message_ == 0 ||
      !PostMessageW(completion_window_, completion_message_, 0,
                    reinterpret_cast<LPARAM>(completion))) {
    delete completion;
  }
}

bool ExifService::FindCached(const std::wstring& key, ExifData* data) {
  if (data == nullptr) return false;
  std::lock_guard lock(cache_mutex_);
  const auto it = index_.find(key);
  if (it == index_.end()) return false;
  *data = it->second->data;
  // Refresh the LRU position.
  lru_.splice(lru_.begin(), lru_, it->second);
  return true;
}

void ExifService::StoreCached(const std::wstring& key, const ExifData& data) {
  std::lock_guard lock(cache_mutex_);
  const auto existing = index_.find(key);
  if (existing != index_.end()) {
    existing->second->data = data;
    lru_.splice(lru_.begin(), lru_, existing->second);
    return;
  }
  lru_.push_front(CacheEntry{key, data});
  index_[key] = lru_.begin();
  while (lru_.size() > cache_entries_) {
    index_.erase(lru_.back().key);
    lru_.pop_back();
  }
}

void ExifService::Request(const std::filesystem::path& path) {
  if (path.empty()) return;
  const std::wstring key = KeyFor(path);

  ExifData cached;
  if (FindCached(key, &cached)) {
    // Still asynchronous: the UI gets every result through one message path.
    auto* completion = new ExifCompletion{path, key, std::move(cached)};
    Post(completion);
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    if (in_flight_key_ == key) return;
    // Latest path wins: a quick sweep replaces the queued request instead of
    // reading every frame the user scrolled past (規劃/22/06 坑 19).
    for (const Pending& pending : queue_) {
      if (pending.key == key) return;
    }
    queue_.clear();
    queue_.push_back(Pending{path, key});
  }
  nv::platform::LogInfo("exif: request queued");
  wake_.notify_one();
}

void ExifService::CancelPending() {
  std::lock_guard lock(mutex_);
  queue_.clear();
}

void ExifService::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    queue_.clear();
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void ExifService::WorkerLoop() {
  // WIC needs COM on the reading thread (HEIC/AVIF fallback path).
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_ready = SUCCEEDED(com);

  for (;;) {
    Pending pending;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) break;
      pending = std::move(queue_.front());
      queue_.pop_front();
      in_flight_key_ = pending.key;
    }

    ExifData data = ReadExif(pending.path);
    StoreCached(pending.key, data);
    {
      std::lock_guard lock(mutex_);
      if (in_flight_key_ == pending.key) in_flight_key_.clear();
      if (stopping_) break;
    }
    auto* completion = new ExifCompletion{pending.path, pending.key,
                                          std::move(data)};
    Post(completion);
  }

  if (com_ready) CoUninitialize();
}

}  // namespace nv
