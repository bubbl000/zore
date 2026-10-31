#pragma once

#include "ui/app_logo.h"
#include "ui/chrome.h"
#include "ui/anim_pill.h"
#include "ui/empty_state.h"
#include "ui/error_card.h"
#include "ui/hit.h"
#include "ui/layout.h"
#include "ui/loading_state.h"
#include "ui/menu.h"
#include "ui/overlay_stack.h"
#include "ui/preview_badge.h"
#include "ui/strip_tooltip.h"
#include "ui/theme_tokens.h"
#include "ui/toast.h"
#include "ui/ui_context.h"

#include "render/overlay_layer.h"

#include <windows.h>
#include <d2d1_1.h>

#include <string>
#include <vector>

namespace nv::ui {

// Everything the app tells the UI per frame. Strings are pre-formatted (i18n
// and number formatting stay in the app); geometry is derived from this.
//
// Startup empty state (規劃/22/03): the app decides which state is showing and
// supplies every string; UiRoot measures the format line and lays the card out.
struct EmptyUiInputs {
  bool visible = false;
  EmptyKind kind = EmptyKind::kNone;
  bool note = false;
  std::wstring title;
  std::wstring formats_prefix;
  std::vector<std::wstring> formats;
  std::wstring formats_more;
  std::wstring primary;
  std::wstring open_folder;
  std::wstring all_formats;
  std::wstring note_text;
  // "Release to open" pill while a drag hovers the window.
  std::wstring drag_hint;
};

// Rows of the "all formats" modal, already grouped and localized by the app.
struct FormatsUiInputs {
  std::wstring title;
  std::wstring footer;
  std::wstring close;
  std::vector<FormatPanelRow> rows;
};

// Error card content (規劃/22/11 §4.2). The app decides which actions exist and
// supplies their localized labels; the card always appends Close.
struct ErrorUiInputs {
  bool visible = false;
  std::wstring title;
  std::wstring message;
  std::wstring detail;
  std::vector<std::wstring> actions;
  std::wstring close;
  // Index of the action Enter runs (-1 when only Close exists).
  int default_action = -1;
};

// Danger confirmation card (規劃/22/11 §4.3), always a modal. The folder-edge
// prompt (規劃/22/10 §4.3) sets `danger = false` and shows the remember row.
struct ConfirmUiInputs {
  bool visible = false;
  std::wstring title;
  std::wstring body;
  std::wstring primary;
  std::wstring secondary;
  bool danger = true;
  bool remember_visible = false;
  bool remember_checked = false;
  std::wstring remember;
};

// Loading indicator (規劃/22/11 §4.4). The app owns the 400 ms delay and the
// animation phase.
struct LoadingUiInputs {
  bool visible = false;
  std::wstring text;
  float progress = 0.0f;
};

// Rename overlay (V3.1-06 §06e). The text itself lives in the app's child EDIT;
// these are the labels and the validation line.
struct RenameUiInputs {
  bool visible = false;
  std::wstring title;
  std::wstring hint;
  std::wstring ok;
  std::wstring cancel;
  std::wstring placeholder;
  std::wstring error;
};

// Frame control of an animated image (規劃/22/12 §3.1). The app owns the state
// machine and the idle fade; the UI only lays out, draws and hit-tests it.
struct AnimUiInputs {
  bool visible = false;
  bool show_buttons = true;
  bool playing = false;
  std::wstring label;
  float opacity = 1.0f;
};

// "Preview" badge of the two-stage load (規劃/22/13 §3.5). The app owns the
// state and the 160 ms fade; the badge never takes clicks.
struct PreviewUiInputs {
  bool visible = false;
  bool failed = false;
  float opacity = 1.0f;
  std::wstring text;
};

// Bird's eye view (v3.0, 規劃/26 §2.4). Only numbers: display size after the view
// rotation, the renderer's effective scale and the client-pixel centre of the
// picture. The panel/canvas/viewport geometry is derived by UiRoot::Update from
// the same frame every other control uses.
struct MinimapUiInputs {
  bool visible = false;
  uint32_t display_width = 0;
  uint32_t display_height = 0;
  float view_scale = 1.0f;
  float view_center_x = 0.0f;
  float view_center_y = 0.0f;
};

struct UiInputs {
  bool has_image = false;
  bool maximized = false;
  bool window_active = true;
  // v3.0 (規劃/26 §2.7): fade factor of the fullscreen reveal strip.
  float fs_reveal_opacity = 0.0f;

