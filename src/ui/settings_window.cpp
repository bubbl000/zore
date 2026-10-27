#include "ui/settings_window.h"

#include "platform/dpi.h"
#include "platform/file_association.h"
#include "platform/i18n.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/theme.h"
#include "platform/window_capture.h"
#include "ui/metrics.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <dxgiformat.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cstdio>
#include <string>
#include <vector>

namespace nv::ui {
namespace {

constexpr wchar_t kWindowClass[] = L"ZoreView.SettingsWindow";
constexpr int kWeightRegular = 400;
constexpr int kWeightSemibold = 600;

bool g_class_registered = false;

float S(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

D2D1_COLOR_F ToColor(uint32_t argb) {
  return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
                      static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
                      static_cast<float>(argb & 0xFFu) / 255.0f,
                      static_cast<float>((argb >> 24) & 0xFFu) / 255.0f);
}

D2D1_RECT_F ToRect(const Rect& rect) {
  return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

std::filesystem::path ExecutablePath() {
  wchar_t buffer[1024]{};
  const DWORD length = GetModuleFileNameW(nullptr, buffer,
                                          static_cast<DWORD>(std::size(buffer)));
  if (length == 0 || length >= std::size(buffer)) return {};
  return std::filesystem::path(buffer);
}

std::wstring Format(const wchar_t* format, int a, int b) {
  wchar_t buffer[256]{};
  std::swprintf(buffer, std::size(buffer), format, a, b);
  return buffer;
}

// Numbers go through this overload: passing a std::wstring to a %d format
// printed the pointer's low word ("已注册 -602939680 个扩展名").
std::wstring FormatString(const wchar_t* format, int value) {
  wchar_t buffer[256]{};
  std::swprintf(buffer, std::size(buffer), format, value);
  return buffer;
}
std::wstring FormatString(const wchar_t* format, const std::wstring& value) {
  wchar_t buffer[512]{};
  std::swprintf(buffer, std::size(buffer), format, value.c_str());
  return buffer;
}

}  // namespace

SettingsWindow::SettingsWindow() = default;

SettingsWindow::~SettingsWindow() {
  if (hwnd_ != nullptr) {
    on_close_ = nullptr;
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  ReleaseSurface();
  ui_.Shutdown();
}

bool SettingsWindow::RegisterWindowClass() {
  if (g_class_registered) return true;
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = &SettingsWindow::WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  g_class_registered = true;
  return true;
}

bool SettingsWindow::Open(HWND owner, nv::platform::Settings* model,
                          ChangedFn on_changed,
                          std::function<void()> on_close) {
  if (hwnd_ != nullptr) {
    SetForegroundWindow(hwnd_);
    return true;
  }
  if (model == nullptr) return false;
  owner_ = owner;
  model_ = model;
  on_changed_ = std::move(on_changed);
  on_close_ = std::move(on_close);
  page_ = SettingsPage::kGeneral;
  scroll_ = 0.0f;
  association_scroll_ = 0.0f;
  confirm_restore_ = false;
  footer_status_.clear();
  focus_ = Target{};
  hover_ = Target{};
  pressed_ = Target{};

  if (!ui_.Create()) {
    nv::platform::LogError("settings: DWrite creation failed");
    return false;
  }
  dpi_scale_ = S(nv::platform::DpiScaleForWindow(owner));
  tokens_ = TokensFor(model_->dark_theme,
                      nv::platform::IsHighContrastEnabled());
  ui_.SetDpiScale(dpi_scale_);

  if (!RegisterWindowClass()) return false;

  const int width = static_cast<int>(std::lround(metrics::kSettingsWidth *
                                                 dpi_scale_));
  const int height = static_cast<int>(std::lround(metrics::kSettingsHeight *
                                                  dpi_scale_));
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  RECT work{0, 0, width, height};
  if (GetMonitorInfoW(monitor, &info)) work = info.rcWork;
  const int w = std::min(width, static_cast<int>(work.right - work.left));
  const int h = std::min(height, static_cast<int>(work.bottom - work.top));
  const int x = work.left + ((work.right - work.left) - w) / 2;
  const int y = work.top + ((work.bottom - work.top) - h) / 2;

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW, kWindowClass, nv::i18n::Text(nv::i18n::StringId::kSettingsTitle),
      WS_POPUP, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), this);
  if (hwnd_ == nullptr) {
    nv::platform::LogError("settings: CreateWindowEx failed");
    return false;
  }

  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                        sizeof(corner));
  nv::platform::LogInfo("settings: open page=" +
                        Narrow(SettingsPageName(page_)));
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  SetForegroundWindow(hwnd_);
  InvalidateRect(hwnd_, nullptr, FALSE);
  return true;
}

void SettingsWindow::Close() {
  if (hwnd_ != nullptr) DestroyWindow(hwnd_);
}

void SettingsWindow::ApplyTheme() {
  if (model_ == nullptr) return;
  tokens_ = TokensFor(model_->dark_theme,
                      nv::platform::IsHighContrastEnabled());
  brushes_.clear();
  Invalidate();
}

