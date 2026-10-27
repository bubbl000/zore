#include "core/disk_cache_writer.h"

#include "core/disk_cache.h"
#include "platform/log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace nv {
namespace {

double QpcMs() {
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

}  // namespace

DiskCacheWriter::DiskCacheWriter(DiskCache* cache, size_t max_queued)
    : cache_(cache), max_queued_(std::max<size_t>(1, max_queued)) {
  if (cache_ != nullptr) {
    worker_ = std::thread([this] { WorkerLoop(); });
  }
}

DiskCacheWriter::~DiskCacheWriter() { Shutdown(); }

void DiskCacheWriter::Enqueue(std::filesystem::path source,
                              std::shared_ptr<Image> image, bool trim,
                              uint32_t edge) {
  if (cache_ == nullptr || !image || image->Empty()) return;

  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    // Bounded queue: a fast navigation should not pin an unbounded number of
    // full frames in memory waiting for the disk.
    while (queue_.size() >= max_queued_) {
      queue_.pop_front();
    }
    queue_.push_back(Entry{std::move(source), std::move(image), trim, edge});
  }
  wake_.notify_one();
}

void DiskCacheWriter::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    queue_.clear();
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void DiskCacheWriter::WorkerLoop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    Entry entry;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      entry = std::move(queue_.front());
      queue_.pop_front();
    }

    const double start = QpcMs();
    if (entry.edge != 0) {
      cache_->SaveTier(entry.source, entry.edge, *entry.image, entry.trim);
    } else {
      cache_->Save(entry.source, *entry.image, entry.trim);
    }
    char line[160]{};
    const double megabytes = static_cast<double>(entry.image->pixels.size()) /
                             (1024.0 * 1024.0);
    if (entry.edge != 0) {
      std::snprintf(line, sizeof(line),
                    "disk cache save: %.1fms %.1fMB edge=%u (bg)",
                    QpcMs() - start, megabytes, entry.edge);
    } else {
      std::snprintf(line, sizeof(line), "disk cache save: %.1fms %.1fMB (bg)",
                    QpcMs() - start, megabytes);
    }
    nv::platform::LogInfo(line);
  }
}

}  // namespace nv
