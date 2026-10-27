#pragma once

#include "core/format_router.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace nv {

// V3.1-01 (規劃/v3.1/01 §V3.1-01 B): may a worker start the next prefetch?
//
// The V3-04 gate closed the prefetch channel completely while a current decode
// was in flight (規劃/25 §7.13-1). At a 200 ms key interval on a 22 MB photo the
// current decode itself takes ~260 ms, so the gate was closed permanently and
// the neighbour chain died after the first miss (24mp25 S3: 1–2 frames shown,
// 18 cancels, 5 prefetches). During a navigation burst one *tier* prefetch (~4
// MB) may therefore run beside the current decode; full-frame prefetches (96 MB)
// stay gated, and outside a burst nothing changes.
//
// Pure so the three rules are unit-tested without threads (tests/core_cache_test).
struct PrefetchGate {
  bool queue_empty = true;
  bool queue_tier = false;  // front entry has max_edge != 0
  bool current_pending = false;
  bool burst = false;
  size_t in_flight_tier = 0;
  size_t in_flight_full = 0;
};

bool CanStartPrefetchNow(const PrefetchGate& gate);

struct DecodeCompletion {
  uint64_t request_id = 0;
  std::filesystem::path path;
  DecodeResult result;
  bool prefetch = false;
  // Two-stage loading (規劃/22/13): the low-resolution result of
  // RequestPreview(). Previews are display-only and must never enter the
  // full-size caches (坑 6).
  bool preview = false;
  // V3-04 display tier (規劃/25 §7.4b): the long edge the request was decoded
  // at, 0 for a full-resolution decode. The UI keys caches and the disk entry
  // with it, so a tier frame can never land in the full-frame slot.
  uint32_t max_edge = 0;
};

// Decodes on worker threads and posts a DecodeCompletion* to the UI thread.
// The UI owns and deletes the completion object.
//
// Previews (規劃/22/13 §3.2) run on their own single thread and queue, so an
// embedded RAW preview never waits behind the two full-size workers or an
// in-flight prefetch: at most one preview task exists at a time and a newer
// request simply replaces it.
class DecodeService {
 public:
  DecodeService(HWND completion_window, UINT completion_message,
                size_t worker_count = 2, UINT preview_message = 0);
  ~DecodeService();

  DecodeService(const DecodeService&) = delete;
  DecodeService& operator=(const DecodeService&) = delete;

  // Cancels queued work and starts a new request. `max_edge` > 0 decodes a
  // display tier at that long edge instead of the full frame (V3-04).
  void Request(const std::filesystem::path& path, uint32_t max_edge = 0);

  // Queues (or replaces) one preview task on the dedicated preview worker.
  // Only useful for formats that can produce a preview without a full decode
  // (currently the RAW camera-embedded JPEG, 規劃/22/13 §3.3).
  void RequestPreview(const std::filesystem::path& path);

  // Low-priority background decodes; results are cached by the UI. `max_edge`
  // > 0 prefetches display tiers, so a neighbour never costs a full frame
  // (規劃/25 §7.5). With a tier cache attached, a disk hit is used instead of
  // decoding again.
  //
  // Two gates (規劃/25 §7.13-1/-4, widened by V3.1-01):
  //  - a current request that is still in flight suppresses new prefetch work
  //    entirely outside a burst; during a burst one tier prefetch may run
  //    beside it (CanStartPrefetchNow),
  //  - a path that is already the current request — or already queued/running
  //    as a prefetch with the same edge — is dropped instead of decoded twice.
  //
  // The queue is replaced even while a current request is pending: the plan of
  // "what comes next" is not work, and dropping it was the second half of the
  // broken chain (V3.1-01 A).
  void Prefetch(const std::vector<std::filesystem::path>& paths,
                uint32_t max_edge = 0);

  // V3.1-01 B: the app reports a navigation burst (two or more navigations
  // within 400 ms) so one tier prefetch may start beside the current decode.
  // Leaves the gate untouched otherwise; notifies waiting workers on a change.
  void SetBurstMode(bool on);
  bool BurstMode() const { return burst_.load(std::memory_order_acquire); }

  // V3.1-02: true when this exact key is already being decoded (as a prefetch,
  // possibly already promoted to the current request). A sharper frame is then
  // imminent, so a placeholder frame would only add a blur — the transition path
  // uses this to stay out of the way of the V3.1-01 chain.
  bool IsPrefetchInFlight(const std::filesystem::path& path, uint32_t max_edge);

  // Optional display-tier disk cache for prefetch (owned by the app). Thread
  // safe for concurrent Load(); the writer publishes atomically.
  void SetTierCache(class DiskCache* cache) { tier_cache_ = cache; }

  void CancelAll();
  void Shutdown();

  uint64_t CurrentId() const { return current_id_.load(std::memory_order_acquire); }
  // Number of prefetch decodes a worker is currently running. Used by the
  // V3.1-01 unit tests to observe the gate without racing on completions.
  size_t PrefetchInFlight();
  // Epoch of the newest queued/running preview, so the UI can drop a stale
  // completion from a previous preview request (規劃/22/13 坑 3).
  uint64_t CurrentPreviewId() const {
    return preview_id_.load(std::memory_order_acquire);
  }
  bool Stopping() const { return stopping_.load(std::memory_order_acquire); }

 private:
  struct PendingRequest {
    uint64_t id = 0;
    std::filesystem::path path;
    bool prefetch = false;
    uint32_t max_edge = 0;
    // V3.1-01 C: when a current Request() hits this already-running prefetch,
    // it records its own request id here. The worker then posts the result as
    // that current completion instead of decoding the same pixels twice.
    uint64_t promote_to_current = 0;
  };

  void WorkerLoop();
  void PreviewWorkerLoop();
  // True when another prefetch may start: the queue is non-empty and fewer
  // than the per-kind cap are decoding. A full-frame prefetch keeps the
  // V3-04-era limit of one (two workers of memory went into it), a tier
  // prefetch may use two because a tier is ~4 MB (規劃/25 §7.13-1).
  bool CanStartPrefetchLocked() const;
  void FinishPrefetch(uint64_t id);
  void FinishCurrent(uint64_t id);

  HWND completion_window_ = nullptr;
  UINT completion_message_ = 0;
  UINT preview_message_ = 0;

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<PendingRequest> current_queue_;
  std::deque<PendingRequest> prefetch_queue_;
  // At most one preview is meaningful; a new RequestPreview() replaces the
  // queued one instead of growing a backlog.
  std::deque<PendingRequest> preview_queue_;
  // Prefetch bookkeeping (規劃/25 §7.13). Guarded by mutex_. The in-flight list
  // is what makes "this neighbour is already decoding" detectable, so a
  // navigation onto it does not start a second decode of the same key.
  std::vector<PendingRequest> prefetch_in_flight_;
  // Id of the newest Request() that is queued or running; 0 = none. Prefetch
  // work is deferred while this is non-zero (規劃/25 §7.13-1).
  uint64_t pending_current_id_ = 0;
  std::vector<std::thread> workers_;
  std::thread preview_worker_;
  class DiskCache* tier_cache_ = nullptr;

  std::atomic<uint64_t> next_id_{1};
  std::atomic<uint64_t> current_id_{0};
  std::atomic<uint64_t> preview_id_{0};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> burst_{false};
};

}  // namespace nv