void SettingsWindow::Invalidate() {
  if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::SetPage(SettingsPage page) {
  page_ = page;
  scroll_ = 0.0f;
  association_scroll_ = 0.0f;
  focus_ = Target{};
  if (page_ == SettingsPage::kAssociation) RefreshAssociation();
  Invalidate();
}

void SettingsWindow::SetFooterStatus(const std::wstring& text) {
  footer_status_ = text;
  Invalidate();
}

// --- Window procedure -------------------------------------------------------

LRESULT CALLBACK SettingsWindow::WindowProc(HWND hwnd, UINT message,
                                            WPARAM wparam, LPARAM lparam) {
  SettingsWindow* self = reinterpret_cast<SettingsWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SettingsWindow*>(create->lpCreateParams);
    if (self != nullptr) {
      self->hwnd_ = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
  }
  if (self != nullptr) return self->HandleMessage(message, wparam, lparam);
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(UINT message, WPARAM wparam,
                                      LPARAM lparam) {
  switch (message) {
    case WM_NCHITTEST: {
      if (layout_.title.empty()) RebuildLayout();
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd_, &point);
      const float x = static_cast<float>(point.x);
      const float y = static_cast<float>(point.y);
      if (layout_.close_button.Contains(x, y)) return HTCLIENT;
      if (layout_.title.Contains(x, y)) return HTCAPTION;
      return HTCLIENT;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      ResizeSurface();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_DPICHANGED: {
      dpi_scale_ = S(static_cast<float>(HIWORD(wparam)) / 96.0f);
      ui_.SetDpiScale(dpi_scale_);
      brushes_.clear();
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      if (suggested != nullptr) {
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = static_cast<LONG>(
          std::lround(metrics::kSettingsMinWidth * dpi_scale_));
      info->ptMinTrackSize.y = static_cast<LONG>(
          std::lround(metrics::kSettingsMinHeight * dpi_scale_));
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(hwnd_, &paint);
      Paint();
      EndPaint(hwnd_, &paint);
      return 0;
    }
    case WM_MOUSEMOVE: {
      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      if (dragging_slider_) {
        SetSliderFromX(x);
        return 0;
      }
      const Target hover = HitTest(x, y);
      if (!(hover == hover_)) {
        hover_ = hover;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
      TrackMouseEvent(&track);
      return 0;
    }
    case WM_MOUSELEAVE:
      hover_ = Target{};
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      SetFocus(hwnd_);
      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      pressed_ = HitTest(x, y);
      if (pressed_.kind == Target::Kind::kRow) {
        const size_t index = static_cast<size_t>(pressed_.index);
        if (index < rows_.size() &&
            rows_[index].field == Field::kCheckerOpacity) {
          dragging_slider_ = true;
          SetCapture(hwnd_);
          SetSliderFromX(x);
        }
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      if (dragging_slider_) {
        dragging_slider_ = false;
        pressed_ = Target{};
        ReleaseCapture();
        // Commit the previewed value once the drag ends.
        NotifyChanged(L"checker_opacity");
        return 0;
      }
      const Target pressed = pressed_;
      pressed_ = Target{};
      if (pressed.kind != Target::Kind::kNone && HitTest(x, y) == pressed) {
        Activate(pressed);
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      const float step = metrics::kSettingsRowHeight * 3.0f * dpi_scale_;
      if (page_ == SettingsPage::kAssociation) {
        association_scroll_ = std::clamp(
            association_scroll_ - static_cast<float>(delta) / 120.0f * step,
            0.0f, association_.max_scroll);
      } else {
        scroll_ = std::clamp(
            scroll_ - static_cast<float>(delta) / 120.0f * step, 0.0f,
            layout_.max_scroll);
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_KEYDOWN: {
      switch (wparam) {
        case VK_ESCAPE:
          Close();
          return 0;
        case VK_TAB:
          MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
          return 0;
        case VK_UP:
          MoveFocus(true);
          return 0;
        case VK_DOWN:
          MoveFocus(false);
          return 0;
        case VK_RETURN:
          ActivateFocused();
          return 0;
        case VK_SPACE:
          ActivateFocused();
          return 0;
        case VK_LEFT:
        case VK_RIGHT: {
          if (focus_.kind != Target::Kind::kRow ||
              static_cast<size_t>(focus_.index) >= rows_.size()) {
            return 0;
          }
          const RowDef& row = rows_[static_cast<size_t>(focus_.index)];
          const int direction = wparam == VK_RIGHT ? 1 : -1;
          if (row.kind == SettingsRowKind::kRadio) {
            const int count = static_cast<int>(row.options.size());
            if (count > 0) {
              const int next = (RadioValue(row.field) + direction + count) % count;
              SetRadioValue(row.field, next);
            }
          } else if (row.kind == SettingsRowKind::kSlider) {
            const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            if (model_ != nullptr) {
              model_->checker_opacity =
                  std::clamp(model_->checker_opacity + direction * step, 0, 100);
              NotifyChanged(L"checker_opacity");
            }
          }
          return 0;
        }
        default:
          return 0;
      }
    }
    case WM_ACTIVATE:
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd_);
      return 0;
    case WM_DESTROY: {
      ReleaseSurface();
      hwnd_ = nullptr;
      const auto callback = std::move(on_close_);
      on_close_ = nullptr;
      nv::platform::LogInfo("settings: closed");
      if (callback) callback();
      return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      ApplyTheme();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, message, wparam, lparam);
}

// --- Surface ----------------------------------------------------------------

bool SettingsWindow::EnsureSurface() {
  if (target_) return true;
  if (hwnd_ == nullptr) return false;
  if (!factory_) {
    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            &options,
            reinterpret_cast<void**>(factory_.GetAddressOf())))) {
      return false;
    }
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const UINT w = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
  const UINT h =
      static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
  const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
      96.0f, 96.0f);
  const D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_properties =
      D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(w, h));
  if (FAILED(factory_->CreateHwndRenderTarget(properties, hwnd_properties,
                                              target_.GetAddressOf()))) {
    return false;
  }
  target_->SetDpi(96.0f, 96.0f);
  brushes_.clear();
  return true;
}

void SettingsWindow::ReleaseSurface() {
  brushes_.clear();
  target_.Reset();
  factory_.Reset();
}

void SettingsWindow::ResizeSurface() {
  if (!target_ || hwnd_ == nullptr) return;
  RECT client{};
  GetClientRect(hwnd_, &client);
  target_->Resize(D2D1::SizeU(
      static_cast<UINT>(std::max<LONG>(1, client.right - client.left)),
      static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top))));
}

// --- Rows and layout --------------------------------------------------------

void SettingsWindow::RebuildRows() {
  using nv::i18n::StringId;
  const auto t = [](StringId id) { return std::wstring(nv::i18n::Text(id)); };
  rows_.clear();
  if (model_ == nullptr) return;

  if (page_ == SettingsPage::kGeneral) {
    rows_.push_back({SettingsRowKind::kGroup, Field::kNone,
                     t(StringId::kSettingsGroupInterface), {}});
    rows_.push_back({SettingsRowKind::kRadio, Field::kLanguage,
                     t(StringId::kSettingsLanguage),
                     {t(StringId::kSettingsFollowSystem),
                      t(StringId::kLanguageChinese),
                      t(StringId::kLanguageEnglish)}});
    rows_.push_back({SettingsRowKind::kRadio, Field::kTheme,
                     t(StringId::kSettingsTheme),
                     {t(StringId::kSettingsThemeDark),
                      t(StringId::kSettingsThemeLight),
                      t(StringId::kSettingsFollowSystem)}});
    rows_.push_back({SettingsRowKind::kCheck, Field::kChecker,
                     t(StringId::kSettingsChecker), {}});
    rows_.push_back({SettingsRowKind::kSlider, Field::kCheckerOpacity,
                     t(StringId::kSettingsCheckerOpacity), {}});
    rows_.push_back({SettingsRowKind::kGroup, Field::kNone,
                     t(StringId::kSettingsGroupThumbnails), {}});
    rows_.push_back({SettingsRowKind::kRadio, Field::kStrip,
                     t(StringId::kSettingsStrip),
                     {t(StringId::kSettingsStripAuto),
                      t(StringId::kSettingsStripPinned),
                      t(StringId::kSettingsStripOff)}});
    rows_.push_back({SettingsRowKind::kCheck, Field::kBirdseye,
                     t(StringId::kSettingsBirdseye), {}});
    rows_.push_back({SettingsRowKind::kGroup, Field::kNone,
                     t(StringId::kSettingsGroupWindow), {}});
    rows_.push_back({SettingsRowKind::kCheck, Field::kRemember,
                     t(StringId::kSettingsRememberWindow), {}});
    rows_.push_back({SettingsRowKind::kCheck, Field::kSingle,
                     t(StringId::kSettingsSingleInstance), {}});
    rows_.push_back({SettingsRowKind::kCheck, Field::kTopmost,
                     t(StringId::kSettingsTopmost), {}});
    rows_.push_back({SettingsRowKind::kNote, Field::kNone,
                     t(StringId::kSettingsRestartNote), {}});
  } else if (page_ == SettingsPage::kView) {
    rows_.push_back({SettingsRowKind::kGroup, Field::kNone,
                     t(StringId::kSettingsGroupZoom), {}});
    rows_.push_back({SettingsRowKind::kRadio, Field::kWheel,
                     t(StringId::kSettingsWheel),
                     {t(StringId::kSettingsWheelZoom),
                      t(StringId::kSettingsWheelPage)}});
    std::vector<std::wstring> steps;
    for (const double step : nv::platform::AllowedZoomSteps()) {
      wchar_t buffer[32]{};
      std::swprintf(buffer, std::size(buffer),
                    nv::i18n::Text(StringId::kSettingsZoomStepFormat), step);
      steps.emplace_back(buffer);
    }
    rows_.push_back({SettingsRowKind::kRadio, Field::kZoomStep,
                     t(StringId::kSettingsZoomStep), std::move(steps)});
    rows_.push_back({SettingsRowKind::kCheck, Field::kPixel,
                     t(StringId::kSettingsPixelMode), {}});
    rows_.push_back({SettingsRowKind::kCheck, Field::kKeepRotation,
                     t(StringId::kSettingsKeepRotation), {}});
    rows_.push_back({SettingsRowKind::kGroup, Field::kNone,
                     t(StringId::kSettingsGroupFullscreen), {}});
    rows_.push_back({SettingsRowKind::kRadio, Field::kFullscreenBg,
                     t(StringId::kSettingsFullscreenBackground),
                     {t(StringId::kSettingsFullscreenTheme),
                      t(StringId::kSettingsFullscreenBlack),
                      t(StringId::kSettingsFullscreenDim)}});
    rows_.push_back({SettingsRowKind::kRadio, Field::kFolderWrap,
                     t(StringId::kSettingsFolderWrap),
                     {t(StringId::kSettingsWrapLoop),
                      t(StringId::kSettingsWrapNext),
                      t(StringId::kSettingsWrapAsk)}});
  }
}

void SettingsWindow::RebuildLayout() {
  RebuildRows();
  RECT client{};
  GetClientRect(hwnd_, &client);
  const Rect rect = Rect::FromLTRB(0.0f, 0.0f,
                                   static_cast<float>(client.right),
                                   static_cast<float>(client.bottom));
  SettingsLayoutInputs inputs;
  inputs.page = page_;
  inputs.scroll = scroll_;
  const float s = dpi_scale_;
  for (const RowDef& row : rows_) {
    SettingsRowSpec spec;
    spec.kind = row.kind;
    if (row.kind == SettingsRowKind::kRadio) {
      for (const std::wstring& option : row.options) {
        spec.option_widths.push_back(
            ui_.MeasureText(option, metrics::kSettingsFontSize, kWeightRegular) +
            (metrics::kSettingsRadioSize + metrics::kSettingsOptionInnerGap) * s);
      }
    }
    inputs.rows.push_back(std::move(spec));
  }
  layout_ = ComputeSettingsLayout(inputs, s, rect);
  scroll_ = layout_.scroll;

  if (page_ == SettingsPage::kAssociation) {
    association_ = ComputeAssociationLayout(
        s, layout_.content, static_cast<int>(assoc_.extensions.size()),
        static_cast<int>(metrics::kAssociationColumns), association_scroll_);
    association_scroll_ = association_.scroll;
  } else {
    association_ = AssociationLayout{};
  }
  BuildFocusItems();
}

void SettingsWindow::BuildFocusItems() {
  focus_items_.clear();
  for (int i = 0; i < kSettingsPageCount; ++i) {
    focus_items_.push_back(Target{Target::Kind::kPage, i, -1});
  }
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].field == Field::kNone) continue;
    focus_items_.push_back(
        Target{Target::Kind::kRow, static_cast<int>(i), -1});
  }
  focus_items_.push_back(Target{Target::Kind::kFooter, 0, -1});
  focus_items_.push_back(Target{Target::Kind::kFooter, 1, -1});
  focus_items_.push_back(Target{Target::Kind::kFooter, 2, -1});
  if (page_ == SettingsPage::kAssociation) {
    focus_items_.push_back(Target{Target::Kind::kAssocButton, 0, -1});
    focus_items_.push_back(Target{Target::Kind::kAssocButton, 1, -1});
    focus_items_.push_back(Target{Target::Kind::kAssocButton, 2, -1});
    focus_items_.push_back(Target{Target::Kind::kAssocButton, 3, -1});
  }
  if (focus_.kind == Target::Kind::kNone) {
    // No focus ring until the user starts keyboard navigation: a ring around
    // the first page on open looks like a selected item, not a focus cue.
    return;
  }
  bool found = false;
  for (const Target& item : focus_items_) {
    if (item == focus_) {
      found = true;
      break;
    }
  }
  if (!found) focus_ = Target{};
}

