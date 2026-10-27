#include "ui/empty_state.h"

#include "ui/draw_util.h"
#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/ui_context.h"

#include <algorithm>

namespace nv::ui {
namespace {

constexpr int kWeightRegular = 400;
constexpr int kWeightSemibold = 600;

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

struct ButtonState {
  bool hovered = false;
  bool pressed = false;
  bool focused = false;
};

void DrawFocusRing(ID2D1DeviceContext* context, UiContext& ui, const Rect& rect,
                   float dpi_scale, const ColorTokens& colors) {
  if (rect.empty()) return;
  draw::StrokeRoundRect(context, ui, rect.Inset(1.0f * dpi_scale, 1.0f * dpi_scale),
                        metrics::kRadiusControl * dpi_scale, 2.0f * dpi_scale,
                        colors.accent);
}

ButtonState StateFor(HitId id, HitId hover, HitId pressed, int focus) {
  ButtonState state;
  state.hovered = id == hover;
  state.pressed = id == pressed;
  const HitId focused = focus == 0   ? HitId::kEmptyPrimary
                        : focus == 1 ? HitId::kEmptyOpenFolder
                                     : HitId::kEmptyAllFormats;
  state.focused = !state.hovered && id == focused;
  return state;
}

}  // namespace

FormatPanelLayout ComputeFormatPanelLayout(const Rect& client, float dpi_scale,
                                           size_t row_count,
                                           size_t first_row) {
  const float s = Scale(dpi_scale);
  FormatPanelLayout layout;
  if (client.empty() || row_count == 0) return layout;

  const float margin = metrics::kFormatsPanelMargin * s;
  const float padding = metrics::kFormatsPadding * s;
  const float row_h = metrics::kFormatsRowHeight * s;
  const float max_width = std::max(0.0f, client.width() - 2.0f * margin);
  const float max_height = std::max(0.0f, client.height() - 2.0f * margin);
  const float width = std::min(metrics::kFormatsPanelWidth * s, max_width);
  if (width < 240.0f * s) return layout;

  const float wanted = padding * 2.0f +
                       metrics::kFormatsHeaderHeight * s +
                       static_cast<float>(row_count) * row_h +
                       metrics::kFormatsFooterHeight * s;
  const float height =
      std::min({wanted, metrics::kFormatsPanelMaxHeight * s, max_height});
  if (height < 160.0f * s) return layout;

  layout.scrim = client;
  layout.panel = Rect::FromXYWH(
      client.left + (client.width() - width) * 0.5f,
      client.top + (client.height() - height) * 0.5f, width, height);

  const float inner_left = layout.panel.left + padding;
  const float inner_right = layout.panel.right - padding;
  layout.title = Rect::FromLTRB(inner_left, layout.panel.top + padding,
                                inner_right,
                                layout.panel.top + padding +
                                    metrics::kFormatsHeaderHeight * s);
  layout.footer = Rect::FromLTRB(
      inner_left, layout.panel.bottom - padding - metrics::kFormatsFooterHeight * s,
      inner_right, layout.panel.bottom - padding);
  layout.close = Rect::FromXYWH(
      inner_right - metrics::kFormatsCloseWidth * s,
      layout.footer.top +
          (layout.footer.height() - metrics::kFormatsCloseHeight * s) * 0.5f,
      metrics::kFormatsCloseWidth * s, metrics::kFormatsCloseHeight * s);

  // Rows that do not fit between the header and the footer are reached by
  // scrolling: only whole rows are laid out, and `first_row` maps each visible
  // rectangle to its family in FormatPanelStrings::rows.
  const float body_height = std::max(0.0f, layout.footer.top - layout.title.bottom);
  const size_t visible =
      std::max<size_t>(1, static_cast<size_t>(body_height / row_h));
  layout.max_first_row = row_count > visible ? row_count - visible : 0;
  layout.first_row = std::min(first_row, layout.max_first_row);
  float y = layout.title.bottom;
  for (size_t index = layout.first_row;
       index < row_count && layout.rows.size() < visible; ++index) {
    layout.rows.push_back(Rect::FromLTRB(inner_left, y, inner_right, y + row_h));
    y += row_h;
  }
  layout.visible = true;
  return layout;
}

HitId HitTestFormatPanel(const FormatPanelLayout& layout, float x, float y) {
  if (!layout.visible) return HitId::kNone;
  if (layout.close.Contains(x, y)) return HitId::kFormatsClose;
  if (layout.panel.Contains(x, y)) return HitId::kFormatsPanel;
  return HitId::kNone;
}

int WrapFormatNames(UiContext& ui, const std::wstring& prefix,
                    const std::vector<std::wstring>& names, float width_px,
                    float font_dip, int weight, std::wstring lines[2],
                    bool* truncated) {
  lines[0].clear();
  lines[1].clear();
  if (truncated != nullptr) *truncated = false;
  if (names.empty() || width_px <= 1.0f) return 0;

  std::wstring current = prefix;
  int used = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    const std::wstring& name = names[i];
    const std::wstring candidate =
        current.empty() ? name : current + L" / " + name;
    const bool fits = ui.MeasureText(candidate, font_dip, weight) <= width_px;
    if (fits || current == prefix || current.empty()) {
      // The first word always goes on the first line, even if it alone is too
      // wide (it is then ellipsized by the draw call, never silently dropped).
      current = candidate;
      continue;
    }
    lines[used] = current;
    ++used;
    if (used >= 2) {
      if (truncated != nullptr) *truncated = true;
      return used;
    }
    current = name;
  }
  lines[used] = current;
  ++used;
  return used;
}

