#pragma once

#include <cmath>

namespace nv::ui {

// Drag-out trigger policy (規劃/22/19 §3.1, revised by V3.1-06 / 規劃/v3.1/06
// §06b). Pure so the rules are testable without a window or OLE.
//
// v2.0 rule: the image area needed Ctrl (plain drag is panning), the thumbnail
// rail dragged without a modifier. The rail drag was a trap — a slightly slow
// click on a thumbnail started a file drag instead of opening the picture — so
// V3.1-06 dropped it. On the picture the gesture is now "leave the window":
// dragging the pointer 16 DIP outside the client area starts the file drag, and
// Ctrl remains as the immediate shortcut for users who knew the old gesture.
inline constexpr float kDragOutThresholdDip = 4.0f;

// How far outside the client rectangle the pointer must be (DIP) before a plain
// left press on the picture turns into a file drag.
inline constexpr float kDragOutLeaveDip = 16.0f;

struct DragOutRequest {
  // The press started on the image area.
  bool image_area = false;
  // The press started on a thumbnail slot. Kept so the rail's click path still
  // reports what it was; it can never start a drag any more (V3.1-06).
  bool strip_item = false;
  // Ctrl was held when the press started.
  bool ctrl = false;
  // Client-pixel movement since the press.
  float dx = 0.0f;
  float dy = 0.0f;
  // DPI-scaled threshold in client pixels (4 DIP).
  float threshold = kDragOutThresholdDip;
  // DIP the pointer is outside the client rectangle (0 or less while inside).
  float outside_dip = 0.0f;
};

inline bool ExceedsDragThreshold(float dx, float dy, float threshold) {
  if (!(threshold > 0.0f)) threshold = kDragOutThresholdDip;
  return dx * dx + dy * dy >= threshold * threshold;
}

inline bool ShouldStartDragOut(const DragOutRequest& request) {
  // V3.1-06: the rail is not a drag-out source. A press there is only a
  // deferred click, so a slow click can no longer turn into a file drag.
  if (!request.image_area) return false;
  if (!ExceedsDragThreshold(request.dx, request.dy, request.threshold)) {
    return false;
  }
  // Ctrl keeps the old immediate gesture; otherwise the pointer must have left
  // the window, so a normal pan can never start a file drag while the picture
  // is still on screen.
  return request.ctrl || request.outside_dip >= kDragOutLeaveDip;
}

}  // namespace nv::ui
