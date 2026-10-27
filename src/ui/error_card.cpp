#include "ui/error_card.h"

#include "ui/draw_util.h"
#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/ui_context.h"

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

constexpr int kWeightRegular = 400;
constexpr int kWeightSemibold = 600;

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

}  // namespace

ErrorCardLayout ComputeErrorCardLayout(const Rect& content, float dpi_scale,
                                       const std::vector<float>& button_widths,
                                       bool has_detail) {
  const float s = Scale(dpi_scale);
  ErrorCardLayout layout;
  if (content.empty()) return layout;

  const float margin = metrics::kErrorCardMargin * s;
  const float padding = metrics::kErrorCardPadding * s;
  const float max_width = std::max(0.0f, content.width() - 2.0f * margin);
  const float width = std::min(metrics::kErrorCardMaxWidth * s, max_width);
  if (width < 240.0f * s) return layout;

  const float title_h = metrics::kErrorTitleHeight * s;
  const float body_h = metrics::kErrorBodyHeight * s;
  const float detail_h = metrics::kErrorDetailHeight * s;
  const float button_h = metrics::kErrorButtonHeight * s;
  const float height = padding * 2.0f + title_h + 4.0f * s + body_h +
                       (has_detail ? 2.0f * s + detail_h : 0.0f) +
                       metrics::kErrorSectionGap * s + button_h;

  layout.card = Rect::FromXYWH(
      content.left + (content.width() - width) * 0.5f,
      content.top + (content.height() - height) * 0.5f, width, height);
  // A very short window would push the card outside the image area; clamp so it
  // stays reachable instead of being drawn under the title bar.
  if (layout.card.top < content.top) {
    layout.card = Rect::FromXYWH(layout.card.left, content.top, width, height);
  }

  const float inner_left = layout.card.left + padding;
  const float inner_right = layout.card.right - padding;
  float y = layout.card.top + padding;

  layout.icon = Rect::FromXYWH(inner_left, y + 2.0f * s,
                               metrics::kErrorIconSize * s,
                               metrics::kErrorIconSize * s);
  layout.title = Rect::FromLTRB(
      inner_left + metrics::kErrorIconSize * s + 10.0f * s, y, inner_right,
      y + title_h);
  y += title_h + 4.0f * s;

  layout.message =
      Rect::FromLTRB(inner_left, y, inner_right, y + body_h);
  y += body_h;

  if (has_detail) {
    y += 2.0f * s;
    layout.detail = Rect::FromLTRB(inner_left, y, inner_right, y + detail_h);
    y += detail_h;
  }

  // Buttons: actions in caller order, then Close at the far right.
  const float gap = metrics::kErrorButtonGap * s;
  const float min_width = metrics::kErrorButtonMinWidth * s;
  const size_t action_count =
      std::min(button_widths.size(), kErrorActionCount);
  std::vector<float> widths(action_count);
  float action_total = 0.0f;
  for (size_t i = 0; i < action_count; ++i) {
    widths[i] = std::clamp(button_widths[i], min_width,
                           metrics::kErrorButtonMaxWidth * s);
    action_total += widths[i];
  }
  const float close_width = min_width;
  const float total_gaps = gap * static_cast<float>(action_count);
  const float available = inner_right - inner_left - close_width - total_gaps;
  if (action_total > available && action_total > 0.0f) {
    const float factor = std::max(0.0f, available) / action_total;
    action_total = 0.0f;
    for (float& value : widths) {
      value = std::max(60.0f * s, value * factor);
      action_total += value;
    }
  }

  const float button_top = layout.card.bottom - padding - button_h;
  float x = inner_left;
  layout.buttons.reserve(action_count);
  for (size_t i = 0; i < action_count; ++i) {
    layout.buttons.push_back(
        Rect::FromXYWH(x, button_top, widths[i], button_h));
    x += widths[i] + gap;
  }
  layout.close = Rect::FromXYWH(inner_right - close_width, button_top,
                                close_width, button_h);
  layout.visible = true;
  return layout;
}

HitId HitTestErrorCard(const ErrorCardLayout& layout, float x, float y) {
  if (!layout.visible) return HitId::kNone;
  for (size_t i = 0; i < layout.buttons.size() && i < kErrorActionCount; ++i) {
    if (layout.buttons[i].Contains(x, y)) {
      return static_cast<HitId>(static_cast<int>(HitId::kErrorAction0) +
                                static_cast<int>(i));
    }
  }
  if (layout.close.Contains(x, y)) return HitId::kErrorClose;
  return HitId::kNone;
}

