#pragma once

#include "ui/rect.h"
#include "ui/ui_context.h"

#include <d2d1_1.h>

#include <cstdint>
#include <string_view>

namespace nv::ui::draw {

inline D2D1_RECT_F ToD2D(const Rect& rect) {
  return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

void FillRect(ID2D1DeviceContext* context, UiContext& ui, const Rect& rect,
              uint32_t argb);

void FillRoundRect(ID2D1DeviceContext* context, UiContext& ui, const Rect& rect,
                   float radius, uint32_t argb);

void StrokeRoundRect(ID2D1DeviceContext* context, UiContext& ui,
                     const Rect& rect, float radius, float stroke_width,
                     uint32_t argb);

// Single line of text inside `rect`. Vertically centred; horizontally centred
// when `center` is true, left-aligned otherwise. `ellipsis` trims with "…"
// instead of clipping when the text does not fit.
void DrawText(ID2D1DeviceContext* context, UiContext& ui, std::wstring_view text,
              float size_dip, int weight, const Rect& rect, uint32_t argb,
              bool ellipsis = false, bool center = false);

}  // namespace nv::ui::draw
