#include "core/decode_service.h"

#include "core/disk_cache.h"
#include "core/display_tier.h"
#include "decoders/libraw_decoder.h"
#include "decoders/svg_decoder.h"
#include "platform/log.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace nv {

bool CanStartPrefetchNow(const PrefetchGate& gate) {
  if (gate.queue_empty) return false;
  if (!gate.current_pending) {
    // 2 in flight for tiers (~4 MB each), 1 for full frames (96 MB apiece; this
    // is what the two-worker V3-04 behaviour effectively allowed).
    return gate.queue_tier ? gate.in_flight_tier < 2 : gate.in_flight_full < 1;
  }
  // A burst is the only time the current decode does not close the channel:
  // exactly one tier prefetch may run beside it, so the neighbour still lands
  // within one key interval. A full-frame prefetch stays gated.
  if (!gate.burst || !gate.queue_tier) return false;
  return gate.in_flight_tier == 0;
}

namespace {
struct WorkerExitGuard {
  ~WorkerExitGuard() { svg::ReleaseThreadContext(); }
};

// Preview worker payload. RAW is the only format that can produce a preview
// without the full decode; everything else uses the shared thumbnail cache on
// the UI thread (規劃/22/13 §3.1), so this stays a narrow, low-risk path.
DecodeResult DecodePreview(const std::filesystem::path& path) {
  return libraw::DecodeEmbeddedPreview(path);
}

// Corruption guard for a stored tier payload, not a decoded-size budget
// (規劃/25 §7.5).
uint64_t TierPayloadGuard(uint32_t edge) {
  return static_cast<uint64_t>(edge) * edge * 4ull;
}

std::string NarrowPath(const std::filesystem::path& path) {
  const std::wstring text = path.wstring();
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

void PostCompletion(HWND window, UINT message, DecodeCompletion* completion) {
  if (window == nullptr ||
      !PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(completion))) {
    delete completion;
  }
}
}  // namespace

DecodeService::DecodeService(HWND completion_window, UINT completion_message,
                             size_t worker_count, UINT preview_message)
    : completion_window_(completion_window),
      completion_message_(completion_message),
      preview_message_(preview_message) {
  worker_count = std::max<size_t>(1, worker_count);
  workers_.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this] { WorkerLoop(); });
  }
  preview_worker_ = std::thread([this] { PreviewWorkerLoop(); });
}

DecodeService::~DecodeService() {
  Shutdown();
}

void DecodeService::Request(const std::filesystem::path& path,
                            uint32_t max_edge) {
  if (stopping_.load(std::memory_order_acquire)) return;

  const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
  current_id_.store(id, std::memory_order_release);

  {
    std::lock_guard lock(mutex_);
    // Only the newest current request matters. Clearing the prefetch queue is
    // the §7.13-4 dedupe for the queued case: a neighbour that was waiting to
    // be prefetched is now decoded once, as the current request. (V3.1-01 A
    // re-queues the new neighbours right after this call; see LoadPath.)
    current_queue_.clear();
    prefetch_queue_.clear();
    pending_current_id_ = id;
    // V3.1-01 C: a prefetch of exactly this key that is already running must
    // not be decoded a second time (~260 ms for a 22 MB photo at 1280). Its
    // result is promoted to this completion instead. Only in-flight entries
    // qualify: a queued one has not started, so cancelling it and decoding as
    // the current request is both simpler and no slower.
    bool merged = false;
    for (PendingRequest& in_flight : prefetch_in_flight_) {
      if (in_flight.path == path && in_flight.max_edge == max_edge) {
        in_flight.promote_to_current = id;
        merged = true;
        nv::platform::LogInfo("decode: current merged into in-flight prefetch"
                              " path=" + NarrowPath(path) +
                              " edge=" + std::to_string(max_edge));
        break;
      }
    }
    if (!merged) {
      current_queue_.push_back(PendingRequest{id, path, false, max_edge});
    }
  }
  wake_.notify_all();
}

