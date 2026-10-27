#pragma once

#include "ui/layout.h"
#include "ui/theme_tokens.h"

#include <d2d1_1.h>

#include <string>

namespace nv::ui {

class UiContext;

// Indeterminate load indicator shown only when a decode takes longer than
// 400 ms (規劃/22/11 §4.4): a 120x3 DIP track with an accent segment bouncing
// back and forth, plus the localized "Loading…" line.
struct LoadingLayout {
  // Backing chip so the indicator stays readable over a bright photograph.
  Rect chip;
  Rect bar;
  Rect label;
  bool visible = false;
};

LoadingLayout ComputeLoadingLayout(const Rect& content, float dpi_scale);

// `phase` is 0..1 and drives the position of the accent segment. Drawing is
// allocation-free so it can run every animation tick.
void DrawLoadingState(ID2D1DeviceContext* context, UiContext& ui,
                      const LoadingLayout& layout, const std::wstring& text,
                      float phase, const ColorTokens& colors);

}  // namespace nv::ui
