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

// Error card content, already localized by the app (規劃/22/11 §4.2). `buttons`
// are the action labels in visual order; Close is always appended by the card.
struct ErrorCardStrings {
  std::wstring title;
  std::wstring message;
  std::wstring detail;  // technical line, may be empty
  std::vector<std::wstring> buttons;
  std::wstring close;
};

struct ErrorCardLayout {
  Rect card;
  Rect icon;
  Rect title;
  Rect message;
  Rect detail;
  std::vector<Rect> buttons;
  Rect close;
  bool visible = false;
};

// Centred card inside the image content rectangle. `button_widths` are measured
// pixel widths (from UiRoot) so long localized labels are not clipped; when the
// row would overflow the card the widths are shrunk proportionally.
ErrorCardLayout ComputeErrorCardLayout(const Rect& content, float dpi_scale,
                                       const std::vector<float>& button_widths,
                                       bool has_detail);

// Buttons only: kErrorAction0..2 for the action row, kErrorClose for Close,
// kNone anywhere else (the card never swallows image clicks).
HitId HitTestErrorCard(const ErrorCardLayout& layout, float x, float y);

void DrawErrorCard(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                   const ErrorCardLayout& layout, const ErrorCardStrings& strings,
                   const ColorTokens& colors, float dpi_scale, HitId hover,
                   HitId pressed, int default_index);

// Danger confirmation card (規劃/22/11 §4.3). A modal: it dims the window, owns
// every click and is closed by Escape/cancel. The folder-edge prompt (22/10)
// reuses the same card with `danger = false` and an optional remember row.
struct ConfirmCardStrings {
  std::wstring title;
  std::wstring body;
  std::wstring primary;    // destructive action
  std::wstring secondary;  // safe action (default, also Enter)
  // Red primary/icon for destructive actions; accent for informational prompts.
  bool danger = true;
  // "Remember my choice" checkbox (empty hides the row).
  bool remember_visible = false;
  bool remember_checked = false;
  std::wstring remember;
};

struct ConfirmCardLayout {
  Rect scrim;
  Rect card;
  Rect title;
  Rect body;
  Rect primary;
  Rect secondary;
  Rect remember;
  Rect remember_box;
  bool has_remember = false;
  bool visible = false;
};

ConfirmCardLayout ComputeConfirmCardLayout(const Rect& client, float dpi_scale,
                                           bool has_remember = false);

HitId HitTestConfirmCard(const ConfirmCardLayout& layout, float x, float y);

void DrawConfirmCard(ID2D1DeviceContext* context, UiContext& ui, IconSet& icons,
                     const ConfirmCardLayout& layout,
                     const ConfirmCardStrings& strings,
                     const ColorTokens& colors, float dpi_scale, HitId hover,
                     HitId pressed);

// Rename overlay (V3.1-06 §06e). Same shell as the confirmation card — title,
// one-line hint, an input box, OK/Cancel — but the text itself lives in a child
// EDIT that the app positions over `input` (that is what keeps IME, selection,
// the caret and the clipboard native while the frame stays ours).
struct RenameCardStrings {
  std::wstring title;
  std::wstring hint;
  std::wstring ok;
  std::wstring cancel;
  // Shown inside the box when the EDIT is empty, and in place of an error line.
  std::wstring placeholder;
  std::wstring error;
};

struct RenameCardLayout {
  Rect scrim;
  Rect card;
  Rect title;
  Rect hint;
  Rect input;
  Rect error;
  Rect primary;
  Rect secondary;
  bool visible = false;
};

RenameCardLayout ComputeRenameCardLayout(const Rect& client, float dpi_scale);

HitId HitTestRenameCard(const RenameCardLayout& layout, float x, float y);

// `text` is drawn only when the app has no EDIT yet (tests, or a renderer that
// cannot host one); with a live EDIT the box is left for the child window.
void DrawRenameCard(ID2D1DeviceContext* context, UiContext& ui,
                    const RenameCardLayout& layout,
                    const RenameCardStrings& strings,
                    const ColorTokens& colors, float dpi_scale, HitId hover,
                    HitId pressed, bool draw_text);

}  // namespace nv::ui