  bool pill_visible = false;
  bool strip_visible = false;
  bool strip_shown = false;
  StripMode strip_mode = StripMode::kPinned;
  // Auto-mode fade factor (規劃/22/08 §3.5). Pinned draws at 1.0, off at 0.
  float strip_opacity = 1.0f;
  // Hover tooltip (規劃/22/08 §3.3): the app owns the 500 ms delay and supplies
  // the slot rectangle in client pixels. Empty text hides the tooltip.
  std::wstring strip_tooltip;
  Rect strip_tooltip_anchor;
  // Previous/next circles in the image area (規劃/22/09 §5.1).
  bool arrows_visible = false;
  // Reveal state of the two circles: 1 = resting, 0 = slid away and invisible
  // (fades out when the pointer leaves the window or idles for five seconds).
  float arrows_opacity = 1.0f;
  float arrows_slide = 0.0f;

  bool percent_available = false;
  bool info_available = false;
  bool rotate_available = false;
  bool more_available = false;

  // OLE drag hover (規劃/22/03 §4.2).
  bool drag_over = false;

  ChromeStrings strings;
  // Rows of the open popup menu (zoom menu or more panel). The app builds this
  // list from the current settings/popup state every time it changes.
  std::vector<MenuItem> menu_items;
  // v3.0 (規劃/26 §2.9): client-pixel anchor of the right-click menu. It is a
  // zero-size rectangle at the click point; the bottom-bar menus ignore it.
  Rect menu_anchor;

  EmptyUiInputs empty;
  FormatsUiInputs formats;
  ErrorUiInputs error;
  ConfirmUiInputs confirm;
  RenameUiInputs rename;
  LoadingUiInputs loading;
  AnimUiInputs anim;
  PreviewUiInputs preview;
  MinimapUiInputs minimap;
};

// Single entry point for the custom-drawn window chrome (規劃/22/01 §3.2).
//
// Owns the DWrite context, the cached layout for the current frame, the
// interaction stack and the icon set. The app only routes messages here; it
// never computes a chrome rectangle itself.
class UiRoot {
 public:
  bool Create();
  // Builds the text formats/measurements used by the chrome while the D3D
  // device is still initializing on its worker thread. Without this the first
  // text layout happens inside the WM_SIZE that ShowWindow triggers, which adds
  // ~100 ms before the window can appear.
  void WarmUp();
  void Shutdown();

  void SetTheme(bool dark_theme, bool high_contrast);
  void SetDpiScale(float scale);
  void SetClientSize(float width, float height);
  float dpi_scale() const { return ui_.dpi_scale(); }

  // Recomputes the layout. Returns the cached frame; drawing and hit-testing
  // always read this, never a locally computed rectangle.
  const LayoutFrame& Update(const UiInputs& inputs);

  const LayoutFrame& layout() const { return layout_; }
  const TitleLayout& title() const { return title_; }
  const BottomLayout& bottom() const { return bottom_; }
  const MenuLayout& menu() const { return menu_; }
  const std::vector<MenuItem>& menu_items() const { return menu_items_; }
  const EmptyLayout& empty() const { return empty_; }
  const EmptyStrings& empty_strings() const { return empty_strings_; }
  const FormatPanelLayout& format_panel() const { return format_panel_; }
  const FormatPanelStrings& format_strings() const { return format_strings_; }
  const ErrorCardLayout& error_card() const { return error_card_; }
  const ErrorCardStrings& error_strings() const { return error_strings_; }
  const ConfirmCardLayout& confirm_card() const { return confirm_card_; }
  const ConfirmCardStrings& confirm_strings() const { return confirm_strings_; }
  const RenameCardLayout& rename_card() const { return rename_card_; }
  const RenameCardStrings& rename_strings() const { return rename_strings_; }
  const LoadingLayout& loading() const { return loading_; }
  const std::wstring& loading_text() const { return loading_text_; }
  const std::vector<Rect>& toast_rects() const { return toast_rects_; }
  const StripTooltipLayout& strip_tooltip() const { return strip_tooltip_; }
  const AnimPillLayout& anim_pill() const { return anim_pill_; }
  const AnimPillStrings& anim_strings() const { return anim_strings_; }
  const PreviewBadgeLayout& preview_badge() const { return preview_badge_; }
  const PreviewBadgeStrings& preview_strings() const { return preview_strings_; }
  // Scrolls the all-formats card by whole family rows (wheel). Clamped to the
  // layout's max_first_row on the next Update.
  void ScrollFormats(int rows);

