#pragma once

#include "ui/layout.h"
#include "ui/theme_tokens.h"

#include <d2d1_1.h>

#include <cstddef>
#include <string>
#include <vector>

namespace nv::ui {

class UiContext;

// One line of bottom-stack feedback (規劃/22/11 §4.1). `danger` adds the 3 px
// red left border used for failures that do not deserve a card.
struct ToastItem {
  std::wstring text;
  bool danger = false;
};

inline constexpr size_t kMaxToasts = 3;

// Pixel rectangle of the toast at `index` (0 = newest, drawn at the bottom).
// `anchor` is LayoutFrame::toast (its bottom edge and centre drive the stack);
// the width follows the measured text so short messages stay small.
Rect ComputeToastRect(const Rect& anchor, float text_width_px, size_t index,
                      float dpi_scale);

// Draws every toast rectangle bottom-up: newest at the bottom. `rects` must be
// produced by ComputeToastRect for the same list, so drawing and hover-pause
// hit-testing cannot disagree.
void DrawToasts(ID2D1DeviceContext* context, UiContext& ui,
                const std::vector<ToastItem>& items,
                const std::vector<Rect>& rects, const ColorTokens& colors,
                int hovered_index);

}  // namespace nv::ui
