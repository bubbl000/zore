#pragma once

#include "core/image.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace nv {

class DiskCache;

// Serializes decoded images into the disk cache on a background thread.
//
// An 8K frame is 126 MB; writing it inline blocked the UI thread for >100 ms
// right after the image appeared (and prefetches added more writes). Entries
// are best-effort: the queue is bounded and dropped on shutdown, because the
// disk cache can always be rebuilt by decoding again.
class DiskCacheWriter {
 public:
  explicit DiskCacheWriter(DiskCache* cache, size_t max_queued = 3);
  ~DiskCacheWriter();

  DiskCacheWriter(const DiskCacheWriter&) = delete;
  DiskCacheWriter& operator=(const DiskCacheWriter&) = delete;

  // `edge` selects the display tier (V3-04); 0 writes the full-resolution
  // frame exactly as before.
  void Enqueue(std::filesystem::path source, std::shared_ptr<Image> image,
               bool trim = true, uint32_t edge = 0);
  void Shutdown();

 private:
  struct Entry {
    std::filesystem::path source;
    std::shared_ptr<Image> image;
    bool trim = true;
    uint32_t edge = 0;
  };

  void WorkerLoop();

  DiskCache* cache_ = nullptr;
  size_t max_queued_ = 3;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Entry> queue_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace nv