// --- Painting ---------------------------------------------------------------

void SettingsWindow::Paint() {
  if (model_ == nullptr || hwnd_ == nullptr) return;
  if (!EnsureSurface()) return;
  RebuildLayout();
  ID2D1HwndRenderTarget* rt = target_.Get();
  const ColorTokens& c = tokens_.color;
  const float s = dpi_scale_;

  rt->BeginDraw();
  rt->SetTransform(D2D1::Matrix3x2F::Identity());
  rt->Clear(ToColor(c.bg_window));

  // Title bar.
  FillRect(rt, layout_.title, c.bg_panel);
  FillRect(rt, Rect::FromLTRB(layout_.title.left, layout_.title.bottom - 1.0f,
                              layout_.title.right, layout_.title.bottom),
           c.border_subtle);
  DrawTextLine(rt, nv::i18n::Text(nv::i18n::StringId::kSettingsTitle),
           metrics::kSettingsTitleFontSize, kWeightSemibold,
           layout_.title.Inset(metrics::kSettingsTitlePadding * s, 0.0f,
                               60.0f * s, 0.0f),
           c.text_primary, false, false);
  {
    const Rect& box = layout_.close_button;
    const bool hovered =
        hover_.kind == Target::Kind::kClose;
    if (hovered) {
      FillRoundRect(rt, box, metrics::kRadiusControl * s, c.danger);
    }
    const uint32_t color = hovered ? 0xFFFFFFFFu : c.text_secondary;
    ID2D1SolidColorBrush* brush = Brush(rt, color);
    if (brush) {
      const float inset = box.width() * 0.28f;
      rt->DrawLine(D2D1::Point2F(box.left + inset, box.top + inset),
                   D2D1::Point2F(box.right - inset, box.bottom - inset), brush,
                   1.4f * s);
      rt->DrawLine(D2D1::Point2F(box.right - inset, box.top + inset),
                   D2D1::Point2F(box.left + inset, box.bottom - inset), brush,
                   1.4f * s);
    }
  }

  // Left navigation.
  FillRect(rt, layout_.nav, c.bg_panel_inactive);
  for (int i = 0; i < kSettingsPageCount; ++i) {
    const Rect& row = layout_.nav_rows[static_cast<size_t>(i)];
    const bool active = static_cast<int>(page_) == i;
    if (active) {
      FillRect(rt, row, c.bg_panel);
      FillRect(rt, Rect::FromLTRB(row.left, row.top, row.left + metrics::kSettingsNavBarWidth * s,
                                  row.bottom),
               c.accent);
    }
    const nv::i18n::StringId ids[] = {
        nv::i18n::StringId::kSettingsPageGeneral,
        nv::i18n::StringId::kSettingsPageView,
        nv::i18n::StringId::kSettingsPageAssociation};
    DrawTextLine(rt, nv::i18n::Text(ids[i]), metrics::kSettingsNavFontSize,
             active ? kWeightSemibold : kWeightRegular,
             row.Inset(metrics::kSettingsNavPadding * s, 0.0f, 8.0f * s, 0.0f),
             active ? c.accent : c.text_secondary, false, false);
  }

  if (page_ == SettingsPage::kAssociation) {
    DrawAssociation(rt);
  } else {
    rt->PushAxisAlignedClip(ToRect(layout_.content),
                            D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < rows_.size() && i < layout_.rows.size(); ++i) {
      DrawRow(rt, layout_.rows[i], rows_[i], static_cast<int>(i));
    }
    rt->PopAxisAlignedClip();

    if (!layout_.scrollbar.empty() && layout_.max_scroll > 0.0f) {
      FillRoundRect(rt, layout_.scrollbar,
                    metrics::kSettingsScrollbarWidth * s * 0.5f,
                    c.border_strong);
      const float ratio = layout_.content.height() / layout_.body_height;
      const float thumb_h = std::max(
          metrics::kSettingsScrollbarWidth * s * 2.0f,
          layout_.scrollbar.height() * std::clamp(ratio, 0.0f, 1.0f));
      const float travel = layout_.scrollbar.height() - thumb_h;
      const float pos = layout_.max_scroll > 0.0f
                            ? layout_.scroll / layout_.max_scroll
                            : 0.0f;
      Rect thumb = layout_.scrollbar;
      thumb.top = layout_.scrollbar.top + travel * pos;
      thumb.bottom = thumb.top + thumb_h;
      FillRoundRect(rt, thumb, metrics::kSettingsScrollbarWidth * s * 0.5f,
                    c.text_muted);
    }
  }

  // Footer.
  FillRect(rt, layout_.footer, c.bg_panel);
  FillRect(rt, Rect::FromLTRB(layout_.footer.left, layout_.footer.top,
                              layout_.footer.right, layout_.footer.top + 1.0f),
           c.border_subtle);
  DrawButton(rt, layout_.reset,
             nv::i18n::Text(confirm_restore_
                                ? nv::i18n::StringId::kSettingsResetConfirm
                                : nv::i18n::StringId::kSettingsReset),
             false, true, Target{Target::Kind::kFooter, 0, -1});
  DrawButton(rt, layout_.open_config,
             nv::i18n::Text(nv::i18n::StringId::kSettingsOpenConfig), false,
             true, Target{Target::Kind::kFooter, 1, -1});
  DrawButton(rt, layout_.close,
             nv::i18n::Text(nv::i18n::StringId::kSettingsClose), true, true,
             Target{Target::Kind::kFooter, 2, -1});
  if (!footer_status_.empty()) {
    DrawTextLine(rt, footer_status_, metrics::kSettingsFontSize - 1.0f,
             kWeightRegular,
             Rect::FromLTRB(layout_.open_config.right + 12.0f * s,
                            layout_.footer.top,
                            layout_.close.left - 12.0f * s,
                            layout_.footer.bottom),
             c.text_secondary, true, false);
  }

  if (focus_.kind != Target::Kind::kNone && GetFocus() == hwnd_) {
    const Rect ring = FocusRect(focus_);
    if (!ring.empty()) DrawFocusRing(rt, ring);
  }

  const HRESULT hr = rt->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    ReleaseSurface();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void SettingsWindow::DrawRow(ID2D1RenderTarget* rt,
                             const SettingsRowLayout& row, const RowDef& def,
                             int index) {
  if (row.row.bottom < layout_.content.top ||
      row.row.top > layout_.content.bottom) {
    return;
  }
  const ColorTokens& c = tokens_.color;
  const float s = dpi_scale_;
  const bool is_slider = def.kind == SettingsRowKind::kSlider;
  const bool clickable = def.kind == SettingsRowKind::kCheck ||
                         def.kind == SettingsRowKind::kRadio || is_slider;
  const bool hovered =
      clickable && hover_.kind != Target::Kind::kNone &&
      ((hover_.kind == Target::Kind::kRow && hover_.index == index) ||
       (hover_.kind == Target::Kind::kOption && hover_.index == index));

  if (def.kind == SettingsRowKind::kGroup) {
    DrawTextLine(rt, def.label, metrics::kSettingsGroupFontSize,
             kWeightSemibold,
             Rect::FromLTRB(row.row.left, row.row.bottom - 22.0f * s,
                            row.row.right, row.row.bottom - 6.0f * s),
             c.text_muted, false, false);
    return;
  }
  if (def.kind == SettingsRowKind::kNote) {
    DrawTextLine(rt, def.label, metrics::kSettingsFontSize - 1.0f,
                 kWeightRegular, row.row, c.text_muted, true, false);
    return;
  }
  if (hovered && !is_slider) {
    FillRoundRect(rt, row.row, metrics::kRadiusControl * s * 0.8f, c.bg_hover);
  }

  DrawTextLine(rt, def.label, metrics::kSettingsFontSize, kWeightRegular,
           row.label,
           def.enabled ? c.text_primary : c.text_muted, true, false);

  switch (def.kind) {
    case SettingsRowKind::kCheck:
      DrawCheck(rt, row.control, CheckValue(def.field), true);
      break;
    case SettingsRowKind::kRadio: {
      const int value = RadioValue(def.field);
      for (size_t i = 0; i < row.options.size() && i < def.options.size();
           ++i) {
        const Rect& option = row.options[i];
        const bool selected = static_cast<int>(i) == value;
        const Rect box = Rect::FromXYWH(
            option.left, option.top, metrics::kSettingsRadioSize * s,
            metrics::kSettingsRadioSize * s);
        DrawRadio(rt, box, selected, true);
        DrawTextLine(rt, def.options[i], metrics::kSettingsFontSize,
                 kWeightRegular,
                 Rect::FromLTRB(
                     box.right + metrics::kSettingsOptionInnerGap * s,
                     row.row.top, option.right, row.row.bottom),
                 selected ? c.accent : c.text_primary, false, false);
      }
      break;
    }
    case SettingsRowKind::kSlider: {
      DrawSlider(rt, row, SliderValue());
      break;
    }
    case SettingsRowKind::kGroup:
    case SettingsRowKind::kNote:
      break;
  }
}

void SettingsWindow::DrawCheck(ID2D1RenderTarget* rt, const Rect& box,
                               bool checked, bool enabled) {
  const ColorTokens& c = tokens_.color;
  const float s = dpi_scale_;
  const float radius = 4.0f * s;
  if (checked) {
    FillRoundRect(rt, box, radius, enabled ? c.accent : c.border_subtle);
    ID2D1SolidColorBrush* brush = Brush(rt, c.on_accent);
    if (brush) {
      const float x = box.left;
      const float y = box.top;
      const float w = box.width();
      const float h = box.height();
      rt->DrawLine(D2D1::Point2F(x + w * 0.24f, y + h * 0.52f),
                   D2D1::Point2F(x + w * 0.43f, y + h * 0.72f), brush,
                   1.8f * s);
      rt->DrawLine(D2D1::Point2F(x + w * 0.43f, y + h * 0.72f),
                   D2D1::Point2F(x + w * 0.78f, y + h * 0.30f), brush,
                   1.8f * s);
    }
  } else {
    StrokeRoundRect(rt, box, radius, 1.0f * s,
                    enabled ? c.border_strong : c.border_subtle);
  }
}

void SettingsWindow::DrawRadio(ID2D1RenderTarget* rt, const Rect& box,
                               bool selected, bool enabled) {
  const ColorTokens& c = tokens_.color;
  const float s = dpi_scale_;
  ID2D1SolidColorBrush* brush =
      Brush(rt, selected ? c.accent : (enabled ? c.border_strong : c.border_subtle));
  if (brush == nullptr) return;
  const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
      D2D1::Point2F((box.left + box.right) * 0.5f,
                    (box.top + box.bottom) * 0.5f),
      box.width() * 0.5f - 0.5f * s, box.height() * 0.5f - 0.5f * s);
  rt->DrawEllipse(ellipse, brush, 1.2f * s);
  if (selected) {
    const D2D1_ELLIPSE dot = D2D1::Ellipse(
        ellipse.point, metrics::kSettingsRadioDot * s * 0.5f,
        metrics::kSettingsRadioDot * s * 0.5f);
    rt->FillEllipse(dot, brush);
  }
}

