#include "ui/anim_pill.h"

#include "ui/draw_util.h"
#include "ui/metrics.h"
#include "ui/ui_context.h"

#include <algorithm>

namespace nv::ui {
namespace {

constexpr int kWeightRegular = 400;
constexpr int kWeightSemibold = 600;

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

Rect VerticalCentre(const Rect& area, float size) {
  const float top = area.top + (area.height() - size) * 0.5f;
  return Rect::FromXYWH(area.left, top, size, size);
}

}  // namespace

AnimPillLayout ComputeAnimPillLayout(const Rect& pill, float dpi_scale,
                                     bool buttons) {
  const float s = Scale(dpi_scale);
  AnimPillLayout layout;
  if (pill.empty()) return layout;

  layout.visible = true;
  layout.interactive = buttons;
  layout.pill = pill;

  const float pad = metrics::kAnimPillPadding * s;
  const float inner_left = pill.left + pad;
  const float inner_right = pill.right - pad;
  if (!buttons) {
    layout.label =
        Rect::FromLTRB(inner_left, pill.top, inner_right, pill.bottom);
    return layout;
  }

  const float size = metrics::kAnimPillButton * s;
  const float gap = metrics::kAnimPillButtonGap * s;
  layout.prev = VerticalCentre(
      Rect::FromLTRB(inner_left, pill.top, inner_left + size, pill.bottom),
      size);
  layout.toggle = VerticalCentre(
      Rect::FromLTRB(inner_right - size, pill.top, inner_right, pill.bottom),
      size);
  layout.next = VerticalCentre(
      Rect::FromLTRB(layout.toggle.left - gap - size, pill.top,
                     layout.toggle.left - gap, pill.bottom),
      size);
  const float label_left = layout.prev.right + gap;
  const float label_right = std::max(label_left, layout.next.left - gap);
  layout.label = Rect::FromLTRB(label_left, pill.top, label_right, pill.bottom);
  return layout;
}

HitId HitTestAnimPill(const AnimPillLayout& layout, float x, float y) {
  if (!layout.visible || !layout.interactive) return HitId::kNone;
  if (layout.prev.Contains(x, y)) return HitId::kAnimPrev;
  if (layout.next.Contains(x, y)) return HitId::kAnimNext;
  if (layout.toggle.Contains(x, y)) return HitId::kAnimToggle;
  return HitId::kNone;
}

void DrawAnimPill(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                  const AnimPillLayout& layout, const AnimPillStrings& strings,
                  const ColorTokens& colors, bool high_contrast,
                  float dpi_scale, HitId hover, HitId pressed) {
  if (context == nullptr || !layout.visible) return;
  const float s = Scale(dpi_scale);
  // High contrast keeps the pill fully opaque with system colours so it stays
  // readable over any photo (規劃/22/12 坑 16).
  const float opacity =
      high_contrast ? 1.0f : std::clamp(strings.opacity, 0.0f, 1.0f);
  if (opacity <= 0.01f) return;

  const auto alpha = [&](uint32_t color, float factor) {
    return WithAlpha(color, std::clamp(factor, 0.0f, 1.0f));
  };

  // The degraded "first frame only" pill is a label, so it shrinks to its text
  // instead of showing a wide empty capsule. The layout rectangle stays the
  // full width (no buttons, so nothing is lost) and is what --layout-dump
  // reports; only the painted box shrinks.
  Rect pill = layout.pill;
  if (!strings.show_buttons) {
    const float text_width =
        ui.MeasureText(strings.label, metrics::kAnimPillFontSize,
                       kWeightSemibold);
    const float width = std::min(
        pill.width(),
        text_width + metrics::kAnimPillPadding * 2.0f * s);
    pill = Rect::FromXYWH(pill.left + (pill.width() - width) * 0.5f, pill.top,
                          width, pill.height());
  }

  draw::FillRoundRect(context, ui, pill, metrics::kAnimPillRadius * s,
                      alpha(colors.bg_panel, 0.94f * opacity));
  draw::StrokeRoundRect(context, ui, pill, metrics::kAnimPillRadius * s,
                        std::max(1.0f, s),
                        alpha(colors.border_subtle, 0.9f * opacity));

  const auto button = [&](const Rect& rect, HitId id, IconId icon) {
    if (rect.empty()) return;
    const bool is_pressed = pressed == id;
    const bool is_hover = hover == id;
    if (is_pressed) {
      draw::FillRoundRect(context, ui, rect, metrics::kRadiusControl * s,
                          alpha(colors.bg_pressed, opacity));
    } else if (is_hover) {
      draw::FillRoundRect(context, ui, rect, metrics::kRadiusControl * s,
                          alpha(colors.bg_hover, opacity));
    }
    const float icon_size = metrics::kAnimPillIconSize * s;
    const Rect box = VerticalCentre(
        Rect::FromLTRB(rect.left + (rect.width() - icon_size) * 0.5f, rect.top,
                       rect.left + (rect.width() + icon_size) * 0.5f,
                       rect.bottom),
        icon_size);
    icons.Draw(context, ui, icon, box,
               alpha(is_hover ? colors.accent : colors.text_primary, opacity),
               std::max(1.0f, 1.5f * s));
  };

  if (strings.show_buttons) {
    button(layout.prev, HitId::kAnimPrev, IconId::kChevronLeft);
    button(layout.next, HitId::kAnimNext, IconId::kChevronRight);
    button(layout.toggle, HitId::kAnimToggle,
           strings.playing ? IconId::kPause : IconId::kPlay);
  }

  draw::DrawText(context, ui, strings.label, metrics::kAnimPillFontSize,
                 strings.show_buttons ? kWeightRegular : kWeightSemibold,
                 layout.label, alpha(colors.text_primary, opacity), true, true);
}

}  // namespace nv::ui
