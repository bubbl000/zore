#include "ui/info_window.h"

#include "platform/theme.h"
#include "platform/window_capture.h"
#include "platform/window_placement.h"
#include "ui/metrics.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace nv::ui {
namespace {

constexpr wchar_t kWindowClass[] = L"ZoreView.InfoWindow";
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

// Every monitor work area in physical pixels. The window only needs them to
// decide "may this saved position be restored" and where to centre, so this is a
// local helper instead of another shared module.
std::vector<nv::platform::IntRect> WorkAreas() {
  std::vector<nv::platform::IntRect> areas;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        auto* out = reinterpret_cast<std::vector<nv::platform::IntRect>*>(param);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
          out->push_back(nv::platform::IntRect{info.rcWork.left, info.rcWork.top,
                                               info.rcWork.right,
                                               info.rcWork.bottom});
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&areas));
  return areas;
}

float ScrollFadeAlpha(int index, int last_visible) {
  if (last_visible < 0) return 1.0f;
  return index > last_visible ? 0.55f : 1.0f;
}

}  // namespace

InfoWindow::InfoWindow() = default;

InfoWindow::~InfoWindow() {
  if (hwnd_ != nullptr) {
    host_ = Host{};
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  ReleaseSurface();
  ui_.Shutdown();
}

bool InfoWindow::RegisterWindowClass() {
  if (g_class_registered) return true;
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = &InfoWindow::WindowProc;
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

bool InfoWindow::Open(HWND owner, const InfoPanelStrings& strings,
                      bool dark_theme, float dpi_scale,
                      bool has_saved_position, int saved_x, int saved_y,
                      Host host) {
  if (hwnd_ != nullptr) {
    SetForegroundWindow(hwnd_);
    return true;
  }
  if (owner == nullptr) return false;
  owner_ = owner;
  strings_ = strings;
  host_ = std::move(host);
  scroll_ = 0.0f;
  hover_ = HitId::kNone;
  pressed_ = HitId::kNone;
  dpi_scale_ = S(dpi_scale);
  dark_theme_ = dark_theme;

  if (!ui_.Create()) return false;
  ui_.SetDpiScale(dpi_scale_);
  colors_ = TokensFor(dark_theme_, nv::platform::IsHighContrastEnabled()).color;

  if (!RegisterWindowClass()) return false;

  // Size from the row count and the owner's monitor work area, then place it:
  // a stored position is honoured when it is still visible, otherwise the card
  // is centred in the work area (規劃/26 §4: missing/invalid values fall back to
  // hidden + centred).
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  nv::platform::IntRect work{0, 0, 1280, 800};
  if (GetMonitorInfoW(monitor, &monitor_info)) {
    work = nv::platform::IntRect{monitor_info.rcWork.left,
                                 monitor_info.rcWork.top,
                                 monitor_info.rcWork.right,
                                 monitor_info.rcWork.bottom};
  }
  const InfoWindowSize size =
      ComputeInfoWindowSize(strings_.lines.size(), dpi_scale_,
                            static_cast<float>(work.right - work.left),
                            static_cast<float>(work.bottom - work.top));
  const int width = static_cast<int>(std::lround(size.width));
  const int height = static_cast<int>(std::lround(size.height));

  nv::platform::IntRect rect{0, 0, width, height};
  const std::vector<nv::platform::IntRect> areas = WorkAreas();
  if (has_saved_position) {
    rect = nv::platform::IntRect{saved_x, saved_y, saved_x + width,
                                 saved_y + height};
    if (!nv::platform::PlacementVisible(rect, areas)) {
      rect = nv::platform::ClampPlacement(rect, areas, width, height);
    }
  } else {
    rect = nv::platform::CenterInWorkArea(work, width, height);
  }

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW, kWindowClass, strings_.title.c_str(), WS_POPUP, rect.left,
      rect.top, width, height, owner, nullptr, GetModuleHandleW(nullptr), this);
  if (hwnd_ == nullptr) return false;

  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                        sizeof(corner));
  RebuildLayout();
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  SetForegroundWindow(hwnd_);
  InvalidateRect(hwnd_, nullptr, FALSE);
  return true;
}

