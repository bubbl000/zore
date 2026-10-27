#include "ui/minimap.h"

#include "render/view_transform.h"
#include "ui/draw_util.h"
#include "ui/metrics.h"
#include "ui/theme_tokens.h"
#include "ui/ui_context.h"

#include <windows.h>
#include <d2d1_1.h>

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

// Rect of `w` x `h` centred inside `area`.
Rect Centred(const Rect& area, float w, float h) {
  return Rect::FromXYWH(area.left + (area.width() - w) * 0.5f,
                        area.top + (area.height() - h) * 0.5f, w, h);
}

}  // namespace

MinimapLayout ComputeMinimapLayout(const MinimapInputs& inputs) {
  const float s = Scale(inputs.dpi_scale);
  MinimapLayout layout;

  if (inputs.display_width == 0 || inputs.display_height == 0) return layout;
  if (inputs.view_scale <= 0.0f) return layout;
  if (inputs.client.empty() || inputs.content.empty()) return layout;

  const float dw = static_cast<float>(inputs.display_width);
  const float dh = static_cast<float>(inputs.display_height);
  const float pad = metrics::kMinimapPadding * s;

  // Never own more than about a third of the picture area: the minimap is a
  // navigation aid, not a second picture.
  const float max_panel_w = std::min(metrics::kMinimapMaxWidth * s,
                                     inputs.content.width() * 0.34f);
  const float max_panel_h = std::min(metrics::kMinimapMaxHeight * s,
                                     inputs.content.height() * 0.42f);
  const float inner_max_w = max_panel_w - 2.0f * pad;
  const float inner_max_h = max_panel_h - 2.0f * pad;
  if (inner_max_w <= 0.0f || inner_max_h <= 0.0f) return layout;

  // Fit the *rotated* aspect ratio: an odd quarter turn already swapped the
  // display size the caller passed, so the panel follows the picture.
  const float fit = std::min(inner_max_w / dw, inner_max_h / dh);
  const float canvas_w = dw * fit;
  const float canvas_h = dh * fit;
  if (canvas_w < metrics::kMinimapMinWidth * s ||
      canvas_h < metrics::kMinimapMinHeight * s) {
    return layout;
  }

  const float panel_w = canvas_w + 2.0f * pad;
  const float panel_h = canvas_h + 2.0f * pad;

  // Anchor: directly above the rail slot (also when the rail is faded out),
  // otherwise above the bottom bar, otherwise at the bottom edge (fullscreen).
  float anchor_top = inputs.client.bottom;
  float gap = metrics::kMinimapMargin * s;
  if (!inputs.bottom_bar.empty()) anchor_top = inputs.bottom_bar.top;
  if (!inputs.strip.empty()) anchor_top = inputs.strip.top;
  if (!inputs.strip.empty() || !inputs.bottom_bar.empty()) {
    gap = metrics::kMinimapGap * s;
  }

  const float margin = metrics::kMinimapMargin * s;
  const float right = inputs.content.right - margin;
  const float bottom = anchor_top - gap;
  float left = right - panel_w;
  float top = bottom - panel_h;

  // Slide up rather than hanging over the bar on a very short window.
  if (top < inputs.content.top + margin) {
    top = inputs.content.top + margin;
  }
  if (left < inputs.content.left + margin) return layout;
  if (top + panel_h > inputs.content.bottom) return layout;

  layout.visible = true;
  layout.display_width = inputs.display_width;
  layout.display_height = inputs.display_height;
  layout.panel = Rect::FromLTRB(left, top, right, top + panel_h);
  layout.canvas = layout.panel.Inset(pad, pad);
  // The canvas was fitted to the display aspect, so the photo fills it exactly.
  layout.image = layout.canvas;
  layout.display_per_canvas =
      dw / std::max(1.0f, static_cast<float>(layout.image.width()));

  // Visible part of the picture: map the content rectangle into display space
  // through the view centre/scale, then clip to the picture. A quarter turn is
  // an axis-aligned mapping, so one rectangle is enough for all four angles.
  const float vs = inputs.view_scale;
  const float x0 = dw * 0.5f + (inputs.content.left - inputs.view_center_x) / vs;
  const float x1 = dw * 0.5f + (inputs.content.right - inputs.view_center_x) / vs;
  const float y0 = dh * 0.5f + (inputs.content.top - inputs.view_center_y) / vs;
  const float y1 = dh * 0.5f + (inputs.content.bottom - inputs.view_center_y) / vs;
  const Rect visible =
      Rect::FromLTRB(std::max(0.0f, x0), std::max(0.0f, y0),
                     std::min(dw, x1), std::min(dh, y1));
  if (!visible.empty()) {
    layout.view_display = visible;
    const float sx = layout.image.width() / dw;
    const float sy = layout.image.height() / dh;
    layout.viewport = Rect::FromLTRB(
        layout.image.left + visible.left * sx,
        layout.image.top + visible.top * sy,
        layout.image.left + visible.right * sx,
        layout.image.top + visible.bottom * sy);
  }
  return layout;
}

