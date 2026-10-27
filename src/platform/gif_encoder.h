#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nv::platform {

// Writes one frame as a GIF89a image (規劃/22/14 §4.1). GIF has no partial
// alpha: pixels with alpha < 128 become fully transparent, everything else is
// quantised to a 255-colour palette (the 256th index is the transparent one).
// FreeImage's Wu quantiser is used when the build links it; a deterministic
// 6x6x6 colour cube is the fallback for WIC-only builds.
bool EncodeGif(const uint8_t* bgra, uint32_t width, uint32_t height,
               uint32_t stride, std::vector<uint8_t>* output,
               std::wstring* error);

}  // namespace nv::platform
