#pragma once

#include <vector>

namespace nv::platform {

// Pure geometry for "remember window position/size" (規劃/22/15 §3).
//
// The HWND work stays in the app: these helpers only answer "may this
// rectangle be restored?" and "where does it end up on the current monitor
// layout?". That keeps the multi-monitor edge cases (negative coordinates,
// unplugged screens, DPI changes, oversized saved rectangles) testable without
// a desktop.
struct IntRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

// True when `rect` overlaps one of `work_areas` by at least
// `min_visible_width` x `min_visible_height` physical pixels. Monitor
// coordinates may be negative: nothing here assumes the primary monitor is at
// 0,0 (規劃/22/15 坑 15).
bool PlacementVisible(const IntRect& rect,
                      const std::vector<IntRect>& work_areas,
                      int min_visible_width = 100,
                      int min_visible_height = 100);

// Moves `rect` fully inside the work area it overlaps most (the nearest area
// when it overlaps none) and clamps the size to that area. Never returns a
// zero/negative rectangle for a sane work area.
IntRect ClampPlacement(const IntRect& rect,
                       const std::vector<IntRect>& work_areas,
                       int min_width = 640,
                       int min_height = 480);

// Centres a `width` x `height` window in `work_area`.
IntRect CenterInWorkArea(const IntRect& work_area, int width, int height);

// Fullscreen and minimized windows must not overwrite the stored placement
// (規劃/22/15 坑 1/2/3): the restored rectangle would otherwise become the
// fullscreen monitor or the -32000 minimized position.
bool ShouldSavePlacement(bool remember_window, bool fullscreen,
                         bool minimized);

}  // namespace nv::platform