void MinimapDragPan(const MinimapLayout& layout, float view_scale, float dx,
                    float dy, float* pan_x, float* pan_y) {
  if (pan_x != nullptr) *pan_x = 0.0f;
  if (pan_y != nullptr) *pan_y = 0.0f;
  if (!layout.visible || layout.image.empty() || view_scale <= 0.0f) return;
  // Canvas pixels -> display pixels -> client pixels. The sign is negative: the
  // frame moves right, the picture moves left.
  const float k = layout.display_per_canvas * view_scale;
  if (pan_x != nullptr) *pan_x = -dx * k;
  if (pan_y != nullptr) *pan_y = -dy * k;
}

bool MinimapDisplayPoint(const MinimapLayout& layout, float x, float y,
                         float* display_x, float* display_y) {
  if (display_x != nullptr) *display_x = 0.0f;
  if (display_y != nullptr) *display_y = 0.0f;
  if (!layout.visible || layout.image.empty()) return false;
  if (layout.display_width == 0 || layout.display_height == 0) return false;
  if (!layout.image.Contains(x, y)) return false;
  if (display_x != nullptr) {
    *display_x = (x - layout.image.left) / layout.image.width() *
                 static_cast<float>(layout.display_width);
  }
  if (display_y != nullptr) {
    *display_y = (y - layout.image.top) / layout.image.height() *
                 static_cast<float>(layout.display_height);
  }
  return true;
}

void DrawMinimap(ID2D1DeviceContext* context, UiContext& ui,
                 const MinimapLayout& layout, ID2D1Bitmap* photo, int rotation,
                 const ColorTokens& colors) {
  if (context == nullptr || !layout.visible || layout.panel.empty()) return;
  const float s = Scale(ui.dpi_scale());

  draw::FillRoundRect(context, ui, layout.panel, metrics::kMinimapRadius * s,
                      WithAlpha(colors.bg_panel, 0.94f));
  draw::StrokeRoundRect(context, ui, layout.panel, metrics::kMinimapRadius * s,
                        std::max(1.0f, s),
                        WithAlpha(colors.border_strong, 0.85f));

  if (layout.canvas.empty() || layout.image.empty()) return;

  if (photo != nullptr) {
    // Clip in client space (identity transform) exactly like the image layer, so
    // a rotated photo can never bleed over the rounded panel.
    context->PushAxisAlignedClip(draw::ToD2D(layout.canvas),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
    const int steps = nv::render::NormalizeRotation(rotation);
    const float dw = static_cast<float>(layout.display_width);
    const float dh = static_cast<float>(layout.display_height);
    const float mini_scale =
        layout.image.width() / std::max(1.0f, dw);
    // The photo is unrotated; an odd quarter turn swaps its source size so the
    // rotated quad matches the canvas the caller sized from the display aspect.
    const float src_w = (steps & 1) != 0 ? dh : dw;
    const float src_h = (steps & 1) != 0 ? dw : dh;
    const Rect dest = Centred(layout.image, src_w * mini_scale,
                              src_h * mini_scale);
    if (steps != 0) {
      const float cx = (layout.image.left + layout.image.right) * 0.5f;
      const float cy = (layout.image.top + layout.image.bottom) * 0.5f;
      const nv::render::ViewMatrix matrix =
          nv::render::RotationAbout(cx, cy, steps);
      context->SetTransform(D2D1::Matrix3x2F(
          matrix.m11, matrix.m12, matrix.m21, matrix.m22, matrix.dx,
          matrix.dy));
    }
    const D2D1_RECT_F photo_rect = draw::ToD2D(dest);
    context->DrawBitmap(photo, &photo_rect, 1.0f,
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    if (steps != 0) context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->PopAxisAlignedClip();
  } else {
    // Bake still pending or failed: show the frame on the canvas colour rather
    // than a hole in the picture.
    draw::FillRect(context, ui, layout.canvas, colors.bg_canvas);
  }

  // A frame that covers the whole photo would only double the panel border and
  // tint the picture, so it is omitted at fit.
  const bool frame_useful =
      !layout.viewport.empty() &&
      (layout.viewport.width() < layout.image.width() - 1.0f ||
       layout.viewport.height() < layout.image.height() - 1.0f);
  if (frame_useful) {
    draw::FillRect(context, ui, layout.viewport,
                   WithAlpha(colors.accent, 0.12f));
    if (ID2D1SolidColorBrush* brush =
            ui.Brush(context, WithAlpha(colors.accent, 0.95f));
        brush != nullptr) {
      context->DrawRectangle(
          draw::ToD2D(layout.viewport), brush,
          std::max(1.0f, metrics::kMinimapFrameStroke * s));
    }
  }
}

}  // namespace nv::ui
