#pragma once

#include "core/format_router.h"

#include <cstdint>

namespace nv {

// Two-stage loading (規劃/22/13 §3.1): decide whether a slow format should show
// a low-resolution preview before the full decode finishes.
//
// The policy is a pure function of the format, the file size, the decoded pixel
// count and whether the shared thumbnail is already cached, so the rule can be
// unit-tested without a window, a decoder or a sample file. Previewing a normal
// JPEG would only add a visible "blurry flash" (坑 1), so a large JPEG is only
// previewed when its thumbnail is already available.
struct PreviewPlan {
  // The shared thumbnail cache can serve a preview immediately (on the UI
  // thread, ~1 ms).
  bool cache = false;
  // RAW only: decode the camera-embedded JPEG on the preview worker when the
  // cache has nothing (規劃/22/13 §3.3).
  bool embedded = false;

  bool any() const { return cache || embedded; }
};

// A large file may be slow even when its pixel count is modest.
inline constexpr uint64_t kPreviewMinFileBytes = 16ull * 1024 * 1024;
// A 24 MP JPEG can be ~12 MB, below the old 32 MB threshold; the pixel count
// catches it without reading the whole file.
inline constexpr uint64_t kPreviewMinPixels = 8ull * 1000 * 1000;
// Probing costs ~1-10 ms (header only); do not pay it for small files.
inline constexpr uint64_t kPreviewProbeMinFileBytes = 4ull * 1024 * 1024;

// Animated sources are excluded: their first frame is already fast and a
// preview would make the playback start late (坑 14). `thumb_cached` gates the
// cache preview for ordinary formats; RAW/PSD keep their previous policy.
PreviewPlan PlanPreview(ImageFormat format, uint64_t file_bytes,
                        uint64_t pixels, bool thumb_cached);

}  // namespace nv