void DecodeService::RequestPreview(const std::filesystem::path& path) {
  if (stopping_.load(std::memory_order_acquire)) return;

  const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
  preview_id_.store(id, std::memory_order_release);

  {
    std::lock_guard lock(mutex_);
    preview_queue_.clear();
    preview_queue_.push_back(PendingRequest{id, path, false, 0});
  }
  wake_.notify_all();
}

bool DecodeService::CanStartPrefetchLocked() const {
  PrefetchGate gate;
  gate.queue_empty = prefetch_queue_.empty();
  gate.queue_tier = !prefetch_queue_.empty() &&
                    prefetch_queue_.front().max_edge != 0;
  gate.current_pending = pending_current_id_ != 0;
  gate.burst = burst_.load(std::memory_order_acquire);
  for (const PendingRequest& r : prefetch_in_flight_) {
    if (r.max_edge == 0) {
      ++gate.in_flight_full;
      continue;
    }
    // A prefetch that a current request merged into *is* the current now
    // (V3.1-01 C), so it must not consume the one slot a burst opens: with it
    // counted, the chain could only ever be one frame deep and every key would
    // still wait a full decode behind the previous one.
    if (r.promote_to_current == 0) ++gate.in_flight_tier;
  }
  return CanStartPrefetchNow(gate);
}

void DecodeService::SetBurstMode(bool on) {
  // Wake the workers on a transition: a queued tier prefetch that was gated by
  // the burst flag must be allowed to start (or must stop starting) now.
  if (burst_.exchange(on, std::memory_order_acq_rel) == on) return;
  nv::platform::LogInfo(std::string("decode: burst=") + (on ? "on" : "off"));
  wake_.notify_all();
}

size_t DecodeService::PrefetchInFlight() {
  std::lock_guard lock(mutex_);
  return prefetch_in_flight_.size();
}

bool DecodeService::IsPrefetchInFlight(const std::filesystem::path& path,
                                       uint32_t max_edge) {
  std::lock_guard lock(mutex_);
  return std::ranges::any_of(prefetch_in_flight_,
                             [&](const PendingRequest& r) {
                               return r.path == path && r.max_edge == max_edge;
                             });
}

void DecodeService::Prefetch(const std::vector<std::filesystem::path>& paths,
                             uint32_t max_edge) {
  if (stopping_.load(std::memory_order_acquire) || paths.empty()) return;

  {
    std::lock_guard lock(mutex_);
    // V3.1-01 A: the queue is a *plan*, not work, so it is replaced even while
    // a current decode runs. The old early return here (plus the cleared queue
    // in Request) is why a miss-heavy burst never re-armed the neighbour chain;
    // whether a worker may start an entry is decided by CanStartPrefetchNow().
    prefetch_queue_.clear();
    size_t skipped = 0;
    for (const auto& path : paths) {
      // §7.13-4: the same key (path + tier) must not be queued twice. An entry
      // that a worker already picked up is the expensive case, so it is
      // checked against the in-flight list, not just the queue.
      const bool duplicate =
          std::ranges::any_of(prefetch_in_flight_, [&](const PendingRequest& r) {
            return r.path == path && r.max_edge == max_edge;
          });
      if (duplicate) {
        ++skipped;
        continue;
      }
      prefetch_queue_.push_back(
          PendingRequest{next_id_.fetch_add(1, std::memory_order_relaxed), path,
                         true, max_edge});
    }
    if (skipped != 0) {
      nv::platform::LogInfo("prefetch: skipped " + std::to_string(skipped) +
                            " duplicate(s)");
    }
  }
  wake_.notify_all();
}

void DecodeService::CancelAll() {
  current_id_.store(0, std::memory_order_release);
  preview_id_.store(0, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    current_queue_.clear();
    prefetch_queue_.clear();
    preview_queue_.clear();
    pending_current_id_ = 0;
    // Do not clear prefetch_in_flight_: those decodes are still running and
    // each erases its own entry when it finishes.
  }
  wake_.notify_all();
}

