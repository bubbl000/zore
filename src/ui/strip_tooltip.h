#pragma once

#include "ui/rect.h"
#include "ui/theme_tokens.h"
#include "ui/ui_context.h"

#include <d2d1_1.h>

#include <string_view>

namespace nv::ui {

// Hover tooltip of the thumbnail rail (規劃/22/08 §3.3): the file name of the
// slot under the pointer, 500 ms after the pointer stops.
//
// Geometry is a pure function of the slot rectangle, the client rectangle and
// the measured text width, so the layout unit test needs no D3D device. The
// tooltip prefers the space above the slot, flips below when the window edge
// would clip it and always clamps horizontally into the client area.
struct StripTooltipLayout {
  Rect panel;
  bool visible = false;
  // True when the panel sits above the slot (the preferred position).
  bool above = true;
};

StripTooltipLayout ComputeStripTooltipLayout(const Rect& anchor,
                                             const Rect& client,
                                             float text_width, float dpi_scale);

// Draws the panel, border and one ellipsized line of text. The layer order is
// fixed by the caller: above the rail, below the bottom bar.
void DrawStripTooltip(ID2D1DeviceContext* context, UiContext& ui,
                      const StripTooltipLayout& layout, std::wstring_view text,
                      const ColorTokens& colors, float dpi_scale);

}  // namespace nv::ui