void DrawErrorCard(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                   const ErrorCardLayout& layout, const ErrorCardStrings& strings,
                   const ColorTokens& colors, float dpi_scale, HitId hover,
                   HitId pressed, int default_index) {
  if (context == nullptr || !layout.visible || layout.card.empty()) return;
  const float s = Scale(dpi_scale);
  const float stroke = std::max(1.0f, s);

  draw::FillRoundRect(context, ui, layout.card,
                      metrics::kErrorCardRadius * s,
                      WithAlpha(colors.bg_panel, 0.98f));
  draw::StrokeRoundRect(context, ui, layout.card,
                        metrics::kErrorCardRadius * s, stroke,
                        colors.border_strong);
  if (!layout.icon.empty()) {
    icons.Draw(context, ui, IconId::kWarning, layout.icon, colors.danger,
               1.5f * s);
  }
  draw::DrawText(context, ui, strings.title, metrics::kErrorTitleFontSize,
                 kWeightSemibold, layout.title, colors.text_primary, true,
                 false);
  draw::DrawText(context, ui, strings.message, metrics::kErrorBodyFontSize,
                 kWeightRegular, layout.message, colors.text_secondary, true,
                 false);
  if (!layout.detail.empty() && !strings.detail.empty()) {
    draw::DrawText(context, ui, strings.detail, metrics::kErrorDetailFontSize,
                   kWeightRegular, layout.detail, colors.text_muted, true,
                   false);
  }

  const auto draw_button = [&](const Rect& rect, const std::wstring& text,
                               HitId id, bool primary) {
    if (rect.empty() || text.empty()) return;
    const bool hovered = hover == id;
    const bool down = pressed == id;
    if (primary) {
      uint32_t fill = colors.accent;
      if (down) {
        fill = colors.accent_pressed;
      } else if (hovered) {
        fill = colors.accent_hover;
      }
      draw::FillRoundRect(context, ui, rect, metrics::kRadiusControl * s,
                          fill);
      draw::DrawText(context, ui, text, metrics::kErrorBodyFontSize,
                     kWeightSemibold, rect, colors.on_accent, false, true);
      return;
    }
    if (down || hovered) {
      draw::FillRoundRect(context, ui, rect, metrics::kRadiusControl * s,
                          down ? colors.bg_pressed : colors.bg_hover);
    }
    draw::StrokeRoundRect(context, ui, rect, metrics::kRadiusControl * s,
                          stroke, colors.border_strong);
    draw::DrawText(context, ui, text, metrics::kErrorBodyFontSize,
                   kWeightRegular, rect,
                   hovered ? colors.accent : colors.text_primary, false, true);
  };

  for (size_t i = 0; i < layout.buttons.size() && i < strings.buttons.size();
       ++i) {
    const HitId id = static_cast<HitId>(static_cast<int>(HitId::kErrorAction0) +
                                        static_cast<int>(i));
    draw_button(layout.buttons[i], strings.buttons[i], id,
                static_cast<int>(i) == default_index);
  }
  draw_button(layout.close, strings.close, HitId::kErrorClose, false);
}