void InfoWindow::Close() {
  if (hwnd_ != nullptr) DestroyWindow(hwnd_);
}

void InfoWindow::SetContent(const InfoPanelStrings& strings) {
  const bool rows_changed = strings.lines.size() != strings_.lines.size();
  strings_ = strings;
  if (hwnd_ == nullptr) return;
  if (rows_changed) ResizeToContent();
  scroll_ = std::clamp(scroll_, 0.0f, layout_.max_scroll);
  RebuildLayout();
  Invalidate();
}

void InfoWindow::ApplyTheme(bool dark_theme) {
  dark_theme_ = dark_theme;
  colors_ = TokensFor(dark_theme_, nv::platform::IsHighContrastEnabled()).color;
  ui_.RefreshAccessibility();
  brushes_.clear();
  Invalidate();
}

void InfoWindow::SetDpiScale(float dpi_scale) {
  dpi_scale_ = S(dpi_scale);
  ui_.SetDpiScale(dpi_scale_);
  brushes_.clear();
  if (hwnd_ != nullptr) {
    ResizeToContent();
    RebuildLayout();
    Invalidate();
  }
}

void InfoWindow::Invalidate() {
  if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

void InfoWindow::ResetScroll() {
  scroll_ = 0.0f;
  if (hwnd_ != nullptr) {
    RebuildLayout();
    Invalidate();
  }
}

bool InfoWindow::CaptureToPng(const std::filesystem::path& path) {
  return nv::platform::CaptureClientToPng(hwnd_, path);
}

RECT InfoWindow::WindowRect() const {
  RECT rect{};
  if (hwnd_ != nullptr) GetWindowRect(hwnd_, &rect);
  return rect;
}

bool InfoWindow::MoveTo(int x, int y) {
  if (hwnd_ == nullptr) return false;
  SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  // The system sends WM_EXITSIZEMOVE when a real drag ends; a synthetic move has
  // to send it so the persistence path runs exactly as it would for a user.
  SendMessageW(hwnd_, WM_EXITSIZEMOVE, 0, 0);
  return true;
}

void InfoWindow::ScrollByRows(int rows) {
  if (rows == 0 || hwnd_ == nullptr) return;
  const float step = metrics::kInfoRowHeight * 3.0f * dpi_scale_;
  scroll_ = std::clamp(scroll_ + static_cast<float>(rows) * step, 0.0f,
                       layout_.max_scroll);
  RebuildLayout();
  Invalidate();
}

bool InfoWindow::SendKey(unsigned virtual_key) {
  if (hwnd_ == nullptr) return false;
  SendMessageW(hwnd_, WM_KEYDOWN, virtual_key, 0);
  return true;
}

// --- Window procedure -------------------------------------------------------

LRESULT CALLBACK InfoWindow::WindowProc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
  InfoWindow* self = reinterpret_cast<InfoWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<InfoWindow*>(create->lpCreateParams);
    if (self != nullptr) {
      self->hwnd_ = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
  }
  if (self != nullptr) return self->HandleMessage(message, wparam, lparam);
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT InfoWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCHITTEST: {
      // Only the header (title band) is a caption, so the card can be dragged
      // there. The body stays a client area: a real right-click on a row must
      // reach WM_CONTEXTMENU ("copy this value") instead of the system menu, and
      // the wheel must keep scrolling. Buttons keep their client clicks.
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd_, &point);
      const float x = static_cast<float>(point.x);
      const float y = static_cast<float>(point.y);
      if (layout_.close.Contains(x, y) || layout_.copy_all.Contains(x, y)) {
        return HTCLIENT;
      }
      if (layout_.header.Contains(x, y)) return HTCAPTION;
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
      RebuildLayout();
      Invalidate();
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
      const HitId hover = HitTestInfoPanel(layout_, x, y);
      if (hover != hover_) {
        hover_ = hover;
        Invalidate();
      }
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
      TrackMouseEvent(&track);
      return 0;
    }
    case WM_MOUSELEAVE:
      hover_ = HitId::kNone;
      Invalidate();
      return 0;
    case WM_LBUTTONDOWN: {
      SetFocus(hwnd_);
      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      pressed_ = HitTestInfoPanel(layout_, x, y);
      Invalidate();
      return 0;
    }
    case WM_LBUTTONUP: {
      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      const HitId pressed = pressed_;
      pressed_ = HitId::kNone;
      if (pressed != HitId::kNone && HitTestInfoPanel(layout_, x, y) == pressed) {
        if (pressed == HitId::kInfoClose) {
          Close();
          return 0;
        }
        if (pressed == HitId::kInfoCopyAll && host_.on_copy_all) {
          host_.on_copy_all();
        }
      }
      Invalidate();
      return 0;
    }
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      if (layout_.scrollable) {
        const float step = metrics::kInfoRowHeight * 3.0f * dpi_scale_;
        scroll_ = std::clamp(scroll_ - static_cast<float>(delta) / 120.0f * step,
                             0.0f, layout_.max_scroll);
        RebuildLayout();
        Invalidate();
      }
      return 0;
    }
    case WM_CONTEXTMENU: {
      // Right-clicking a row copies that value (規劃/22/06 §4.3). There is no
      // application context menu in this window: it belongs to the main window.
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x == -1 && point.y == -1) GetCursorPos(&point);
      POINT client = point;
      if (!ScreenToClient(hwnd_, &client)) return 0;
      const int row = InfoLineAt(layout_, strings_.lines,
                                 static_cast<float>(client.x),
                                 static_cast<float>(client.y));
      if (row >= 0 && host_.on_copy_row) host_.on_copy_row(row);
      return 0;
    }
    case WM_KEYDOWN:
      // The card owns its keyboard: Escape closes it, and `I` (the key that opens
      // it) closes it again, because opening the window moves focus here and the
      // main window would otherwise never see the second press.
      if (wparam == VK_ESCAPE || wparam == 'I') {
        Close();
        return 0;
      }
      return 0;
    case WM_ACTIVATE:
      Invalidate();
      return 0;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      // The app chooses light/dark and pushes it; high contrast changes arrive
      // here too, so the palette is rebuilt from the last pushed theme
      // (規劃/27 坑 119/120).
      ApplyTheme(dark_theme_);
      return 0;
    case WM_DISPLAYCHANGE:
      ClampIntoWorkArea();
      return 0;
    case WM_EXITSIZEMOVE: {
      RECT rect{};
      GetWindowRect(hwnd_, &rect);
      if (host_.on_moved) host_.on_moved(rect.left, rect.top);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd_);
      return 0;
    case WM_DESTROY: {
      ReleaseSurface();
      hwnd_ = nullptr;
      const auto callback = std::move(host_.on_close);
      host_ = Host{};
      if (callback) callback();
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd_, message, wparam, lparam);
}

