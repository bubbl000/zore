#pragma once

#include "platform/settings.h"
#include "ui/settings_layout.h"
#include "ui/theme_tokens.h"
#include "ui/ui_context.h"

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nv::ui {

// Self-drawn, non-modal settings window (規劃/22/07 §4).
//
// The window edits the app's single Settings instance in place. It never saves
// by itself: every change goes through `on_changed`, and the app applies the
// value to the live UI and persists it. That keeps one source of truth
// (`Settings`) and lets the main window update while the dialog stays open.
//
// It is an owned top-level window with WS_EX_TOOLWINDOW: it does not appear in
// the taskbar, and because it owns keyboard focus its Escape never reaches the
// main window's overlay stack (規劃/22/07 坑 11/20).
class SettingsWindow {
 public:
  SettingsWindow();
  ~SettingsWindow();

  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;

  using ChangedFn = std::function<void(const std::wstring& key)>;

  bool Open(HWND owner, nv::platform::Settings* model, ChangedFn on_changed,
            std::function<void()> on_close);
  void Close();
  bool IsOpen() const { return hwnd_ != nullptr; }
  HWND hwnd() const { return hwnd_; }

  // Re-evaluates colours when the theme/high-contrast state changes.
  void ApplyTheme();
  void Invalidate();
  void SetPage(SettingsPage page);
  // Inline footer message (save failure, restore result).
  void SetFooterStatus(const std::wstring& text);

  // --- Test hooks -----------------------------------------------------------
  // Activates the control named by `name` exactly like a click would
  // ("page.view", "row.theme.2", "row.checker", "footer.close",
  // "assoc.cell.3", ...). Returns false for an unknown name.
  bool ActivateByName(const std::wstring& name);
  // Writes the window state and the live model as `key=value` lines.
  std::wstring DumpState() const;
  // Captures the client area to a PNG (PrintWindow). Used by the settings
  // screenshot checks; returns false when Windows cannot print the window.
  bool CaptureToPng(const std::wstring& path);

 private:
  enum class Field {
    kNone,
    kLanguage,
    kTheme,
    kChecker,
    kCheckerOpacity,
    kStrip,
    kBirdseye,
    kRemember,
    kSingle,
    kTopmost,
    kWheel,
    kZoomStep,
    kPixel,
    kKeepRotation,
    kFullscreenBg,
    kFolderWrap,
  };

  struct RowDef {
    SettingsRowKind kind = SettingsRowKind::kCheck;
    Field field = Field::kNone;
    std::wstring label;
    std::vector<std::wstring> options;
    bool enabled = true;
  };

  // One hit-testable element; `index` is the row/option/cell index.
  struct Target {
    enum class Kind {
      kNone,
      kPage,
      kRow,
      kOption,
      kFooter,
      kAssocCell,
      kAssocButton,
      kClose,
    };
    Kind kind = Kind::kNone;
    int index = -1;
    int option = -1;
    bool operator==(const Target& other) const {
      return kind == other.kind && index == other.index &&
             option == other.option;
    }
  };
  enum class Footer { kReset, kOpenConfig, kClose };
  enum class AssocButton { kSelectAll, kRegister, kUnregister, kSetDefault };

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  bool RegisterWindowClass();
  bool EnsureSurface();
  void ReleaseSurface();
  void ResizeSurface();
  void Paint();

  void RebuildRows();
  void RebuildLayout();
  void RefreshAssociation();
  void NotifyChanged(const std::wstring& key);

  Target HitTest(float x, float y) const;
  void OnClick(float x, float y);
  void Activate(const Target& target);
  void SetRadioValue(Field field, int option);
  int RadioValue(Field field) const;
  bool CheckValue(Field field) const;
  void ToggleCheck(Field field);
  int SliderValue() const;
  void SetSliderFromX(float x);

  void DrawRow(ID2D1RenderTarget* rt, const SettingsRowLayout& row,
               const RowDef& def, int index);
  void DrawCheck(ID2D1RenderTarget* rt, const Rect& box, bool checked,
                 bool enabled);
  void DrawRadio(ID2D1RenderTarget* rt, const Rect& box, bool selected,
                 bool enabled);
  void DrawSlider(ID2D1RenderTarget* rt, const SettingsRowLayout& row,
                  int value);
  void DrawButton(ID2D1RenderTarget* rt, const Rect& box,
                  const std::wstring& label, bool primary, bool enabled,
                  const Target& target);
  void DrawAssociation(ID2D1RenderTarget* rt);
  void DrawTextLine(ID2D1RenderTarget* rt, const std::wstring& text,
                float size_dip, int weight, const Rect& box, uint32_t argb,
                bool ellipsis, bool center);
  ID2D1SolidColorBrush* Brush(ID2D1RenderTarget* rt, uint32_t argb);
  void FillRect(ID2D1RenderTarget* rt, const Rect& rect, uint32_t argb);
  void FillRoundRect(ID2D1RenderTarget* rt, const Rect& rect, float radius,
                     uint32_t argb);
  void StrokeRoundRect(ID2D1RenderTarget* rt, const Rect& rect, float radius,
                       float width, uint32_t argb);
  void DrawFocusRing(ID2D1RenderTarget* rt, const Rect& rect);

  void MoveFocus(bool backward);
  void ActivateFocused();
  void BuildFocusItems();
  Rect FocusRect(const Target& target) const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  nv::platform::Settings* model_ = nullptr;
  ChangedFn on_changed_;
  std::function<void()> on_close_;

  UiContext ui_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  std::map<uint32_t, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushes_;
  TokenSet tokens_;

  SettingsPage page_ = SettingsPage::kGeneral;
  std::vector<RowDef> rows_;
  SettingsLayout layout_;
  AssociationLayout association_;
  float scroll_ = 0.0f;
  float association_scroll_ = 0.0f;
  float dpi_scale_ = 1.0f;

  Target hover_;
  Target pressed_;
  Target focus_;
  std::vector<Target> focus_items_;
  bool dragging_slider_ = false;

  struct AssociationState {
    std::vector<std::wstring> extensions;
    std::vector<bool> selected;
    std::vector<bool> open_with;
    std::vector<bool> is_default;
    std::wstring status;
    std::wstring status_secondary;
    bool confirm_default = false;
  } assoc_;
  bool confirm_restore_ = false;
  std::wstring footer_status_;
};

}  // namespace nv::ui
