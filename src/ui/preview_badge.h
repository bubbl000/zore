#pragma once

#include "ui/rect.h"
#include "ui/theme_tokens.h"

#include <d2d1_1.h>

#include <string>

namespace nv::ui {

class UiContext;

// "Preview" badge of the two-stage loading path (規劃/22/13 §3.5):
//
//     ┌──────────────┐
//     │   预览        │      top-right corner of the image area
//     └──────────────┘
//
// It tells the user that the sharp frame is still decoding. It is purely
// informational: no hit rectangle, so panning and navigation below it work.
struct PreviewBadgeLayout {
  bool visible = false;
  Rect badge;
};

// `text_width` is the measured width of the localized label in pixels. The
// badge is right-aligned inside `content` with a 12 DIP margin and grows with
// the text (clamped), so a translated label never clips.
PreviewBadgeLayout ComputePreviewBadgeLayout(const Rect& content,
                                             float dpi_scale,
                                             float text_width);

struct PreviewBadgeStrings {
  std::wstring text;
  bool failed = false;
  // 1.0 while the preview is the best frame available; fades to 0 over 160 ms
  // once the full image is shown.
  float opacity = 1.0f;
};

void DrawPreviewBadge(ID2D1DeviceContext* context, UiContext& ui,
                      const PreviewBadgeLayout& layout,
                      const PreviewBadgeStrings& strings,
                      const ColorTokens& colors, bool high_contrast,
                      float dpi_scale);

}  // namespace nv::ui