ConfirmCardLayout ComputeConfirmCardLayout(const Rect& client, float dpi_scale,
                                           bool has_remember) {
  const float s = Scale(dpi_scale);
  ConfirmCardLayout layout;
  if (client.empty()) return layout;
  const float margin = metrics::kConfirmCardMargin * s;
  const float padding = metrics::kConfirmPadding * s;
  const float width =
      std::min(metrics::kConfirmCardWidth * s,
               std::max(0.0f, client.width() - 2.0f * margin));
  if (width < 280.0f * s) return layout;

  const float title_h = metrics::kConfirmTitleHeight * s;
  const float body_h = metrics::kConfirmBodyHeight * s;
  const float button_h = metrics::kConfirmButtonHeight * s;
  const float remember_h =
      has_remember ? metrics::kConfirmRememberHeight * s : 0.0f;
  const float remember_gap =
      has_remember ? metrics::kConfirmRememberGap * s : 0.0f;
  const float height = padding * 2.0f + title_h + 8.0f * s + body_h +
                       metrics::kErrorSectionGap * s + button_h +
                       remember_gap + remember_h;

  layout.scrim = client;
  layout.card = Rect::FromXYWH(
      client.left + (client.width() - width) * 0.5f,
      client.top + (client.height() - height) * 0.5f, width, height);
  if (layout.card.top < client.top) {
    layout.card = Rect::FromXYWH(layout.card.left, client.top, width, height);
  }

  const float inner_left = layout.card.left + padding;
  const float inner_right = layout.card.right - padding;
  layout.title = Rect::FromLTRB(inner_left, layout.card.top + padding,
                                inner_right,
                                layout.card.top + padding + title_h);
  layout.body = Rect::FromLTRB(inner_left, layout.title.bottom + 8.0f * s,
                               inner_right, layout.title.bottom + 8.0f * s +
                                                body_h);
  const float button_top = layout.card.bottom - padding - button_h;
  const float button_w = metrics::kConfirmButtonWidth * s;
  const float gap = metrics::kConfirmButtonGap * s;
  layout.primary = Rect::FromXYWH(inner_right - button_w, button_top, button_w,
                                  button_h);
  layout.secondary = Rect::FromXYWH(layout.primary.left - gap - button_w,
                                    button_top, button_w, button_h);
  layout.has_remember = has_remember;
  if (has_remember) {
    const float box = metrics::kConfirmRememberBox * s;
    const float row_top = button_top - remember_gap - remember_h;
    layout.remember_box =
        Rect::FromXYWH(inner_left, row_top + (remember_h - box) * 0.5f, box, box);
    layout.remember = Rect::FromLTRB(
        layout.remember_box.right + 8.0f * s, row_top, inner_right,
        row_top + remember_h);
  }
  layout.visible = true;
  return layout;
}

HitId HitTestConfirmCard(const ConfirmCardLayout& layout, float x, float y) {
  if (!layout.visible) return HitId::kNone;
  if (layout.primary.Contains(x, y)) return HitId::kConfirmPrimary;
  if (layout.secondary.Contains(x, y)) return HitId::kConfirmSecondary;
  if (layout.has_remember &&
      (layout.remember.Contains(x, y) || layout.remember_box.Contains(x, y))) {
    return HitId::kConfirmRemember;
  }
  if (layout.card.Contains(x, y)) return HitId::kConfirmPanel;
  // Outside the card is still owned by the modal (clicking the scrim does
  // nothing, it must not reach the image behind it, 規劃/22/20 §3.5).
  return HitId::kConfirmPanel;
}

