#pragma once

#include "ui/toast.h"

#include <cstddef>
#include <deque>
#include <string>

namespace nv::ui {

// In-window layers above the chrome (規劃/22/20 §3). The settings window is a
// separate top-level window and is tracked only as a flag so Escape can give it
// the highest priority.
enum class OverlayKind {
  kNone = 0,
  kMore,
  kPercent,
  // Right-click menu (v3.0, 規劃/26 §2.9). It shares the popup layer with the
  // zoom menu and the more panel, so Escape order and click-outside handling
  // stay identical.
  kContext,
  kConfirm,
  kPrintOptions,
  // Rename overlay (V3.1-06 §06e): the same visual shell as the confirmation
  // card, with an EDIT child for IME/selection instead of a Win32 dialog.
  kRename,
  // "All formats" card opened from the empty state (規劃/22/03 §3.3).
  kFormats,
};

const wchar_t* OverlayKindName(OverlayKind kind);

// Which single layer one Escape press closed. Tests assert that a press never
// closes two layers at once.
enum class EscapeAction {
  kNone = 0,
  kClosedSettings,
  kClosedModal,
  kClosedPopup,
  // The error card sits below popups but above fullscreen (22/11 §4.2).
  kClosedError,
  kExitedFullscreen,
};

const char* EscapeActionName(EscapeAction action);

// One interaction stack for everything drawn inside the main window. All input
// routing asks this object instead of each panel tracking its own state.
class OverlayStack {
 public:
  static constexpr size_t kMaxToasts = ::nv::ui::kMaxToasts;

  bool settings_open() const { return settings_open_; }
  void SetSettingsOpen(bool open) { settings_open_ = open; }

  OverlayKind modal() const { return modal_; }
  OverlayKind popup() const { return popup_; }
  bool error_open() const { return error_open_; }
  bool fullscreen() const { return fullscreen_; }

  // Opens a modal. Fails (returns false) when another modal is already open:
  // nested modals are not supported by design.
  bool OpenModal(OverlayKind kind);
  bool CloseModal();
  bool IsModal(OverlayKind kind) const { return modal_ == kind; }

  bool OpenPopup(OverlayKind kind);
  bool ClosePopup();
  bool TogglePopup(OverlayKind kind);
  bool IsPopup(OverlayKind kind) const { return popup_ == kind; }

  void SetErrorOpen(bool open) { error_open_ = open; }

  void SetFullscreen(bool fullscreen) { fullscreen_ = fullscreen; }

  // Closes exactly one layer, in the documented priority:
  // settings -> modal -> popup -> error card -> fullscreen. The info card is its
  // own top-level window and owns its Escape (規劃/27 坑 219).
  EscapeAction Escape();

  // A modal disables every image command (open/next/zoom/...); a popup only
  // disables navigation and view commands that its menu owns.
  bool BlocksImageCommands() const { return modal_ != OverlayKind::kNone; }
  bool BlocksNavigation() const {
    return modal_ != OverlayKind::kNone || popup_ != OverlayKind::kNone;
  }

  // Clicking outside a popup closes it; info and modals stay open.
  bool ClickOutsideClosesPopup() const { return popup_ != OverlayKind::kNone; }

  // Toast queue: at most kMaxToasts entries, newest last. Pushing a text that
  // is already queued moves it to the newest slot instead of duplicating it
  // (規劃/22/11 §4.1 / 坑 5).
  void PushToast(std::wstring text, bool danger = false);
  void PopToast();
  void ClearToasts() { toasts_.clear(); }
  const std::deque<ToastItem>& toasts() const { return toasts_; }
  bool toast_visible() const { return !toasts_.empty(); }

 private:
  bool settings_open_ = false;
  OverlayKind modal_ = OverlayKind::kNone;
  OverlayKind popup_ = OverlayKind::kNone;
  bool error_open_ = false;
  bool fullscreen_ = false;
  std::deque<ToastItem> toasts_;
};

}  // namespace nv::ui