// --- Surface ----------------------------------------------------------------

bool InfoWindow::EnsureSurface() {
  if (target_) return true;
  if (hwnd_ == nullptr) return false;
  if (!factory_) {
    // One D2D factory per window is the settings-window pattern; it is not a
    // second D3D device, so device-loss handling stays local (規劃/27 坑 220).
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 factory_.GetAddressOf()))) {
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

void InfoWindow::ReleaseSurface() {
  brushes_.clear();
  target_.Reset();
  factory_.Reset();
}

void InfoWindow::ResizeSurface() {
  if (!target_ || hwnd_ == nullptr) return;
  RECT client{};
  GetClientRect(hwnd_, &client);
  target_->Resize(D2D1::SizeU(
      static_cast<UINT>(std::max<LONG>(1, client.right - client.left)),
      static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top))));
}

void InfoWindow::RebuildLayout() {
  RECT client{};
  if (hwnd_ != nullptr) GetClientRect(hwnd_, &client);
  const Rect panel = Rect::FromXYWH(0.0f, 0.0f,
                                    static_cast<float>(client.right - client.left),
                                    static_cast<float>(client.bottom - client.top));
  const float button_text =
      ui_.MeasureText(strings_.copy_all, metrics::kInfoRowFontSize,
                      kWeightSemibold);
  const float button_width =
      button_text + metrics::kInfoButtonPadding * 2.0f * dpi_scale_;
  layout_ = ComputeInfoPanelLayout(panel, dpi_scale_, strings_.lines, scroll_,
                                   button_width);
  scroll_ = layout_.scroll;
}

