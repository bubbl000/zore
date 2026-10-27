#include "core/thumbnail_service.h"

#include "core/cache_key.h"
#include "core/disk_cache.h"
#include "decoders/svg_decoder.h"

#include <algorithm>
#include <utility>

namespace nv {
namespace {

struct ThumbnailWorkerExitGuard {
  ~ThumbnailWorkerExitGuard() { svg::ReleaseThreadContext(); }
};

// Queued thumbnails are speculative; when the user scrolls faster than the
// worker can decode, the oldest entries are simply dropped.
//
// V3.1-07 S1-2: 24 is the replica index worker's queue depth (規劃/v3.1/07
// §1.1), and it is also what keeps a pre-read plan (≤13 entries: the current
// file + ±6) plus a rail's visible slots from queuing more work than the single
// below-normal worker can finish before the next key. Dropping is safe: every
// dropped entry is re-requested the next time the rail or the pre-read looks at
// that file, and the completion handler only applies what still resolves.
constexpr size_t kMaxQueued = 24;
constexpr uint64_t kDiskCacheBudget = 256ull * 1024 * 1024;
constexpr int kTrimEvery = 32;

}  // namespace

ThumbnailService::ThumbnailService(HWND completion_window,
                                   UINT completion_message, uint32_t max_edge,
                                   size_t worker_count,
                                   uint64_t memory_budget,
                                   std::filesystem::path disk_cache_dir)
    : completion_window_(completion_window),
      completion_message_(completion_message),
      max_edge_(std::max<uint32_t>(16, max_edge)),
      memory_budget_(memory_budget) {
  if (!disk_cache_dir.empty()) {
    // The directory name is `e<edge>v<version>`, and both parts isolate payloads
    // that a different decode would produce:
    //   e128v2 -> EXIF orientation and ICC conversion baked into the pixels;
    //   e160v1 -> V3.1-07 S3: 160 px is the transition-frame size the replica
    //             uses (規劃/v3.1/07 §1.1/§3), so the tree is replaced rather
    //             than mixed with 128 px entries. The edge is part of the name,
    //             so the old directory is simply never read again.
    disk_cache_ = std::make_unique<DiskCache>(
        disk_cache_dir / (L"e" + std::to_wstring(max_edge_) + L"v1"),
        kDiskCacheBudget);
  }

  worker_count = std::max<size_t>(1, worker_count);
  workers_.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this] { WorkerLoop(); });
  }
}

ThumbnailService::~ThumbnailService() { Shutdown(); }

std::shared_ptr<Image> ThumbnailService::GetCached(
    const std::filesystem::path& path) {
  const std::wstring key = MakeCacheKey(path);
  std::lock_guard lock(cache_mutex_);
  const auto it = index_.find(key);
  if (it == index_.end()) return nullptr;
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->image;
}

std::shared_ptr<Image> ThumbnailService::TryLoadPreview(
    const std::filesystem::path& path) {
  if (auto cached = GetCached(path)) return cached;
  if (!disk_cache_) return nullptr;

  // Thumbnails are small by construction; the payload cap is a corruption
  // guard, not a budget (規劃/22/13 坑 10).
  auto loaded = disk_cache_->Load(path, 8ull * 1024 * 1024);
  if (!loaded || loaded->Empty()) return nullptr;
  CachePut(MakeCacheKey(path), loaded);
  return loaded;
}

bool ThumbnailService::Request(size_t index,
                               const std::filesystem::path& path) {
  if (stopping_.load(std::memory_order_acquire)) return false;

  const std::wstring key = MakeCacheKey(path);
  {
    std::lock_guard lock(cache_mutex_);
    if (index_.find(key) != index_.end()) return false;
  }

  {
    std::lock_guard lock(mutex_);
    if (stopping_.load(std::memory_order_acquire)) return false;
    if (!pending_keys_.insert(key).second) return false;
    while (queue_.size() >= kMaxQueued) {
      pending_keys_.erase(MakeCacheKey(queue_.front().path));
      queue_.pop_front();
    }
    queue_.push_back(Pending{index, path});
  }
  wake_.notify_one();
  return true;
}

void ThumbnailService::CancelPending() {
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    pending_keys_.clear();
  }
}

void ThumbnailService::Shutdown() {
  if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    pending_keys_.clear();
  }
  wake_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
}

void ThumbnailService::PostCompletion(ThumbnailCompletion* completion) const {
  if (completion_window_ == nullptr ||
      !PostMessageW(completion_window_, completion_message_, 0,
                    reinterpret_cast<LPARAM>(completion))) {
    delete completion;
  }
}

void ThumbnailService::CachePut(const std::wstring& key,
                                std::shared_ptr<Image> image) {
  if (!image || image->Empty() || memory_budget_ == 0) return;
  const size_t bytes = image->pixels.size();
  if (bytes > memory_budget_) return;

  std::lock_guard lock(cache_mutex_);
  const auto existing = index_.find(key);
  if (existing != index_.end()) {
    used_bytes_ -= existing->second->bytes;
    lru_.erase(existing->second);
    index_.erase(existing);
  }
  while (!lru_.empty() && used_bytes_ + bytes > memory_budget_) {
    CacheEntry& back = lru_.back();
    used_bytes_ -= back.bytes;
    index_.erase(back.key);
    lru_.pop_back();
  }
  lru_.push_front(CacheEntry{key, std::move(image), bytes});
  index_[key] = lru_.begin();
  used_bytes_ += bytes;
}

void ThumbnailService::WorkerLoop() {
  ThumbnailWorkerExitGuard exit_guard;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

  int saved_since_trim = 0;
  for (;;) {
    Pending request;
    std::wstring key;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] {
        return stopping_.load(std::memory_order_acquire) || !queue_.empty();
      });
      if (stopping_.load(std::memory_order_acquire)) return;
      request = std::move(queue_.front());
      queue_.pop_front();
      key = MakeCacheKey(request.path);
      pending_keys_.erase(key);
    }

    if (auto cached = GetCached(request.path)) {
      PostCompletion(new ThumbnailCompletion{request.index, request.path,
                                             std::move(cached), {}});
      continue;
    }

    if (disk_cache_) {
      if (auto cached = disk_cache_->Load(request.path)) {
        CachePut(key, cached);
        PostCompletion(new ThumbnailCompletion{request.index, request.path,
                                               std::move(cached), {}});
        continue;
      }
    }

    DecodeOptions options;
    options.max_edge = max_edge_;
    DecodeResult decoded = FormatRouter::Decode(request.path, options);
    if (!decoded.ok()) {
      PostCompletion(new ThumbnailCompletion{request.index, request.path,
                                             nullptr, decoded.error});
      continue;
    }

    if (disk_cache_) {
      const bool trim_now = ++saved_since_trim >= kTrimEvery;
      if (trim_now) saved_since_trim = 0;
      disk_cache_->Save(request.path, *decoded.image, trim_now);
    }

    CachePut(key, decoded.image);
    PostCompletion(new ThumbnailCompletion{request.index, request.path,
                                           std::move(decoded.image), {}});
  }
}

}  // namespace nv
