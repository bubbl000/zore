#pragma once

#include "core/format_router.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace nv::webp {

// One ANMF record of an animated WebP. Only the fields we composite with are
// kept; the pixel data comes from the WIC frame of the same index (規劃/22/12
// §3.4 keeps the container parse for timing and layout, WIC for pixels).
struct AnimFrameInfo {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t duration_ms = 0;
  bool dispose_to_background = false;
  bool no_blend = false;
};

struct AnimInfo {
  bool animated = false;
  uint32_t canvas_width = 0;
  uint32_t canvas_height = 0;
  uint32_t loop_count = 0;  // 0 = loop forever
  std::vector<AnimFrameInfo> frames;
};

// Parses the RIFF/WEBP container (VP8X/ANIM/ANMF). Never decodes pixels, so a
// malformed payload leaves `animated=false` instead of failing a normal open.
AnimInfo Inspect(const std::filesystem::path& path);

// Decodes every frame into a composited BGRA canvas and returns the playable
// sequence. When animation is not requested (thumbnails/prefetch) only the WIC
// first frame is returned. On a container/codec problem the caller falls back
// to the first frame and the pill reports "first frame only".
DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options);

}  // namespace nv::webp
