#pragma once

namespace nv::ui {

// Normal / Maximized / Fullscreen, with the invariant that fullscreen always
// remembers the state to restore (規劃/22/01 §4.3).
//
// Pure logic: the caller applies the returned mode to the HWND. Keeping it
// separate is what makes "F11 from a maximized window returns to maximized"
// testable without a desktop.
enum class WindowMode {
  kNormal = 0,
  kMaximized,
  kFullscreen,
};

const char* WindowModeName(WindowMode mode);

class WindowStateMachine {
 public:
  WindowMode mode() const { return mode_; }
  bool fullscreen() const { return mode_ == WindowMode::kFullscreen; }
  bool maximized() const { return mode_ == WindowMode::kMaximized; }

  // The mode restored when fullscreen ends. kNormal when not fullscreen.
  WindowMode mode_before_fullscreen() const { return before_fullscreen_; }

  // Maximize/restore. Ignored while fullscreen (the title-bar button is
  // disabled there, but the shortcut and double click can still arrive).
  WindowMode ToggleMaximize();

  // F11. Remembers whether the window was maximized.
  WindowMode ToggleFullscreen();
  WindowMode ExitFullscreen();

  // WM_SIZE / snap / system menu changed the state behind our back.
  WindowMode SetMaximized(bool maximized);

 private:
  WindowMode mode_ = WindowMode::kNormal;
  WindowMode before_fullscreen_ = WindowMode::kNormal;
};

}  // namespace nv::ui
