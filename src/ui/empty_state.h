#pragma once

#include "ui/hit.h"
#include "ui/layout.h"
#include "ui/theme_tokens.h"

#include <d2d1_1.h>

#include <cstddef>
#include <string>
#include <vector>

namespace nv::ui {

class UiContext;
class IconSet;

// All strings of the empty card, already localized/formatted by the app. The
// format line is wrapped here because only the UI knows the measured width.
struct EmptyStrings {
  std::wstring title;
  std::wstring format_lines[2];
  int format_line_count = 0;
  bool format_truncated = false;
  std::wstring formats_more;
  std::wstring primary;
  std::wstring open_folder;
  std::wstring all_formats;
  std::wstring note;
  // "Release to open" pill while a drag hovers the window.
  std::wstring drag_hint;
};

// One row of the "all formats" modal.
struct FormatPanelRow {
  std::wstring name;
  std::wstring extensions;
  std::wstring note;  // empty for an available family
};

struct FormatPanelStrings {
  std::wstring title;
  std::wstring footer;
  std::wstring close;
  std::vector<FormatPanelRow> rows;
};

struct FormatPanelLayout {
  Rect scrim;
  Rect panel;
  Rect title;
  Rect footer;
  Rect close;
  std::vector<Rect> rows;
  // First family index shown; the card scrolls by whole rows when the family
  // list is taller than the available body (規劃/22/16: 52 extensions).
  size_t first_row = 0;
  size_t max_first_row = 0;
  bool visible = false;
};

// Centred modal card. The height follows the row count and is capped so the
// card never leaves the client; rows that would overflow are scrollable.
FormatPanelLayout ComputeFormatPanelLayout(const Rect& client, float dpi_scale,
                                           size_t row_count,
                                           size_t first_row = 0);

// kFormatsClose inside the close button, kFormatsPanel on the card, kNone
// outside (a modal never closes on an outside click, 規劃/22/20 §3.5).
HitId HitTestFormatPanel(const FormatPanelLayout& layout, float x, float y);

// Greedy wrap of `names` into at most two lines starting with `prefix`.
// Returns the used line count and reports truncation when names did not fit.
int WrapFormatNames(UiContext& ui, const std::wstring& prefix,
                    const std::vector<std::wstring>& names, float width_px,
                    float font_dip, int weight, std::wstring lines[2],
                    bool* truncated);

void DrawEmptyCard(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                   const EmptyLayout& layout, const EmptyStrings& strings,
                   EmptyKind kind, const ColorTokens& colors, float dpi_scale,
                   HitId hover, HitId pressed, int focus, bool drag_over);

// Accent wash over the image area while a drag hovers the window.
void DrawDragWash(ID2D1DeviceContext* context, UiContext& ui,
                  const Rect& content, const ColorTokens& colors);
// 2 px accent border around the whole client; drawn on the chrome layer so the
// title bar and bottom bar are framed too.
void DrawDragBorder(ID2D1DeviceContext* context, UiContext& ui,
                    const Rect& client, const ColorTokens& colors, float scale);
void DrawDragHint(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                  const LayoutFrame& layout, const std::wstring& text,
                  const ColorTokens& colors);

void DrawFormatPanel(ID2D1DeviceContext* context, UiContext& ui,
                     const FormatPanelLayout& layout,
                     const FormatPanelStrings& strings,
                     const ColorTokens& colors, float dpi_scale, HitId hover,
                     HitId pressed);

}  // namespace nv::ui
