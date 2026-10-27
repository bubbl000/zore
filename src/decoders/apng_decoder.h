#pragma once

#include "core/format_router.h"

#include <cstdint>
#include <filesystem>

namespace nv::apng {

struct Info {
  bool is_apng = false;
  uint32_t frames = 1;
  uint32_t plays = 0;  // 0 = loop forever.
};

// Cheap chunk scan: an acTL chunk before the first IDAT makes the file an APNG.
Info Inspect(const std::filesystem::path& path);

// Decodes every animation frame into composited BGRA frames (fcTL/fdAT,
// dispose/blend, delay, num_plays). When DecodeOptions.animation is false or a
// thumbnail size is requested only the default image is returned (WIC path),
// with frame_count/animated filled in from acTL.
DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options);

}  // namespace nv::apng
