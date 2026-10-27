#pragma once

// Background EXIF reader for the info panel (規劃/22/06 §4.2).
//
// One low-priority worker; the UI thread never opens a file for metadata. The
// latest request wins (a quick ←/→ sweep must not pile up reads), a small
// path+mtime cache avoids re-reading an image that was already inspected, and
// the completion is posted to the window so the app can validate the path
// before applying it (規劃/22/06 坑 3).

#include "core/exif.h"

#include <windows.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nv {

struct ExifCompletion {
  std::filesystem::path path;
  // path + mtime at request time; the app compares it with its current file.
  std::wstring key;
  ExifData data;
};

class ExifService {
 public:
  ExifService(HWND completion_window, UINT completion_message,
              size_t cache_entries = 8);
  ~ExifService();

  ExifService(const ExifService&) = delete;
  ExifService& operator=(const ExifService&) = delete;

  // Cache key helper (path + last-write-time) so a replaced file is re-read.
  static std::wstring MakeKey(const std::filesystem::path& path);

  // Serves a cached result by posting a completion, or queues a read. Repeated
  // requests for the same key while one is in flight are ignored.
  void Request(const std::filesystem::path& path);

  // Drops queued (not in-flight) requests. In-flight work completes and the app
  // discards it by comparing the key.
  void CancelPending();

  void Shutdown();

  size_t cache_entries() const { return cache_entries_; }

 private:
  struct Pending {
    std::filesystem::path path;
    std::wstring key;
  };

  struct CacheEntry {
    std::wstring key;
    ExifData data;
  };

  void WorkerLoop();
  void Post(ExifCompletion* completion) const;
  bool FindCached(const std::wstring& key, ExifData* data);
  void StoreCached(const std::wstring& key, const ExifData& data);

  HWND completion_window_ = nullptr;
  UINT completion_message_ = 0;
  size_t cache_entries_ = 8;

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Pending> queue_;
  std::wstring in_flight_key_;
  std::thread worker_;
  bool stopping_ = false;

  mutable std::mutex cache_mutex_;
  std::list<CacheEntry> lru_;
  std::unordered_map<std::wstring, std::list<CacheEntry>::iterator> index_;
};

}  // namespace nv