void SettingsWindow::DrawSlider(ID2D1RenderTarget* rt,
                                const SettingsRowLayout& row, int value) {
  const ColorTokens& c = tokens_.color;
  const float s = dpi_scale_;
  const Rect& track = row.control;
  const float radius = track.height() * 0.5f;
  FillRoundRect(rt, track, radius, c.border_strong);
  const float fraction = std::clamp(static_cast<float>(value) / 100.0f, 0.0f, 1.0f);
  Rect filled = track;
  filled.right = track.left + track.width() * fraction;
  if (filled.width() > 0.5f) FillRoundRect(rt, filled, radius, c.accent);
  const float knob = metrics::kSettingsSliderKnob * s;
  const float cx = track.left + track.width() * fraction;
  const Rect knob_rect = Rect::FromXYWH(
      cx - knob * 0.5f, track.top + (track.height() - knob) * 0.5f, knob, knob);
  FillRoundRect(rt, knob_rect, knob * 0.5f, c.accent_hover);
  wchar_t text[16]{};
  std::swprintf(text, std::size(text), L"%d%%", value);
  DrawTextLine(rt, text, metrics::kSettingsFontSize, kWeightRegular, row.value,
           c.text_secondary, false, true);
}

void SettingsWindow::DrawButton(ID2D1RenderTarget* rt, const Rect& box,
                                const std::wstring& label, bool primary,
                                bool enabled, const Target& target) {
  const ColorTokens& c = tokens_.color;
  const float s = dpi_scale_;
  const bool hovered = enabled && hover_ == target;
  const float radius = metrics::kRadiusControl * s;
  if (primary) {
    const uint32_t fill = !enabled
                              ? c.border_subtle
                              : (hovered ? c.accent_hover : c.accent);
    FillRoundRect(rt, box, radius, fill);
  } else {
    if (hovered) FillRoundRect(rt, box, radius, c.bg_hover);
    StrokeRoundRect(rt, box, radius, 1.0f * s, c.border_strong);
  }
  DrawTextLine(rt, label, metrics::kSettingsFontSize,
           primary ? kWeightSemibold : kWeightRegular, box,
           primary ? c.on_accent : (enabled ? c.text_primary : c.text_muted),
           true, true);
}

