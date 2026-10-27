#include "platform/window_placement.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace nv::platform {
namespace {

int64_t IntersectionArea(const IntRect& a, const IntRect& b) {
  const int width = std::min(a.right, b.right) - std::max(a.left, b.left);
  const int height = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
  if (width <= 0 || height <= 0) return 0;
  return static_cast<int64_t>(width) * height;
}

int64_t CenterDistanceSquared(const IntRect& a, const IntRect& b) {
  const int64_t ax = static_cast<int64_t>(a.left) + a.right;
  const int64_t ay = static_cast<int64_t>(a.top) + a.bottom;
  const int64_t bx = static_cast<int64_t>(b.left) + b.right;
  const int64_t by = static_cast<int64_t>(b.top) + b.bottom;
  const int64_t dx = ax - bx;
  const int64_t dy = ay - by;
  return dx * dx + dy * dy;
}

int ClampLength(int value, int minimum, int maximum) {
  if (maximum <= 0) return std::max(1, value);
  const int lower = std::min(minimum, maximum);
  return std::clamp(value, std::max(1, lower), maximum);
}

}  // namespace

bool PlacementVisible(const IntRect& rect,
                      const std::vector<IntRect>& work_areas,
                      int min_visible_width, int min_visible_height) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
  for (const IntRect& area : work_areas) {
    const int width = std::min(rect.right, area.right) - std::max(rect.left, area.left);
    const int height =
        std::min(rect.bottom, area.bottom) - std::max(rect.top, area.top);
    if (width >= min_visible_width && height >= min_visible_height) {
      return true;
    }
  }
  return false;
}

IntRect ClampPlacement(const IntRect& rect,
                       const std::vector<IntRect>& work_areas,
                       int min_width, int min_height) {
  if (work_areas.empty()) return rect;

  const IntRect* best = &work_areas.front();
  int64_t best_area = -1;
  int64_t best_distance = (std::numeric_limits<int64_t>::max)();
  for (const IntRect& area : work_areas) {
    const int64_t overlap = IntersectionArea(rect, area);
    if (overlap > best_area) {
      best = &area;
      best_area = overlap;
    } else if (overlap == best_area && overlap > 0) {
      // Two screens can be mirrored; either is fine, keep the first.
      continue;
    }
  }
  if (best_area <= 0) {
    // Off every screen (for example the monitor was unplugged): use the one
    // whose centre is nearest, matching MonitorFromRect's behaviour.
    for (const IntRect& area : work_areas) {
      const int64_t distance = CenterDistanceSquared(rect, area);
      if (distance < best_distance) {
        best = &area;
        best_distance = distance;
      }
    }
  }

  const IntRect work = *best;
  if (work.right <= work.left || work.bottom <= work.top) return rect;

  const int width = ClampLength(rect.right - rect.left, min_width,
                                work.right - work.left);
  const int height = ClampLength(rect.bottom - rect.top, min_height,
                                 work.bottom - work.top);
  IntRect result{};
  result.left = std::clamp(rect.left, work.left, work.right - width);
  result.top = std::clamp(rect.top, work.top, work.bottom - height);
  result.right = result.left + width;
  result.bottom = result.top + height;
  return result;
}

IntRect CenterInWorkArea(const IntRect& work_area, int width, int height) {
  IntRect result{};
  if (work_area.right <= work_area.left || work_area.bottom <= work_area.top) {
    return result;
  }
  const int work_width = work_area.right - work_area.left;
  const int work_height = work_area.bottom - work_area.top;
  const int w = std::clamp(width, 1, work_width);
  const int h = std::clamp(height, 1, work_height);
  result.left = work_area.left + (work_width - w) / 2;
  result.top = work_area.top + (work_height - h) / 2;
  result.right = result.left + w;
  result.bottom = result.top + h;
  return result;
}

bool ShouldSavePlacement(bool remember_window, bool fullscreen,
                         bool minimized) {
  return remember_window && !fullscreen && !minimized;
}

}  // namespace nv::platform
