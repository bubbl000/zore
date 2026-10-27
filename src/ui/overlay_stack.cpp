#include "ui/overlay_stack.h"

#include <utility>

namespace nv::ui {

const wchar_t* OverlayKindName(OverlayKind kind) {
  switch (kind) {
    case OverlayKind::kNone:
      return L"none";
    case OverlayKind::kMore:
      return L"more";
    case OverlayKind::kPercent:
      return L"percent";
    case OverlayKind::kContext:
      return L"context";
    case OverlayKind::kConfirm:
      return L"confirm";
    case OverlayKind::kPrintOptions:
      return L"print";
    case OverlayKind::kRename:
      return L"rename";
    case OverlayKind::kFormats:
      return L"formats";
  }
  return L"none";
}

const char* EscapeActionName(EscapeAction action) {
  switch (action) {
    case EscapeAction::kNone:
      return "none";
    case EscapeAction::kClosedSettings:
      return "settings";
    case EscapeAction::kClosedModal:
      return "modal";
    case EscapeAction::kClosedPopup:
      return "popup";
    case EscapeAction::kClosedError:
      return "error";
    case EscapeAction::kExitedFullscreen:
      return "fullscreen";
  }
  return "none";
}

bool OverlayStack::OpenModal(OverlayKind kind) {
  if (kind == OverlayKind::kNone || modal_ != OverlayKind::kNone) return false;
  modal_ = kind;
  // A modal owns the keyboard, so any popup underneath is dismissed.
  popup_ = OverlayKind::kNone;
  return true;
}

bool OverlayStack::CloseModal() {
  if (modal_ == OverlayKind::kNone) return false;
  modal_ = OverlayKind::kNone;
  return true;
}

bool OverlayStack::OpenPopup(OverlayKind kind) {
  if (kind == OverlayKind::kNone || modal_ != OverlayKind::kNone) return false;
  popup_ = kind;
  return true;
}

bool OverlayStack::ClosePopup() {
  if (popup_ == OverlayKind::kNone) return false;
  popup_ = OverlayKind::kNone;
  return true;
}

bool OverlayStack::TogglePopup(OverlayKind kind) {
  if (kind == OverlayKind::kNone) return false;
  if (popup_ == kind) return ClosePopup();
  return OpenPopup(kind);
}

EscapeAction OverlayStack::Escape() {
  if (settings_open_) {
    settings_open_ = false;
    return EscapeAction::kClosedSettings;
  }
  if (modal_ != OverlayKind::kNone) {
    modal_ = OverlayKind::kNone;
    return EscapeAction::kClosedModal;
  }
  if (popup_ != OverlayKind::kNone) {
    popup_ = OverlayKind::kNone;
    return EscapeAction::kClosedPopup;
  }
  if (error_open_) {
    error_open_ = false;
    return EscapeAction::kClosedError;
  }
  if (fullscreen_) {
    fullscreen_ = false;
    return EscapeAction::kExitedFullscreen;
  }
  return EscapeAction::kNone;
}

void OverlayStack::PushToast(std::wstring text, bool danger) {
  if (text.empty()) return;
  // Same text already visible: move it to the newest slot instead of showing a
  // duplicate (規劃/22/11 坑 5).
  for (auto it = toasts_.begin(); it != toasts_.end();) {
    if (it->text == text) {
      it = toasts_.erase(it);
    } else {
      ++it;
    }
  }
  toasts_.push_back(ToastItem{std::move(text), danger});
  while (toasts_.size() > kMaxToasts) toasts_.pop_front();
}

void OverlayStack::PopToast() {
  if (!toasts_.empty()) toasts_.pop_front();
}

}  // namespace nv::ui
