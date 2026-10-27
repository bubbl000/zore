#include "ui/menu.h"

#include "ui/metrics.h"

#include <algorithm>

namespace nv::ui {
namespace {

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

// Separators sit on a compact row; commands, toggles and captions keep the
// uniform height the v2.0 menus use.
float RowHeight(MenuItemKind kind, float s) {
  return (kind == MenuItemKind::kSeparator ? metrics::kMenuSeparatorRowHeight
                                           : metrics::kMenuRowHeight) *
         s;
}

}  // namespace

MenuLayout ComputeMenuLayout(const Rect& anchor, const Rect& bounds,
                             const std::vector<MenuItem>& items,
                             float dpi_scale, float width_dip,
                             MenuOpenSide side) {
  const float s = Scale(dpi_scale);
  MenuLayout layout;
  layout.dpi_scale = s;
  if (items.empty() || bounds.empty()) return layout;

  const float padding = metrics::kMenuPadding * s;
  const float margin = metrics::kMenuMargin * s;
  float rows_total = 0.0f;
  for (const MenuItem& item : items) rows_total += RowHeight(item.kind, s);
  const float height = rows_total + padding * 2.0f;

  float width =
      std::min(width_dip * s, std::max(0.0f, bounds.width() - 2.0f * margin));

  // Prefer the anchor's left edge, but a menu near the right edge (更多▾, or a
  // right-click close to the border) falls back to right-aligning with the
  // anchor and is then clamped inside the client.
  const float min_left = bounds.left + margin;
  const float max_left = bounds.right - margin - width;
  float left = anchor.left;
  if (left + width > bounds.right - margin) left = anchor.right - width;
  if (max_left < min_left) {
    left = min_left;
    width = std::max(0.0f, bounds.width() - 2.0f * margin);
  } else {
    left = std::clamp(left, min_left, max_left);
  }

  // Bottom-bar menus prefer opening upwards; the right-click menu prefers
  // opening below the pointer. Both flip when the preferred side has no room.
  const float gap = metrics::kMenuGap * s;
  float top = 0.0f;
  if (side == MenuOpenSide::kBelowAnchor) {
    top = anchor.bottom + gap;
    if (top + height > bounds.bottom - margin) {
      top = anchor.top - gap - height;
      layout.above = true;
    } else {
      layout.above = false;
    }
  } else {
    top = anchor.top - gap - height;
    if (top < bounds.top + margin) {
      top = anchor.bottom + gap;
      layout.above = false;
    }
  }
  top = std::clamp(top, bounds.top + margin,
                   std::max(bounds.top + margin,
                            bounds.bottom - margin - height));

  layout.panel = Rect::FromXYWH(left, top, width, height);
  layout.visible = !layout.panel.empty();

  layout.rows.reserve(items.size());
  float y = top + padding;
  for (const MenuItem& item : items) {
    const float row_h = RowHeight(item.kind, s);
    layout.rows.push_back(
        Rect::FromLTRB(left + metrics::kMenuRowInset * s, y,
                       left + width - metrics::kMenuRowInset * s, y + row_h));
    y += row_h;
  }
  return layout;
}

int MenuRowAt(const MenuLayout& layout, float x, float y) {
  if (!layout.visible || !layout.panel.Contains(x, y)) return -1;
  for (size_t i = 0; i < layout.rows.size(); ++i) {
    if (layout.rows[i].Contains(x, y)) return static_cast<int>(i);
  }
  return -1;
}

int MenuItemAt(const MenuLayout& layout, const std::vector<MenuItem>& items,
               float x, float y) {
  const int row = MenuRowAt(layout, x, y);
  if (row < 0) return -1;
  if (static_cast<size_t>(row) >= items.size()) return -1;
  const MenuItem& item = items[static_cast<size_t>(row)];
  if (!item.selectable() || !item.enabled) return -1;
  return row;
}

int NextSelectableRow(const std::vector<MenuItem>& items, int current,
                      int delta) {
  const int count = static_cast<int>(items.size());
  if (count == 0 || delta == 0) return -1;
  int row = current;
  if (row < -1 || row >= count) row = -1;
  const int step = delta > 0 ? 1 : -1;
  for (int i = 0; i < count; ++i) {
    row += step;
    if (row < 0) row = count - 1;
    if (row >= count) row = 0;
    if (row == current && i > 0) break;  // wrapped all the way around
    const MenuItem& item = items[static_cast<size_t>(row)];
    if (item.selectable() && item.enabled) return row;
  }
  return -1;
}

}  // namespace nv::ui
