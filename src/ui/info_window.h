#pragma once

// Standalone image-information window (v3.0, 規劃/26 §2.1 / item #1).
//
// It replaces the in-window info overlay: a borderless, non-taskbar tool window
// that owns its own ID2D1HwndRenderTarget and reuses the pure layout in
// ui/info_panel.h. Like the settings window it is never deleted from its own
// WM_DESTROY handler (規劃/22/07 坑 218) and it handles Escape itself
// (規劃/22/07 坑 219), so the main window's overlay stack stops owning it.
//
// The app owns the content: it pushes freshly formatted lines and receives
// callbacks for close/copy/move, which keeps i18n and file state out of the UI
// module and lets the window stay a pure presenter.

#include "ui/info_panel.h"
#include "ui/rect.h"
#include "ui/theme_tokens.h"
#include "ui/ui_context.h"

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace nv::ui {

class InfoWindow {
 public:
  // `on_close` runs for every user-driven close (close button, Escape, system
  // close) after the HWND is gone; `on_moved` after the user finishes dragging.
  struct Host {
    std::function<void()> on_close;
    std::function<void(int x, int y)> on_moved;
    std::function<void()> on_copy_all;
    std::function<void(int index)> on_copy_row;
  };

  InfoWindow();
  ~InfoWindow();

  InfoWindow(const InfoWindow&) = delete;
  InfoWindow& operator=(const InfoWindow&) = delete;

  // `saved_x/saved_y` are used when `has_saved_position`; otherwise the window is
  // centred in the owner's work area. Returns false when the window class,
  // rendering surface or HWND cannot be created.
  bool Open(HWND owner, const InfoPanelStrings& strings, bool dark_theme,
            float dpi_scale, bool has_saved_position, int saved_x, int saved_y,
            Host host);
  void Close();
  bool IsOpen() const { return hwnd_ != nullptr; }
  HWND hwnd() const { return hwnd_; }

  // Replaces the displayed lines. Resizes the window height when the row count
  // changed, keeping the top-left corner (the row set is usually stable, so this
  // is a no-op per image in practice).
  void SetContent(const InfoPanelStrings& strings);
  void ApplyTheme(bool dark_theme);
  void SetDpiScale(float dpi_scale);
  void Invalidate();
  // Resets the body scroll offset (a new file starts at the top).
  void ResetScroll();

  bool CaptureToPng(const std::filesystem::path& path);

  // --- Test hooks / inspection ---------------------------------------------
  const InfoPanelLayout& layout() const { return layout_; }
  const InfoPanelStrings& strings() const { return strings_; }
  float scroll() const { return scroll_; }
  int RowCount() const { return static_cast<int>(strings_.lines.size()); }
  // Screen rectangle of the whole window, in physical pixels.
  RECT WindowRect() const;
  // Moves the window and sends the WM_EXITSIZEMOVE the system would send when
  // the user let go of the frame, so placement persistence is testable without
  // a real mouse (same trick as --move-window).
  bool MoveTo(int x, int y);
  // Scrolls the body by whole rows (test hook --info-scroll).
  void ScrollByRows(int rows);
  // Delivers one key the way the focused window would (test hook --info-key).
  bool SendKey(unsigned virtual_key);

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  bool RegisterWindowClass();
  bool EnsureSurface();
  void ReleaseSurface();
  void ResizeSurface();
  void Paint();

  void RebuildLayout();
  void ResizeToContent();
  void ClampIntoWorkArea();

  ID2D1SolidColorBrush* Brush(ID2D1RenderTarget* rt, uint32_t argb);
  void FillRect(ID2D1RenderTarget* rt, const Rect& rect, uint32_t argb);
  void FillRoundRect(ID2D1RenderTarget* rt, const Rect& rect, float radius,
                     uint32_t argb);
  void StrokeRoundRect(ID2D1RenderTarget* rt, const Rect& rect, float radius,
                       float width, uint32_t argb);
  void DrawTextLine(ID2D1RenderTarget* rt, const std::wstring& text,
                    float size_dip, int weight, const Rect& rect, uint32_t argb,
                    bool ellipsis, bool center);

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  Host host_;

  UiContext ui_;
  Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  std::map<uint32_t, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushes_;
  ColorTokens colors_;

  InfoPanelStrings strings_;
  InfoPanelLayout layout_;
  float scroll_ = 0.0f;
  float dpi_scale_ = 1.0f;
  bool dark_theme_ = true;
  HitId hover_ = HitId::kNone;
  HitId pressed_ = HitId::kNone;
};

}  // namespace nv::ui