void InfoWindow::ResizeToContent() {
  if (hwnd_ == nullptr) return;
  RECT window{};
  GetWindowRect(hwnd_, &window);
  HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  nv::platform::IntRect work{0, 0, 1280, 800};
  if (GetMonitorInfoW(monitor, &info)) {
    work = nv::platform::IntRect{info.rcWork.left, info.rcWork.top,
                                 info.rcWork.right, info.rcWork.bottom};
  }
  const InfoWindowSize size =
      ComputeInfoWindowSize(strings_.lines.size(), dpi_scale_,
                            static_cast<float>(work.right - work.left),
                            static_cast<float>(work.bottom - work.top));
  const int width = static_cast<int>(std::lround(size.width));
  const int height = static_cast<int>(std::lround(size.height));
  if (window.right - window.left == width && window.bottom - window.top == height) {
    return;
  }
  SetWindowPos(hwnd_, nullptr, 0, 0, width, height,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void InfoWindow::ClampIntoWorkArea() {
  if (hwnd_ == nullptr) return;
  RECT rect{};
  GetWindowRect(hwnd_, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const std::vector<nv::platform::IntRect> areas = WorkAreas();
  if (areas.empty()) return;
  nv::platform::IntRect placed{rect.left, rect.top, rect.right, rect.bottom};
  if (nv::platform::PlacementVisible(placed, areas)) return;
  placed = nv::platform::ClampPlacement(placed, areas, width, height);
  SetWindowPos(hwnd_, nullptr, placed.left, placed.top, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  if (host_.on_moved) host_.on_moved(placed.left, placed.top);
}

// --- Painting ---------------------------------------------------------------

ID2D1SolidColorBrush* InfoWindow::Brush(ID2D1RenderTarget* rt, uint32_t argb) {
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

void InfoWindow::FillRect(ID2D1RenderTarget* rt, const Rect& rect,
                          uint32_t argb) {
  if (rt == nullptr || rect.empty()) return;
  ID2D1SolidColorBrush* brush = Brush(rt, argb);
  if (brush != nullptr) rt->FillRectangle(ToRect(rect), brush);
}

void InfoWindow::FillRoundRect(ID2D1RenderTarget* rt, const Rect& rect,
                               float radius, uint32_t argb) {
  if (rt == nullptr || rect.empty()) return;
  ID2D1SolidColorBrush* brush = Brush(rt, argb);
  if (brush == nullptr) return;
  const float r = std::min(radius, std::min(rect.width(), rect.height()) * 0.5f);
  rt->FillRoundedRectangle(D2D1::RoundedRect(ToRect(rect), r, r), brush);
}

void InfoWindow::StrokeRoundRect(ID2D1RenderTarget* rt, const Rect& rect,
                                 float radius, float width, uint32_t argb) {
  if (rt == nullptr || rect.empty()) return;
  ID2D1SolidColorBrush* brush = Brush(rt, argb);
  if (brush == nullptr) return;
  const float r = std::min(radius, std::min(rect.width(), rect.height()) * 0.5f);
  rt->DrawRoundedRectangle(D2D1::RoundedRect(ToRect(rect), r, r), brush, width);
}

void InfoWindow::DrawTextLine(ID2D1RenderTarget* rt, const std::wstring& text,
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

void InfoWindow::Paint() {
  if (hwnd_ == nullptr) return;
  if (!EnsureSurface()) return;
  RebuildLayout();
  ID2D1HwndRenderTarget* rt = target_.Get();
  const ColorTokens& c = colors_;
  const float s = dpi_scale_;

  rt->BeginDraw();
  rt->SetTransform(D2D1::Matrix3x2F::Identity());
  rt->Clear(ToColor(c.bg_panel));

  // Panel border: one pixel inside the client so high contrast still shows the
  // window edge (規劃/27 坑 120).
  StrokeRoundRect(rt, Rect::FromLTRB(0.5f, 0.5f,
                                     layout_.panel.right - 0.5f,
                                     layout_.panel.bottom - 0.5f),
                  0.0f, 1.0f, c.border_strong);

  DrawTextLine(rt, strings_.title, metrics::kInfoTitleFontSize, kWeightSemibold,
               Rect::FromLTRB(layout_.header.left, layout_.header.top,
                              layout_.close.left - 4.0f * s,
                              layout_.header.bottom),
               c.text_primary, true, false);

  {
    const Rect& box = layout_.close;
    const bool hovered = hover_ == HitId::kInfoClose;
    if (hovered) {
      FillRoundRect(rt, box, metrics::kRadiusControl * s, c.bg_hover);
    }
    ID2D1SolidColorBrush* brush =
        Brush(rt, hovered ? c.text_primary : c.text_secondary);
    if (brush != nullptr) {
      const float inset = box.width() * 0.28f;
      rt->DrawLine(D2D1::Point2F(box.left + inset, box.top + inset),
                   D2D1::Point2F(box.right - inset, box.bottom - inset), brush,
                   1.4f * s);
      rt->DrawLine(D2D1::Point2F(box.right - inset, box.top + inset),
                   D2D1::Point2F(box.left + inset, box.bottom - inset), brush,
                   1.4f * s);
    }
  }

  // Body, clipped so a partially scrolled line cannot bleed into header/footer.
  const D2D1_RECT_F clip = ToRect(layout_.body);
  rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
  const float label_w = metrics::kInfoLabelWidth * s;
  for (size_t i = 0; i < layout_.line.size() && i < strings_.lines.size(); ++i) {
    const Rect& row = layout_.line[i];
    if (row.bottom <= layout_.body.top + 0.5f) continue;
    if (row.top >= layout_.body.bottom - 0.5f) break;
    const InfoLine& line = strings_.lines[i];
    if (line.header) {
      DrawTextLine(rt, line.label, metrics::kInfoGroupFontSize, kWeightSemibold,
                   Rect::FromLTRB(row.left, row.top, row.right, row.bottom),
                   c.text_muted, true, false);
      continue;
    }
    const float alpha =
        ScrollFadeAlpha(static_cast<int>(i), layout_.last_visible);
    DrawTextLine(rt, line.label, metrics::kInfoRowFontSize, kWeightRegular,
                 Rect::FromLTRB(row.left, row.top, row.left + label_w,
                                row.bottom),
                 WithAlpha(c.text_muted, alpha), true, false);
    DrawTextLine(rt, line.value, metrics::kInfoRowFontSize, kWeightRegular,
                 Rect::FromLTRB(row.left + label_w, row.top, row.right,
                                row.bottom),
                 WithAlpha(c.text_secondary, alpha), true, false);
  }
  rt->PopAxisAlignedClip();

  if (layout_.scrollable && !layout_.scrollbar_thumb.empty()) {
    FillRoundRect(rt, layout_.scrollbar_track,
                  metrics::kInfoScrollbarWidth * s * 0.5f,
                  WithAlpha(c.border_subtle, 0.5f));
    FillRoundRect(rt, layout_.scrollbar_thumb,
                  metrics::kInfoScrollbarWidth * s * 0.5f, c.border_strong);
  }

  // Footer separator plus the copy-all button.
  FillRect(rt, Rect::FromLTRB(layout_.footer.left, layout_.footer.top,
                              layout_.footer.right,
                              layout_.footer.top + 1.0f),
           c.border_subtle);
  {
    const bool hot = hover_ == HitId::kInfoCopyAll;
    const bool down = pressed_ == HitId::kInfoCopyAll;
    const uint32_t fill =
        down ? c.accent_pressed : (hot ? c.accent_hover : c.accent);
    FillRoundRect(rt, layout_.copy_all, metrics::kRadiusControl * s, fill);
    DrawTextLine(rt, strings_.copy_all, metrics::kInfoRowFontSize,
                 kWeightSemibold, layout_.copy_all, c.on_accent, true, true);
  }

  const HRESULT result = rt->EndDraw();
  if (result == D2DERR_RECREATE_TARGET) {
    ReleaseSurface();
    Invalidate();
  }
}

}  // namespace nv::ui
