#pragma once

#include "ui/rect.h"

#include <cstddef>
#include <string>
#include <vector>

namespace nv::ui {

// In-window popup menus (規劃/22/02 §3.3): the zoom menu behind the percentage
// button and the "more" panel behind 更多▾. Both are the same widget: a vertical
// list of rows with an optional switch/radio mark, laid out by one pure
// function so drawing and hit-testing cannot disagree.
//
// The app owns the commands and the strings; this module only knows geometry,
// kind and checked/enabled state.

enum class MenuItemKind {
  kCommand,    // plain row, closes the menu when chosen
  kToggle,     // row with a switch on the right
  kRadio,      // row with a check mark for the selected value
  kSeparator,  // thin line, never hit-testable
  kHeader,     // muted group caption, never hit-testable
};

struct MenuItem {
  std::wstring label;
  MenuItemKind kind = MenuItemKind::kCommand;
  // Opaque app command id; only meaningful for selectable rows.
  int command = 0;
  bool checked = false;
  bool enabled = true;

  bool selectable() const {
    return kind == MenuItemKind::kCommand || kind == MenuItemKind::kToggle ||
           kind == MenuItemKind::kRadio;
  }
};

struct MenuLayout {
  Rect panel;
  std::vector<Rect> rows;  // one rect per item, same order as the input
  float dpi_scale = 1.0f;
  bool visible = false;
  bool above = true;  // false when the menu was flipped below its anchor
};

// Which way a menu prefers to open. Bottom-bar menus (zoom / more) open upwards;
// the right-click menu hangs below the pointer (v3.0, 規劃/26 §2.9).
enum class MenuOpenSide {
  kAboveAnchor = 0,
  kBelowAnchor,
};

// Anchors `items` above `anchor` (the owning bottom-bar part), left-aligned with
// it, clamped into `bounds`; flips below the anchor when there is not enough
// room above (規劃/22/02 坑 7). `side = kBelowAnchor` mirrors that rule for the
// right-click menu: it opens below/right of the anchor and flips up/left near an
// edge. Separator rows use the compact `kMenuSeparatorRowHeight`; every other
// row keeps the uniform `kMenuRowHeight`. `width_dip` is DIP; every returned
// rectangle is in client pixels.
MenuLayout ComputeMenuLayout(const Rect& anchor, const Rect& bounds,
                             const std::vector<MenuItem>& items,
                             float dpi_scale, float width_dip,
                             MenuOpenSide side = MenuOpenSide::kAboveAnchor);

// Index of the hit row, or -1 when the point is outside the panel or on a row
// that cannot be selected (separator/header/disabled). Disabled rows still
// return their index through `MenuRowAt` so callers can decide to keep the menu
// open instead of acting.
int MenuRowAt(const MenuLayout& layout, float x, float y);
int MenuItemAt(const MenuLayout& layout, const std::vector<MenuItem>& items,
               float x, float y);

// Moves the keyboard highlight by `delta` rows (+1 down / -1 up), wrapping at
// the ends and skipping separators, headers and disabled rows. `current` may be
// -1 (nothing hovered): +1 picks the first selectable row, -1 the last one.
// Returns -1 when no row can be selected.
int NextSelectableRow(const std::vector<MenuItem>& items, int current,
                      int delta);

}  // namespace nv::ui