void SettingsWindow::DrawAssociation(ID2D1RenderTarget* rt) {
  const ColorTokens& c = tokens_.color;
  const float s = dpi_scale_;
  const AssociationLayout& a = association_;
  int registered = 0;
  for (const bool value : assoc_.open_with) {
    if (value) ++registered;
  }
  const std::wstring count =
      Format(nv::i18n::Text(nv::i18n::StringId::kSettingsAssocCountFormat),
             registered, static_cast<int>(assoc_.extensions.size()));
  DrawTextLine(rt, count, metrics::kSettingsFontSize, kWeightSemibold, a.header,
           c.text_primary, false, false);
  DrawCheck(rt, a.select_all, !assoc_.selected.empty() &&
                                  std::all_of(assoc_.selected.begin(),
                                              assoc_.selected.end(),
                                              [](bool v) { return v; }),
            true);
  DrawTextLine(rt, nv::i18n::Text(nv::i18n::StringId::kSettingsAssocSelectAll),
           metrics::kSettingsFontSize, kWeightRegular,
           Rect::FromLTRB(a.select_all.left - 64.0f * s, a.header.top,
                          a.select_all.left - 8.0f * s, a.header.bottom),
           c.text_primary, false, false);

  rt->PushAxisAlignedClip(ToRect(a.list), D2D1_ANTIALIAS_MODE_ALIASED);
  for (size_t i = 0; i < a.cells.size() && i < assoc_.extensions.size(); ++i) {
    const Rect& cell = a.cells[i].rect;
    const bool hovered = hover_.kind == Target::Kind::kAssocCell &&
                         hover_.index == static_cast<int>(i);
    if (hovered) {
      FillRoundRect(rt, cell, metrics::kRadiusControl * s * 0.7f, c.bg_hover);
    }
    const float box_size = metrics::kSettingsCheckSize * s;
    const Rect box = Rect::FromXYWH(
        cell.left + 4.0f * s, cell.top + (cell.height() - box_size) * 0.5f,
        box_size, box_size);
    DrawCheck(rt, box, assoc_.selected[i], true);
    DrawTextLine(rt, assoc_.extensions[i], metrics::kSettingsFontSize,
             kWeightRegular,
             Rect::FromLTRB(box.right + 8.0f * s, cell.top, cell.right,
                            cell.bottom),
             c.text_primary, true, false);
  }
  rt->PopAxisAlignedClip();

  if (!a.scrollbar.empty() && a.max_scroll > 0.0f) {
    FillRoundRect(rt, a.scrollbar,
                  metrics::kSettingsScrollbarWidth * s * 0.5f,
                  c.border_strong);
    const float ratio = a.list.height() / a.content_height;
    const float thumb_h = std::max(
        metrics::kSettingsScrollbarWidth * s * 2.0f,
        a.scrollbar.height() * std::clamp(ratio, 0.0f, 1.0f));
    const float travel = a.scrollbar.height() - thumb_h;
    const float pos = a.max_scroll > 0.0f ? a.scroll / a.max_scroll : 0.0f;
    Rect thumb = a.scrollbar;
    thumb.top = a.scrollbar.top + travel * pos;
    thumb.bottom = thumb.top + thumb_h;
    FillRoundRect(rt, thumb, metrics::kSettingsScrollbarWidth * s * 0.5f,
                  c.text_muted);
  }

  DrawButton(rt, a.register_button,
             nv::i18n::Text(nv::i18n::StringId::kSettingsAssocRegister), false,
             true, Target{Target::Kind::kAssocButton, 1, -1});
  DrawButton(rt, a.unregister_button,
             nv::i18n::Text(nv::i18n::StringId::kSettingsAssocUnregister),
             false, true, Target{Target::Kind::kAssocButton, 2, -1});
  DrawButton(rt, a.default_button,
             nv::i18n::Text(assoc_.confirm_default
                                ? nv::i18n::StringId::kSettingsAssocDefaultConfirm
                                : nv::i18n::StringId::kSettingsAssocDefault),
             false, true, Target{Target::Kind::kAssocButton, 3, -1});
  DrawTextLine(rt, assoc_.status, metrics::kSettingsFontSize, kWeightRegular,
           a.status_primary, c.text_secondary, true, false);
  DrawTextLine(rt, assoc_.status_secondary, metrics::kSettingsFontSize - 1.0f,
           kWeightRegular, a.status_secondary, c.text_muted, true, false);
}

void SettingsWindow::DrawTextLine(ID2D1RenderTarget* rt, const std::wstring& text,
                              float size_dip, int weight, const Rect& box,
                              uint32_t argb, bool ellipsis, bool center) {
  if (rt == nullptr || box.empty() || text.empty()) return;
  IDWriteTextFormat* format = ui_.TextFormat(size_dip, weight, true, center);
  if (format == nullptr) return;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(ui_.dwrite()->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), format, box.width(),
          box.height(), layout.GetAddressOf())) ||
      !layout) {
    return;
  }
  if (ellipsis) {
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* sign = ui_.Ellipsis(format);
    if (sign != nullptr) layout->SetTrimming(&trimming, sign);
  }
  ID2D1SolidColorBrush* brush = Brush(rt, argb);
  if (brush != nullptr) {
    rt->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), brush);
  }
}

ID2D1SolidColorBrush* SettingsWindow::Brush(ID2D1RenderTarget* rt,
                                            uint32_t argb) {
  if (rt == nullptr) return nullptr;
  auto it = brushes_.find(argb);
  if (it != brushes_.end()) return it->second.Get();
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(rt->CreateSolidColorBrush(ToColor(argb), &brush)) || !brush) {
    return nullptr;
  }
  auto inserted = brushes_.emplace(argb, std::move(brush));
  return inserted.first->second.Get();
}

void SettingsWindow::FillRect(ID2D1RenderTarget* rt, const Rect& rect,
                              uint32_t argb) {
  if (rt == nullptr || rect.empty()) return;
  ID2D1SolidColorBrush* brush = Brush(rt, argb);
  if (brush != nullptr) rt->FillRectangle(ToRect(rect), brush);
}

void SettingsWindow::FillRoundRect(ID2D1RenderTarget* rt, const Rect& rect,
                                   float radius, uint32_t argb) {
  if (rt == nullptr || rect.empty()) return;
  ID2D1SolidColorBrush* brush = Brush(rt, argb);
  if (brush != nullptr) {
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(ToRect(rect), radius, radius), brush);
  }
}

void SettingsWindow::StrokeRoundRect(ID2D1RenderTarget* rt, const Rect& rect,
                                     float radius, float width,
                                     uint32_t argb) {
  if (rt == nullptr || rect.empty()) return;
  ID2D1SolidColorBrush* brush = Brush(rt, argb);
  if (brush != nullptr) {
    rt->DrawRoundedRectangle(
        D2D1::RoundedRect(ToRect(rect), radius, radius), brush, width);
  }
}

void SettingsWindow::DrawFocusRing(ID2D1RenderTarget* rt, const Rect& rect) {
  const Rect ring = rect.Inset(-3.0f * dpi_scale_, -3.0f * dpi_scale_);
  StrokeRoundRect(rt, ring, metrics::kRadiusControl * dpi_scale_,
                  2.0f * dpi_scale_, tokens_.color.accent);
}

// --- Values -----------------------------------------------------------------

int SettingsWindow::RadioValue(Field field) const {
  if (model_ == nullptr) return 0;
  switch (field) {
    case Field::kLanguage:
      if (model_->language == L"zh") return 1;
      if (model_->language == L"en") return 2;
      return 0;
    case Field::kTheme:
      return static_cast<int>(model_->theme);
    case Field::kStrip:
      if (model_->thumbstrip_mode == L"auto") return 0;
      if (model_->thumbstrip_mode == L"pinned") return 1;
      return 2;
    case Field::kWheel:
      return model_->wheel == L"page" ? 1 : 0;
    case Field::kZoomStep: {
      const auto& steps = nv::platform::AllowedZoomSteps();
      for (size_t i = 0; i < steps.size(); ++i) {
        if (std::fabs(steps[i] - model_->zoom_step) < 0.001) {
          return static_cast<int>(i);
        }
      }
      return 2;
    }
    case Field::kFullscreenBg:
      if (model_->fullscreen_bg == L"black") return 1;
      if (model_->fullscreen_bg == L"opacity" || model_->fullscreen_bg == L"dim") return 2;
      return 0;
    case Field::kFolderWrap:
      if (model_->folder_wrap == L"next") return 1;
      if (model_->folder_wrap == L"ask") return 2;
      return 0;
    default:
      return 0;
  }
}

