#pragma once

#include "core/format_router.h"

#include <filesystem>

namespace nv::gif {

// Decodes an animated GIF into composited BGRA frames. When
// DecodeOptions.animation is false, or a thumbnail size is requested, only the
// first frame is returned (frames stays empty).
DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options);

}  // namespace nv::gif
