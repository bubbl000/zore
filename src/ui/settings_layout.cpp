#include "ui/settings_layout.h"

#include "ui/metrics.h"

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

float RowHeight(SettingsRowKind kind, float s) {
  switch (kind) {
    case SettingsRowKind::kGroup:
      return metrics::kSettingsGroupHeight * s;
    case SettingsRowKind::kNote:
      return metrics::kSettingsNoteHeight * s;
    case SettingsRowKind::kRadio:
    case SettingsRowKind::kCheck:
    case SettingsRowKind::kSlider:
      break;
  }
  return metrics::kSettingsRowHeight * s;
}

}  // namespace

const wchar_t* SettingsPageName(SettingsPage page) {
  switch (page) {
    case SettingsPage::kView: return L"view";
    case SettingsPage::kAssociation: return L"association";
    case SettingsPage::kGeneral: break;
  }
  return L"general";
}

SettingsLayout ComputeSettingsLayout(const SettingsLayoutInputs& inputs,
                                     float dpi_scale, const Rect& client) {
  const float s = Scale(dpi_scale);
  SettingsLayout layout;
  layout.dpi_scale = s;
  layout.client = client;
  if (client.empty()) return layout;

  layout.title = Rect::FromLTRB(client.left, client.top, client.right,
                                client.top + metrics::kSettingsTitleHeight * s);
  const float close = metrics::kSettingsCloseSize * s;
  layout.close_button = Rect::FromXYWH(
      client.right - metrics::kSettingsTitlePadding * s - close,
      layout.title.top + (layout.title.height() - close) * 0.5f, close, close);

  layout.footer = Rect::FromLTRB(
      client.left, client.bottom - metrics::kSettingsFooterHeight * s,
      client.right, client.bottom);
  layout.nav = Rect::FromLTRB(client.left, layout.title.bottom,
                              client.left + metrics::kSettingsNavWidth * s,
                              layout.footer.top);
  layout.content = Rect::FromLTRB(layout.nav.right + metrics::kSettingsPadding * s,
                                  layout.title.bottom, client.right,
                                  layout.footer.top);

  // Left navigation: one 40 DIP row per page, 16 DIP top padding.
  layout.nav_rows.reserve(kSettingsPageCount);
  float nav_y = layout.nav.top + metrics::kSettingsNavPadding * s;
  for (int i = 0; i < kSettingsPageCount; ++i) {
    layout.nav_rows.push_back(Rect::FromLTRB(
        layout.nav.left, nav_y, layout.nav.right,
        nav_y + metrics::kSettingsNavRowHeight * s));
    nav_y += metrics::kSettingsNavRowHeight * s;
  }

  // Footer buttons: reset + open-config on the left, close on the right.
  const float button_h = metrics::kSettingsButtonHeight * s;
  const float button_y =
      layout.footer.top + (layout.footer.height() - button_h) * 0.5f;
  const float reset_w = 120.0f * s;
  const float open_w = 120.0f * s;
  const float close_w = metrics::kSettingsButtonMinWidth * s;
  layout.reset = Rect::FromXYWH(layout.footer.left + metrics::kSettingsPadding * s,
                                button_y, reset_w, button_h);
  layout.open_config = Rect::FromXYWH(
      layout.reset.right + metrics::kSettingsButtonGap * s, button_y, open_w,
      button_h);
  layout.close = Rect::FromXYWH(
      layout.footer.right - metrics::kSettingsPadding * s - close_w, button_y,
      close_w, button_h);

  const float padding = metrics::kSettingsPadding * s;
  const float scrollbar = metrics::kSettingsScrollbarWidth * s;
  const float body_gap = 6.0f * s;
  const float body_left = layout.content.left + padding;
  const float body_right = layout.content.right - padding;

  // First pass: total height so the scroll range and the body origin are known.
  float total = padding * 2.0f;
  for (const SettingsRowSpec& spec : inputs.rows) {
    total += RowHeight(spec.kind, s);
  }
  layout.body_height = total;
  layout.max_scroll =
      std::max(0.0f, total - layout.content.height());
  layout.scroll = std::clamp(inputs.scroll, 0.0f, layout.max_scroll);

  const float right_edge =
      layout.max_scroll > 0.0f ? body_right - scrollbar - body_gap : body_right;
  layout.body = Rect::FromLTRB(body_left, layout.content.top - layout.scroll,
                               right_edge,
                               layout.content.top - layout.scroll + total);

  if (layout.max_scroll > 0.0f) {
    layout.scrollbar = Rect::FromLTRB(
        layout.content.right - padding + 3.0f * s, layout.content.top + 2.0f * s,
        layout.content.right - padding + 3.0f * s + scrollbar,
        layout.content.bottom - 2.0f * s);
  }

  // Second pass: rows, starting after the top padding inside the body.
  float y = layout.body.top + padding;
  layout.rows.reserve(inputs.rows.size());
  for (const SettingsRowSpec& spec : inputs.rows) {
    const float h = RowHeight(spec.kind, s);
    SettingsRowLayout row;
    row.kind = spec.kind;
    row.row = Rect::FromLTRB(body_left, y, right_edge, y + h);

    switch (spec.kind) {
      case SettingsRowKind::kGroup:
        row.label = row.row;
        break;
      case SettingsRowKind::kNote:
        row.label = row.row;
        break;
      case SettingsRowKind::kCheck: {
        const float box = metrics::kSettingsCheckSize * s;
        row.control = Rect::FromXYWH(right_edge - box,
                                     y + (h - box) * 0.5f, box, box);
        row.label = Rect::FromLTRB(
            row.row.left, row.row.top, row.control.left - 8.0f * s,
            row.row.bottom);
        break;
      }
      case SettingsRowKind::kSlider: {
        const float value_w = metrics::kSettingsSliderValueWidth * s;
        const float track_w = metrics::kSettingsSliderWidth * s;
        row.value = Rect::FromLTRB(right_edge - value_w, y, right_edge,
                                   y + h);
        row.control = Rect::FromXYWH(
            row.value.left - track_w, y + (h - metrics::kSettingsSliderTrack * s) * 0.5f,
            track_w, metrics::kSettingsSliderTrack * s);
        row.label = Rect::FromLTRB(
            row.row.left, row.row.top, row.control.left - 12.0f * s,
            row.row.bottom);
        break;
      }
      case SettingsRowKind::kRadio: {
        row.label = Rect::FromLTRB(
            row.row.left, row.row.top,
            row.row.left + metrics::kSettingsLabelWidth * s, row.row.bottom);
        const float inner = metrics::kSettingsRadioSize * s;
        const float gap = metrics::kSettingsOptionGap * s;
        float x = row.label.right;
        for (const float width : spec.option_widths) {
          const float available =
              std::max(0.0f, right_edge - x);
          const float w = std::min(width, available);
          row.options.push_back(Rect::FromXYWH(
              x, y + (h - inner) * 0.5f, w, inner));
          x += w + gap;
        }
        break;
      }
    }
    layout.rows.push_back(std::move(row));
    y += h;
  }

  return layout;
}

