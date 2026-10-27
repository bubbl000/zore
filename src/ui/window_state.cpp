#include "ui/window_state.h"

namespace nv::ui {

const char* WindowModeName(WindowMode mode) {
  switch (mode) {
    case WindowMode::kNormal:
      return "normal";
    case WindowMode::kMaximized:
      return "maximized";
    case WindowMode::kFullscreen:
      return "fullscreen";
  }
  return "normal";
}

WindowMode WindowStateMachine::ToggleMaximize() {
  if (mode_ == WindowMode::kFullscreen) return mode_;
  mode_ = mode_ == WindowMode::kMaximized ? WindowMode::kNormal
                                          : WindowMode::kMaximized;
  return mode_;
}

WindowMode WindowStateMachine::ToggleFullscreen() {
  if (mode_ == WindowMode::kFullscreen) return ExitFullscreen();
  before_fullscreen_ =
      mode_ == WindowMode::kMaximized ? WindowMode::kMaximized : WindowMode::kNormal;
  mode_ = WindowMode::kFullscreen;
  return mode_;
}

WindowMode WindowStateMachine::ExitFullscreen() {
  if (mode_ != WindowMode::kFullscreen) return mode_;
  mode_ = before_fullscreen_;
  before_fullscreen_ = WindowMode::kNormal;
  return mode_;
}

WindowMode WindowStateMachine::SetMaximized(bool maximized) {
  if (mode_ == WindowMode::kFullscreen) return mode_;
  mode_ = maximized ? WindowMode::kMaximized : WindowMode::kNormal;
  return mode_;
}

}  // namespace nv::ui