void DrawEmptyCard(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                   const EmptyLayout& layout, const EmptyStrings& strings,
                   EmptyKind kind, const ColorTokens& colors, float dpi_scale,
                   HitId hover, HitId pressed, int focus, bool drag_over) {
  if (context == nullptr || !layout.visible || layout.card.empty()) return;
  const float dpi = Scale(dpi_scale);
  const float stroke = std::max(1.0f, dpi);

  draw::FillRoundRect(context, ui, layout.card,
                      metrics::kEmptyCardRadius * dpi,
                      WithAlpha(colors.bg_panel, 0.97f));
  draw::StrokeRoundRect(context, ui, layout.card,
                        metrics::kEmptyCardRadius * dpi, stroke,
                        colors.border_strong);

  if (!layout.icon.empty()) {
    icons.Draw(context, ui, IconId::kImage, layout.icon, colors.text_secondary,
               1.5f * dpi);
  }

  draw::DrawText(context, ui, strings.title, metrics::kEmptyTitleFontSize,
                 kWeightSemibold, layout.title, colors.text_primary, false, true);

  const float line_h = metrics::kEmptyFormatLineHeight * dpi;
  for (int i = 0; i < layout.format_lines && i < 2; ++i) {
    if (strings.format_lines[i].empty()) continue;
    const Rect line = Rect::FromLTRB(layout.formats.left,
                                     layout.formats.top + line_h * i,
                                     layout.formats.right,
                                     layout.formats.top + line_h * (i + 1));
    const bool last = i == layout.format_lines - 1;
    draw::DrawText(context, ui, strings.format_lines[i],
                   metrics::kEmptyFormatsFontSize, kWeightRegular, line,
                   colors.text_secondary, last && strings.format_truncated,
                   true);
  }

  draw::DrawText(context, ui, strings.formats_more,
                 metrics::kEmptyMoreFontSize, kWeightRegular,
                 layout.formats_more, colors.text_muted, false, true);

  if (!layout.note.empty() && !strings.note.empty()) {
    draw::DrawText(context, ui, strings.note, metrics::kEmptyNoteFontSize,
                   kWeightRegular, layout.note,
                   kind == EmptyKind::kUnsupported ? colors.danger
                                                   : colors.text_muted,
                   false, true);
  }

  // Primary button: accent fill, white text, 2 px focus ring. While a drag
  // hovers the window the button is pre-highlighted (規劃/22/03 §4.2).
  const ButtonState primary =
      StateFor(HitId::kEmptyPrimary, hover, pressed, focus);
  uint32_t fill = primary.hovered || drag_over ? colors.accent_hover
                                               : colors.accent;
  if (primary.pressed) {
    fill = colors.accent_pressed;
  }
  draw::FillRoundRect(context, ui, layout.primary,
                      metrics::kRadiusControl * dpi, fill);
  draw::DrawText(context, ui, strings.primary, metrics::kEmptyButtonFontSize,
                 kWeightSemibold, layout.primary, colors.on_accent, false, true);
  if (primary.focused) {
    DrawFocusRing(context, ui, layout.primary, dpi, colors);
  }

  const struct {
    const Rect* rect;
    const std::wstring* text;
    HitId id;
  } kFooter[] = {{&layout.open_folder, &strings.open_folder,
                  HitId::kEmptyOpenFolder},
                 {&layout.all_formats, &strings.all_formats,
                  HitId::kEmptyAllFormats}};
  for (const auto& item : kFooter) {
    if (item.rect->empty() || item.text->empty()) continue;
    const ButtonState state = StateFor(item.id, hover, pressed, focus);
    if (state.pressed || state.hovered) {
      draw::FillRoundRect(context, ui, *item.rect,
                          metrics::kRadiusControl * dpi, colors.bg_hover);
    }
    draw::DrawText(context, ui, *item.text, metrics::kEmptyFooterFontSize,
                   kWeightRegular, *item.rect,
                   state.hovered ? colors.accent : colors.text_secondary, false,
                   true);
    if (state.focused) {
      DrawFocusRing(context, ui, *item.rect, dpi, colors);
    }
  }
}

void DrawDragWash(ID2D1DeviceContext* context, UiContext& ui,
                  const Rect& content, const ColorTokens& colors) {
  if (content.empty()) return;
  draw::FillRect(context, ui, content, WithAlpha(colors.accent, 0.08f));
}

