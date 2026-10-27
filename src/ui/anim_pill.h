#pragma once

#include "ui/hit.h"
#include "ui/icons.h"
#include "ui/rect.h"
#include "ui/theme_tokens.h"

#include <d2d1_1.h>

#include <string>

namespace nv::ui {

class UiContext;

// Frame control of an animated image (規劃/22/12 §3.1):
//
//     ┌──────────────────────────────┐
//     │  ◀   12 / 48   ▶      ⏸      │
//     └──────────────────────────────┘
//
// It sits on the bottom stack (above the bottom bar and a pinned rail) and is
// laid out only while an animated image is loaded, so a static file has neither
// a pill nor a hit target (坑 15).
struct AnimPillLayout {
  bool visible = false;
  // False for the degraded "first frame only" state: the pill is a label, not
  // a control, because there is nothing to step through.
  bool interactive = false;
  Rect pill;
  Rect prev;
  Rect label;
  Rect next;
  Rect toggle;
};

// `buttons` false lays the pill out as a centred label (first frame only).
AnimPillLayout ComputeAnimPillLayout(const Rect& pill, float dpi_scale,
                                     bool buttons);

// Buttons only; the pill surface itself is not a target so a click between the
// buttons falls through to the image instead of doing nothing.
HitId HitTestAnimPill(const AnimPillLayout& layout, float x, float y);

struct AnimPillStrings {
  // Already localised by the app: "12 / 48", or "仅首帧" in the degraded state.
  std::wstring label;
  bool playing = false;
  bool show_buttons = true;
  // 1.0 normally, fading to 0.35 after two idle seconds (規劃/22/12 §3.1).
  float opacity = 1.0f;
};

void DrawAnimPill(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                  const AnimPillLayout& layout, const AnimPillStrings& strings,
                  const ColorTokens& colors, bool high_contrast,
                  float dpi_scale, HitId hover, HitId pressed);

}  // namespace nv::ui