void SettingsWindow::SetRadioValue(Field field, int option) {
  if (model_ == nullptr) return;
  switch (field) {
    case Field::kLanguage:
      model_->language = option == 1 ? L"zh" : (option == 2 ? L"en" : L"");
      NotifyChanged(L"language");
      break;
    case Field::kTheme:
      model_->theme = static_cast<nv::platform::ThemeMode>(
          std::clamp(option, 0, 2));
      model_->dark_theme = nv::platform::ResolveDarkTheme(model_->theme);
      NotifyChanged(L"theme");
      break;
    case Field::kStrip:
      model_->thumbstrip_mode =
          option == 0 ? L"auto" : (option == 1 ? L"pinned" : L"off");
      NotifyChanged(L"strip_mode");
      break;
    case Field::kWheel:
      model_->wheel = option == 1 ? L"page" : L"zoom";
      NotifyChanged(L"wheel");
      break;
    case Field::kZoomStep: {
      const auto& steps = nv::platform::AllowedZoomSteps();
      if (option >= 0 && static_cast<size_t>(option) < steps.size()) {
        model_->zoom_step = steps[static_cast<size_t>(option)];
        NotifyChanged(L"zoom_step");
      }
      break;
    }
    case Field::kFullscreenBg:
      model_->fullscreen_bg =
          option == 1 ? L"black" : (option == 2 ? L"opacity" : L"theme");
      NotifyChanged(L"fullscreen_bg");
      break;
    case Field::kFolderWrap:
      model_->folder_wrap =
          option == 1 ? L"next" : (option == 2 ? L"ask" : L"loop");
      NotifyChanged(L"folder_wrap");
      break;
    default:
      break;
  }
}

bool SettingsWindow::CheckValue(Field field) const {
  if (model_ == nullptr) return false;
  switch (field) {
    case Field::kChecker: return model_->checkerboard;
    case Field::kBirdseye: return model_->birdseye;
    case Field::kRemember: return model_->remember_window;
    case Field::kSingle: return model_->single_instance;
    case Field::kTopmost: return model_->topmost;
    case Field::kPixel: return model_->pixel_mode;
    case Field::kKeepRotation: return model_->keep_rotation;
    default: return false;
  }
}

void SettingsWindow::ToggleCheck(Field field) {
  if (model_ == nullptr) return;
  switch (field) {
    case Field::kChecker:
      model_->checkerboard = !model_->checkerboard;
      NotifyChanged(L"checker");
      break;
    case Field::kBirdseye:
      model_->birdseye = !model_->birdseye;
      NotifyChanged(L"birdseye");
      break;
    case Field::kRemember:
      model_->remember_window = !model_->remember_window;
      NotifyChanged(L"remember_window");
      break;
    case Field::kSingle:
      model_->single_instance = !model_->single_instance;
      NotifyChanged(L"single_instance");
      break;
    case Field::kTopmost:
      model_->topmost = !model_->topmost;
      NotifyChanged(L"topmost");
      break;
    case Field::kPixel:
      model_->pixel_mode = !model_->pixel_mode;
      NotifyChanged(L"pixel_mode");
      break;
    case Field::kKeepRotation:
      model_->keep_rotation = !model_->keep_rotation;
      NotifyChanged(L"keep_rotation");
      break;
    default:
      break;
  }
}

int SettingsWindow::SliderValue() const {
  return model_ == nullptr ? 0 : model_->checker_opacity;
}

void SettingsWindow::SetSliderFromX(float x) {
  if (model_ == nullptr) return;
  Rect track;
  for (size_t i = 0; i < rows_.size() && i < layout_.rows.size(); ++i) {
    if (rows_[i].field == Field::kCheckerOpacity) {
      track = layout_.rows[i].control;
      break;
    }
  }
  if (track.empty()) return;
  const float fraction =
      std::clamp((x - track.left) / std::max(1.0f, track.width()), 0.0f, 1.0f);
  const int value = static_cast<int>(std::lround(fraction * 100.0f));
  if (value == model_->checker_opacity) return;
  model_->checker_opacity = value;
  // Live preview only: the disk write happens once on mouse-up, so dragging the
  // slider cannot hammer settings.ini (規劃/22/07 坑 6).
  if (on_changed_) on_changed_(L"checker_opacity_live");
  Invalidate();
}

void SettingsWindow::NotifyChanged(const std::wstring& key) {
  nv::platform::LogInfo("settings: change=" + Narrow(key));
  if (on_changed_) on_changed_(key);
  Invalidate();
}

// --- Hit-testing and activation --------------------------------------------

SettingsWindow::Target SettingsWindow::HitTest(float x, float y) const {
  if (layout_.close_button.Contains(x, y)) {
    return Target{Target::Kind::kClose, -1, -1};
  }
  if (layout_.reset.Contains(x, y)) return Target{Target::Kind::kFooter, 0, -1};
  if (layout_.open_config.Contains(x, y)) {
    return Target{Target::Kind::kFooter, 1, -1};
  }
  if (layout_.close.Contains(x, y)) return Target{Target::Kind::kFooter, 2, -1};

  const int nav = SettingsNavAt(layout_, x, y);
  if (nav >= 0) return Target{Target::Kind::kPage, nav, -1};

  if (page_ == SettingsPage::kAssociation) {
    const Rect select_all = association_.select_all.Inset(-8.0f, -6.0f);
    if (select_all.Contains(x, y)) {
      return Target{Target::Kind::kAssocButton, 0, -1};
    }
    if (association_.register_button.Contains(x, y)) {
      return Target{Target::Kind::kAssocButton, 1, -1};
    }
    if (association_.unregister_button.Contains(x, y)) {
      return Target{Target::Kind::kAssocButton, 2, -1};
    }
    if (association_.default_button.Contains(x, y)) {
      return Target{Target::Kind::kAssocButton, 3, -1};
    }
    const int cell = AssociationCellAt(association_, x, y);
    if (cell >= 0) return Target{Target::Kind::kAssocCell, cell, -1};
    return Target{};
  }

  if (layout_.content.Contains(x, y)) {
    for (size_t i = 0; i < rows_.size() && i < layout_.rows.size(); ++i) {
      const SettingsRowLayout& row = layout_.rows[i];
      const RowDef& def = rows_[i];
      if (def.kind == SettingsRowKind::kGroup ||
          def.kind == SettingsRowKind::kNote) {
        continue;
      }
      if (def.kind == SettingsRowKind::kRadio) {
        for (size_t o = 0; o < row.options.size(); ++o) {
          if (row.options[o].Contains(x, y)) {
            return Target{Target::Kind::kOption, static_cast<int>(i),
                          static_cast<int>(o)};
          }
        }
        continue;
      }
      if (row.row.Contains(x, y)) {
        return Target{Target::Kind::kRow, static_cast<int>(i), -1};
      }
    }
  }
  return Target{};
}

void SettingsWindow::OnClick(float x, float y) {
  const Target target = HitTest(x, y);
  Activate(target);
}

