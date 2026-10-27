#pragma once

#include "core/image.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace nv {

// EXIF orientations 5..8 rotate the image by 90 degrees, so width and height
// swap.
bool OrientationSwapsAxes(uint16_t orientation);

// Returns an upright BGRA8 copy according to the EXIF orientation (1..8).
// `stride` is the source row pitch in bytes. Returns an empty vector when the
// arguments are invalid; orientation 1 still returns a copy.
std::vector<uint8_t> TransformBgra(const uint8_t* source, uint32_t width,
                                   uint32_t height, uint32_t stride,
                                   uint16_t orientation, uint32_t* out_width,
                                   uint32_t* out_height);

// Bakes the orientation into `image` (updates dimensions/stride/pixels).
// No-op for orientation 1 (or invalid values).
void ApplyOrientation(Image* image, uint16_t orientation);

// View rotation for "save as"/"copy image": rotates a BGRA8 buffer by `steps`
// clockwise quarter turns (0..3). Returns an empty vector for invalid input;
// steps 0 still returns a copy.
std::vector<uint8_t> RotateBgra90(const uint8_t* source, uint32_t width,
                                  uint32_t height, uint32_t stride, int steps,
                                  uint32_t* out_width, uint32_t* out_height);

// Applies the view rotation to `image` in place (pixels, width/height/stride,
// full_*). Used only when exporting the current view; the decoder and caches
// keep the upright pixels (規劃/22/05 §3.7).
void RotateImageForView(Image* image, int steps);

// Export copy of the current view (規劃/22/05 §3.7, 22/14 §4): drops the
// animation frames and returns either the source itself (rotation 0, no copy)
// or a rotated deep copy. Used by save/copy/wallpaper and by the background
// save / size-estimate workers so neither touches the displayed image.
std::shared_ptr<const Image> MakeExportImage(
    const std::shared_ptr<const Image>& source, int rotation);

}  // namespace nv
