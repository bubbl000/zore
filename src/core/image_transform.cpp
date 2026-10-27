#include "core/image_transform.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace nv {
namespace {

bool ValidOrientation(uint16_t orientation) {
  return orientation >= 1 && orientation <= 8;
}

}  // namespace

bool OrientationSwapsAxes(uint16_t orientation) {
  return orientation >= 5 && orientation <= 8;
}

std::vector<uint8_t> TransformBgra(const uint8_t* source, uint32_t width,
                                   uint32_t height, uint32_t stride,
                                   uint16_t orientation, uint32_t* out_width,
                                   uint32_t* out_height) {
  if (out_width == nullptr || out_height == nullptr || source == nullptr ||
      width == 0 || height == 0 || stride < width * 4u ||
      !ValidOrientation(orientation)) {
    return {};
  }

  const uint32_t dst_width = OrientationSwapsAxes(orientation) ? height : width;
  const uint32_t dst_height = OrientationSwapsAxes(orientation) ? width : height;
  const uint32_t dst_stride = dst_width * 4u;
  std::vector<uint8_t> destination(
      static_cast<size_t>(dst_stride) * dst_height);

  for (uint32_t y = 0; y < dst_height; ++y) {
    uint8_t* row = destination.data() + static_cast<size_t>(y) * dst_stride;
    for (uint32_t x = 0; x < dst_width; ++x) {
      uint32_t sx = x;
      uint32_t sy = y;
      switch (orientation) {
        case 2:  // Mirror horizontal.
          sx = width - 1 - x;
          break;
        case 3:  // Rotate 180.
          sx = width - 1 - x;
          sy = height - 1 - y;
          break;
        case 4:  // Mirror vertical.
          sy = height - 1 - y;
          break;
        case 5:  // Transpose (mirror horizontal + rotate 270 CW).
          sx = y;
          sy = x;
          break;
        case 6:  // Rotate 90 CW.
          sx = y;
          sy = height - 1 - x;
          break;
        case 7:  // Transverse (mirror horizontal + rotate 90 CW).
          sx = width - 1 - y;
          sy = height - 1 - x;
          break;
        case 8:  // Rotate 270 CW (90 CCW).
          sx = width - 1 - y;
          sy = x;
          break;
        default:  // 1: identity.
          break;
      }
      std::memcpy(row + static_cast<size_t>(x) * 4u,
                  source + static_cast<size_t>(sy) * stride +
                      static_cast<size_t>(sx) * 4u,
                  4);
    }
  }

  *out_width = dst_width;
  *out_height = dst_height;
  return destination;
}

void ApplyOrientation(Image* image, uint16_t orientation) {
  if (image == nullptr || image->pixels.empty() ||
      !ValidOrientation(orientation) || orientation == 1) {
    return;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> upright = TransformBgra(
      image->pixels.data(), image->info.width, image->info.height,
      image->info.stride, orientation, &width, &height);
  if (upright.empty()) return;

  // full_* describe the original file; swap them for 90-degree transforms so
  // they remain expressed in the upright coordinate system.
  if (OrientationSwapsAxes(orientation)) {
    std::swap(image->info.full_width, image->info.full_height);
  }
  image->pixels = std::move(upright);
  image->info.width = width;
  image->info.height = height;
  image->info.stride = width * 4u;
  image->info.orientation_applied = true;
}

std::vector<uint8_t> RotateBgra90(const uint8_t* source, uint32_t width,
                                  uint32_t height, uint32_t stride, int steps,
                                  uint32_t* out_width, uint32_t* out_height) {
  if (out_width == nullptr || out_height == nullptr || source == nullptr ||
      width == 0 || height == 0 || stride < width * 4u) {
    return {};
  }
  const int turns = ((steps % 4) + 4) % 4;
  const bool swap = (turns & 1) != 0;
  const uint32_t dst_width = swap ? height : width;
  const uint32_t dst_height = swap ? width : height;
  const uint32_t dst_stride = dst_width * 4u;
  std::vector<uint8_t> destination(
      static_cast<size_t>(dst_stride) * dst_height);

  for (uint32_t dy = 0; dy < dst_height; ++dy) {
    uint8_t* row = destination.data() + static_cast<size_t>(dy) * dst_stride;
    for (uint32_t dx = 0; dx < dst_width; ++dx) {
      uint32_t sx = dx;
      uint32_t sy = dy;
      switch (turns) {
        case 1:  // 90 clockwise: dst(dx,dy) <- src(dy, height-1-dx)
          sx = dy;
          sy = height - 1 - dx;
          break;
        case 2:  // 180
          sx = width - 1 - dx;
          sy = height - 1 - dy;
          break;
        case 3:  // 90 counter-clockwise
          sx = width - 1 - dy;
          sy = dx;
          break;
        default:
          break;
      }
      std::memcpy(row + static_cast<size_t>(dx) * 4u,
                  source + static_cast<size_t>(sy) * stride +
                      static_cast<size_t>(sx) * 4u,
                  4);
    }
  }

  *out_width = dst_width;
  *out_height = dst_height;
  return destination;
}

void RotateImageForView(Image* image, int steps) {
  if (image == nullptr || image->pixels.empty() || image->info.width == 0 ||
      image->info.height == 0) {
    return;
  }
  const int turns = ((steps % 4) + 4) % 4;
  if (turns == 0) return;

  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rotated =
      RotateBgra90(image->pixels.data(), image->info.width, image->info.height,
                   image->info.stride, turns, &width, &height);
  if (rotated.empty()) return;

  if ((turns & 1) != 0) {
    std::swap(image->info.full_width, image->info.full_height);
  }
  image->pixels = std::move(rotated);
  image->info.width = width;
  image->info.height = height;
  image->info.stride = width * 4u;
}

std::shared_ptr<const Image> MakeExportImage(
    const std::shared_ptr<const Image>& source, int rotation) {
  if (!source || source->Empty()) return source;
  const int turns = ((rotation % 4) + 4) % 4;
  if (turns == 0 && source->frames.empty()) return source;

  auto copy = std::make_shared<Image>(*source);
  // Exports are single frames; a whole animation must not travel with them.
  copy->frames.clear();
  copy->loop_count = 0;
  RotateImageForView(copy.get(), turns);
  return copy;
}

}  // namespace nv