void DrawDragBorder(ID2D1DeviceContext* context, UiContext& ui,
                    const Rect& client, const ColorTokens& colors, float scale) {
  if (client.empty()) return;
  const float s = Scale(scale);
  const float width = 2.0f * s;
  draw::StrokeRoundRect(context, ui, client.Inset(width * 0.5f, width * 0.5f),
                        0.0f, width, colors.accent);
}

void DrawDragHint(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                  const LayoutFrame& layout, const std::wstring& text,
                  const ColorTokens& colors) {
  (void)icons;
  if (!layout.drag_over || layout.drag_hint.empty() || text.empty()) return;
  const float s = layout.dpi_scale;
  // V3.1-06 (§06a): 50 % instead of 96 % — the hint floats over the picture
  // (or the empty card) and should read as an overlay, not as a panel.
  draw::FillRoundRect(context, ui, layout.drag_hint,
                      metrics::kRadiusControl * s,
                      WithAlpha(colors.bg_panel, 0.5f));
  draw::StrokeRoundRect(context, ui, layout.drag_hint,
                        metrics::kRadiusControl * s, std::max(1.0f, s),
                        colors.accent);
  draw::DrawText(context, ui, text, metrics::kEmptyButtonFontSize,
                 kWeightSemibold, layout.drag_hint, colors.accent, false, true);
}

void DrawFormatPanel(ID2D1DeviceContext* context, UiContext& ui,
                     const FormatPanelLayout& layout,
                     const FormatPanelStrings& strings,
                     const ColorTokens& colors, float dpi_scale, HitId hover,
                     HitId pressed) {
  if (context == nullptr || !layout.visible || layout.panel.empty()) return;
  const float s = Scale(dpi_scale);
  const float stroke = std::max(1.0f, s);

  // Scrim: the modal owns the whole window, so the rest of the UI is dimmed
  // instead of staying partially interactive-looking (規劃/22/20 §3.1).
  draw::FillRect(context, ui, layout.scrim, colors.scrim);

  draw::FillRoundRect(context, ui, layout.panel, metrics::kRadiusControl * s,
                      colors.bg_panel);
  draw::StrokeRoundRect(context, ui, layout.panel, metrics::kRadiusControl * s,
                        stroke, colors.border_strong);

  draw::DrawText(context, ui, strings.title, metrics::kFormatsTitleFontSize,
                 kWeightSemibold, layout.title, colors.text_primary, true,
                 false);
  draw::FillRect(context, ui,
                 Rect::FromLTRB(layout.title.left, layout.title.bottom - 1.0f,
                                layout.title.right, layout.title.bottom),
                 colors.border_subtle);

  const float name_width = metrics::kFormatsNameWidth * s;
  const float note_width = metrics::kFormatsNoteWidth * s;
  for (size_t i = 0; i < layout.rows.size(); ++i) {
    const size_t index = layout.first_row + i;
    if (index >= strings.rows.size()) break;
    const Rect& row = layout.rows[i];
    const FormatPanelRow& item = strings.rows[index];
    const bool available = item.note.empty();
    draw::DrawText(context, ui, item.name, metrics::kFormatsRowFontSize,
                   kWeightSemibold,
                   Rect::FromLTRB(row.left, row.top, row.left + name_width,
                                  row.bottom),
                   available ? colors.text_primary : colors.text_muted, true,
                   false);
    const float ext_right = row.right - (item.note.empty() ? 0.0f : note_width);
    draw::DrawText(context, ui, item.extensions, metrics::kFormatsRowFontSize,
                   kWeightRegular,
                   Rect::FromLTRB(row.left + name_width + 8.0f * s, row.top,
                                  ext_right, row.bottom),
                   colors.text_secondary, true, false);
    if (!item.note.empty()) {
      draw::DrawText(context, ui, item.note, metrics::kFormatsNoteFontSize,
                     kWeightRegular,
                     Rect::FromLTRB(ext_right, row.top, row.right, row.bottom),
                     colors.text_muted, true, true);
    }
  }

  draw::DrawText(context, ui, strings.footer, metrics::kFormatsNoteFontSize,
                 kWeightRegular, layout.footer, colors.text_muted, true,
                 false);

  const bool close_hover = hover == HitId::kFormatsClose;
  const bool close_pressed = pressed == HitId::kFormatsClose;
  if (close_pressed || close_hover) {
    draw::FillRoundRect(context, ui, layout.close,
                        metrics::kRadiusControl * s,
                        close_pressed ? colors.bg_pressed : colors.bg_hover);
  }
  draw::StrokeRoundRect(context, ui, layout.close,
                        metrics::kRadiusControl * s, std::max(1.0f, s),
                        colors.border_strong);
  draw::DrawText(context, ui, strings.close, metrics::kFormatsRowFontSize,
                 kWeightRegular, layout.close,
                 close_hover ? colors.accent : colors.text_primary, false,
                 true);
}

}  // namespace nv::ui
