// v2.0 S0/S1: pure layout / hit / overlay / window-state / empty-state checks.
//
// Everything asserted here is also read by the drawing code, so a regression
// shows up as a failed check instead of a control that is painted somewhere it
// cannot be clicked.

#include "tests/ui_layout_tests.h"

#include "core/animation_controller.h"
#include "core/folder_navigator.h"
#include "core/format_summary.h"
#include "core/image_transform.h"
#include "core/preview_plan.h"
#include "core/supported_extensions.h"
#include "core/ui_error.h"
#include "render/draw_quality.h"
#include "render/view_transform.h"
#include "render/zoom_math.h"
#include "ui/command.h"
#include "ui/anim_pill.h"
#include "ui/empty_state.h"
#include "ui/error_card.h"
#include "ui/info_panel.h"
#include "ui/layout.h"
#include "ui/loading_state.h"
#include "ui/menu.h"
#include "ui/metrics.h"
#include "ui/overlay_stack.h"
#include "ui/preview_badge.h"
#include "ui/strip_tooltip.h"
#include "ui/theme_tokens.h"
#include "ui/toast.h"
#include "ui/window_state.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* name) {
  if (condition) {
    std::printf("ok   %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
    ++g_failures;
  }
}

void CheckNear(float actual, float expected, const char* name) {
  const bool ok = std::fabs(actual - expected) <= 0.02f;
  std::printf("%s %s (actual=%.2f expected=%.2f)\n", ok ? "ok  " : "FAIL", name,
              actual, expected);
  if (!ok) ++g_failures;
}

using nv::ui::ComputeBottomLayout;
using nv::ui::ComputeEmptyLayout;
using nv::ui::ComputeLayout;
using nv::ui::ComputeTitleLayout;
using nv::ui::EmptyKind;
using nv::ui::HitId;
using nv::ui::HitTestEmpty;
using nv::ui::HitTestFrame;
using nv::ui::LayoutInputs;
using nv::ui::Rect;
using nv::ui::StripMode;

