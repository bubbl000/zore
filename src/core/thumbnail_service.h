#pragma once

#include "core/format_router.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nv {

class DiskCache;

struct ThumbnailCompletion {
  size_t index = 0;
  std::filesystem::path path;
  std::shared_ptr<Image> image;
  Error error;
};

// Generates small previews on a low-priority worker and posts a
// ThumbnailCompletion* to the UI thread, which owns and deletes it.
//
// Thumbnails live in a bounded in-memory LRU and in a disk cache shared across
// sessions. The UI only asks for items it is about to draw, so a folder with
// thousands of files never costs more than the visible window plus a margin.
class ThumbnailService {
 public:
  ThumbnailService(HWND completion_window, UINT completion_message,
                   uint32_t max_edge = 128, size_t worker_count = 1,
                   uint64_t memory_budget = 64ull * 1024 * 1024,
                   std::filesystem::path disk_cache_dir = {});
  ~ThumbnailService();

  ThumbnailService(const ThumbnailService&) = delete;
  ThumbnailService& operator=(const ThumbnailService&) = delete;

  // Memory-cache hit, or nullptr. Cheap; safe to call every frame.
  std::shared_ptr<Image> GetCached(const std::filesystem::path& path);

  // Two-stage loading (規劃/22/13 §4 step 1): returns the shared thumbnail for
  // `path` from the memory cache or the on-disk thumbnail cache, or nullptr.
  // Runs on the UI thread and is expected to take ~1 ms; the disk cache loader
  // already validates the NVIMG header and the payload size (坑 10). A hit is
  // also promoted into the memory LRU. The returned image is display-only: the
  // caller must not write it into the full-size caches (坑 6).
  std::shared_ptr<Image> TryLoadPreview(const std::filesystem::path& path);

  // Queues generation when the thumbnail is neither cached nor already
  // pending. Returns true when a new job was queued.
  bool Request(size_t index, const std::filesystem::path& path);

  // Drops queued jobs (used when the folder changes). In-flight work still
  // completes; the UI validates the path before applying it.
  void CancelPending();

  void Shutdown();

  uint32_t max_edge() const { return max_edge_; }

 private:
  struct Pending {
    size_t index = 0;
    std::filesystem::path path;
  };

  struct CacheEntry {
    std::wstring key;
    std::shared_ptr<Image> image;
    size_t bytes = 0;
  };

  void WorkerLoop();
  void PostCompletion(ThumbnailCompletion* completion) const;
  void CachePut(const std::wstring& key, std::shared_ptr<Image> image);

  HWND completion_window_ = nullptr;
  UINT completion_message_ = 0;
  uint32_t max_edge_ = 128;
  uint64_t memory_budget_ = 0;
  std::unique_ptr<DiskCache> disk_cache_;

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Pending> queue_;
  std::unordered_set<std::wstring> pending_keys_;
  std::vector<std::thread> workers_;
  std::atomic<bool> stopping_{false};

  mutable std::mutex cache_mutex_;
  std::list<CacheEntry> lru_;
  std::unordered_map<std::wstring, std::list<CacheEntry>::iterator> index_;
  size_t used_bytes_ = 0;
};

}  // namespace nv