void DecodeService::Shutdown() {
  if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
  current_id_.store(0, std::memory_order_release);
  preview_id_.store(0, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    current_queue_.clear();
    prefetch_queue_.clear();
    preview_queue_.clear();
    pending_current_id_ = 0;
  }
  wake_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
  if (preview_worker_.joinable()) preview_worker_.join();
}

void DecodeService::WorkerLoop() {
  WorkerExitGuard exit_guard;
  for (;;) {
    PendingRequest request;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] {
        return stopping_.load(std::memory_order_acquire) ||
               !current_queue_.empty() || CanStartPrefetchLocked();
      });
      if (stopping_.load(std::memory_order_acquire)) return;
      if (!current_queue_.empty()) {
        request = std::move(current_queue_.front());
        current_queue_.pop_front();
      } else {
        request = std::move(prefetch_queue_.front());
        prefetch_queue_.pop_front();
        prefetch_in_flight_.push_back(request);
      }
    }

    // Prefetch work is not tied to the current epoch; it is always cacheable.
    if (!request.prefetch &&
        request.id != current_id_.load(std::memory_order_acquire)) {
      FinishCurrent(request.id);
      continue;
    }

    // Posting the completion (and ending the request) is the same for a disk
    // hit, a decoded prefetch that a current request merged into, and a plain
    // current decode, so it lives in one place (V3.1-01 C).
    //
    // `as_prefetch` (V3.1-07 S3b) is for a *current* decode that a newer key
    // superseded: the pixels are this file's tier and are still worth caching,
    // so the completion is posted as a prefetch (the UI caches it and never
    // shows it — the request id no longer matches). See the call site.
    const auto finish = [this, &request](DecodeResult&& result, uint32_t edge,
                                         bool as_prefetch) {
      uint64_t promoted = 0;
      if (request.prefetch) {
        std::lock_guard lock(mutex_);
        for (const PendingRequest& r : prefetch_in_flight_) {
          if (r.id != request.id) continue;
          if (r.promote_to_current != 0 &&
              r.promote_to_current ==
                  current_id_.load(std::memory_order_acquire)) {
            promoted = r.promote_to_current;
          }
          break;
        }
      }
      auto* completion = new DecodeCompletion;
      completion->request_id = promoted != 0 ? promoted : request.id;
      completion->path = std::move(request.path);
      completion->result = std::move(result);
      completion->prefetch =
          as_prefetch || (request.prefetch && promoted == 0);
      completion->max_edge = edge;
      if (promoted != 0) {
        nv::platform::LogInfo("decode: prefetch promoted to current path=" +
                              NarrowPath(completion->path) + " edge=" +
                              std::to_string(edge));
      }
      if (request.prefetch) {
        FinishPrefetch(request.id);
      } else {
        FinishCurrent(request.id);
      }
      // A merged current never entered current_queue_, so its pending flag has
      // to be released here or the prefetch gate would stay closed forever.
      if (promoted != 0) FinishCurrent(promoted);
      PostCompletion(completion_window_, completion_message_, completion);
    };

    // Display-tier prefetch: a stored tier is ~10 ms instead of a decode, and
    // the UI turns it into a memory-cache entry only (規劃/25 §7.5).
    if (request.prefetch && request.max_edge != 0 && tier_cache_ != nullptr) {
      if (auto stored = tier_cache_->LoadTier(request.path, request.max_edge,
                                              TierPayloadGuard(request.max_edge))) {
        if (!stored->Empty()) {
          nv::platform::LogInfo("prefetch: tier disk hit edge=" +
                                std::to_string(request.max_edge) + " path=" +
                                NarrowPath(request.path));
          DecodeResult result;
          result.image = std::move(stored);
          finish(std::move(result), request.max_edge, /*as_prefetch=*/false);
          continue;
        }
      }
    }

    DecodeOptions options;
    // Animation frames are never tiers; a tier decode is a still frame.
    options.animation = !request.prefetch && request.max_edge == 0;
    options.max_edge = request.max_edge;
    DecodeResult result = FormatRouter::Decode(request.path, options);

    // A newer file may have been requested while this decode was running. The
    // decision is made *after* the tier contract below: a superseded decode is
    // still valid pixels of this file's tier, and at 5-9 key presses per second
    // the old code threw one decode away per key (measured: 6 of 20 presses
    // landed nothing at 110 ms). Caching it instead makes the next pass over
    // that folder free; a stale *full frame* (96 MB) is still dropped.
    const bool stale = !request.prefetch &&
                       request.id != current_id_.load(std::memory_order_acquire);

    // The tier contract: the frame handed to the UI really is at most
    // `max_edge`. Direct-composite or fallback paths can ignore `max_edge`, so
    // one CPU pass guarantees it instead of caching a 96 MB frame as a tier.
    uint32_t effective_edge = request.max_edge;
    if (effective_edge != 0 && result.ok()) {
      const uint32_t longest = ImageLongEdge(*result.image);
      if (longest > effective_edge + effective_edge / 100) {
        if (auto tier = MakeDisplayTier(*result.image, effective_edge)) {
          result.image = std::move(tier);
        }
      } else if (result.image->info.full_width != 0 ||
                 result.image->info.full_height != 0) {
        // Nothing had to be scaled *and* the real file is not larger than the
        // edge (a small neighbour in a folder of large ones), so this is the
        // full frame: key it as one, otherwise the next load would look in the
        // full-frame slot, miss, and decode again.
        const uint32_t source_long =
            result.image->info.full_width > result.image->info.full_height
                ? result.image->info.full_width
                : result.image->info.full_height;
        if (source_long <= effective_edge) effective_edge = 0;
      }
    }

    if (stale) {
      const bool cacheable = result.ok() && effective_edge != 0 &&
                             !result.image->info.animated;
      if (!cacheable) {
        FinishCurrent(request.id);
        continue;
      }
      finish(std::move(result), effective_edge, /*as_prefetch=*/true);
      continue;
    }

    finish(std::move(result), effective_edge, /*as_prefetch=*/false);
  }
}