  // Index of the toast under the pointer, or -1. Used to pause the expiry timer
  // while the pointer rests on a toast (規劃/22/11 §4.1).
  int ToastAt(float x, float y) const;
  int menu_hover() const { return menu_hover_; }
  const ColorTokens& colors() const { return tokens_.color; }
  const UiInputs& inputs() const { return inputs_; }

  UiContext& context() { return ui_; }
  Chrome& chrome() { return chrome_; }
  OverlayStack& overlay() { return overlays_; }
  const OverlayStack& overlay() const { return overlays_; }

  void SetHover(HitId id);
  void SetPressed(HitId id);
  HitId hover() const { return hover_; }
  HitId pressed() const { return pressed_; }
  void ClearPointerState();

  // Keyboard focus inside the empty card (Tab / Shift+Tab, Enter activates).
  // 0 = choose image, 1 = open folder, 2 = all formats.
  int empty_focus() const { return empty_focus_; }
  void SetEmptyFocus(int index);
  void CycleEmptyFocus(bool backward);

  // Full hit stack: overlay layers first, then title buttons, bottom bar, strip
  // and image area. Returns kNone when the point belongs to no layer.
  HitId HitTest(float x, float y) const;
  // Title-bar buttons only; used by WM_NCHITTEST before the rest of the stack.
  HitId HitTestTitleButtons(float x, float y) const;

  // Popup menu geometry. An open menu owns the pointer: the app asks these
  // instead of the chrome hit stack (規劃/22/20 §3.2).
  bool MenuContains(float x, float y) const;
  int MenuRowAt(float x, float y) const;
  // Selectable row under the point, or -1 (outside, separator, header, disabled).
  int MenuItemAt(float x, float y) const;
  // Updates hover for the open menu; returns true when the highlighted row
  // changed so the caller can repaint.
  bool UpdateMenuHover(float x, float y);
  // Moves the keyboard highlight one selectable row (v3.0, 規劃/26 §2.9).
  // Returns true when the highlight changed so the caller can repaint.
  bool StepMenuHover(int delta);

  // One Escape press closes exactly one layer (規劃/22/20 §3.4).
  EscapeAction HandleEscape();

  // Draws one overlay layer. The renderer calls this once per layer in order so
  // "title bar last" is guaranteed by the renderer, not by call order.
  void Draw(ID2D1DeviceContext* context, nv::render::OverlayLayer layer);

  // GDI first frame (before D3D is ready): fills the chrome bands with their
  // theme colours so the window never looks like an unstyled rectangle.
  void PaintGdiBackground(HDC dc) const;

 private:
  void DrawToasts(ID2D1DeviceContext* context);
  ChromeState MakeChromeState();

  UiContext ui_;
  Chrome chrome_;
  AppLogo app_logo_;
  ID2D1DeviceContext* draw_context_ = nullptr;
  OverlayStack overlays_;
  TokenSet tokens_{};
  LayoutFrame layout_{};
  TitleLayout title_{};
  BottomLayout bottom_{};
  MenuLayout menu_{};
  std::vector<MenuItem> menu_items_;
  EmptyLayout empty_{};
  EmptyStrings empty_strings_{};
  FormatPanelLayout format_panel_{};
  FormatPanelStrings format_strings_{};
  size_t format_first_row_ = 0;
  ErrorCardLayout error_card_{};
  ErrorCardStrings error_strings_{};
  ConfirmCardLayout confirm_card_{};
  RenameCardLayout rename_card_{};
  RenameCardStrings rename_strings_{};
  ConfirmCardStrings confirm_strings_{};
  LoadingLayout loading_{};
  std::wstring loading_text_;
  float loading_phase_ = 0.0f;
  std::vector<Rect> toast_rects_;
  StripTooltipLayout strip_tooltip_{};
  AnimPillLayout anim_pill_{};
  AnimPillStrings anim_strings_{};
  PreviewBadgeLayout preview_badge_{};
  PreviewBadgeStrings preview_strings_{};
  UiInputs inputs_{};
  HitId hover_ = HitId::kNone;
  HitId pressed_ = HitId::kNone;
  int menu_hover_ = -1;
  int empty_focus_ = 0;
  float client_width_ = 0.0f;
  float client_height_ = 0.0f;
};

}  // namespace nv::ui