int SettingsRowAt(const SettingsLayout& layout, float x, float y) {
  if (!layout.content.Contains(x, y)) return -1;
  for (size_t i = 0; i < layout.rows.size(); ++i) {
    if (layout.rows[i].row.Contains(x, y)) return static_cast<int>(i);
  }
  return -1;
}

int SettingsNavAt(const SettingsLayout& layout, float x, float y) {
  if (!layout.nav.Contains(x, y)) return -1;
  for (size_t i = 0; i < layout.nav_rows.size(); ++i) {
    if (layout.nav_rows[i].Contains(x, y)) return static_cast<int>(i);
  }
  return -1;
}

AssociationLayout ComputeAssociationLayout(float dpi_scale,
                                           const Rect& content, int count,
                                           int columns, float scroll) {
  const float s = Scale(dpi_scale);
  AssociationLayout layout;
  layout.dpi_scale = s;
  if (content.empty() || columns <= 0) return layout;

  const float padding = metrics::kSettingsPadding * s;
  layout.header = Rect::FromLTRB(content.left + padding, content.top + padding,
                                 content.right - padding,
                                 content.top + padding +
                                     metrics::kAssociationHeaderHeight * s);
  const float select_box = metrics::kSettingsCheckSize * s;
  layout.select_all = Rect::FromXYWH(
      layout.header.right - 96.0f * s,
      layout.header.top + (layout.header.height() - select_box) * 0.5f,
      select_box, select_box);

  const float buttons_h = metrics::kAssociationButtonHeight * s;
  const float status_h = metrics::kAssociationStatusHeight * s;
  const float gap = metrics::kAssociationListGap * s;

  layout.status_secondary = Rect::FromLTRB(
      content.left + padding, content.bottom - padding - status_h,
      content.right - padding, content.bottom - padding);
  layout.status_primary = Rect::FromLTRB(
      content.left + padding,
      layout.status_secondary.top - status_h,
      content.right - padding, layout.status_secondary.top);
  const Rect button_row = Rect::FromLTRB(
      content.left + padding,
      layout.status_primary.top - gap - buttons_h,
      content.right - padding,
      layout.status_primary.top - gap);
  const float button_w = 120.0f * s;
  layout.register_button = Rect::FromXYWH(button_row.left, button_row.top,
                                          button_w, buttons_h);
  layout.unregister_button = Rect::FromXYWH(
      layout.register_button.right + metrics::kSettingsButtonGap * s,
      button_row.top, button_w, buttons_h);
  layout.default_button = Rect::FromXYWH(
      layout.unregister_button.right + metrics::kSettingsButtonGap * s,
      button_row.top, button_w, buttons_h);

  layout.list = Rect::FromLTRB(content.left + padding,
                               layout.header.bottom + gap,
                               content.right - padding, button_row.top - gap);

  const int rows = (count + columns - 1) / columns;
  layout.row_height = metrics::kAssociationRowHeight * s;
  const float scrollbar = metrics::kSettingsScrollbarWidth * s;
  layout.content_height = rows * layout.row_height + padding * 2.0f;
  layout.max_scroll =
      std::max(0.0f, layout.content_height - layout.list.height());
  layout.scroll = std::clamp(scroll, 0.0f, layout.max_scroll);

  if (layout.max_scroll > 0.0f) {
    layout.scrollbar = Rect::FromLTRB(
        layout.list.right - scrollbar, layout.list.top + 2.0f * s,
        layout.list.right, layout.list.bottom - 2.0f * s);
  }

  const float list_width =
      layout.list.width() - (layout.max_scroll > 0.0f ? scrollbar + 6.0f * s : 0.0f);
  const float cell_w = list_width / static_cast<float>(columns);
  const float top = layout.list.top - layout.scroll + padding;
  layout.cells.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const int row = i / columns;
    const int column = i % columns;
    layout.cells.push_back(AssociationCell{
        Rect::FromLTRB(layout.list.left + cell_w * static_cast<float>(column),
                       top + layout.row_height * static_cast<float>(row),
                       layout.list.left +
                           cell_w * static_cast<float>(column + 1),
                       top + layout.row_height * static_cast<float>(row + 1)),
        i});
  }
  return layout;
}

int AssociationCellAt(const AssociationLayout& layout, float x, float y) {
  if (!layout.list.Contains(x, y)) return -1;
  for (const AssociationCell& cell : layout.cells) {
    if (cell.rect.Contains(x, y)) return cell.index;
  }
  return -1;
}

}  // namespace nv::ui
