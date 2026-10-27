#include "ui/info_panel.h"

#include "ui/metrics.h"

#include <algorithm>
#include <cmath>

namespace nv::ui {

InfoWindowSize ComputeInfoWindowSize(size_t row_count, float dpi_scale,
                                     float work_area_width,
                                     float work_area_height) {
  InfoWindowSize size;
  const float s = dpi_scale > 0.0f ? dpi_scale : 1.0f;
  size.width = metrics::kInfoWidth * s;
  if (work_area_width > 0.0f) {
    size.width = std::min(size.width, work_area_width);
  }

  // padding + header + the two body gaps + lines + footer. The footer is always
  // reserved so the window never jumps when the first EXIF result arrives, and
  // the gaps are what makes the body exactly fit the rows when nothing is capped
  // (without them the last line would always trigger a scrollbar, 規劃/22/06 §4.1).
  const float wanted =
      (metrics::kInfoHeaderHeight + metrics::kInfoFooterHeight +
       metrics::kInfoPadding * 2.0f + metrics::kInfoBodyGap * 2.0f) *
          s +
      static_cast<float>(row_count) * metrics::kInfoRowHeight * s;
  float height = wanted;
  if (work_area_height > 0.0f) {
    const float cap = work_area_height * metrics::kInfoMaxHeightRatio;
    if (cap > 0.0f) height = std::min(height, cap);
  }
  size.height = std::max(0.0f, height);
  return size;
}

InfoPanelLayout ComputeInfoPanelLayout(const Rect& panel, float dpi_scale,
                                       const std::vector<InfoLine>& lines,
                                       float scroll, float copy_all_width) {
  InfoPanelLayout layout;
  layout.panel = panel;
  layout.dpi_scale = dpi_scale;
  if (panel.empty()) return layout;
  layout.visible = true;

  const float s = dpi_scale > 0.0f ? dpi_scale : 1.0f;
  const float padding = metrics::kInfoPadding * s;
  const float row_h = metrics::kInfoRowHeight * s;
  const float header_h = metrics::kInfoHeaderHeight * s;
  const float footer_h = metrics::kInfoFooterHeight * s;

  const Rect inner = Rect::FromLTRB(panel.left + padding, panel.top + padding,
                                    panel.right - padding,
                                    panel.bottom - padding);
  layout.header = Rect::FromLTRB(inner.left, inner.top, inner.right,
                                 inner.top + header_h);
  const float close = metrics::kInfoCloseSize * s;
  layout.close = Rect::FromLTRB(inner.right - close, inner.top, inner.right,
                                inner.top + close);
  layout.footer = Rect::FromLTRB(inner.left, inner.bottom - footer_h,
                                 inner.right, inner.bottom);

  const float button_h = metrics::kInfoButtonHeight * s;
  const float button_w =
      std::min(std::max(copy_all_width, metrics::kInfoButtonMinWidth * s),
               inner.width());
  layout.copy_all = Rect::FromLTRB(
      inner.right - button_w,
      layout.footer.top + (footer_h - button_h) * 0.5f, inner.right,
      layout.footer.top + (footer_h + button_h) * 0.5f);

  const float body_top = layout.header.bottom + metrics::kInfoBodyGap * s;
  const float body_bottom = layout.footer.top - metrics::kInfoBodyGap * s;
  layout.scrollable = false;
  layout.body = Rect::FromLTRB(inner.left, body_top,
                               inner.right - metrics::kInfoScrollbarWidth * s -
                                   4.0f * s,
                               std::max(body_top, body_bottom));
  if (layout.body.empty()) {
    layout.body = Rect::FromLTRB(inner.left, body_top, inner.right, body_top);
  }

  layout.content_height = static_cast<float>(lines.size()) * row_h;
  layout.max_scroll = std::max(0.0f, layout.content_height - layout.body.height());
  layout.scroll = std::clamp(scroll, 0.0f, layout.max_scroll);
  layout.scrollable = layout.max_scroll > 0.5f;

  layout.line.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    const float top = layout.body.top + static_cast<float>(i) * row_h -
                      layout.scroll;
    layout.line.push_back(Rect::FromLTRB(layout.body.left, top, layout.body.right,
                                         top + row_h));
  }

  if (!layout.scrollable) {
    layout.first_visible = 0;
    layout.last_visible =
        lines.empty() ? -1 : static_cast<int>(lines.size()) - 1;
  } else {
    layout.first_visible = static_cast<int>(layout.scroll / row_h);
    // Last *fully* visible line; the partially visible one below it is faded.
    layout.last_visible = static_cast<int>(
                              std::floor((layout.scroll + layout.body.height()) /
                                         row_h)) -
                          1;
    layout.last_visible =
        std::clamp(layout.last_visible, 0,
                   static_cast<int>(lines.size()) - 1);

    // Thin scrollbar on the right edge of the body viewport.
    const float track_w = metrics::kInfoScrollbarWidth * s;
    const float track_x = inner.right - track_w;
    layout.scrollbar_track =
        Rect::FromLTRB(track_x, layout.body.top, track_x + track_w,
                       layout.body.bottom);
    const float ratio = layout.body.height() / layout.content_height;
    const float thumb_h = std::max(24.0f * s, layout.body.height() * ratio);
    const float travel = layout.body.height() - thumb_h;
    const float progress =
        layout.max_scroll > 0.0f ? layout.scroll / layout.max_scroll : 0.0f;
    layout.scrollbar_thumb =
        Rect::FromLTRB(track_x, layout.body.top + travel * progress,
                       track_x + track_w,
                       layout.body.top + travel * progress + thumb_h);
  }
  return layout;
}

HitId HitTestInfoPanel(const InfoPanelLayout& layout, float x, float y) {
  if (!layout.visible || layout.panel.empty()) return HitId::kNone;
  if (!layout.panel.Contains(x, y)) return HitId::kNone;
  if (layout.close.Contains(x, y)) return HitId::kInfoClose;
  if (layout.copy_all.Contains(x, y)) return HitId::kInfoCopyAll;
  return HitId::kInfoPanel;
}

int InfoLineAt(const InfoPanelLayout& layout, const std::vector<InfoLine>& lines,
               float x, float y) {
  if (!layout.visible || !layout.body.Contains(x, y)) return -1;
  for (size_t i = 0; i < layout.line.size() && i < lines.size(); ++i) {
    if (lines[i].header) continue;
    if (layout.line[i].Contains(x, y)) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace nv::ui