void DrawConfirmCard(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                     const ConfirmCardLayout& layout,
                     const ConfirmCardStrings& strings,
                     const ColorTokens& colors, float dpi_scale, HitId hover,
                     HitId pressed) {
  if (context == nullptr || !layout.visible || layout.card.empty()) return;
  const float s = Scale(dpi_scale);
  const float stroke = std::max(1.0f, s);

  draw::FillRect(context, ui, layout.scrim, colors.scrim);
  draw::FillRoundRect(context, ui, layout.card,
                      metrics::kErrorCardRadius * s, colors.bg_panel);
  draw::StrokeRoundRect(context, ui, layout.card,
                        metrics::kErrorCardRadius * s, stroke,
                        colors.border_strong);

  const Rect icon = Rect::FromXYWH(
      layout.title.left, layout.title.top + 2.0f * s,
      metrics::kErrorIconSize * s, metrics::kErrorIconSize * s);
  const uint32_t accent = strings.danger ? colors.danger : colors.accent;
  icons.Draw(context, ui, strings.danger ? IconId::kWarning : IconId::kInfo,
             icon, accent, 1.5f * s);
  const Rect title_text =
      Rect::FromLTRB(icon.right + 10.0f * s, layout.title.top,
                     layout.title.right, layout.title.bottom);
  draw::DrawText(context, ui, strings.title, metrics::kErrorTitleFontSize,
                 kWeightSemibold, title_text, colors.text_primary, true, false);
  draw::DrawText(context, ui, strings.body, metrics::kErrorBodyFontSize,
                 kWeightRegular, layout.body, colors.text_secondary, true,
                 false);

  if (layout.has_remember) {
    const bool hovered = hover == HitId::kConfirmRemember;
    if (strings.remember_checked) {
      draw::FillRoundRect(context, ui, layout.remember_box, 3.0f * s,
                          colors.accent);
      const Rect check = Rect::FromXYWH(
          layout.remember_box.left + 2.0f * s,
          layout.remember_box.top + 2.0f * s,
          layout.remember_box.width() - 4.0f * s,
          layout.remember_box.height() - 4.0f * s);
      icons.Draw(context, ui, IconId::kCheck, check, colors.on_accent, 1.6f * s);
    } else {
      draw::StrokeRoundRect(context, ui, layout.remember_box, 3.0f * s,
                            stroke,
                            hovered ? colors.accent : colors.border_strong);
    }
    if (!strings.remember.empty()) {
      draw::DrawText(context, ui, strings.remember,
                     metrics::kErrorBodyFontSize, kWeightRegular,
                     layout.remember,
                     hovered ? colors.accent : colors.text_secondary, false,
                     false);
    }
  }

  const bool primary_hover = hover == HitId::kConfirmPrimary;
  const bool primary_down = pressed == HitId::kConfirmPrimary;
  uint32_t primary_fill = accent;
  if (primary_down) {
    primary_fill = strings.danger ? colors.danger_hover : colors.accent_pressed;
  } else if (primary_hover) {
    primary_fill = strings.danger ? colors.danger_hover : colors.accent_hover;
  }
  draw::FillRoundRect(context, ui, layout.primary,
                      metrics::kRadiusControl * s, primary_fill);
  draw::DrawText(context, ui, strings.primary, metrics::kErrorBodyFontSize,
                 kWeightSemibold, layout.primary, colors.on_accent, false, true);

  const bool secondary_hover = hover == HitId::kConfirmSecondary;
  const bool secondary_down = pressed == HitId::kConfirmSecondary;
  if (secondary_hover || secondary_down) {
    draw::FillRoundRect(context, ui, layout.secondary,
                        metrics::kRadiusControl * s,
                        secondary_down ? colors.bg_pressed : colors.bg_hover);
  }
  draw::StrokeRoundRect(context, ui, layout.secondary,
                        metrics::kRadiusControl * s, stroke,
                        colors.border_strong);
  draw::DrawText(context, ui, strings.secondary, metrics::kErrorBodyFontSize,
                 kWeightRegular, layout.secondary,
                 secondary_hover ? colors.accent : colors.text_primary, false,
                 true);
}

// --- V3.1-06 §06e: rename overlay -------------------------------------------
//
// Geometry mirrors the confirmation card (same radius/padding/button sizes), so
// the two modals are visually one family. The input box is a child EDIT in the
// app; this layout is the rectangle that EDIT is placed over, in DIPs.
RenameCardLayout ComputeRenameCardLayout(const Rect& client, float dpi_scale) {
  const float s = Scale(dpi_scale);
  RenameCardLayout layout;
  if (client.empty()) return layout;
  const float margin = metrics::kConfirmCardMargin * s;
  const float padding = metrics::kConfirmPadding * s;
  const float width =
      std::min(metrics::kConfirmCardWidth * s,
               std::max(0.0f, client.width() - 2.0f * margin));
  if (width < 280.0f * s) return layout;

  const float title_h = metrics::kConfirmTitleHeight * s;
  const float hint_h = metrics::kConfirmBodyHeight * s;
  const float input_h = metrics::kRenameInputHeight * s;
  const float error_h = metrics::kRenameErrorHeight * s;
  const float button_h = metrics::kConfirmButtonHeight * s;
  const float height = padding * 2.0f + title_h + 6.0f * s + hint_h +
                       10.0f * s + input_h + error_h +
                       metrics::kErrorSectionGap * s + button_h;

  layout.scrim = client;
  layout.card = Rect::FromXYWH(
      client.left + (client.width() - width) * 0.5f,
      client.top + (client.height() - height) * 0.5f, width, height);
  if (layout.card.top < client.top) {
    layout.card = Rect::FromXYWH(layout.card.left, client.top, width, height);
  }

  const float inner_left = layout.card.left + padding;
  const float inner_right = layout.card.right - padding;
  layout.title = Rect::FromLTRB(inner_left, layout.card.top + padding,
                                inner_right,
                                layout.card.top + padding + title_h);
  layout.hint = Rect::FromLTRB(inner_left, layout.title.bottom + 6.0f * s,
                               inner_right,
                               layout.title.bottom + 6.0f * s + hint_h);
  layout.input = Rect::FromLTRB(inner_left, layout.hint.bottom + 10.0f * s,
                                inner_right,
                                layout.hint.bottom + 10.0f * s + input_h);
  layout.error = Rect::FromLTRB(inner_left, layout.input.bottom, inner_right,
                                layout.input.bottom + error_h);
  const float button_top = layout.card.bottom - padding - button_h;
  const float button_w = metrics::kConfirmButtonWidth * s;
  const float gap = metrics::kConfirmButtonGap * s;
  layout.primary = Rect::FromXYWH(inner_right - button_w, button_top, button_w,
                                  button_h);
  layout.secondary = Rect::FromXYWH(layout.primary.left - gap - button_w,
                                    button_top, button_w, button_h);
  layout.visible = true;
  return layout;
}

