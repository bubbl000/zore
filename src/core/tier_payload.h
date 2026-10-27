#pragma once

#include "core/image.h"

#include <cstdint>
#include <vector>

namespace nv {

// Compressed display-tier payload for the disk cache (規劃/25 §7.5).
//
// A tier is stored re-encoded, not as BGRA: raw BGRA costs 16 MB for the 2048
// tier (~60 entries in 1 GB), while a JPEG/PNG stream costs ~0.3–2 MB
// (~1500 entries), so this keeps the cache small for the same memory budget.
//
// JPEG q92 for opaque sources, PNG for sources with alpha. The plan named WebP
// q90 for alpha; PNG is used instead because the WIC WebP *decoder* is not
// guaranteed to be installed (Windows ships it as a Store extension), and PNG is
// lossless for PNG/PSD sources. Raw BGRA stays available as the fallback when an
// encode fails.
enum class TierCodec : uint32_t {
  kBgra = 0,
  kJpeg = 1,
  kPng = 2,
};

inline constexpr int kTierJpegQuality = 92;

// Encodes `image` (BGRA8, straight alpha) into `payload` and reports which codec
// was used. Returns false when no payload could be produced; the caller then
// stores the frame as kBgra or skips the write.
bool EncodeTierPayload(const Image& image, TierCodec* codec,
                       std::vector<uint8_t>* payload);

}  // namespace nv
