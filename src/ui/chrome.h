#pragma once

#include "ui/hit.h"
#include "ui/icons.h"
#include "ui/layout.h"
#include "ui/menu.h"
#include "ui/theme_tokens.h"

#include <d2d1_1.h>

#include <string>
#include <utility>
#include <vector>

namespace nv::ui {

class UiContext;

// The five title-bar segments, already formatted by the app (i18n and number
// formatting stay out of the UI module).
struct ChromeStrings {
  std::wstring file_name;
  std::wstring dimension;
  std::wstring file_size;
  std::wstring index;
  std::wstring percent;
};

// One frame of chrome state. Everything the drawing code needs comes from the
// cached LayoutFrame, so drawing and hit-testing cannot drift apart.
struct ChromeState {
  const LayoutFrame* layout = nullptr;
  const TitleLayout* title = nullptr;
  const BottomLayout* bottom = nullptr;
  const ColorTokens* colors = nullptr;
  const ChromeStrings* strings = nullptr;
  const MenuLayout* menu = nullptr;
  const std::vector<MenuItem>* menu_items = nullptr;
  // Brand logo for the title bar (ui/app_logo.cpp); null falls back to
  // the vector glyph, e.g. if the resource is missing.
  ID2D1Bitmap* app_logo = nullptr;
  HitId hover = HitId::kNone;
  HitId pressed = HitId::kNone;
  int menu_hover = -1;
  bool active = true;
  bool has_image = false;
  bool maximized = false;
  bool fullscreen = false;
  bool topmost = false;
};

class Chrome {
 public:
  void Shutdown();

  void DrawTitleBar(ID2D1DeviceContext* context, UiContext& ui,
                    const ChromeState& state);
  void DrawBottomBar(ID2D1DeviceContext* context, UiContext& ui,
                     const ChromeState& state);
  // Popup menus are drawn on the top-panel layer, above the image area but
  // below the title bar. The info card is its own top-level window
  // (ui/info_window.*, 規劃/26 §2.1).
  void DrawImageArrows(ID2D1DeviceContext* context, UiContext& ui,
                       const ChromeState& state);
  void DrawMenu(ID2D1DeviceContext* context, UiContext& ui,
                const ChromeState& state);
  // v3.0 (規劃/26 §2.7): the two buttons that float in the top-right corner of
  // a fullscreen window. Same visual language as the title-bar buttons.
  void DrawFullscreenReveal(ID2D1DeviceContext* context, UiContext& ui,
                            const ChromeState& state);

  IconSet& icons() { return icons_; }

 private:
  void DrawTitleButton(ID2D1DeviceContext* context, UiContext& ui,
                       const ChromeState& state, HitId id, IconId icon);
  void DrawBottomPart(ID2D1DeviceContext* context, UiContext& ui,
                      const ChromeState& state, BottomPart part, bool enabled);
  void DrawMenuRow(ID2D1DeviceContext* context, UiContext& ui,
                   const ChromeState& state, const MenuItem& item,
                   const Rect& row, bool hovered);
  void DrawToggle(ID2D1DeviceContext* context, UiContext& ui, const Rect& box,
                  bool on, bool enabled, const ColorTokens& colors, float s);

  IconSet icons_;
};

}  // namespace nv::ui
