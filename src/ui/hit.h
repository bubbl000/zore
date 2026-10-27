#pragma once

#include "ui/rect.h"

#include <cstddef>

namespace nv::ui {

// Every clickable region of the custom-drawn window. The hit stack is the only
// entry point for pointer input (規劃/22/20 §3.2); adding a control means adding
// an id here and its rectangle in layout.cpp, never a second coordinate system.
enum class HitId {
  kNone = 0,

  // Title bar (order matters: it is the button order in TitleLayout).
  kTitleMenu,
  kTitleFullscreen,
  kTitleMinimize,
  kTitleMaximize,
  kTitleClose,

  // Bottom bar.
  kBottomInfo,
  kBottomPercent,
  kBottomIndex,
  kBottomFit,
  kBottomActual,
  kBottomRotateLeft,
  kBottomRotateRight,
  kBottomMore,

  // Content.
  kStrip,
  kImage,

  // Popup surface (zoom menu / more panel). The overlay stack owns the popup
  // state; this id only tells the app "the click landed on the open menu".
  kMenuPanel,

  // Empty state card (規劃/22/03) and the all-formats modal it opens.
  kEmptyPrimary,
  kEmptyOpenFolder,
  kEmptyAllFormats,
  kFormatsPanel,
  kFormatsClose,

  // Image-area previous/next circles (規劃/22/09 §5.1).
  kArrowLeft,
  kArrowRight,

  // Frame control of an animated image (規劃/22/12 §3.1).
  kAnimPrev,
  kAnimNext,
  kAnimToggle,

  // Error card actions, in visual order, then the close button (規劃/22/11).
  // The card itself is not a hit target: clicks outside its buttons fall
  // through to the image so it never blocks panning.
  kErrorAction0,
  kErrorAction1,
  kErrorAction2,
  kErrorClose,

  // Danger confirmation card (delete): modal, two buttons. The folder-edge
  // prompt reuses it and adds the optional "remember" row (規劃/22/10 §4.3).
  kConfirmPrimary,
  kConfirmSecondary,
  kConfirmRemember,
  kConfirmPanel,

  // Info panel (規劃/22/06). The panel body is itself a hit target so a click
  // inside the card never pans the image; the two buttons sit on top of it.
  kInfoPanel,
  kInfoClose,
  kInfoCopyAll,

  // Fullscreen reveal (v3.0, 規劃/26 §2.7): the two buttons that float in the
  // top-right corner while the pointer is there. They sit on the chrome layer,
  // above the image, and are hit before everything else while visible.
  kFullscreenExit,
  kFullscreenClose,

  // Bird's eye view (v3.0, 規劃/26 §2.4). The whole panel is one drag surface:
  // pressing inside the frame drags it, pressing the blank margin re-centres the
  // view there. It is hit above the rail and the image area.
  kMinimap,

  // Rename overlay (V3.1-06 §06e). The input box itself is an EDIT child, so it
  // owns its own mouse/keyboard; these two are the card's buttons and kRenamePanel
  // is the modal surface that swallows every other click.
  kRenameOk,
  kRenameCancel,
  kRenamePanel,
};

inline constexpr size_t kTitleButtonCount = 5;
inline constexpr size_t kErrorActionCount = 3;

// Info-panel buttons; the panel surface itself is not a button.
inline bool IsInfoAction(HitId id) {
  return id == HitId::kInfoClose || id == HitId::kInfoCopyAll;
}

inline bool IsTitleButton(HitId id) {
  return id >= HitId::kTitleMenu && id <= HitId::kTitleClose;
}

inline bool IsBottomPart(HitId id) {
  return id >= HitId::kBottomInfo && id <= HitId::kBottomMore;
}

// Buttons of the startup empty-state card. They take the press/release path in
// the window procedure just like the chrome buttons.
inline bool IsEmptyAction(HitId id) {
  return id >= HitId::kEmptyPrimary && id <= HitId::kEmptyAllFormats;
}

// Image-area navigation circles. They are hit before the image so a click on
// them can never start a pan (規劃/22/09 坑 6).
inline bool IsArrow(HitId id) {
  return id == HitId::kArrowLeft || id == HitId::kArrowRight;
}

// Buttons of the animation frame pill (規劃/22/12). The pill surface itself is
// not a target, so clicks between the buttons still reach the image.
inline bool IsAnimPillButton(HitId id) {
  return id >= HitId::kAnimPrev && id <= HitId::kAnimToggle;
}

// Error-card buttons (0..kErrorActionCount-1) plus its close button.
inline bool IsErrorAction(HitId id) {
  return id >= HitId::kErrorAction0 && id <= HitId::kErrorClose;
}

// Fullscreen reveal buttons (v3.0, 規劃/26 §2.7).
inline bool IsFullscreenAction(HitId id) {
  return id == HitId::kFullscreenExit || id == HitId::kFullscreenClose;
}

// Rename overlay buttons (V3.1-06 §06e). kRenamePanel is the modal surface.
inline bool IsRenameAction(HitId id) {
  return id == HitId::kRenameOk || id == HitId::kRenameCancel;
}

// Zero-based error-card button index; kErrorClose is reported by the caller
// separately, everything else is -1.
inline int ErrorActionIndex(HitId id) {
  if (id < HitId::kErrorAction0 || id > HitId::kErrorAction2) return -1;
  return static_cast<int>(id) - static_cast<int>(HitId::kErrorAction0);
}

// Zero-based index of a title button in TitleLayout.buttons; -1 for other ids.
inline int TitleButtonIndex(HitId id) {
  if (!IsTitleButton(id)) return -1;
  return static_cast<int>(id) - static_cast<int>(HitId::kTitleMenu);
}

struct HitRegion {
  HitId id = HitId::kNone;
  Rect rect;
  bool enabled = true;
};

// Returns the first enabled region containing the point, or HitId::kNone.
// Regions must be ordered from the topmost layer down (the hit stack order).
inline HitId HitTestRegions(const HitRegion* regions, size_t count, float x,
                            float y) {
  if (regions == nullptr) return HitId::kNone;
  for (size_t i = 0; i < count; ++i) {
    if (!regions[i].enabled) continue;
    if (regions[i].rect.Contains(x, y)) return regions[i].id;
  }
  return HitId::kNone;
}

}  // namespace nv::ui