// Ends one prefetch: drop it from the in-flight list and let a worker that was
// waiting for a slot take the next entry (規劃/25 §7.13-1).
void DecodeService::FinishPrefetch(uint64_t id) {
  {
    std::lock_guard lock(mutex_);
    std::erase_if(prefetch_in_flight_,
                  [id](const PendingRequest& r) { return r.id == id; });
  }
  wake_.notify_all();
}

// Ends one current request. Only the newest one clears the flag: a newer
// Request() may already own it, and its prefetch gate must stay closed.
void DecodeService::FinishCurrent(uint64_t id) {
  {
    std::lock_guard lock(mutex_);
    if (pending_current_id_ == id) pending_current_id_ = 0;
  }
  wake_.notify_all();
}

void DecodeService::PreviewWorkerLoop() {
  WorkerExitGuard exit_guard;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

  for (;;) {
    PendingRequest request;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] {
        return stopping_.load(std::memory_order_acquire) ||
               !preview_queue_.empty();
      });
      if (stopping_.load(std::memory_order_acquire)) return;
      request = std::move(preview_queue_.front());
      preview_queue_.pop_front();
    }

    if (request.id != preview_id_.load(std::memory_order_acquire)) continue;

    DecodeResult result = DecodePreview(request.path);

    // Late preview: the user already navigated away or the load was cancelled
    // (規劃/22/13 坑 3). The UI validates the path again before showing it.
    if (request.id != preview_id_.load(std::memory_order_acquire)) continue;

    auto* completion = new DecodeCompletion;
    completion->request_id = request.id;
    completion->path = std::move(request.path);
    completion->result = std::move(result);
    completion->prefetch = false;
    completion->preview = true;

    const UINT message = preview_message_ != 0 ? preview_message_
                                               : completion_message_;
    PostCompletion(completion_window_, message, completion);
  }
}

}  // namespace nv
