#include "core/preview_plan.h"

namespace nv {

PreviewPlan PlanPreview(ImageFormat format, uint64_t file_bytes,
                        uint64_t pixels, bool thumb_cached) {
  // GIF/APNG/WebP animation: never preview (規劃/22/13 坑 14). Animated WebP is
  // routed as kWebp and cannot be told apart without parsing the container, so
  // the whole family is skipped; the first frame is cheap either way.
  if (format == ImageFormat::kGif || format == ImageFormat::kApng ||
      format == ImageFormat::kWebp) {
    return {};
  }

  // RAW is always slow (1.1-2.7 s) and has a camera-embedded preview that
  // arrives in well under 200 ms (規劃/22/13 §3.1/§3.3).
  if (format == ImageFormat::kRaw) return PreviewPlan{true, true};

  // PSD/PSB: the merged-image decode is usually fast, but a large layered file
  // can take hundreds of milliseconds, so the cached thumbnail is used when it
  // exists and nothing is decoded otherwise (規劃/22/13 §3.1).
  if (format == ImageFormat::kPsd) return PreviewPlan{true, false};

  // Ordinary formats: a 24 MP JPEG can be only ~12 MB, so the old 32 MB size
  // test misses it. Require an existing thumbnail so the first open does not
  // flash a blurry frame or queue a redundant thumbnail job.
  if (!thumb_cached) return {};
  if (pixels >= kPreviewMinPixels || file_bytes >= kPreviewMinFileBytes) {
    return PreviewPlan{true, false};
  }

  return {};
}

}  // namespace nv
