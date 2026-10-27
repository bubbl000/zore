#pragma once

#include "core/image.h"

#include <cstdint>
#include <memory>

namespace nv {

// Display-tier chain (規劃/25 §7.1/§7.2/§7.5).
//
// A *display tier* is the same picture decoded (or scaled) to a bounded long
// edge, so the UI never needs the full 96 MB BGRA frame of a 24 MP photo just to
// draw it at 1280 px. The ladder is the one the 0.9 replica uses.
//
// The tier is always >= the content rectangle, so at fit the tier is never
// stretched; only the §7.6 upgrade path (V3-04b) ever needs the full frame.
inline constexpr uint32_t kDisplayTiers[] = {512,  768,  1024, 1280, 1600,
                                             2048, 2560, 3200, 4096};
inline constexpr uint32_t kDisplayTierMax = 4096;
inline constexpr uint32_t kDisplayTierMin = 512;

// Smallest tier >= `needed_long_edge`, clamped to [512, 4096]; 0 stays 0,
// which means "full frame, no tier".
uint32_t ChooseTier(uint32_t needed_long_edge);

// Which tier a *load* may use, as a pure decision (規劃/25 §7.3 + §7.6 + §7.8).
// Returns the tier edge, or 0 for "decode the full frame". The rules are the
// ones that were inline in app/main.cpp during V3-04 and are unit-tested here
// (規劃/25 §7.9-5) because getting one of them wrong is invisible until the
// user zooms in or exports:
//
//  - `full_frame_available` (§7.3 rule 3): once this session produced the full
//    frame of a file, a later load must never fall back to the blurry tier.
//  - `scale >= 1.0` (§7.6): a 100 % view needs real pixels, so the load goes
//    straight to the full frame instead of showing a tier and upgrading later.
//  - `tier >= source_long_edge`: the file is not bigger than the tier, so the
//    "tier" would be the full frame under a second key — decode it once.
//  - `force` (--display-cache=force, §7.8) skips the last two rules so a test
//    can prove the tier chain works on a small sample.
struct TierLoadInputs {
  uint32_t needed_long_edge = 0;  // content long edge, in file pixels
  float scale = 0.0f;             // client pixels per file pixel
  uint32_t source_long_edge = 0;  // real file long edge (header probe)
  bool full_frame_available = false;
  bool force = false;
};

uint32_t DecideTierEdge(const TierLoadInputs& in);

// --- V3.1-08: when does the tier stop being enough? -------------------------
//
// The tier on screen is a *display object*: once the requested view needs more
// pixels than it has, every screen pixel becomes an interpolation of it. V3-04b
// only upgraded at 100 % of the file, which left 50-99 % permanently soft. The
// a viewer may upgrade as soon as the tier would be upscaled (`need_edge >
// have_edge + 8`); this policy keeps that
// idea with three deliberate differences:
//
//  - a *ratio* (`kTierUpscaleLimit`) instead of +8 px, so a 1280 tier is not
//    replaced for a 1289-pixel need. 1.15 (was 1.25): at 25 % a 6000 px file
//    already needs more than a 1280 tier can give, and the user's complaint was
//    that the picture stayed soft *before* the threshold. The full decode is
//    cheap for JPEG (~290 ms) but costs 1.1-1.5 s for RAW, which is why RAW does
//    not use this branch at all;
//  - RAW keeps the 100 % rule: its preview feed is ~80 ms while a LibRaw full
//    decode is 1.1-1.5 s, so upgrading at 30 % would stall the machine for a
//    picture that is still sharp enough;
//  - a file-size gate, because a full frame is width*height*4 bytes and some
//    formats (PSD/TIFF) can be hundreds of MB (the replica's 120 MB rule,
//    shell_app_decode.cpp:811, tightened to 64 MB - our disk-cache limit).
inline constexpr float kTierUpscaleLimit = 1.15f;
inline constexpr uint64_t kFullDecodeFileLimitBytes = 64ull * 1024ull * 1024ull;

struct TierUpgradeInputs {
  uint32_t have_long_edge = 0;   // current bitmap's long edge
  uint32_t file_long_edge = 0;   // logical (file) long edge
  float file_scale = 0.0f;       // client pixels per file pixel
  bool pixel_mode = false;       // --force-quality=pixel / 1:1 pixel promise
  bool raw = false;              // camera RAW (preview-as-final path)
  uint64_t file_bytes = 0;       // 0 = unknown (the gate stays open)
};

// True when the frame on screen cannot serve the current view any more.
bool ShouldUpgradeToFullFrame(const TierUpgradeInputs& in);

// Smallest tier strictly greater than `edge`, or 0 when there is none. Used by
// the "look upward on a miss" rule (規劃/25 §7.3 rule 1): a larger tier is
// sharper and always acceptable, only more payload.
uint32_t NextTier(uint32_t edge);

// Largest tier strictly smaller than `edge`, or 0 when there is none.
// V3.1-02: a smaller tier that is already in memory is a *free* transition
// frame while the requested tier decodes — the same "show what you have"
// contract as the two-stage preview, without a decode (規劃/v3.1/01 §V3.1-02).
uint32_t PrevTier(uint32_t edge);

inline uint32_t ImageLongEdge(const Image& image) {
  return image.info.width > image.info.height ? image.info.width
                                              : image.info.height;
}

// Scales a BGRA8 frame down to `edge` with the WIC scaler (Fant), which is the
// same filter the decoder path uses for `max_edge` (wic_decoder.cpp). Returns
// nullptr when nothing can be done: empty frame, non-BGRA8 pixels, `edge == 0`,
// or `edge >=` the current long edge (a tier must not upscale).
//
// This is the guarantee behind "a tier image really is a tier": decoders that
// ignore `max_edge` (direct-composite paths, damaged containers that fall back
// to a whole-file decode) get one CPU pass here instead of silently shipping a
// 96 MB frame that the caller will cache as a small tier.
std::shared_ptr<Image> MakeDisplayTier(const Image& source, uint32_t edge);

}  // namespace nv