void SettingsWindow::Activate(const Target& target) {
  if (target.kind == Target::Kind::kNone || model_ == nullptr) return;
  switch (target.kind) {
    case Target::Kind::kClose:
      Close();
      return;
    case Target::Kind::kPage:
      page_ = static_cast<SettingsPage>(std::clamp(target.index, 0, 2));
      scroll_ = 0.0f;
      association_scroll_ = 0.0f;
      focus_ = Target{};
      confirm_restore_ = false;
      assoc_.confirm_default = false;
      footer_status_.clear();
      if (page_ == SettingsPage::kAssociation) RefreshAssociation();
      nv::platform::LogInfo("settings: page=" +
                            Narrow(SettingsPageName(page_)));
      Invalidate();
      return;
    case Target::Kind::kRow: {
      if (static_cast<size_t>(target.index) >= rows_.size()) return;
      const RowDef& def = rows_[static_cast<size_t>(target.index)];
      if (def.kind == SettingsRowKind::kCheck) ToggleCheck(def.field);
      return;
    }
    case Target::Kind::kOption: {
      if (static_cast<size_t>(target.index) >= rows_.size()) return;
      SetRadioValue(rows_[static_cast<size_t>(target.index)].field,
                    target.option);
      return;
    }
    case Target::Kind::kFooter: {
      if (target.index == 0) {
        if (!confirm_restore_) {
          confirm_restore_ = true;
          footer_status_ = nv::i18n::Text(
              nv::i18n::StringId::kSettingsResetHint);
          Invalidate();
          return;
        }
        const nv::platform::Settings defaults =
            nv::platform::DefaultsForRestore(*model_);
        *model_ = defaults;
        confirm_restore_ = false;
        footer_status_ = nv::i18n::Text(nv::i18n::StringId::kSettingsRestored);
        NotifyChanged(L"*");
        return;
      }
      if (target.index == 1) {
        const std::wstring dir = nv::platform::DataDir().wstring();
        ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return;
      }
      Close();
      return;
    }
    case Target::Kind::kAssocCell: {
      if (target.index < 0 ||
          static_cast<size_t>(target.index) >= assoc_.selected.size()) {
        return;
      }
      assoc_.selected[static_cast<size_t>(target.index)] =
          !assoc_.selected[static_cast<size_t>(target.index)];
      Invalidate();
      return;
    }
    case Target::Kind::kAssocButton:
      break;
  }

  // Association buttons.
  if (target.kind == Target::Kind::kAssocButton) {
    const std::filesystem::path exe = ExecutablePath();
    std::vector<std::wstring> selected;
    for (size_t i = 0; i < assoc_.extensions.size() && i < assoc_.selected.size();
         ++i) {
      if (assoc_.selected[i]) selected.push_back(assoc_.extensions[i]);
    }
    std::wstring error;
    switch (target.index) {
      case 0: {  // Select all / none
        const bool all = !assoc_.selected.empty() &&
                         std::all_of(assoc_.selected.begin(),
                                     assoc_.selected.end(),
                                     [](bool v) { return v; });
        std::fill(assoc_.selected.begin(), assoc_.selected.end(), !all);
        Invalidate();
        return;
      }
      case 1: {  // Register selected
        if (nv::platform::UpdateOpenWithForExtensions(exe, selected, true,
                                                      &error)) {
          assoc_.status = FormatString(
              nv::i18n::Text(nv::i18n::StringId::kSettingsAssocRegisteredFormat),
              static_cast<int>(selected.size()));
        } else {
          assoc_.status = FormatString(
              nv::i18n::Text(nv::i18n::StringId::kSettingsAssocFailedFormat),
              error);
        }
        break;
      }
      case 2: {  // Unregister selected
        if (nv::platform::UpdateOpenWithForExtensions(exe, selected, false,
                                                      &error)) {
          assoc_.status = FormatString(
              nv::i18n::Text(nv::i18n::StringId::kSettingsAssocRegisteredFormat),
              0);
        } else {
          assoc_.status = FormatString(
              nv::i18n::Text(nv::i18n::StringId::kSettingsAssocFailedFormat),
              error);
        }
        break;
      }
      case 3: {  // Set as default (two-step confirmation)
        if (!assoc_.confirm_default) {
          assoc_.confirm_default = true;
          assoc_.status = nv::i18n::Text(
              nv::i18n::StringId::kSettingsAssocDefaultHint);
          nv::platform::LogInfo("settings: assoc action=3 confirm");
          Invalidate();
          return;
        }
        assoc_.confirm_default = false;
        std::wstring warning;
        if (nv::platform::RegisterFileAssociations(exe, true, &error, &warning)) {
          assoc_.status = FormatString(
              nv::i18n::Text(nv::i18n::StringId::kSettingsAssocRegisteredFormat),
              static_cast<int>(selected.size()));
        } else {
          assoc_.status = FormatString(
              nv::i18n::Text(nv::i18n::StringId::kSettingsAssocFailedFormat),
              error);
        }
        if (!warning.empty()) assoc_.status_secondary = warning;
        break;
      }
      default:
        return;
    }
    nv::platform::LogInfo("settings: assoc action=" +
                          std::to_string(target.index));
    RefreshAssociation();
    assoc_.confirm_default = false;
    Invalidate();
  }
}

void SettingsWindow::RefreshAssociation() {
  // Keep the user's selection across registry reads; only a first load (or an
  // extension-count change) starts with everything selected.
  const std::vector<bool> previous = assoc_.selected;
  assoc_.extensions.clear();
  assoc_.selected.clear();
  assoc_.open_with.clear();
  assoc_.is_default.clear();

  const auto states = nv::platform::GetAssociationExtensionStates();
  int registered = 0;
  int defaults = 0;
  for (const auto& state : states) {
    assoc_.extensions.push_back(state.extension);
    assoc_.open_with.push_back(state.open_with);
    assoc_.is_default.push_back(state.is_default);
    if (state.open_with) ++registered;
    if (state.is_default) ++defaults;
  }
  if (previous.size() == assoc_.extensions.size()) {
    assoc_.selected = previous;
  } else {
    assoc_.selected.assign(assoc_.extensions.size(), true);
  }
  if (assoc_.status.empty()) {
    assoc_.status = Format(
        nv::i18n::Text(nv::i18n::StringId::kSettingsAssocCountFormat),
        registered, static_cast<int>(assoc_.extensions.size()));
  }
  if (defaults > 0) {
    assoc_.status_secondary =
        nv::i18n::Text(nv::i18n::StringId::kSettingsAssocDefaultIsOurs);
  } else if (registered > 0) {
    assoc_.status_secondary =
        nv::i18n::Text(nv::i18n::StringId::kSettingsAssocDefaultOther);
  } else {
    assoc_.status_secondary =
        nv::i18n::Text(nv::i18n::StringId::kSettingsAssocNone);
  }
}

// --- Focus ------------------------------------------------------------------

Rect SettingsWindow::FocusRect(const Target& target) const {
  switch (target.kind) {
    case Target::Kind::kPage:
      if (target.index >= 0 && target.index < kSettingsPageCount) {
        return layout_.nav_rows[static_cast<size_t>(target.index)];
      }
      break;
    case Target::Kind::kRow:
    case Target::Kind::kOption: {
      if (target.kind == Target::Kind::kOption) {
        if (static_cast<size_t>(target.index) < layout_.rows.size()) {
          const auto& options =
              layout_.rows[static_cast<size_t>(target.index)].options;
          if (target.option >= 0 &&
              static_cast<size_t>(target.option) < options.size()) {
            return options[static_cast<size_t>(target.option)];
          }
        }
        break;
      }
      if (static_cast<size_t>(target.index) < layout_.rows.size()) {
        const auto& row = layout_.rows[static_cast<size_t>(target.index)];
        if (row.kind == SettingsRowKind::kRadio && !row.options.empty()) {
          const int value = RadioValue(rows_[static_cast<size_t>(target.index)].field);
          if (value >= 0 && static_cast<size_t>(value) < row.options.size()) {
            return row.options[static_cast<size_t>(value)];
          }
        }
        if (!row.control.empty()) return row.control;
        return row.row;
      }
      break;
    }
    case Target::Kind::kFooter:
      if (target.index == 0) return layout_.reset;
      if (target.index == 1) return layout_.open_config;
      if (target.index == 2) return layout_.close;
      break;
    case Target::Kind::kAssocButton:
      if (target.index == 0) return association_.select_all;
      if (target.index == 1) return association_.register_button;
      if (target.index == 2) return association_.unregister_button;
      if (target.index == 3) return association_.default_button;
      break;
    case Target::Kind::kAssocCell:
      if (static_cast<size_t>(target.index) < association_.cells.size()) {
        return association_.cells[static_cast<size_t>(target.index)].rect;
      }
      break;
    case Target::Kind::kClose:
      return layout_.close_button;
    case Target::Kind::kNone:
      break;
  }
  return Rect{};
}

void SettingsWindow::MoveFocus(bool backward) {
  if (focus_items_.empty()) return;
  size_t found = focus_items_.size();
  for (size_t i = 0; i < focus_items_.size(); ++i) {
    if (focus_items_[i] == focus_) {
      found = i;
      break;
    }
  }
  const size_t count = focus_items_.size();
  size_t index = 0;
  if (found == count) {
    index = backward ? count - 1 : 0;
  } else {
    index = backward ? (found + count - 1) % count : (found + 1) % count;
  }
  focus_ = focus_items_[index];
  Invalidate();
}

