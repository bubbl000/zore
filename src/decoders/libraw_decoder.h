#pragma once

#include "core/exif.h"
#include "core/format_router.h"

#include <cstdint>
#include <filesystem>

namespace nv::libraw {

bool Available();

// LibRaw dcraw flip code (0..7, or 90/180/270) -> EXIF orientation 1..8.
uint16_t ExifOrientationFromFlip(int flip);
bool FlipSwapsImageAxes(int flip);

ProbeResult Probe(const std::filesystem::path& path);
DecodeResult Decode(const std::filesystem::path& path,
                    const DecodeOptions& options);

// Two-stage loading (規劃/22/13 §3.3): unpacks the camera-embedded JPEG preview
// (libraw_unpack_thumb) and decodes it through WIC in memory. Usually available
// in well under 200 ms, which is what makes a RAW file usable while the full
// demosaic runs. The main image's orientation is baked in so the preview and
// the final frame are upright in the same way. Returns an error (never throws)
// when the file has no usable preview; the caller then falls back to the
// thumbnail cache or to the single-stage path.
DecodeResult DecodeEmbeddedPreview(const std::filesystem::path& path);

// Metadata-only read for the info panel (規劃/22/06): opens the file, copies
// the camera tags and closes it without unpacking raw pixels. Serialized with
// the decoders because LibRaw keeps process-global tables.
ExifData ReadExif(const std::filesystem::path& path);

}  // namespace nv::libraw