nv::ui::LayoutFrame PinnedFrame() {
  LayoutInputs inputs;
  inputs.has_image = true;
  inputs.strip_visible = true;
  inputs.strip_shown = true;
  inputs.strip_mode = StripMode::kPinned;
  return ComputeLayout(inputs, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
}

void TestContentAndChrome() {
  const nv::ui::LayoutFrame frame = PinnedFrame();
  CheckNear(frame.title.height(), 32.0f, "title bar is 32 DIP");
  CheckNear(frame.bottom_bar.height(), 44.0f, "bottom bar is 44 DIP");
  CheckNear(frame.strip.height(), 76.0f, "pinned strip is 76 DIP");
  CheckNear(frame.content.top, 32.0f, "content starts below the title bar");
  CheckNear(frame.content.bottom, 800.0f - 44.0f - 76.0f,
            "content ends above strip + bottom bar");
  Check(nv::ui::NearEqual(frame.strip, Rect::FromLTRB(0, 680, 1280, 756)),
        "strip sits directly above the bottom bar");
  Check(nv::ui::NearEqual(frame.bottom_bar,
                          Rect::FromLTRB(0, 756, 1280, 800)),
        "bottom bar is flush with the client bottom");
  Check(frame.strip_counts_in_content, "pinned strip reduces the content rect");

  LayoutInputs no_strip;
  no_strip.has_image = true;
  const nv::ui::LayoutFrame off =
      ComputeLayout(no_strip, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
  CheckNear(off.content.bottom, 756.0f, "no strip: content stops above the bar");
  Check(off.strip.empty(), "no strip: no strip rectangle");
  Check(!off.strip_counts_in_content, "no strip: content is not reduced");

  LayoutInputs fullscreen;
  fullscreen.has_image = true;
  fullscreen.fullscreen = true;
  fullscreen.strip_visible = true;
  const nv::ui::LayoutFrame fs =
      ComputeLayout(fullscreen, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
  Check(fs.title.empty() && fs.bottom_bar.empty(),
        "fullscreen hides title and bottom bars");
  Check(nv::ui::NearEqual(fs.content, Rect::FromLTRB(0, 0, 1280, 800)),
        "fullscreen content is the whole client");
}

void TestAutoStripAndTooltip() {
  // Auto, faded out: the rail rectangle still exists because it hosts the 8 DIP
  // reveal hot zone, but the content is not reduced and the area stays image
  // content so a click can never select an invisible thumbnail (22/08 坑 3).
  LayoutInputs inputs;
  inputs.has_image = true;
  inputs.strip_visible = true;
  inputs.strip_mode = StripMode::kAuto;
  inputs.strip_shown = false;
  const nv::ui::TitleLayout title{};
  const nv::ui::BottomLayout bottom{};
  const nv::ui::EmptyLayout empty{};
  const nv::ui::LayoutFrame hidden =
      ComputeLayout(inputs, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
  CheckNear(hidden.content.bottom, 756.0f,
            "auto hidden: the content reaches the bottom bar");
  Check(!hidden.strip.empty(),
        "auto hidden: the rail keeps its rectangle for the hot zone");
  Check(!hidden.strip_counts_in_content, "auto hidden: no space is reserved");
  Check(!hidden.strip_interactive, "auto hidden: the rail owns no clicks");
  Check(HitTestFrame(hidden, title, bottom, empty, 640, 700) == HitId::kImage,
        "auto hidden: the rail area is image content");

  inputs.strip_shown = true;
  const nv::ui::LayoutFrame shown =
      ComputeLayout(inputs, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
  CheckNear(shown.content.bottom, 756.0f,
            "auto shown: the content still reaches the bottom bar");
  Check(shown.strip_interactive, "auto shown: the rail owns clicks");
  Check(HitTestFrame(shown, title, bottom, empty, 640, 700) == HitId::kStrip,
        "auto shown: the rail area hits kStrip");

  // Tooltip geometry: above the slot by default, clamped into the client,
  // capped at 320 DIP, below the slot when the top edge is too close, and
  // scaled by DPI.
  const Rect client = Rect::FromLTRB(0, 0, 1280, 800);
  const Rect slot = Rect::FromLTRB(100, 680, 168, 756);
  const nv::ui::StripTooltipLayout tip =
      nv::ui::ComputeStripTooltipLayout(slot, client, 120.0f, 1.0f);
  Check(tip.visible && tip.above, "tooltip sits above the slot");
  CheckNear(tip.panel.bottom, 674.0f, "tooltip is 6 DIP above the slot");
  Check(tip.panel.left >= 4.0f && tip.panel.right <= 1276.0f,
        "tooltip stays inside the client");
  CheckNear(tip.panel.height(), 26.0f, "tooltip is 26 DIP tall");

  const nv::ui::StripTooltipLayout wide =
      nv::ui::ComputeStripTooltipLayout(slot, client, 900.0f, 1.0f);
  CheckNear(wide.panel.width(), 320.0f, "tooltip caps at 320 DIP");

  const nv::ui::StripTooltipLayout edge = nv::ui::ComputeStripTooltipLayout(
      Rect::FromLTRB(0, 680, 68, 756), client, 200.0f, 1.0f);
  CheckNear(edge.panel.left, 4.0f, "tooltip clamps at the left edge");

  const nv::ui::StripTooltipLayout flipped = nv::ui::ComputeStripTooltipLayout(
      Rect::FromLTRB(600, 10, 668, 86), client, 120.0f, 1.0f);
  Check(!flipped.above && flipped.panel.top >= 86.0f,
        "tooltip flips below near the top edge");

  const nv::ui::StripTooltipLayout scaled = nv::ui::ComputeStripTooltipLayout(
      Rect::FromLTRB(200, 600, 268, 676), client, 120.0f, 1.25f);
  CheckNear(scaled.panel.height(), 32.5f, "tooltip scales with DPI");

  // A client narrower than the minimum still gets one valid panel (the clamp
  // bounds must not invert).
  const nv::ui::StripTooltipLayout narrow = nv::ui::ComputeStripTooltipLayout(
      Rect::FromLTRB(10, 680, 78, 756), Rect::FromLTRB(0, 0, 40, 800), 900.0f,
      1.0f);
  Check(narrow.visible && narrow.panel.width() <= 32.01f,
        "tooltip shrinks into a very narrow client");
}

void TestBottomStack() {
  LayoutInputs inputs;
  inputs.has_image = true;
  inputs.strip_visible = true;
  inputs.strip_mode = StripMode::kPinned;
  inputs.pill_visible = true;
  inputs.toast_visible = true;
  const nv::ui::LayoutFrame frame =
      ComputeLayout(inputs, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));

  // stack_top = 800 - 44 - 76 = 680
  CheckNear(frame.pill.bottom, 668.0f, "pill sits 12 above the stack top");
  CheckNear(frame.pill.height(), 36.0f, "pill is 36 DIP tall");
  CheckNear(frame.toast.bottom, 668.0f - 48.0f,
            "toast clears the visible pill");
  CheckNear(frame.toast.bottom - frame.toast.top, 36.0f, "toast is 36 DIP tall");

  inputs.pill_visible = false;
  const nv::ui::LayoutFrame without_pill =
      ComputeLayout(inputs, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
  CheckNear(without_pill.toast.bottom, 668.0f,
            "without a pill the toast sits on the stack top");
}

void TestDpiScaling() {
  for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
    LayoutInputs inputs;
    inputs.has_image = true;
    inputs.strip_visible = true;
    inputs.strip_mode = StripMode::kPinned;
    const nv::ui::LayoutFrame frame =
        ComputeLayout(inputs, scale, Rect::FromXYWH(0, 0, 1280, 800));
    CheckNear(frame.title.height(), 32.0f * scale, "title scales with DPI");
    CheckNear(frame.bottom_bar.height(), 44.0f * scale, "bottom scales with DPI");
    CheckNear(frame.strip.height(), 76.0f * scale, "strip scales with DPI");
    CheckNear(frame.content.top, 32.0f * scale, "content top scales with DPI");
    CheckNear(frame.content.bottom, 800.0f - (44.0f + 76.0f) * scale,
              "content bottom scales with DPI");
  }
}

void TestTitleSegments() {
  const nv::ui::LayoutFrame frame = PinnedFrame();
  const std::array<float, nv::ui::kSegCount> widths = {200, 100, 80};
  const nv::ui::TitleLayout title =
      ComputeTitleLayout(frame.title, 1.0f, true, widths);
  Check(title.buttons[0].right <= frame.title.right + 0.02f,
        "title buttons stay inside the bar");
  CheckNear(title.buttons[nv::ui::kTitleButtonCount - 1].right,
            frame.title.right, "the close button is flush right");
  CheckNear(title.button_width, 46.0f, "title buttons are 46 DIP normally");
  for (size_t i = 0; i < nv::ui::kSegCount; ++i) {
    Check(title.segment_visible[i], "wide window shows all three segments");
  }
  CheckNear(title.segment[0].left, title.text_area.left,
            "the first segment starts at the text area");
  Check(title.segment[0].right < title.segment[1].left,
        "segments do not overlap");
  CheckNear(title.segment[0].height(), 32.0f, "segments fill the bar height");

  // Narrow window: segments drop in priority order, the file name never does.
  const nv::ui::LayoutFrame narrow =
      ComputeLayout(LayoutInputs{}, 1.0f, Rect::FromXYWH(0, 0, 560, 800));
  const nv::ui::TitleLayout compact =
      ComputeTitleLayout(narrow.title, 1.0f, true, widths);
  Check(compact.compact, "a narrow window uses the compact button width");
  CheckNear(compact.button_width, 40.0f, "compact buttons are 40 DIP");
  Check(compact.segment_visible[nv::ui::kSegFileName],
        "the file name is never dropped");
  Check(!compact.segment_visible[nv::ui::kSegFileSize],
        "file size is the first segment to drop");

  // Without the icon the text starts at the left padding.
  const nv::ui::TitleLayout no_icon =
      ComputeTitleLayout(frame.title, 1.0f, false, widths);
  CheckNear(no_icon.text_area.left, 12.0f,
            "without an icon the text starts at the padding");
}

void TestBottomParts() {
  const nv::ui::LayoutFrame frame = PinnedFrame();
  nv::ui::BottomInputs inputs;
  inputs.has_image = true;
  inputs.percent_available = true;
  inputs.info_available = true;
  inputs.more_available = true;
  inputs.rotate_available = true;
  const nv::ui::BottomLayout bottom =
      ComputeBottomLayout(frame.bottom_bar, 1.0f, inputs);
  CheckNear(bottom.part[nv::ui::kPartInfo].left, 12.0f,
            "the bottom cluster starts at the padding");
  CheckNear(bottom.part[nv::ui::kPartInfo].width(), 28.0f, "info is 28 DIP");
  CheckNear(bottom.part[nv::ui::kPartPercent].width(), 56.0f,
            "percent is 56 DIP");
  Check(!bottom.enabled[nv::ui::kPartIndex], "the index is display only");
  CheckNear(bottom.part[nv::ui::kPartMore].right, 1280.0f - 12.0f,
            "the more button is flush right");
  Check(bottom.enabled[nv::ui::kPartFit] &&
            bottom.enabled[nv::ui::kPartActual],
        "fit and 1:1 are enabled with an image");
  Check(bottom.enabled[nv::ui::kPartRotateLeft] &&
            bottom.enabled[nv::ui::kPartRotateRight],
        "the rotate pair is live with an image (22/05)");

  // No image: every part except 更多 is disabled; 更多 stays usable because
  // the view switches work without a picture (planning 22/02 3.3).
  nv::ui::BottomInputs none;
  none.more_available = true;
  const nv::ui::BottomLayout empty =
      ComputeBottomLayout(frame.bottom_bar, 1.0f, none);
  for (size_t i = 0; i < nv::ui::kPartCount; ++i) {
    if (i == nv::ui::kPartMore) continue;
    Check(!empty.enabled[i], "no image disables every other bottom part");
  }
  Check(empty.enabled[nv::ui::kPartMore], "more stays enabled without an image");

  // The right cluster is laid out from the right edge inwards.
  const float more_left = bottom.part[nv::ui::kPartMore].left;
  CheckNear(bottom.part[nv::ui::kPartRotateRight].right, more_left,
            "rotate right sits left of more");
  Check(bottom.part[nv::ui::kPartFit].right <=
            bottom.part[nv::ui::kPartActual].left + 0.02f,
        "fit sits left of 1:1");
}

// v3.0 (規劃/26 §2.9): the right-click menu reuses ComputeMenuLayout with a
// point anchor and a below-first placement, and uses compact separator rows.
// The keyboard highlight walks the selectable rows only.
void TestMenuLayout() {
  using nv::ui::ComputeMenuLayout;
  using nv::ui::MenuItem;
  using nv::ui::MenuItemKind;
  using nv::ui::MenuLayout;
  using nv::ui::MenuOpenSide;
  using nv::ui::NextSelectableRow;

  const Rect client = Rect::FromLTRB(0, 0, 1280, 800);

  MenuItem command;
  command.kind = MenuItemKind::kCommand;
  command.label = L"a";
  command.command = 1;
  MenuItem separator;
  separator.kind = MenuItemKind::kSeparator;
  MenuItem disabled = command;
  disabled.enabled = false;
  MenuItem second = command;
  second.label = L"b";
  second.command = 2;
  const std::vector<MenuItem> items = {command, separator, disabled, second};

  // Bottom-bar placement is unchanged: above the anchor and left-aligned.
  const Rect bar_anchor = Rect::FromLTRB(100, 700, 220, 740);
  const MenuLayout above =
      ComputeMenuLayout(bar_anchor, client, items, 1.0f, 240.0f);
  Check(above.visible, "the popup menu is laid out");
  Check(above.above, "a bottom-bar menu still opens above its anchor");
  Check(above.panel.bottom <= bar_anchor.top + 0.02f,
        "the bottom-bar panel sits above the anchor");
  CheckNear(above.panel.left, 100.0f, "the bottom-bar panel is left-aligned");
  CheckNear(above.rows[1].top - above.rows[0].top, 32.0f,
            "a command row keeps the uniform 32 DIP height");
  CheckNear(above.rows[2].top - above.rows[1].top, 10.0f,
            "a separator owns a compact 10 DIP row");
  CheckNear(above.panel.height(), 122.0f,
            "the panel height sums command and separator rows");

  // Right-click placement: below/right of a zero-size anchor.
  const Rect point = Rect::FromLTRB(640, 400, 640, 400);
  const MenuLayout below = ComputeMenuLayout(point, client, items, 1.0f, 240.0f,
                                             MenuOpenSide::kBelowAnchor);
  Check(!below.above, "the right-click menu opens below the point");
  Check(below.panel.top >= 400.0f, "the panel starts below the click");
  CheckNear(below.panel.left, 640.0f, "the panel starts at the click x");

  // Near the right edge it flips to the left of the point.
  const Rect right_point = Rect::FromLTRB(1200, 300, 1200, 300);
  const MenuLayout flipped =
      ComputeMenuLayout(right_point, client, items, 1.0f, 240.0f,
                        MenuOpenSide::kBelowAnchor);
  Check(flipped.panel.right <= 1200.02f,
        "a menu near the right edge flips to the left of the point");
  Check(flipped.panel.left >= 8.0f, "the flipped menu stays inside the client");

  // Near the bottom edge it flips above the point.
  const Rect bottom_point = Rect::FromLTRB(500, 780, 500, 780);
  const MenuLayout raised =
      ComputeMenuLayout(bottom_point, client, items, 1.0f, 240.0f,
                        MenuOpenSide::kBelowAnchor);
  Check(raised.above, "a menu near the bottom edge flips above the point");
  Check(raised.panel.bottom <= 780.02f, "the raised panel stays above the click");

  // Keyboard walk: separators and disabled rows are skipped, ends wrap.
  Check(NextSelectableRow(items, -1, 1) == 0,
        "Down from nothing selects the first command");
  Check(NextSelectableRow(items, -1, -1) == 3,
        "Up from nothing selects the last command");
  Check(NextSelectableRow(items, 0, 1) == 3,
        "Down skips the separator and the disabled row");
  Check(NextSelectableRow(items, 3, 1) == 0, "Down wraps to the first command");
  Check(NextSelectableRow(items, 0, -1) == 3,
        "Up wraps past the separator to the last command");
  const std::vector<MenuItem> none = {separator, disabled};
  Check(NextSelectableRow(none, -1, 1) == -1,
        "a menu with no selectable row returns -1");
}

void TestHitStack() {
  const nv::ui::LayoutFrame frame = PinnedFrame();
  const std::array<float, nv::ui::kSegCount> widths = {200, 100, 80};
  const nv::ui::TitleLayout title =
      ComputeTitleLayout(frame.title, 1.0f, true, widths);
  nv::ui::BottomInputs bottom_inputs;
  bottom_inputs.has_image = true;
  bottom_inputs.percent_available = true;
  const nv::ui::BottomLayout bottom =
      ComputeBottomLayout(frame.bottom_bar, 1.0f, bottom_inputs);

  const nv::ui::EmptyLayout empty{};
  const Rect fullscreen_button = title.buttons[1];
  Check(HitTestFrame(frame, title, bottom, empty,
                     fullscreen_button.left + 4, fullscreen_button.top + 4) ==
            HitId::kTitleFullscreen,
        "title button 2 hits kTitleFullscreen");
  const Rect fit = bottom.part[nv::ui::kPartFit];
  Check(HitTestFrame(frame, title, bottom, empty, fit.left + 4, fit.top + 4) ==
            HitId::kBottomFit,
        "fit part hits kBottomFit");
  Check(HitTestFrame(frame, title, bottom, empty, 640, 400) == HitId::kImage,
        "image area hits kImage");
  Check(HitTestFrame(frame, title, bottom, empty, 640, 700) == HitId::kStrip,
        "strip area hits kStrip");
  Check(HitTestFrame(frame, title, bottom, empty, -5, 400) == HitId::kNone,
        "outside the client hits nothing");

  // Disabled parts are transparent to the hit stack (no image -> fit is grey).
  nv::ui::BottomInputs no_image;
  const nv::ui::BottomLayout disabled =
      ComputeBottomLayout(frame.bottom_bar, 1.0f, no_image);
  Check(HitTestFrame(frame, title, disabled, empty, fit.left + 4,
                     fit.top + 4) == HitId::kNone,
        "a disabled bottom part is not hit-testable");
}

void TestOverlayStack() {
  nv::ui::OverlayStack stack;
  Check(stack.Escape() == nv::ui::EscapeAction::kNone,
        "escape with nothing open does nothing");

  stack.SetSettingsOpen(true);
  stack.SetFullscreen(true);
  Check(stack.Escape() == nv::ui::EscapeAction::kClosedSettings,
        "escape closes settings before anything else");
  Check(stack.Escape() == nv::ui::EscapeAction::kExitedFullscreen,
        "a second escape leaves fullscreen");

  Check(stack.OpenModal(nv::ui::OverlayKind::kConfirm), "a modal opens");
  Check(!stack.OpenModal(nv::ui::OverlayKind::kRename),
        "a second modal is rejected");
  Check(stack.BlocksImageCommands(), "a modal blocks image commands");
  Check(stack.Escape() == nv::ui::EscapeAction::kClosedModal,
        "escape closes the modal");

  Check(stack.OpenPopup(nv::ui::OverlayKind::kMore), "a popup opens");
  Check(stack.BlocksNavigation(), "a popup blocks navigation");
  Check(stack.Escape() == nv::ui::EscapeAction::kClosedPopup,
        "escape closes the popup");
  Check(stack.TogglePopup(nv::ui::OverlayKind::kPercent),
        "the percent popup opens");
  Check(stack.IsPopup(nv::ui::OverlayKind::kPercent), "percent is the popup");
  Check(stack.TogglePopup(nv::ui::OverlayKind::kPercent),
        "toggling the same popup closes it");

  // v3.0 (規劃/26 §2.1): the info card is a top-level window, so the stack no
  // longer owns an info layer and Escape cannot close it from the main window.

  // A modal dismisses any popup underneath it.
  Check(stack.OpenPopup(nv::ui::OverlayKind::kMore), "popup reopens");
  Check(stack.OpenModal(nv::ui::OverlayKind::kPrintOptions), "modal opens");
  Check(stack.popup() == nv::ui::OverlayKind::kNone,
        "opening a modal dismisses the popup");

  // Toasts: bounded queue, newest at the back, duplicates merged (22/11 坑 5).
  for (int i = 0; i < 5; ++i) {
    stack.PushToast(L"t" + std::to_wstring(i));
  }
  Check(stack.toasts().size() == nv::ui::OverlayStack::kMaxToasts,
        "toasts are bounded");
  Check(stack.toasts().back().text == L"t4", "the newest toast is last");
  stack.PushToast(L"t4");
  Check(stack.toasts().size() == nv::ui::OverlayStack::kMaxToasts &&
            stack.toasts().back().text == L"t4",
        "a repeated toast is merged instead of duplicated");
  stack.PushToast(L"danger", true);
  Check(stack.toasts().back().danger, "a danger toast keeps its flag");
  stack.PopToast();
  Check(stack.toasts().size() == nv::ui::OverlayStack::kMaxToasts - 1,
        "popping a toast removes one");

  // The error card is not modal but sits above fullscreen in the Escape chain;
  // popups/info close first (規劃/22/11 §4.2).
  Check(stack.Escape() == nv::ui::EscapeAction::kClosedModal,
        "the print modal closes before the error checks");
  stack.SetFullscreen(true);
  stack.SetErrorOpen(true);
  Check(stack.Escape() == nv::ui::EscapeAction::kClosedError,
        "escape closes the error card");
  Check(stack.Escape() == nv::ui::EscapeAction::kExitedFullscreen,
        "the next escape leaves fullscreen");
  stack.SetErrorOpen(false);
  stack.SetErrorOpen(true);
  Check(stack.Escape() == nv::ui::EscapeAction::kClosedError &&
            !stack.error_open(),
        "the error card is still a single Escape owner");
  stack.SetErrorOpen(false);
}

void TestWindowState() {
  nv::ui::WindowStateMachine machine;
  Check(machine.mode() == nv::ui::WindowMode::kNormal, "starts normal");
  Check(machine.ToggleMaximize() == nv::ui::WindowMode::kMaximized, "maximize");
  Check(machine.maximized(), "reports maximized");
  Check(machine.ToggleFullscreen() == nv::ui::WindowMode::kFullscreen,
        "fullscreen from maximized");
  Check(machine.mode_before_fullscreen() == nv::ui::WindowMode::kMaximized,
        "fullscreen remembers maximized");
  Check(machine.ToggleMaximize() == nv::ui::WindowMode::kFullscreen,
        "maximize is ignored while fullscreen");
  Check(machine.ExitFullscreen() == nv::ui::WindowMode::kMaximized,
        "leaving fullscreen restores maximized");
  Check(machine.SetMaximized(false) == nv::ui::WindowMode::kNormal,
        "a snap/restore updates the state");
  Check(std::string(nv::ui::WindowModeName(machine.mode())) == "normal",
        "mode name round-trips");
}

void TestStripModeAndTokens() {
  StripMode mode = StripMode::kAuto;
  Check(nv::ui::ParseStripMode(L"pinned", &mode) && mode == StripMode::kPinned,
        "strip mode pinned parses");
  Check(nv::ui::ParseStripMode(L"auto", &mode) && mode == StripMode::kAuto,
        "strip mode auto parses");
  Check(nv::ui::ParseStripMode(L"off", &mode) && mode == StripMode::kOff,
        "strip mode off parses");
  Check(!nv::ui::ParseStripMode(L"bogus", &mode),
        "unknown strip mode is rejected");
  Check(std::wstring(nv::ui::StripModeName(StripMode::kOff)) == L"off",
        "strip mode name round-trips");

  Check((nv::ui::WithAlpha(0xFF112233u, 0.0f) >> 24) == 0u,
        "WithAlpha clears alpha at 0");
  Check((nv::ui::WithAlpha(0xFF112233u, 1.0f) >> 24) == 0xFFu,
        "WithAlpha sets full alpha");
  Check(nv::ui::ColorRefToArgb(nv::ui::ArgbToColorRef(0xFF123456u)) ==
            0xFF123456u,
        "ARGB <-> COLORREF round-trips");

  const nv::ui::TokenSet dark = nv::ui::TokensFor(true, false);
  const nv::ui::TokenSet light = nv::ui::TokensFor(false, false);
  Check(dark.color.accent != light.color.accent,
        "light and dark use different accents");
  Check((dark.color.on_accent & 0xFFu) == 0xFFu,
        "primary-button text is opaque");
}

// v3.0 (規劃/26 §2.1): the card is its own window now, so what the main layout
// used to reserve is gone and the window size is a pure function instead.
void TestInfoWindowSize() {
  // padding 2x12 + header 30 + the two 8 DIP body gaps + footer 44
  // + 6 rows x 24 = 258 (the gaps make the body fit exactly, no scrollbar).
  nv::ui::InfoWindowSize size = nv::ui::ComputeInfoWindowSize(6, 1.0f, 0.0f, 0.0f);
  CheckNear(size.width, 360.0f, "info window is 360 DIP wide");
  CheckNear(size.height, 258.0f, "info window height follows the row count");

  size = nv::ui::ComputeInfoWindowSize(0, 1.0f, 0.0f, 0.0f);
  CheckNear(size.height, 114.0f,
            "an empty info window is header, footer and padding");

  // 200 rows x 24 + 114 = 4914 > 70% of 800, so the body scrolls instead.
  size = nv::ui::ComputeInfoWindowSize(200, 1.0f, 1280.0f, 800.0f);
  CheckNear(size.height, 800.0f * 0.7f,
            "the window is capped at 70% of the work area");

  size = nv::ui::ComputeInfoWindowSize(6, 1.5f, 0.0f, 0.0f);
  CheckNear(size.width, 540.0f, "the width scales with DPI");
  CheckNear(size.height, 387.0f, "the height scales with DPI");

  // A work area narrower than the card clamps the width instead of overflowing.
  size = nv::ui::ComputeInfoWindowSize(6, 1.0f, 300.0f, 800.0f);
  CheckNear(size.width, 300.0f, "the width never exceeds the work area");
}

// --- EXIF info panel internals (規劃/22/06) ---------------------------------

std::vector<nv::ui::InfoLine> SampleInfoLines(size_t rows) {
  std::vector<nv::ui::InfoLine> lines;
  lines.push_back(nv::ui::InfoLine{L"\u6587\u4ef6", L"", true});
  for (size_t i = 1; i < rows; ++i) {
    lines.push_back(nv::ui::InfoLine{L"Name", L"photo.jpg", false});
  }
  return lines;
}

void TestInfoPanelLayout() {
  const Rect panel = Rect::FromXYWH(900.0f, 300.0f, 360.0f, 400.0f);
  const std::vector<nv::ui::InfoLine> lines = SampleInfoLines(25);
  const nv::ui::InfoPanelLayout layout =
      ComputeInfoPanelLayout(panel, 1.0f, lines, 0.0f, 100.0f);
  Check(layout.visible, "the panel layout is visible");
  CheckNear(layout.header.top, 312.0f, "the header starts at the padding");
  CheckNear(layout.close.width(), 22.0f, "the close button is 22 DIP");
  Check(layout.close.right <= panel.right - 12.0f + 0.02f,
        "the close button is inside the padding");
  CheckNear(layout.footer.bottom, panel.bottom - 12.0f,
            "the footer ends at the bottom padding");
  CheckNear(layout.copy_all.height(), 28.0f, "the copy-all button is 28 DIP");
  Check(layout.copy_all.left >= panel.left + 12.0f,
        "the copy-all button fits the panel width");
  CheckNear(layout.content_height, 25.0f * 24.0f,
            "content height is one row per line");
  // body = 400 - 24 padding - 30 header - 44 footer - 2x8 gaps = 286
  CheckNear(layout.body.height(), 286.0f, "the body is the remaining height");
  Check(layout.scrollable, "25 lines overflow the body");
  CheckNear(layout.max_scroll, 25.0f * 24.0f - 286.0f,
            "max scroll is content minus viewport");

  const nv::ui::InfoPanelLayout scrolled =
      ComputeInfoPanelLayout(panel, 1.0f, lines, 60.0f, 100.0f);
  CheckNear(scrolled.scroll, 60.0f, "the scroll offset is kept");
  CheckNear(scrolled.line[2].top, scrolled.body.top + 2 * 24.0f - 60.0f,
            "lines move with the scroll offset");
  Check(scrolled.first_visible == 2, "first visible line follows the offset");

  const nv::ui::InfoPanelLayout clamped =
      ComputeInfoPanelLayout(panel, 1.0f, lines, 10000.0f, 100.0f);
  CheckNear(clamped.scroll, clamped.max_scroll, "scroll clamps at the end");

  const nv::ui::InfoPanelLayout short_panel =
      ComputeInfoPanelLayout(panel, 1.0f, SampleInfoLines(3), 0.0f, 100.0f);
  Check(!short_panel.scrollable, "a short panel does not scroll");
  CheckNear(short_panel.scroll, 0.0f, "a short panel ignores the offset");

  // Hit-testing: the body consumes clicks, the two buttons win over it.
  const Rect point = layout.copy_all;
  Check(nv::ui::HitTestInfoPanel(layout, point.left + 2.0f, point.top + 2.0f) ==
            HitId::kInfoCopyAll,
        "the copy-all button hits");
  Check(nv::ui::HitTestInfoPanel(layout,
                                 layout.close.left + 2.0f,
                                 layout.close.top + 2.0f) == HitId::kInfoClose,
        "the close button hits");
  Check(nv::ui::HitTestInfoPanel(layout, layout.body.left + 4.0f,
                                 layout.body.top + 4.0f) == HitId::kInfoPanel,
        "the body consumes the click");
  Check(nv::ui::HitTestInfoPanel(layout, panel.left - 4.0f,
                                 panel.top + 4.0f) == HitId::kNone,
        "outside the panel misses");
  Check(nv::ui::InfoLineAt(layout, lines, layout.body.left + 4.0f,
                           layout.line[1].top + 4.0f) == 1,
        "a row is addressable for copy");
  Check(nv::ui::InfoLineAt(layout, lines, layout.body.left + 4.0f,
                           layout.line[0].top + 4.0f) == -1,
        "a group header is not copyable");
}

// --- Startup empty state (planning 22/03) ----------------------------------

nv::ui::EmptyLayout EmptyAt(EmptyKind kind, int lines, bool note,
                            Rect content = Rect::FromLTRB(0, 32, 1280, 680),
                            float scale = 1.0f) {
  nv::ui::EmptyInputs inputs;
  inputs.kind = kind;
  inputs.format_lines = lines;
  inputs.has_note = note;
  return ComputeEmptyLayout(content, scale, inputs);
}

void TestEmptyLayout() {
  const nv::ui::EmptyLayout hidden = EmptyAt(EmptyKind::kNone, 1, false);
  Check(!hidden.visible && hidden.card.empty(), "kNone hides the empty card");

  const nv::ui::EmptyLayout one = EmptyAt(EmptyKind::kEmpty, 1, false);
  Check(one.visible, "the empty card is visible when requested");
  CheckNear(one.card.width(), 520.0f, "the card is at most 520 DIP wide");
  // height = 28*2 padding + 64 icon + 20 gap + 28 title + 12 gap + 18 line +
  //          4 gap + 16 count + 22 section + 36 button + 14 gap + 24 footer
  CheckNear(one.card.height(), 314.0f, "one-line card height");
  CheckNear(one.card.left, (1280.0f - 520.0f) * 0.5f,
            "the card is centred horizontally");
  CheckNear(one.card.top, 32.0f + (648.0f - 314.0f) * 0.5f,
            "the card is centred vertically");
  CheckNear(one.icon.height(), 64.0f, "the icon is 64 DIP");
  CheckNear(one.icon.width(), 64.0f, "the icon is square");
  CheckNear(one.primary.height(), 36.0f, "the primary button is 36 DIP tall");
  CheckNear(one.primary.width(), 132.0f, "the primary button is 132 DIP wide");
  Check(one.primary.left > one.card.left && one.primary.right < one.card.right,
        "the primary button is inset inside the card");
  Check(one.title.bottom <= one.formats.top + 0.02f,
        "the title sits above the format line");
  Check(one.formats.bottom <= one.formats_more.top + 0.02f,
        "the format line sits above the count");
  Check(one.formats_more.bottom <= one.primary.top + 0.02f,
        "the count sits above the primary button");
  Check(one.open_folder.bottom <= one.card.bottom + 0.02f &&
            one.all_formats.bottom <= one.card.bottom + 0.02f,
        "the footer buttons stay inside the card");
  Check(one.open_folder.right <= one.all_formats.left,
        "the two footer text buttons do not overlap");
  Check(one.note.empty(), "a card without a note has no note rectangle");

  // Two format lines make the card 18 DIP taller and move the buttons down.
  const nv::ui::EmptyLayout two = EmptyAt(EmptyKind::kEmpty, 2, false);
  CheckNear(two.card.height(), one.card.height() + 18.0f,
            "a second format line grows the card by one line height");
  Check(two.primary.top > one.primary.top,
        "the primary button moves down for the second line");

  // The unsupported note adds one more line; the folder state uses the same
  // geometry with a different title provided by the app.
  const nv::ui::EmptyLayout note = EmptyAt(EmptyKind::kUnsupported, 1, true);
  Check(!note.note.empty(), "an unsupported note has a rectangle");
  CheckNear(note.card.height(), one.card.height() + 24.0f,
            "the note grows the card by its gap and line");
  const nv::ui::EmptyLayout folder = EmptyAt(EmptyKind::kFolder, 1, true);
  Check(nv::ui::NearEqual(folder.card, note.card),
        "folder and unsupported states share the layout");

  // 125%: every DIP value scales, the card stays centred in the content.
  const Rect scaled_content = Rect::FromLTRB(0, 40, 1600, 850);
  const nv::ui::EmptyLayout scaled =
      EmptyAt(EmptyKind::kEmpty, 1, false, scaled_content, 1.25f);
  CheckNear(scaled.card.width(), 650.0f, "card width scales with DPI");
  CheckNear(scaled.primary.height(), 45.0f, "button height scales with DPI");
  CheckNear(scaled.card.left + scaled.card.width() * 0.5f,
            scaled_content.left + scaled_content.width() * 0.5f,
            "the scaled card stays horizontally centred");

  // A very short content rectangle drops the icon instead of clipping the
  // primary button (e.g. a forced-DPI small window).
  const nv::ui::EmptyLayout compact =
      EmptyAt(EmptyKind::kEmpty, 1, false, Rect::FromLTRB(0, 32, 640, 340));
  Check(compact.visible && compact.compact, "a short card drops the icon");
  Check(compact.icon.empty(), "the compact card has no icon");
  Check(compact.primary.bottom <= compact.card.bottom + 0.02f,
        "the compact card keeps the primary button inside");
}

void TestEmptyHit() {
  const nv::ui::EmptyLayout layout = EmptyAt(EmptyKind::kEmpty, 1, false);
  const auto center = [](const Rect& rect) {
    return std::pair<float, float>((rect.left + rect.right) * 0.5f,
                                   (rect.top + rect.bottom) * 0.5f);
  };
  auto [px, py] = center(layout.primary);
  Check(HitTestEmpty(layout, px, py) == HitId::kEmptyPrimary,
        "the primary button hits kEmptyPrimary");
  auto [ox, oy] = center(layout.open_folder);
  Check(HitTestEmpty(layout, ox, oy) == HitId::kEmptyOpenFolder,
        "open folder hits kEmptyOpenFolder");
  auto [ax, ay] = center(layout.all_formats);
  Check(HitTestEmpty(layout, ax, ay) == HitId::kEmptyAllFormats,
        "all formats hits kEmptyAllFormats");
  Check(HitTestEmpty(layout, layout.card.left + 2, layout.card.top + 2) ==
            HitId::kNone,
        "the card surface itself is not a button");
  const nv::ui::EmptyLayout hidden = EmptyAt(EmptyKind::kNone, 1, false);
  Check(HitTestEmpty(hidden, 100, 100) == HitId::kNone,
        "a hidden card has no hit targets");

  // The buttons win over the image area (planning 22/03 pitfall 11).
  const nv::ui::LayoutFrame frame =
      ComputeLayout(LayoutInputs{}, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
  const std::array<float, nv::ui::kSegCount> widths = {100, 0, 0};
  const nv::ui::TitleLayout title =
      ComputeTitleLayout(frame.title, 1.0f, true, widths);
  const nv::ui::BottomLayout bottom =
      ComputeBottomLayout(frame.bottom_bar, 1.0f, nv::ui::BottomInputs{});
  const auto [bx, by] = center(layout.primary);
  Check(HitTestFrame(frame, title, bottom, layout, bx, by) ==
            HitId::kEmptyPrimary,
        "the empty primary button is hit before the image area");
}

// --- Format summary (planning 22/03 3.3) -----------------------------------

nv::FormatCapabilities AllCapabilities() {
  nv::FormatCapabilities caps;
  caps.wic = true;
  caps.psd = true;
  caps.freeimage = true;
  caps.libraw = true;
  caps.heif = true;
  caps.avif = true;
  return caps;
}

bool Contains(const std::vector<std::wstring>& values,
              const std::wstring& value) {
  for (const std::wstring& item : values) {
    if (item == value) return true;
  }
  return false;
}

void TestFormatSummary() {
  const nv::FormatSummary full = nv::BuildFormatSummary(AllCapabilities());
  Check(full.extension_count > 30, "a full build advertises many extensions");
  Check(full.extension_count == nv::SupportedExtensions().size(),
        "every routed extension is advertised in a full build");
  Check(full.highlights.size() >= 8,
        "the format line has the expected highlight families");
  Check(Contains(full.highlights, L"JPG") && Contains(full.highlights, L"PNG") &&
            Contains(full.highlights, L"PSD") &&
            Contains(full.highlights, L"HEIC/HEIF") &&
            Contains(full.highlights, L"RAW") &&
            Contains(full.highlights, L"HDR/EXR"),
        "the highlights include the headline families");
  Check(!Contains(full.highlights, L"AVIF"),
        "AVIF is listed in all formats but not in the short line");

  // Every extension appears in exactly one family, and every family has a name.
  size_t counted = 0;
  std::vector<std::wstring> seen;
  for (const nv::FormatFamilyInfo& family : full.families) {
    Check(!family.name.empty(), "every family has a display name");
    counted += family.extensions.size();
    for (const std::wstring& extension : family.extensions) {
      Check(extension.size() >= 2 && extension[0] == L'.',
            "extensions keep their leading dot");
      Check(!Contains(seen, extension), "an extension belongs to one family");
      seen.push_back(extension);
    }
  }
  Check(counted == full.extension_count,
        "the family extension counts add up");
  Check(seen.size() == nv::SupportedExtensions().size(),
        "the summary covers the supported extension table exactly once");

  // Debug/no-codec build: FreeImage and LibRaw are absent, so TGA/PCX/PNM,
  // RAW and HDR disappear from the line; WIC and PSD stay.
  nv::FormatCapabilities minimal;
  minimal.wic = true;
  minimal.psd = true;
  const nv::FormatSummary lean = nv::BuildFormatSummary(minimal);
  Check(lean.extension_count < full.extension_count,
        "a build without FreeImage/LibRaw advertises fewer extensions");
  Check(!Contains(lean.highlights, L"RAW"),
        "RAW is hidden when LibRaw/FreeImage are absent");
  Check(!Contains(lean.highlights, L"HEIC/HEIF"),
        "HEIF is hidden when the system codec is missing");
  Check(Contains(lean.highlights, L"JPG") && Contains(lean.highlights, L"PSD"),
        "WIC and PSD families remain available");
  Check(lean.extension_count == 17,
        "the minimal build counts only its decodable extensions");

  // A family that is routed but unavailable still appears in the full list,
  // carrying the reason instead of a false "supported".
  bool saw_raw_unavailable = false;
  for (const nv::FormatFamilyInfo& family : lean.families) {
    if (family.name == L"RAW") {
      saw_raw_unavailable = !family.available && !family.extensions.empty();
    }
  }
  Check(saw_raw_unavailable,
        "the all-formats list keeps an unavailable family with its reason");
}

void TestFormatTableDerivation() {
  nv::FormatCapabilities caps;
  caps.wic = true;
  caps.psd = true;

  // The summary follows the injected extension table, not a hard-coded line.
  const nv::FormatSummary jpg =
      nv::BuildFormatSummary(caps, {L".jpg"});
  Check(jpg.families.size() == 1 && jpg.families[0].name == L"JPG",
        "one routed extension yields one family");
  Check(jpg.extension_count == 1, "one decodable extension is counted");
  Check(jpg.highlights.size() == 1 && jpg.highlights[0] == L"JPG",
        "the highlight line follows the extension table");

  // A second extension adds its family and the count.
  const nv::FormatSummary two =
      nv::BuildFormatSummary(caps, {L".jpg", L".png"});
  Check(two.families.size() == 2 && two.extension_count == 2,
        "adding an extension adds its family and the count");
  Check(Contains(two.highlights, L"PNG"), "the added family joins the line");

  // TGA is routed but unavailable without FreeImage: it appears in the full
  // list with its reason, but not in the short line and not in the count.
  const nv::FormatSummary tga =
      nv::BuildFormatSummary(caps, {L".jpg", L".tga"});
  Check(tga.families.size() == 2, "an unavailable family stays in the list");
  Check(tga.extension_count == 1, "an unavailable extension is not counted");
  Check(tga.routed_extension_count == 2, "the routed count keeps it");
  Check(!Contains(tga.highlights, L"TGA"), "TGA is not advertised in the line");
  bool tga_unavailable = false;
  for (const nv::FormatFamilyInfo& family : tga.families) {
    if (family.name == L"TGA") tga_unavailable = !family.available;
  }
  Check(tga_unavailable, "the TGA family reports itself unavailable");

  nv::FormatCapabilities free_caps = caps;
  free_caps.freeimage = true;
  const nv::FormatSummary tga_ready =
      nv::BuildFormatSummary(free_caps, {L".jpg", L".tga"});
  Check(tga_ready.extension_count == 2,
        "linking FreeImage makes TGA decodable and counted");

  // A table with only an unavailable family has an empty highlight line.
  const nv::FormatSummary only_tga =
      nv::BuildFormatSummary(caps, {L".tga"});
  Check(only_tga.families.size() == 1 && only_tga.highlights.empty(),
        "an all-unavailable table has no highlight line");

  // HDR/EXR/PFM are routed now, so the family appears and its extensions count
  // in a FreeImage build (規劃/22/16).
  const nv::FormatSummary hdr =
      nv::BuildFormatSummary(AllCapabilities(), {L".jpg", L".hdr"});
  Check(hdr.families.size() == 2, "a routed .hdr adds its family");
  Check(hdr.extension_count == 2, "a routed .hdr is counted");
  Check(Contains(hdr.highlights, L"HDR/EXR"), "the HDR family joins the line");
}

void TestFormatPanelLayout() {
  const Rect client = Rect::FromXYWH(0, 0, 1280, 800);
  const nv::ui::FormatPanelLayout layout =
      nv::ui::ComputeFormatPanelLayout(client, 1.0f, 17);
  Check(layout.visible, "the all-formats card is visible");
  CheckNear(layout.panel.width(), 420.0f, "the card is 420 DIP wide");
  CheckNear(layout.panel.left + layout.panel.width() * 0.5f, 640.0f,
            "the card is horizontally centred");
  CheckNear(layout.panel.top + layout.panel.height() * 0.5f, 400.0f,
            "the card is vertically centred");
  Check(layout.panel.bottom <= client.bottom, "the card stays inside the client");
  Check(layout.rows.size() == 17, "one row rect per family");
  CheckNear(layout.close.width(), 72.0f, "the close button has its width");
  CheckNear(layout.close.height(), 26.0f, "the close button has its height");
  Check(layout.close.right <= layout.panel.right + 0.02f &&
            layout.close.bottom <= layout.panel.bottom + 0.02f,
        "the close button stays inside the card");
  for (size_t i = 0; i < layout.rows.size(); ++i) {
    Check(layout.rows[i].bottom <= layout.footer.top + 0.02f,
          "rows stay above the footer");
    if (i > 0) {
      Check(layout.rows[i].top >= layout.rows[i - 1].bottom - 0.02f,
            "rows stack downwards without overlap");
    }
  }
  Check(nv::ui::HitTestFormatPanel(layout, layout.close.left + 4,
                                   layout.close.top + 4) ==
            HitId::kFormatsClose,
        "the close button hits kFormatsClose");
  Check(nv::ui::HitTestFormatPanel(layout, layout.panel.left + 8,
                                   layout.title.top + 4) ==
            HitId::kFormatsPanel,
        "the card surface hits kFormatsPanel");
  Check(nv::ui::HitTestFormatPanel(layout, 4, 4) == HitId::kNone,
        "outside the card hits nothing");

  // 22 families (52 extensions) no longer fit the card: it shows whole rows and
  // exposes a scroll range instead of silently dropping the tail (22/16).
  const nv::ui::FormatPanelLayout tall =
      nv::ui::ComputeFormatPanelLayout(client, 1.0f, 22);
  Check(tall.visible && tall.rows.size() < 22,
        "a tall family list is clipped to the visible rows");
  Check(tall.max_first_row == 22 - tall.rows.size(),
        "the scroll range covers the hidden rows");
  const nv::ui::FormatPanelLayout scrolled =
      nv::ui::ComputeFormatPanelLayout(client, 1.0f, 22, tall.max_first_row);
  Check(scrolled.first_row == tall.max_first_row &&
            scrolled.rows.size() == tall.rows.size() &&
            scrolled.rows.back().bottom <= scrolled.footer.top + 0.02f,
        "scrolling reaches the last family without leaving the body");

  // Small window: the card is clamped inside the client and rows that do not
  // fit are not laid out (drawing and hit-testing read the same vector).
  const nv::ui::FormatPanelLayout tiny = nv::ui::ComputeFormatPanelLayout(
      Rect::FromXYWH(0, 0, 500, 300), 1.0f, 17);
  Check(tiny.visible && tiny.panel.width() <= 468.0f,
        "a small window clamps the card width");
  Check(tiny.panel.height() <= 268.0f, "a small window clamps the height");
  Check(tiny.rows.size() < 17, "rows that do not fit are not laid out");
  for (const Rect& row : tiny.rows) {
    Check(row.bottom <= tiny.panel.bottom + 0.02f,
          "every laid-out row stays inside the clamped card");
  }

  // DPI: the card scales and stays centred.
  const nv::ui::FormatPanelLayout scaled = nv::ui::ComputeFormatPanelLayout(
      Rect::FromXYWH(0, 0, 2560, 1600), 2.0f, 17);
  CheckNear(scaled.panel.width(), 840.0f, "the card width scales with DPI");
  CheckNear(scaled.rows.front().height(), 36.0f, "rows scale with DPI");
}

// --- Navigation and zoom (planning 22/09) ----------------------------------

void TestImageArrows() {
  LayoutInputs inputs;
  inputs.has_image = true;
  inputs.strip_visible = true;
  inputs.strip_mode = StripMode::kPinned;
  inputs.arrows_visible = true;
  const nv::ui::LayoutFrame frame =
      ComputeLayout(inputs, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
  Check(frame.arrows_visible, "arrows are visible with more than one file");
  CheckNear(frame.arrows_left.width(), 56.0f, "arrow circle is 56 DIP wide");
  CheckNear(frame.arrows_left.height(), 56.0f, "arrow circle is 56 DIP tall");
  CheckNear(frame.arrows_left.left, frame.content.left + 16.0f,
            "left arrow is 16 DIP from the content edge");
  CheckNear(frame.arrows_right.right, frame.content.right - 16.0f,
            "right arrow is 16 DIP from the content edge");
  const float content_center_y = (frame.content.top + frame.content.bottom) * 0.5f;
  CheckNear(frame.arrows_left.top + frame.arrows_left.height() * 0.5f,
            content_center_y, "arrows are vertically centred in the content");
  Check(frame.content.Contains(frame.arrows_left.left + 1,
                               frame.arrows_left.top + 1) &&
            frame.content.Contains(frame.arrows_right.right - 1,
                                   frame.arrows_right.bottom - 1),
        "both arrows stay inside the content rectangle");

  const std::array<float, nv::ui::kSegCount> widths = {200, 100, 80};
  const nv::ui::TitleLayout title =
      ComputeTitleLayout(frame.title, 1.0f, true, widths);
  nv::ui::BottomInputs bottom_inputs;
  bottom_inputs.has_image = true;
  const nv::ui::BottomLayout bottom =
      ComputeBottomLayout(frame.bottom_bar, 1.0f, bottom_inputs);
  const nv::ui::EmptyLayout empty{};
  Check(HitTestFrame(frame, title, bottom, empty,
                     frame.arrows_left.left + 4, frame.arrows_left.top + 4) ==
            HitId::kArrowLeft,
        "left circle hits kArrowLeft");
  Check(HitTestFrame(frame, title, bottom, empty,
                     frame.arrows_right.right - 4,
                     frame.arrows_right.top + 4) == HitId::kArrowRight,
        "right circle hits kArrowRight");

  // Hidden: a single file has no rectangles and no hit target, the point falls
  // through to the image (so a pan still works there).
  inputs.arrows_visible = false;
  const nv::ui::LayoutFrame hidden =
      ComputeLayout(inputs, 1.0f, Rect::FromXYWH(0, 0, 1280, 800));
  Check(!hidden.arrows_visible, "arrows are hidden for a single file");
  Check(hidden.arrows_left.empty() && hidden.arrows_right.empty(),
        "hidden arrows have no rectangles");
  Check(HitTestFrame(hidden, title, bottom, empty,
                     hidden.arrows_left.left + 4,
                     hidden.arrows_left.top + 4) != HitId::kArrowLeft,
        "hidden arrows are not hit-testable");

  // DPI: the circles scale with the window.
  inputs.arrows_visible = true;
  const nv::ui::LayoutFrame scaled =
      ComputeLayout(inputs, 1.25f, Rect::FromXYWH(0, 0, 1600, 1000));
  CheckNear(scaled.arrows_left.width(), 70.0f, "arrow size scales with DPI");
  CheckNear(scaled.arrows_left.left, scaled.content.left + 20.0f,
            "arrow margin scales with DPI");
}

void TestCommandQuery() {
  using nv::ui::CommandContext;
  using nv::ui::CommandId;
  using nv::ui::QueryCommand;

  CommandContext none;
  for (const CommandId id :
       {CommandId::kPrev, CommandId::kNext, CommandId::kFirst, CommandId::kLast,
        CommandId::kZoomIn, CommandId::kZoomOut, CommandId::kFit,
        CommandId::kActual, CommandId::kToggleInfo}) {
    Check(!QueryCommand(id, none).enabled,
          "image commands are disabled without a picture");
  }
  Check(QueryCommand(CommandId::kToggleFullscreen, none).enabled,
        "fullscreen is available without a picture");
  Check(QueryCommand(CommandId::kToggleStrip, none).enabled,
        "the thumbnail strip toggle works without a picture");
  Check(!QueryCommand(CommandId::kToggleAnimation, none).enabled,
        "animation needs an animated image");

  CommandContext single;
  single.has_image = true;
  single.file_count = 1;
  Check(!QueryCommand(CommandId::kNext, single).enabled,
        "a single image does not navigate");
  Check(!QueryCommand(CommandId::kFirst, single).enabled,
        "Home is a no-op for a single image");
  Check(QueryCommand(CommandId::kZoomIn, single).enabled,
        "zoom works with a single image");
  Check(QueryCommand(CommandId::kToggleInfo, single).enabled,
        "info works with a single image");

  CommandContext folder = single;
  folder.file_count = 3;
  Check(QueryCommand(CommandId::kPrev, folder).enabled &&
            QueryCommand(CommandId::kLast, folder).enabled,
        "navigation is enabled with more than one file");

  CommandContext animation = folder;
  animation.has_multiple_frames = true;
  Check(QueryCommand(CommandId::kToggleAnimation, animation).enabled,
        "animation is enabled for a multi-frame image");
  Check(!QueryCommand(CommandId::kToggleAnimation, animation).checked,
        "animation starts unchecked");
  animation.animation_playing = true;
  Check(QueryCommand(CommandId::kToggleAnimation, animation).checked,
        "the animation toggle reports the playing state");
  Check(QueryCommand(CommandId::kFrameNext, animation).enabled,
        "frame stepping needs multiple frames");

  CommandContext fit = folder;
  fit.fit = true;
  Check(QueryCommand(CommandId::kFit, fit).checked,
        "the fit command is checked in fit mode");
  CommandContext full = folder;
  full.fullscreen = true;
  full.info_open = true;
  full.strip_visible = false;
  Check(QueryCommand(CommandId::kToggleFullscreen, full).checked &&
            QueryCommand(CommandId::kToggleInfo, full).checked &&
            !QueryCommand(CommandId::kToggleStrip, full).checked,
        "view toggles report checked state");

  Check(std::string(nv::ui::CommandName(CommandId::kNext)) == "next" &&
            std::string(nv::ui::CommandName(CommandId::kZoomIn)) == "zoom.in",
        "command names are stable for logs and tests");
}

void TestCommandKeys() {
  const auto plain = [](unsigned key) {
    return nv::ui::CommandForKey(key, false, false, false);
  };
  Check(plain(VK_LEFT) == nv::ui::CommandId::kPrev, "Left is prev");
  Check(plain(VK_RIGHT) == nv::ui::CommandId::kNext, "Right is next");
  Check(plain(VK_PRIOR) == nv::ui::CommandId::kPrev, "PgUp is prev");
  Check(plain(VK_NEXT) == nv::ui::CommandId::kNext, "PgDn is next");
  Check(plain(VK_HOME) == nv::ui::CommandId::kFirst, "Home is first");
  Check(plain(VK_END) == nv::ui::CommandId::kLast, "End is last");
  Check(plain(VK_OEM_PLUS) == nv::ui::CommandId::kZoomIn,
        "the '=' / '+' key zooms in");
  Check(nv::ui::CommandForKey(VK_OEM_PLUS, false, true, false) ==
            nv::ui::CommandId::kZoomIn,
        "Shift+'=' still zooms in");
  Check(plain(VK_ADD) == nv::ui::CommandId::kZoomIn,
        "numpad plus zooms in");
  Check(plain(VK_SUBTRACT) == nv::ui::CommandId::kZoomOut,
        "numpad minus zooms out");
  Check(plain(VK_OEM_MINUS) == nv::ui::CommandId::kZoomOut,
        "the '-' key zooms out");
  Check(plain('0') == nv::ui::CommandId::kFit, "0 fits the window");
  Check(plain('1') == nv::ui::CommandId::kActual, "1 is actual size");
  Check(nv::ui::CommandForKey('0', true, false, false) ==
            nv::ui::CommandId::kFit,
        "Ctrl+0 fits the window");
  Check(nv::ui::CommandForKey('1', true, false, false) ==
            nv::ui::CommandId::kActual,
        "Ctrl+1 is actual size");
  Check(nv::ui::CommandForKey('T', true, false, false) ==
            nv::ui::CommandId::kToggleStrip,
        "Ctrl+T toggles the strip");
  Check(nv::ui::CommandForKey(VK_RETURN, false, false, true) ==
            nv::ui::CommandId::kToggleInfo,
        "Alt+Enter toggles the info panel");
  Check(plain(VK_RETURN) == nv::ui::CommandId::kToggleFullscreen,
        "Enter toggles fullscreen");
  Check(plain(VK_F11) == nv::ui::CommandId::kToggleFullscreen, "F11 fullscreen");
  Check(plain('I') == nv::ui::CommandId::kToggleInfo, "I toggles the info panel");
  Check(plain(VK_SPACE) == nv::ui::CommandId::kToggleAnimation,
        "Space maps to the animation toggle (falls back to next)");
  Check(plain(VK_OEM_COMMA) == nv::ui::CommandId::kFramePrev,
        "comma steps to the previous frame");
  Check(plain(VK_OEM_PERIOD) == nv::ui::CommandId::kFrameNext,
        "period steps to the next frame");

  // Chords that belong to other handlers must not be stolen.
  Check(nv::ui::CommandForKey('O', true, false, false) ==
            nv::ui::CommandId::kNone,
        "Ctrl+O stays with the open dialog");
  Check(nv::ui::CommandForKey('C', true, false, false) ==
            nv::ui::CommandId::kNone,
        "Ctrl+C stays with copy image");
  Check(nv::ui::CommandForKey('C', false, false, false) ==
            nv::ui::CommandId::kNone,
        "plain C stays with the checkerboard toggle");
  Check(nv::ui::CommandForKey('T', false, false, false) ==
            nv::ui::CommandId::kNone,
        "plain T stays with the theme toggle");
  Check(nv::ui::CommandForKey('I', true, false, false) ==
            nv::ui::CommandId::kNone,
        "Ctrl+I is not in the key table");
  Check(nv::ui::CommandForKey(VK_OEM_PLUS, true, false, false) ==
            nv::ui::CommandId::kNone,
        "Ctrl+plus is not in the key table");
  Check(nv::ui::CommandForKey('L', true, false, false) ==
            nv::ui::CommandId::kRotateLeft,
        "Ctrl+L rotates the view left");
  Check(nv::ui::CommandForKey('R', true, false, false) ==
            nv::ui::CommandId::kRotateRight,
        "Ctrl+R rotates the view right");
  Check(nv::ui::CommandForKey('R', true, true, false) ==
            nv::ui::CommandId::kNone,
        "Ctrl+Shift+R stays unbound");
  Check(plain('R') == nv::ui::CommandId::kNone &&
            plain('L') == nv::ui::CommandId::kNone,
        "plain L/R are not view commands");
}

// 規劃/22/05: quarter-turn geometry is exact (0/+-1 matrix entries) and the
// export rotation uses the same clockwise convention as the renderer.
void TestViewRotation() {
  using nv::render::NormalizeRotation;
  using nv::render::RotateSteps;
  using nv::render::RotationAbout;
  using nv::render::RotationDegrees;
  using nv::render::RotationSwapsAxes;
  using nv::render::UnrotatePoint;
  using nv::render::ViewMatrix;

  Check(NormalizeRotation(-1) == 3, "negative quarter turns wrap to 3");
  Check(NormalizeRotation(4) == 0, "four quarter turns are upright");
  Check(RotateSteps(0, 1) == 1 && RotateSteps(0, -1) == 3,
        "left/right rotate in opposite directions");
  Check(RotationSwapsAxes(1) && RotationSwapsAxes(3) &&
            !RotationSwapsAxes(0) && !RotationSwapsAxes(2),
        "only odd quarter turns swap the effective axes");
  Check(RotationDegrees(3) == 270, "the angle reported to the UI is in degrees");

  const float cx = 400.0f;
  const float cy = 300.0f;
  const auto apply = [](const ViewMatrix& m, float x, float y, float* ox,
                        float* oy) {
    *ox = x * m.m11 + y * m.m21 + m.dx;
    *oy = x * m.m12 + y * m.m22 + m.dy;
  };
  for (int steps = 0; steps < 4; ++steps) {
    const ViewMatrix m = RotationAbout(cx, cy, steps);
    Check(m.m11 == (steps == 0 ? 1.0f : (steps == 2 ? -1.0f : 0.0f)) &&
              m.m22 == (steps == 0 ? 1.0f : (steps == 2 ? -1.0f : 0.0f)),
          "quarter-turn matrices use exact 0/+-1 coefficients");
    float x = 0.0f;
    float y = 0.0f;
    apply(m, cx, cy, &x, &y);
    CheckNear(x, cx, "the rotation centre stays fixed (x)");
    CheckNear(y, cy, "the rotation centre stays fixed (y)");

    // Forward -> inverse must return to the same client point.
    const float px = 123.0f;
    const float py = 456.0f;
    float fx = 0.0f;
    float fy = 0.0f;
    apply(m, px, py, &fx, &fy);
    float ux = fx;
    float uy = fy;
    UnrotatePoint(cx, cy, steps, &ux, &uy);
    CheckNear(ux, px, "unrotate inverts the view rotation (x)");
    CheckNear(uy, py, "unrotate inverts the view rotation (y)");
  }

  // 2x2 marker: TL=1, TR=2, BL=3, BR=4 (stored in the red byte of BGRA).
  const uint8_t marker[16] = {1, 0, 0, 255, 2, 0, 0, 255,
                              3, 0, 0, 255, 4, 0, 0, 255};
  struct Expected {
    int steps;
    uint8_t tl, tr, bl, br;
  };
  const Expected kExpected[] = {
      {0, 1, 2, 3, 4}, {1, 3, 1, 4, 2}, {2, 4, 3, 2, 1}, {3, 2, 4, 1, 3}};
  for (const Expected& expected : kExpected) {
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> rotated =
        nv::RotateBgra90(marker, 2, 2, 8, expected.steps, &width, &height);
    Check(rotated.size() == 16, "the rotated buffer keeps 4 bytes per pixel");
    const auto at = [&rotated, width](uint32_t x, uint32_t y) {
      return rotated[(static_cast<size_t>(y) * width + x) * 4u];
    };
    Check(at(0, 0) == expected.tl && at(1, 0) == expected.tr &&
              at(0, 1) == expected.bl && at(1, 1) == expected.br,
          "quarter turns place the four corners correctly");
  }

  nv::Image image;
  image.info.width = 2;
  image.info.height = 2;
  image.info.stride = 8;
  image.info.format = nv::PixelFormat::kBgra8;
  image.info.full_width = 2;
  image.info.full_height = 2;
  image.pixels.assign(marker, marker + 16);
  nv::RotateImageForView(&image, 1);
  Check(image.info.width == 2 && image.info.height == 2 &&
            image.info.full_width == 2,
        "a square export rotation keeps its dimensions");
  nv::RotateImageForView(&image, 1);
  Check(image.info.width == 2 && image.info.stride == 8,
        "two quarter turns keep the stride valid");
  nv::RotateImageForView(&image, 2);
  Check(std::equal(image.pixels.begin(), image.pixels.end(), marker),
        "four quarter turns reproduce the original pixels exactly");

  // Query: rotation needs a picture but not a folder.
  nv::ui::CommandContext none;
  Check(!nv::ui::QueryCommand(nv::ui::CommandId::kRotateLeft, none).enabled,
        "rotation is disabled without a picture");
  nv::ui::CommandContext single;
  single.has_image = true;
  single.file_count = 1;
  Check(nv::ui::QueryCommand(nv::ui::CommandId::kRotateLeft, single).enabled &&
            nv::ui::QueryCommand(nv::ui::CommandId::kRotateRight, single)
                .enabled,
        "rotation works with a single image");
}

void TestZoomMath() {
  CheckNear(nv::render::MinZoomFor(0.5f), 0.10f,
            "the lower zoom bound is 10% for normal images");
  CheckNear(nv::render::MinZoomFor(0.08f), 0.08f,
            "an 8% fit allows zooming below 10% (8K in a small window)");
  CheckNear(nv::render::ClampZoomFor(0.01f, 0.08f), 0.08f,
            "zoom clamps to the derived lower bound");
  CheckNear(nv::render::ClampZoomFor(40.0f, 0.5f), 32.0f,
            "zoom clamps to 3200%");
  CheckNear(nv::render::ClampZoomFor(1.0f, 0.5f), 1.0f,
            "100% is inside the range");
  Check(nv::render::ZoomPercent(0.999f) == 100,
        "percent rounds instead of truncating 100% to 99%");
  Check(nv::render::ZoomPercent(0.084f) == 8,
        "an 8K fit reports 8%");
}

// --- 22/11 error model, cards, loading and toast layout ---------------------
void TestUiErrorModel() {
  using nv::ErrorAction;
  using nv::ErrorKind;
  using nv::i18n::StringId;

  const nv::UiError missing = nv::MakeUiError(ErrorKind::kFileMissing);
  Check(missing.message_id == StringId::kErrorFileMissing,
        "a missing file maps to its own message");
  Check(nv::HasErrorAction(missing.actions, ErrorAction::kCopyPath) &&
            nv::HasErrorAction(missing.actions, ErrorAction::kReveal) &&
            !nv::HasErrorAction(missing.actions, ErrorAction::kRetry),
        "a missing file offers copy/reveal but not retry");

  Check(nv::HasErrorAction(nv::DefaultErrorActions(ErrorKind::kCorrupt),
                           ErrorAction::kRetry),
        "a corrupt image offers retry");
  Check(nv::HasErrorAction(nv::DefaultErrorActions(ErrorKind::kCodecMissing),
                           ErrorAction::kOpenWith),
        "a missing codec offers Open with");

  Check(nv::ErrorKindForHresult(static_cast<long>(0x88982F50L)) ==
            ErrorKind::kCodecMissing,
        "WINCODEC_ERR_COMPONENTNOTFOUND is a missing codec");
  Check(nv::ErrorKindForHresult(static_cast<long>(E_OUTOFMEMORY)) ==
            ErrorKind::kOutOfMemory,
        "E_OUTOFMEMORY is classified as out of memory");
  Check(nv::ErrorKindForHresult(
            static_cast<long>(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))) ==
            ErrorKind::kFileMissing,
        "ERROR_FILE_NOT_FOUND is classified as missing");

  ErrorKind parsed = ErrorKind::kUnknown;
  Check(nv::ParseErrorKind(L"codec", &parsed) && parsed == ErrorKind::kCodecMissing,
        "--force-error kind names parse");
  Check(!nv::ParseErrorKind(L"nope", &parsed), "unknown error kind is rejected");

  // A file that does not exist at all is "missing", never "corrupt".
  const nv::UiError missing_file = nv::ClassifyDecodeError(
      nv::Error::Make(2000, "WIC: failed", 0x88982F60L),
      L"Z:\\nv-no-such-file.png", nv::ImageFormat::kPng);
  Check(missing_file.kind == ErrorKind::kFileMissing,
        "a vanished file is classified before the decoder error");

  // An existing file with a decoder failure is corrupt.
  wchar_t windows_dir[MAX_PATH]{};
  GetWindowsDirectoryW(windows_dir, MAX_PATH);
  const std::filesystem::path existing =
      std::filesystem::path(windows_dir) / L"win.ini";
  if (std::filesystem::exists(existing)) {
    const nv::UiError corrupt = nv::ClassifyDecodeError(
        nv::Error::Make(2000, "WIC: failed", 0x88982F60L), existing,
        nv::ImageFormat::kPng);
    Check(corrupt.kind == ErrorKind::kCorrupt,
          "an existing unreadable image is corrupt");
    const nv::UiError oom = nv::ClassifyDecodeError(
        nv::Error::Make(2000, "WIC: failed", E_OUTOFMEMORY), existing,
        nv::ImageFormat::kPng);
    Check(oom.kind == ErrorKind::kOutOfMemory,
          "E_OUTOFMEMORY wins over the generic corrupt fallback");
  }
}

void TestErrorCardLayout() {
  const nv::ui::Rect content = nv::ui::Rect::FromXYWH(0, 40, 1200, 800);
  const std::vector<float> widths = {90.0f, 110.0f, 96.0f};
  const nv::ui::ErrorCardLayout card =
      nv::ui::ComputeErrorCardLayout(content, 1.0f, widths, true);
  Check(card.visible, "the error card is laid out");
  Check(card.card.left >= content.left && card.card.right <= content.right,
        "the error card stays inside the content width");
  Check(card.buttons.size() == 3, "three action buttons are laid out");
  Check(card.buttons[0].left < card.buttons[1].left &&
            card.buttons[1].left < card.buttons[2].left,
        "error buttons run left to right");
  Check(card.close.left >= card.buttons.back().right,
        "Close sits to the right of the action row");
  Check(!card.detail.empty(), "the detail row exists when a detail is given");

  Check(nv::ui::HitTestErrorCard(card, card.buttons[1].left + 5,
                                 card.buttons[1].top + 5) ==
            nv::ui::HitId::kErrorAction1,
        "the second error button hits its own action");
  Check(nv::ui::HitTestErrorCard(card, card.close.left + 5,
                                 card.close.top + 5) ==
            nv::ui::HitId::kErrorClose,
        "Close hits the close id");
  Check(nv::ui::HitTestErrorCard(card, card.card.left + 2,
                                 card.card.top + 2) == nv::ui::HitId::kNone,
        "the error card body is not a hit target (clicks pass through)");

  const nv::ui::ErrorCardLayout no_detail =
      nv::ui::ComputeErrorCardLayout(content, 1.0f, {100.0f}, false);
  Check(no_detail.card.height() < card.card.height(),
        "a card without a detail line is shorter");

  // At 200% every rectangle doubles and still fits the content.
  const nv::ui::Rect scaled_content = nv::ui::Rect::FromXYWH(0, 80, 2400, 1600);
  const nv::ui::ErrorCardLayout scaled =
      nv::ui::ComputeErrorCardLayout(scaled_content, 2.0f,
                                     {180.0f, 220.0f, 192.0f}, true);
  CheckNear(scaled.card.height(), card.card.height() * 2.0f,
            "the error card scales with DPI");
}

void TestConfirmCardLayout() {
  const nv::ui::Rect client = nv::ui::Rect::FromXYWH(0, 0, 1280, 800);
  const nv::ui::ConfirmCardLayout card =
      nv::ui::ComputeConfirmCardLayout(client, 1.0f);
  Check(card.visible, "the confirmation card is laid out");
  Check(card.scrim.left == client.left && card.scrim.right == client.right,
        "the confirmation card dims the whole client");
  Check(card.primary.left > card.secondary.right,
        "the destructive button is on the right");
  Check(nv::ui::HitTestConfirmCard(card, card.primary.left + 4,
                                   card.primary.top + 4) ==
            nv::ui::HitId::kConfirmPrimary,
        "the primary confirmation button hits");
  Check(nv::ui::HitTestConfirmCard(card, card.secondary.left + 4,
                                   card.secondary.top + 4) ==
            nv::ui::HitId::kConfirmSecondary,
        "the secondary confirmation button hits");
  Check(nv::ui::HitTestConfirmCard(card, 5.0f, 5.0f) ==
            nv::ui::HitId::kConfirmPanel,
        "the scrim belongs to the modal");

  // Folder-edge prompt (22/10): the same card grows a remember row whose box
  // and label are both clickable.
  const nv::ui::ConfirmCardLayout prompt =
      nv::ui::ComputeConfirmCardLayout(client, 1.0f, true);
  Check(prompt.visible && prompt.has_remember,
        "the folder prompt lays out the remember row");
  Check(prompt.card.height() > card.card.height(),
        "the remember row makes the folder prompt taller");
  Check(prompt.remember_box.width() > 0 &&
            prompt.remember_box.bottom <= prompt.primary.top,
        "the remember box sits above the buttons");
  Check(nv::ui::HitTestConfirmCard(prompt, prompt.remember_box.left + 2,
                                   prompt.remember_box.top + 2) ==
            nv::ui::HitId::kConfirmRemember,
        "the remember checkbox hits");
  Check(nv::ui::HitTestConfirmCard(prompt, prompt.remember.left + 2,
                                   prompt.remember.top + 2) ==
            nv::ui::HitId::kConfirmRemember,
        "the remember label hits");
  Check(nv::ui::HitTestConfirmCard(card, card.secondary.left + 4,
                                   card.secondary.top + 4) ==
            nv::ui::HitId::kConfirmSecondary,
        "the danger card has no remember row");
}

void TestLoadingAndToastLayout() {
  const nv::ui::Rect content = nv::ui::Rect::FromXYWH(0, 40, 1000, 700);
  const nv::ui::LoadingLayout loading =
      nv::ui::ComputeLoadingLayout(content, 1.0f);
  Check(loading.visible, "the loading indicator is laid out when visible");
  CheckNear((loading.bar.left + loading.bar.right) * 0.5f,
            content.left + content.width() * 0.5f,
            "the loading bar is horizontally centred");
  Check(loading.label.top >= loading.bar.bottom,
        "the loading label sits under the bar");
  Check(loading.label.bottom <= content.bottom, "loading stays in the content");

  const nv::ui::Rect anchor =
      nv::ui::Rect::FromXYWH(500, 700, 360, 36);
  const nv::ui::Rect short_toast =
      nv::ui::ComputeToastRect(anchor, 60.0f, 0, 1.0f);
  const nv::ui::Rect long_toast =
      nv::ui::ComputeToastRect(anchor, 600.0f, 0, 1.0f);
  Check(long_toast.width() > short_toast.width(),
        "a long toast grows up to the maximum width");
  Check(long_toast.width() <= 480.0f, "a toast never exceeds 480 DIP");
  Check(short_toast.width() >= 180.0f, "a short toast keeps a minimum width");
  CheckNear(short_toast.bottom, anchor.bottom, "toast 0 sits on the stack top");
  const nv::ui::Rect second =
      nv::ui::ComputeToastRect(anchor, 60.0f, 1, 1.0f);
  Check(second.bottom < short_toast.top,
        "a second toast stacks above the first");
}

void TestAnimationController() {
  nv::AnimationController anim;
  Check(anim.stopped(), "animation starts stopped");
  anim.Start(1, 0);
  Check(!anim.playing() && anim.stopped(),
        "a single frame never starts playback (pill has no buttons)");

  anim.Start(4, 2);
  Check(anim.playing() && anim.index() == 0,
        "a multi-frame image starts playing from frame 0");

  nv::AnimStep step = anim.Advance(4, 2);
  Check(step.show && step.index == 1, "advance shows the next frame");
  anim.Advance(4, 2);
  anim.Advance(4, 2);
  Check(anim.index() == 3, "advance reaches the last frame");
  step = anim.Advance(4, 2);
  Check(step.show && step.index == 0 && anim.loops_done() == 1,
        "a finite loop wraps to frame 0 and counts the pass");
  anim.Advance(4, 2);
  anim.Advance(4, 2);
  anim.Advance(4, 2);
  step = anim.Advance(4, 2);
  Check(step.ended && !step.show && anim.index() == 3 && anim.ended(),
        "the second pass ends on the last frame without wrapping");

  Check(anim.Toggle(4) && anim.playing() && anim.index() == 0,
        "Play from Ended replays from frame 0");
  Check(!anim.Toggle(4) && anim.state() == nv::AnimState::kPaused,
        "Play toggles a playing animation to paused");
  Check(anim.Resume(4) && anim.playing(), "Resume returns to playing");

  step = anim.Step(4, -1);
  Check(anim.state() == nv::AnimState::kPaused && step.index == 3,
        "stepping pauses first and wraps backwards from frame 0");
  Check(anim.Step(4, +1).index == 0,
        "stepping forward from the last frame wraps to the first");

  anim.Resume(4);
  Check(anim.OnMinimized() && anim.state() == nv::AnimState::kPaused &&
            anim.paused_for_minimize(),
        "minimising an active animation pauses and remembers it");
  Check(anim.OnRestored(4) && anim.playing(),
        "restoring resumes the remembered playback");
  Check(!anim.OnRestored(4), "restoring without a remembered pause is a no-op");

  nv::AnimationController infinite;
  infinite.Start(3, 0);
  bool ended = false;
  for (int i = 0; i < 10; ++i) ended = infinite.Advance(3, 0).ended;
  Check(!ended && infinite.playing(),
        "loop_count 0 (infinite) never ends playback");

  infinite.Reset();
  Check(infinite.stopped() && infinite.index() == 0 && !infinite.playing(),
        "Reset returns the controller to stopped at frame 0");

  nv::AnimationController paused_min;
  paused_min.Start(2, 0);
  paused_min.Pause();
  Check(!paused_min.OnMinimized() && !paused_min.paused_for_minimize(),
        "minimising a paused animation does not arm the resume");
}

void TestAnimPillLayout() {
  const Rect pill = Rect::FromXYWH(400, 600, 248, 36);
  const nv::ui::AnimPillLayout layout =
      nv::ui::ComputeAnimPillLayout(pill, 1.0f, true);
  Check(layout.visible && layout.interactive, "the pill is laid out with buttons");
  Check(layout.prev.left >= pill.left && layout.toggle.right <= pill.right,
        "all pill buttons stay inside the pill");
  CheckNear(layout.prev.width(), 28.0f, "a pill button is 28 DIP wide");
  CheckNear(layout.toggle.height(), 28.0f, "a pill button is 28 DIP tall");
  Check(layout.prev.right <= layout.label.left,
        "the label starts after the previous button");
  Check(layout.label.right <= layout.next.left,
        "the label ends before the next button");
  Check(layout.next.right <= layout.toggle.left,
        "next precedes the play/pause button");

  const auto centre = [](const Rect& rect) {
    return std::pair<float, float>((rect.left + rect.right) * 0.5f,
                                   (rect.top + rect.bottom) * 0.5f);
  };
  auto point = centre(layout.prev);
  Check(nv::ui::HitTestAnimPill(layout, point.first, point.second) ==
            HitId::kAnimPrev,
        "the previous button is a hit target");
  point = centre(layout.next);
  Check(nv::ui::HitTestAnimPill(layout, point.first, point.second) ==
            HitId::kAnimNext,
        "the next button is a hit target");
  point = centre(layout.toggle);
  Check(nv::ui::HitTestAnimPill(layout, point.first, point.second) ==
            HitId::kAnimToggle,
        "the play/pause button is a hit target");
  point = centre(layout.label);
  Check(nv::ui::HitTestAnimPill(layout, point.first, point.second) ==
            HitId::kNone,
        "the frame label is not clickable");
  Check(nv::ui::HitTestAnimPill(layout, pill.left + 2.0f, pill.top + 2.0f) ==
            HitId::kNone,
        "the pill padding falls through to the image");

  const nv::ui::AnimPillLayout label_only =
      nv::ui::ComputeAnimPillLayout(pill, 1.0f, false);
  Check(label_only.visible && !label_only.interactive,
        "the first-frame-only state is a label, not a control");
  Check(label_only.prev.empty() && label_only.next.empty() &&
            label_only.toggle.empty() && !label_only.label.empty(),
        "the label-only pill has no buttons");
  point = centre(label_only.label);
  Check(nv::ui::HitTestAnimPill(label_only, point.first, point.second) ==
            HitId::kNone,
        "the label-only pill has no hit target");

  const nv::ui::AnimPillLayout scaled = nv::ui::ComputeAnimPillLayout(
      Rect::FromXYWH(0, 0, 310, 45), 1.25f, true);
  CheckNear(scaled.prev.width(), 35.0f, "pill buttons scale with DPI");
}

void TestPreviewPlan() {
  // Two-stage loading (規劃/22/13 §3.1, V3-03): a pure policy matrix. A small
  // JPEG must never preview; a large one only when its thumbnail is cached.
  nv::PreviewPlan plan =
      nv::PlanPreview(nv::ImageFormat::kJpeg, 2ull << 20, 2'000'000, false);
  Check(!plan.any(), "a small JPEG keeps the single-stage path");
  plan = nv::PlanPreview(nv::ImageFormat::kPng, 8ull << 20, 2'000'000, false);
  Check(!plan.any(), "a small PNG keeps the single-stage path");

  plan = nv::PlanPreview(nv::ImageFormat::kJpeg, nv::kPreviewMinFileBytes,
                         1'000'000, true);
  Check(plan.cache && !plan.embedded,
        "a file at the 16 MB threshold previews from a cached thumbnail");
  plan = nv::PlanPreview(nv::ImageFormat::kJpeg, nv::kPreviewMinFileBytes,
                         1'000'000, false);
  Check(!plan.any(), "the same file does not preview without a cached thumbnail");

  plan = nv::PlanPreview(nv::ImageFormat::kJpeg, 12ull << 20, 24'000'000, true);
  Check(plan.cache && !plan.embedded,
        "a 24 MP JPEG previews from the thumbnail cache");

  plan = nv::PlanPreview(nv::ImageFormat::kTiff, 64ull << 20, 0, true);
  Check(plan.cache && !plan.embedded,
        "a 64 MB TIFF previews from the thumbnail cache");

  plan = nv::PlanPreview(nv::ImageFormat::kPsd, 4096, 0, false);
  Check(plan.cache && !plan.embedded,
        "PSD previews from the cache even when it is small");

  plan = nv::PlanPreview(nv::ImageFormat::kRaw, 24ull << 20, 0, false);
  Check(plan.cache && plan.embedded,
        "RAW tries the cache and falls back to the embedded JPEG");

  plan = nv::PlanPreview(nv::ImageFormat::kGif, 100ull << 20, 0, true);
  const bool gif_quiet = !plan.any();
  plan = nv::PlanPreview(nv::ImageFormat::kApng, 100ull << 20, 0, true);
  const bool apng_quiet = !plan.any();
  plan = nv::PlanPreview(nv::ImageFormat::kWebp, 100ull << 20, 0, true);
  Check(gif_quiet && apng_quiet && !plan.any(),
        "animated GIF/APNG/WebP never preview");
}

void TestPreviewBadgeLayout() {
  const Rect content = Rect::FromXYWH(0, 32, 1000, 700);
  nv::ui::PreviewBadgeLayout layout =
      nv::ui::ComputePreviewBadgeLayout(content, 1.0f, 40.0f);
  Check(layout.visible, "the preview badge is laid out inside the image area");
  CheckNear(layout.badge.height(), 22.0f, "the badge is 22 DIP tall");
  CheckNear(layout.badge.right, content.right - 12.0f,
            "the badge keeps a 12 DIP right margin");
  CheckNear(layout.badge.top, content.top + 12.0f,
            "the badge keeps a 12 DIP top margin");
  // 40 px of text + 2 * 10 DIP padding.
  CheckNear(layout.badge.width(), 60.0f, "the badge grows with its label");
  Check(layout.badge.left >= content.left &&
            layout.badge.bottom <= content.bottom,
        "the badge stays inside the image area");

  layout = nv::ui::ComputePreviewBadgeLayout(content, 1.0f, 4.0f);
  CheckNear(layout.badge.width(), 48.0f, "a short label uses the 48 DIP minimum");
  layout = nv::ui::ComputePreviewBadgeLayout(content, 1.0f, 500.0f);
  CheckNear(layout.badge.width(), 240.0f, "a long label is clamped at 240 DIP");

  // The caller measures the text at the window DPI, so pass 80 px here.
  layout = nv::ui::ComputePreviewBadgeLayout(content, 2.0f, 80.0f);
  CheckNear(layout.badge.height(), 44.0f, "the badge scales with DPI");
  CheckNear(layout.badge.width(), 120.0f, "the badge width scales with DPI");
  CheckNear(layout.badge.right, content.right - 24.0f,
            "the badge margin scales with DPI");

  layout = nv::ui::ComputePreviewBadgeLayout(Rect{}, 1.0f, 40.0f);
  Check(!layout.visible, "an empty content rectangle has no badge");
  layout = nv::ui::ComputePreviewBadgeLayout(
      Rect::FromXYWH(0, 0, 60, 30), 1.0f, 40.0f);
  Check(!layout.visible, "a window too small for the badge hides it");
}


// v2.0 S3 / 22/18: interpolation policy. The renderer only reads these pure
// functions, so the pixel/good/fast decisions are pinned without a device.
void TestDrawQuality() {
  using nv::render::DrawQuality;
  using nv::render::DrawQualityFromValue;
  using nv::render::DrawQualityInputs;
  using nv::render::DrawQualityName;
  using nv::render::DrawQualityValue;
  using nv::render::ProxyEdge;
  using nv::render::ProxyInputs;
  using nv::render::SelectDrawQuality;
  using nv::render::ShouldUseProxy;

  DrawQualityInputs in;
  in.scale = 0.16f;
  Check(SelectDrawQuality(in) == DrawQuality::kGood,
        "a static fit view renders with cubic (good)");
  in.interactive = true;
  Check(SelectDrawQuality(in) == DrawQuality::kFast,
        "during pan/zoom the renderer drops to linear (fast)");
  in.interactive = false;
  in.animating = true;
  Check(SelectDrawQuality(in) == DrawQuality::kFast,
        "animation playback stays linear");
  in.animating = false;

  in.pixel_mode = true;
  in.scale = 1.0f;
  Check(SelectDrawQuality(in) == DrawQuality::kPixel,
        "pixel mode at 100% uses nearest neighbour");
  in.scale = 4.0f;
  Check(SelectDrawQuality(in) == DrawQuality::kPixel,
        "pixel mode above 100% uses nearest neighbour");
  in.interactive = true;
  Check(SelectDrawQuality(in) == DrawQuality::kPixel,
        "pixel mode keeps nearest neighbour while panning");
  in.interactive = false;
  in.scale = 0.999f;
  Check(SelectDrawQuality(in) == DrawQuality::kGood,
        "pixel mode below 100% still uses cubic (no shrink aliasing)");
  in.scale = 0.2f;
  in.interactive = true;
  Check(SelectDrawQuality(in) == DrawQuality::kFast,
        "pixel mode below 100% is still fast while interactive");
  in.pixel_mode = false;
  in.interactive = false;
  in.scale = 1.0f;
  Check(SelectDrawQuality(in) == DrawQuality::kGood,
        "100% without pixel mode is a static good frame");

  Check(nv::render::kQualityUpgradeMs == 150,
        "the interaction window is 150 ms");
  Check(std::string(DrawQualityName(DrawQuality::kFast)) == "fast" &&
            std::string(DrawQualityName(DrawQuality::kGood)) == "good" &&
            std::string(DrawQualityName(DrawQuality::kPixel)) == "pixel",
        "quality names match the --force-quality tokens");
  for (DrawQuality quality :
       {DrawQuality::kFast, DrawQuality::kGood, DrawQuality::kPixel}) {
    if (DrawQualityFromValue(DrawQualityValue(quality)) != quality) {
      Check(false, "quality value round-trips");
      break;
    }
  }
  Check(nv::render::kQualityAuto == -1,
        "the automatic quality override is -1");

  Check(ProxyEdge(7680) == 1920 && ProxyEdge(4320) == 1080,
        "proxy edges are a quarter of the source");
  Check(ProxyEdge(1) == 1 && ProxyEdge(3) == 1 && ProxyEdge(5) == 2,
        "proxy edges round up for tiny sources");
  Check(static_cast<uint64_t>(ProxyEdge(7680)) * ProxyEdge(4320) * 4 <=
            9ull * 1024 * 1024,
        "the 8K proxy stays under 9 MB (1/2 would be 33 MB)");

  ProxyInputs proxy;
  proxy.image_width = 7680;
  proxy.image_height = 4320;
  proxy.content_width = 1280;
  proxy.content_height = 700;
  proxy.scale = 0.16f;
  Check(ShouldUseProxy(proxy), "8K at 16% uses the 1/4 proxy");
  proxy.scale = 0.5f;
  Check(!ShouldUseProxy(proxy), "50% and above never uses the proxy");
  proxy.scale = 0.49f;
  proxy.pixel_mode = true;
  Check(!ShouldUseProxy(proxy), "pixel mode never uses the proxy");
  proxy.pixel_mode = false;
  proxy.enabled = false;
  Check(!ShouldUseProxy(proxy), "--proxy=off disables the proxy");
  proxy.enabled = true;
  proxy.image_width = 640;
  proxy.image_height = 480;
  proxy.content_width = 1280;
  proxy.content_height = 700;
  proxy.scale = 0.4f;
  Check(!ShouldUseProxy(proxy), "small images are never proxied");
  proxy.image_width = 2560;
  proxy.image_height = 1400;
  proxy.content_width = 1280;
  proxy.content_height = 700;
  proxy.scale = 0.4f;
  Check(!ShouldUseProxy(proxy),
        "an image only 4x the content area is not proxied (strict >)");
  proxy.content_width = 1280;
  proxy.content_height = 700;
  proxy.image_width = 3000;
  proxy.image_height = 1400;
  Check(ShouldUseProxy(proxy), "an image just above 4x is proxied");
  proxy.content_width = 0;
  Check(!ShouldUseProxy(proxy), "a zero content area never proxies");
}

// --- Bird's eye view (v3.0, 規劃/26 §2.4) -----------------------------------
// The same pure function feeds painting, hit-testing, dragging and the layout
// dump, so these checks pin down the panel placement and the canvas <-> display
// mapping without a device.
void TestMinimapLayout() {
  using nv::ui::BottomLayout;
  using nv::ui::ComputeMinimapLayout;
  using nv::ui::EmptyLayout;
  using nv::ui::MinimapDragPan;
  using nv::ui::MinimapDisplayPoint;
  using nv::ui::MinimapInputs;
  using nv::ui::MinimapLayout;
  using nv::ui::TitleLayout;

  MinimapInputs in;
  in.client = Rect::FromLTRB(0.0f, 0.0f, 1280.0f, 800.0f);
  in.content = Rect::FromLTRB(0.0f, 40.0f, 1280.0f, 650.0f);
  in.strip = Rect::FromLTRB(0.0f, 650.0f, 1280.0f, 745.0f);
  in.bottom_bar = Rect::FromLTRB(0.0f, 745.0f, 1280.0f, 800.0f);
  in.dpi_scale = 1.0f;

  // No picture: no panel, even with everything else in place.
  MinimapLayout none = ComputeMinimapLayout(in);
  Check(!none.visible, "no display size means no bird's eye panel");

  in.display_width = 800;
  in.display_height = 600;
  in.view_scale = 0.5f;  // content 1280x610 shows the whole 800x600 picture
  in.view_center_x = (in.content.left + in.content.right) * 0.5f;
  in.view_center_y = (in.content.top + in.content.bottom) * 0.5f;
  MinimapLayout full = ComputeMinimapLayout(in);
  Check(full.visible, "a picture places the bird's eye panel");
  CheckNear(full.panel.right, in.content.right - 12.0f,
            "the panel is right-aligned with the content margin");
  CheckNear(full.panel.bottom, in.strip.top - 8.0f,
            "the panel floats above the rail slot");
  CheckNear(full.canvas.width() / full.canvas.height(), 800.0f / 600.0f,
            "the canvas keeps the picture aspect ratio");
  CheckNear(full.image.left, full.canvas.left, "the photo fills the canvas");
  Check(full.viewport.left <= full.image.left + 0.5f &&
            full.viewport.right >= full.image.right - 0.5f &&
            full.viewport.top <= full.image.top + 0.5f &&
            full.viewport.bottom >= full.image.bottom - 0.5f,
        "a fully visible picture outlines the whole photo");

  // A quarter turn swaps the display size: the panel becomes portrait.
  const Rect portrait_panel = full.panel;
  in.display_width = 600;
  in.display_height = 800;
  MinimapLayout rotated = ComputeMinimapLayout(in);
  Check(rotated.visible, "a rotated picture still places the panel");
  Check(rotated.panel.height() > rotated.panel.width(),
        "a 90 degree view makes the bird's eye portrait");
  Check(rotated.panel.height() > portrait_panel.height(),
        "the rotated panel is taller than the landscape one");

  // Zoomed-in view: the frame is a fraction of the photo and maps back to the
  // same display-space rectangle the renderer draws.
  in.display_width = 4000;
  in.display_height = 3000;
  in.view_scale = 1.0f;
  in.content = Rect::FromLTRB(0.0f, 0.0f, 1280.0f, 650.0f);
  in.view_center_x = 640.0f;
  in.view_center_y = 325.0f;
  MinimapLayout zoomed = ComputeMinimapLayout(in);
  Check(zoomed.visible, "a zoomed view keeps the panel");
  CheckNear(zoomed.view_display.left, 2000.0f - 640.0f,
            "the visible display rect starts where the content starts");
  CheckNear(zoomed.view_display.right, 2000.0f + 640.0f,
            "the visible display rect ends where the content ends");
  CheckNear(zoomed.viewport.width(),
            zoomed.image.width() * (1280.0f / 4000.0f),
            "the frame width is the visible fraction of the photo");
  Check(zoomed.viewport.width() < zoomed.image.width() * 0.5f,
        "a 1:1 view is a small frame, not the whole photo");

  // Pan far past the picture: the visible rectangle is clipped to the photo and
  // never escapes the panel.
  in.view_center_x = -6000.0f;
  MinimapLayout off = ComputeMinimapLayout(in);
  Check(off.visible, "panning off the picture keeps the panel");
  Check(off.view_display.empty() ||
            (off.view_display.left >= 0.0f &&
             off.view_display.right <= 4000.0f),
        "the visible display rect never leaves the photo");
  Check(off.viewport.empty() ||
            (off.viewport.left >= off.image.left - 0.01f &&
             off.viewport.right <= off.image.right + 0.01f),
        "the frame never leaves the photo");

  // Drag conversion: canvas pixels -> client pan. Display pixels per canvas
  // pixel times the view scale; the sign is negative.
  in.view_center_x = 640.0f;  MinimapLayout drag = ComputeMinimapLayout(in);
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  MinimapDragPan(drag, 1.0f, 10.0f, -4.0f, &pan_x, &pan_y);
  CheckNear(pan_x, -10.0f * drag.display_per_canvas,
            "dragging the frame right pans the picture left");
  CheckNear(pan_y, 4.0f * drag.display_per_canvas,
            "dragging the frame up pans the picture down");
  Check(drag.display_per_canvas > 1.0f,
        "one canvas pixel covers several display pixels at this size");

  // Canvas point -> display point, the inverse direction used by a blank click.
  float dx = 0.0f;
  float dy = 0.0f;
  const float mid_x = (drag.image.left + drag.image.right) * 0.5f;
  const float mid_y = (drag.image.top + drag.image.bottom) * 0.5f;
  Check(MinimapDisplayPoint(drag, mid_x, mid_y, &dx, &dy),
        "the canvas centre maps into display space");
  CheckNear(dx, 2000.0f, "the canvas centre maps to the picture centre (x)");
  CheckNear(dy, 1500.0f, "the canvas centre maps to the picture centre (y)");
  Check(!MinimapDisplayPoint(drag, drag.image.left - 1.0f, mid_y, &dx, &dy),
        "a point left of the photo does not map");

  // Fullscreen has no bottom bar and no rail slot: the panel floats at the
  // bottom edge with the margin, far from the top-right reveal buttons.
  MinimapInputs fs = in;
  fs.fullscreen = true;
  fs.client = Rect::FromLTRB(0.0f, 0.0f, 1920.0f, 1080.0f);
  fs.content = fs.client;
  fs.strip = Rect{};
  fs.bottom_bar = Rect{};
  fs.view_center_x = 960.0f;
  fs.view_center_y = 540.0f;
  fs.view_scale = 0.4f;
  MinimapLayout fullscreen = ComputeMinimapLayout(fs);
  Check(fullscreen.visible, "fullscreen keeps the bird's eye panel");
  CheckNear(fullscreen.panel.bottom, fs.client.bottom - 12.0f,
            "in fullscreen the panel sits at the bottom edge");
  Check(fullscreen.panel.right <= fs.client.right - 11.0f,
        "in fullscreen the panel stays inside the right margin");
  Check(fullscreen.panel.top > 100.0f,
        "the panel never reaches the fullscreen top-right buttons");

  // Too little room: no panel instead of a dead control.
  MinimapInputs tiny = in;
  tiny.content = Rect::FromLTRB(0.0f, 0.0f, 100.0f, 80.0f);
  tiny.strip = Rect{};
  tiny.bottom_bar = Rect{};
  Check(!ComputeMinimapLayout(tiny).visible,
        "a window too small for the panel hides it");
  // Extreme aspect ratio: a 1:20 panorama would be a 7 px strip.
  MinimapInputs panorama = tiny;
  panorama.content = Rect::FromLTRB(0.0f, 0.0f, 640.0f, 480.0f);
  panorama.display_width = 4000;
  panorama.display_height = 12000;
  panorama.view_scale = 0.04f;
  panorama.view_center_x = 320.0f;
  panorama.view_center_y = 240.0f;
  Check(!ComputeMinimapLayout(panorama).visible,
        "an extreme panorama aspect hides the panel");

  // Hit-stack integration: the panel wins over the image area it covers, and a
  // click outside it still reaches the picture.
  LayoutInputs inputs;
  inputs.has_image = true;
  inputs.strip_visible = true;
  inputs.strip_mode = StripMode::kPinned;
  inputs.strip_shown = true;
  inputs.minimap_visible = true;
  inputs.minimap_display_width = 800;
  inputs.minimap_display_height = 600;
  inputs.minimap_view_scale = 0.5f;
  inputs.minimap_view_center_x = 640.0f;
  inputs.minimap_view_center_y = 345.0f;
  const nv::ui::LayoutFrame frame =
      ComputeLayout(inputs, 1.0f, Rect::FromLTRB(0.0f, 0.0f, 1280.0f, 800.0f));
  Check(frame.minimap.visible, "ComputeLayout carries the minimap rectangle");
  TitleLayout title = ComputeTitleLayout(frame.title, 1.0f, true, {});
  BottomLayout bottom = ComputeBottomLayout(frame.bottom_bar, 1.0f, {});
  EmptyLayout empty_layout;
  const float px = (frame.minimap.panel.left + frame.minimap.panel.right) * 0.5f;
  const float py = (frame.minimap.panel.top + frame.minimap.panel.bottom) * 0.5f;
  Check(HitTestFrame(frame, title, bottom, empty_layout, px, py) ==
            HitId::kMinimap,
        "the bird's eye panel is hit before the image");
  Check(HitTestFrame(frame, title, bottom, empty_layout, 40.0f, 100.0f) ==
            HitId::kImage,
        "the image is still hit outside the panel");
  Check(HitTestFrame(frame, title, bottom, empty_layout, 640.0f, 700.0f) ==
            HitId::kStrip,
        "the rail is still hit below the panel");
}

}  // namespace

namespace nv::tests {
int RunUiLayoutTests() {
  g_failures = 0;
  TestContentAndChrome();
  TestAutoStripAndTooltip();
  TestBottomStack();
  TestDpiScaling();
  TestTitleSegments();
  TestBottomParts();
  TestMenuLayout();
  TestHitStack();
  TestOverlayStack();
  TestWindowState();
  TestStripModeAndTokens();
  TestInfoWindowSize();
  TestInfoPanelLayout();
  TestEmptyLayout();
  TestEmptyHit();
  TestFormatSummary();
  TestFormatTableDerivation();
  TestFormatPanelLayout();
  TestImageArrows();
  TestCommandQuery();
  TestCommandKeys();
  TestViewRotation();
  TestZoomMath();
  TestUiErrorModel();
  TestErrorCardLayout();
  TestConfirmCardLayout();
  TestLoadingAndToastLayout();
  TestAnimationController();
  TestAnimPillLayout();
  TestPreviewPlan();
  TestPreviewBadgeLayout();
  TestDrawQuality();
  TestMinimapLayout();
  return g_failures;
}
}  // namespace nv::tests