void SettingsWindow::ActivateFocused() {
  if (focus_.kind == Target::Kind::kNone) {
    Close();
    return;
  }
  Activate(focus_);
}

// --- Test hooks -------------------------------------------------------------

bool SettingsWindow::ActivateByName(const std::wstring& name) {
  if (!IsOpen() || model_ == nullptr) return false;
  RebuildLayout();
  if (name == L"footer.close" || name == L"close") {
    Close();
    return true;
  }
  if (name == L"footer.reset") {
    Activate(Target{Target::Kind::kFooter, 0, -1});
    return true;
  }
  if (name == L"footer.open_config") {
    Activate(Target{Target::Kind::kFooter, 1, -1});
    return true;
  }
  if (name.rfind(L"page.", 0) == 0) {
    const std::wstring page = name.substr(5);
    for (int i = 0; i < kSettingsPageCount; ++i) {
      if (page == SettingsPageName(static_cast<SettingsPage>(i))) {
        Activate(Target{Target::Kind::kPage, i, -1});
        return true;
      }
    }
    return false;
  }
  if (name.rfind(L"assoc.", 0) == 0) {
    const std::wstring action = name.substr(6);
    if (action == L"select_all") {
      Activate(Target{Target::Kind::kAssocButton, 0, -1});
      return true;
    }
    if (action == L"register") {
      Activate(Target{Target::Kind::kAssocButton, 1, -1});
      return true;
    }
    if (action == L"unregister") {
      Activate(Target{Target::Kind::kAssocButton, 2, -1});
      return true;
    }
    if (action == L"default") {
      Activate(Target{Target::Kind::kAssocButton, 3, -1});
      return true;
    }
    if (action.rfind(L"cell.", 0) == 0) {
      const int index = _wtoi(action.substr(5).c_str());
      if (index >= 0 && static_cast<size_t>(index) < assoc_.selected.size()) {
        assoc_.selected[static_cast<size_t>(index)] =
            !assoc_.selected[static_cast<size_t>(index)];
        Invalidate();
        return true;
      }
    }
    return false;
  }
  if (name.rfind(L"row.", 0) == 0) {
    std::wstring rest = name.substr(4);
    std::wstring value_part;
    const size_t dot = rest.find_last_of(L'.');
    if (dot != std::wstring::npos) {
      value_part = rest.substr(dot + 1);
      rest = rest.substr(0, dot);
    }
    static const std::pair<const wchar_t*, Field> kFields[] = {
        {L"language", Field::kLanguage},
        {L"theme", Field::kTheme},
        {L"checker", Field::kChecker},
        {L"checker_opacity", Field::kCheckerOpacity},
        {L"strip", Field::kStrip},
        {L"birdseye", Field::kBirdseye},
        {L"remember", Field::kRemember},
        {L"single", Field::kSingle},
        {L"topmost", Field::kTopmost},
        {L"wheel", Field::kWheel},
        {L"zoom_step", Field::kZoomStep},
        {L"pixel", Field::kPixel},
        {L"keep_rotation", Field::kKeepRotation},
        {L"fullscreen_bg", Field::kFullscreenBg},
        {L"folder_wrap", Field::kFolderWrap},
    };
    Field field = Field::kNone;
    for (const auto& [key, id] : kFields) {
      if (rest == key) {
        field = id;
        break;
      }
    }
    if (field == Field::kNone) return false;
    if (field == Field::kCheckerOpacity && !value_part.empty()) {
      model_->checker_opacity = std::clamp(_wtoi(value_part.c_str()), 0, 100);
      NotifyChanged(L"checker_opacity");
      return true;
    }
    switch (field) {
      case Field::kChecker:
      case Field::kBirdseye:
      case Field::kRemember:
      case Field::kSingle:
      case Field::kTopmost:
      case Field::kPixel:
      case Field::kKeepRotation:
        ToggleCheck(field);
        return true;
      default:
        break;
    }
    if (value_part.empty()) return false;
    SetRadioValue(field, _wtoi(value_part.c_str()));
    return true;
  }
  return false;
}

std::wstring SettingsWindow::DumpState() const {
  std::wstring out;
  const auto line = [&out](const wchar_t* key, const std::wstring& value) {
    out += key;
    out += L"=";
    out += value;
    out += L"\n";
  };
  line(L"open", IsOpen() ? L"1" : L"0");
  line(L"page", SettingsPageName(page_));
  line(L"scroll", std::to_wstring(static_cast<int>(scroll_)));
  line(L"assoc_scroll",
       std::to_wstring(static_cast<int>(association_scroll_)));
  if (model_ == nullptr) return out;
  line(L"language",
       model_->language.empty() ? L"system" : model_->language);
  line(L"theme", nv::platform::ThemeModeName(model_->theme));
  line(L"dark_theme", model_->dark_theme ? L"1" : L"0");
  line(L"checker", model_->checkerboard ? L"1" : L"0");
  line(L"checker_opacity", std::to_wstring(model_->checker_opacity));
  line(L"strip_mode", model_->thumbstrip_mode);
  line(L"birdseye", model_->birdseye ? L"1" : L"0");
  line(L"remember_window", model_->remember_window ? L"1" : L"0");
  line(L"single_instance", model_->single_instance ? L"1" : L"0");
  line(L"topmost", model_->topmost ? L"1" : L"0");
  line(L"wheel", model_->wheel);
  line(L"zoom_step", [&] {
    wchar_t buffer[16]{};
    std::swprintf(buffer, std::size(buffer), L"%.2f", model_->zoom_step);
    return std::wstring(buffer);
  }());
  line(L"pixel_mode", model_->pixel_mode ? L"1" : L"0");
  line(L"keep_rotation", model_->keep_rotation ? L"1" : L"0");
  line(L"fullscreen_bg", model_->fullscreen_bg);
  line(L"folder_wrap", model_->folder_wrap);
  line(L"window_x", std::to_wstring(model_->window_x));
  line(L"window_y", std::to_wstring(model_->window_y));
  line(L"window_w", std::to_wstring(model_->window_w));
  line(L"window_h", std::to_wstring(model_->window_h));
  line(L"window_maximized", model_->window_maximized ? L"1" : L"0");
  line(L"window_saved", model_->window_saved ? L"1" : L"0");
  line(L"assoc_count", std::to_wstring(assoc_.extensions.size()));
  line(L"assoc_status", assoc_.status);
  int registered = 0;
  int defaults = 0;
  for (const bool value : assoc_.open_with) {
    if (value) ++registered;
  }
  for (const bool value : assoc_.is_default) {
    if (value) ++defaults;
  }
  line(L"assoc_registered", std::to_wstring(registered));
  line(L"assoc_defaults", std::to_wstring(defaults));
  line(L"assoc_selected", std::to_wstring(std::count(
                              assoc_.selected.begin(), assoc_.selected.end(),
                              true)));
  line(L"nav_rows", std::to_wstring(layout_.nav_rows.size()));
  line(L"rows", std::to_wstring(rows_.size()));
  line(L"content_h", std::to_wstring(static_cast<int>(layout_.content.height())));
  line(L"body_h", std::to_wstring(static_cast<int>(layout_.body_height)));
  line(L"max_scroll", std::to_wstring(static_cast<int>(layout_.max_scroll)));
  line(L"title_rect", [&] {
    wchar_t buffer[96]{};
    std::swprintf(buffer, std::size(buffer), L"%d,%d,%d,%d",
                  static_cast<int>(layout_.title.left),
                  static_cast<int>(layout_.title.top),
                  static_cast<int>(layout_.title.right),
                  static_cast<int>(layout_.title.bottom));
    return std::wstring(buffer);
  }());
  line(L"close_rect", [&] {
    wchar_t buffer[96]{};
    std::swprintf(buffer, std::size(buffer), L"%d,%d,%d,%d",
                  static_cast<int>(layout_.close_button.left),
                  static_cast<int>(layout_.close_button.top),
                  static_cast<int>(layout_.close_button.right),
                  static_cast<int>(layout_.close_button.bottom));
    return std::wstring(buffer);
  }());
  return out;
}

bool SettingsWindow::CaptureToPng(const std::wstring& path) {
  return nv::platform::CaptureClientToPng(hwnd_, std::filesystem::path(path));
}

}  // namespace nv::ui