HitId HitTestRenameCard(const RenameCardLayout& layout, float x, float y) {
  if (!layout.visible) return HitId::kNone;
  if (layout.primary.Contains(x, y)) return HitId::kRenameOk;
  if (layout.secondary.Contains(x, y)) return HitId::kRenameCancel;
  // The whole card is a modal surface: a click between the controls must not
  // reach the image or the rail behind it (規劃/22/20 §3.5).
  return HitId::kRenamePanel;
}

void DrawRenameCard(ID2D1DeviceContext* context, UiContext& ui,
                    const RenameCardLayout& layout,
                    const RenameCardStrings& strings,
                    const ColorTokens& colors, float dpi_scale, HitId hover,
                    HitId pressed, bool draw_text) {
  if (context == nullptr || !layout.visible || layout.card.empty()) return;
  const float s = Scale(dpi_scale);
  const float stroke = std::max(1.0f, s);

  draw::FillRect(context, ui, layout.scrim, colors.scrim);
  draw::FillRoundRect(context, ui, layout.card, metrics::kErrorCardRadius * s,
                      colors.bg_panel);
  draw::StrokeRoundRect(context, ui, layout.card, metrics::kErrorCardRadius * s,
                        stroke, colors.border_strong);

  draw::DrawText(context, ui, strings.title, metrics::kErrorTitleFontSize,
                 kWeightSemibold, layout.title, colors.text_primary, true,
                 false);
  draw::DrawText(context, ui, strings.hint, metrics::kErrorBodyFontSize,
                 kWeightRegular, layout.hint, colors.text_secondary, true,
                 false);

  // The box: an accent outline while the EDIT has focus (it always does while
  // the overlay is open), the same radius as the other controls.
  draw::FillRoundRect(context, ui, layout.input, metrics::kRadiusControl * s,
                      colors.bg_panel_inactive);
  draw::StrokeRoundRect(context, ui, layout.input, metrics::kRadiusControl * s,
                        stroke, colors.accent);
  if (draw_text) {
    draw::DrawText(context, ui, strings.placeholder,
                   metrics::kErrorBodyFontSize, kWeightRegular,
                   layout.input.Inset(10.0f * s, 0.0f), colors.text_secondary,
                   false, false);
  }
  if (!strings.error.empty()) {
    draw::DrawText(context, ui, strings.error, metrics::kErrorBodyFontSize,
                   kWeightRegular, layout.error, colors.danger, true, false);
  }

  const bool primary_hover = hover == HitId::kRenameOk;
  const bool primary_down = pressed == HitId::kRenameOk;
  uint32_t primary_fill = colors.accent;
  if (primary_down) {
    primary_fill = colors.accent_pressed;
  } else if (primary_hover) {
    primary_fill = colors.accent_hover;
  }
  draw::FillRoundRect(context, ui, layout.primary, metrics::kRadiusControl * s,
                      primary_fill);
  draw::DrawText(context, ui, strings.ok, metrics::kErrorBodyFontSize,
                 kWeightSemibold, layout.primary, colors.on_accent, false, true);

  const bool secondary_hover = hover == HitId::kRenameCancel;
  const bool secondary_down = pressed == HitId::kRenameCancel;
  if (secondary_hover || secondary_down) {
    draw::FillRoundRect(context, ui, layout.secondary,
                        metrics::kRadiusControl * s,
                        secondary_down ? colors.bg_pressed : colors.bg_hover);
  }
  draw::StrokeRoundRect(context, ui, layout.secondary,
                        metrics::kRadiusControl * s, stroke,
                        colors.border_strong);
  draw::DrawText(context, ui, strings.cancel, metrics::kErrorBodyFontSize,
                 kWeightRegular, layout.secondary,
                 secondary_hover ? colors.accent : colors.text_primary, false,
                 true);
}

}  // namespace nv::ui
