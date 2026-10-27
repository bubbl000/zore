#pragma once

#include "core/image.h"
#include "render/draw_quality.h"
#include "render/overlay_layer.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace nv::render {

// WM_TIMER id owned by the renderer: MarkInteractive() arms it once, and the
// app repaints when it fires so the final static frame upgrades to cubic
// (規劃/22/18 §3.2). The app must not use this id for anything else.
inline constexpr UINT_PTR kQualityUpgradeTimerId = 30;

// v3.0 (規劃/26 §2.4): the bird's eye photo is a one-off GPU downscale of the
// current frame, long edge at most this many pixels. It is a derived copy, not
// the V3-04 display-grade chain: no re-decode, no disk cache, ~1 MB of VRAM.
inline constexpr uint32_t kMinimapBakeEdge = 512;

class Renderer {
 public:
  // Called once per OverlayLayer after the image and before EndDraw, so the app
  // can paint UI chrome (rail, bottom bar, toasts, title bar) with the
  // renderer's own device context. Layer order is fixed by the renderer.
  using OverlayFn = std::function<void(ID2D1DeviceContext*, const D2D1_RECT_F&,
                                       bool dark_theme, bool high_contrast,
                                       OverlayLayer layer)>;

  // Device/D2D creation costs most of startup (driver + DLL initialization),
  // so it can run on a worker thread before the window exists. The bundle is
  // then adopted by Initialize() on whichever thread owns the window.
  struct DeviceBundle {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_device;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_context;
  };

  static std::unique_ptr<DeviceBundle> CreateDeviceBundle();

  Renderer();
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // Synchronous path: creates the device inline, then binds it to `hwnd`.
  bool Initialize(HWND hwnd);
  // Adopts a bundle produced by CreateDeviceBundle() and creates the parts that
  // need the window: swap chain, render target, checker brush.
  bool Initialize(HWND hwnd, std::unique_ptr<DeviceBundle> bundle);

  // True once the renderer can draw (swap chain and D2D target exist).
  bool ready() const { return d2d_context_ != nullptr && target_bitmap_ != nullptr; }

  void Shutdown();

  // Two-stage loading (規劃/22/13 §3.4): when the full decode replaces a
  // preview, `preserve_view` keeps the current zoom and re-centres the new
  // (larger) frame on the same picture point, so the replacement never jumps.
  // `reset_view` still wins: a new file starts in fit mode.
  bool SetImage(const Image& image, bool reset_view = true,
                bool preserve_view = false);
  // Frame updates for animation reuse the current view (fit/zoom/pan).
  bool SetImage(const ImageInfo& info, const std::vector<uint8_t>& pixels,
                bool reset_view = true, bool preserve_view = false);
  void ClearImage();

  void Resize(uint32_t width, uint32_t height);
  bool Render();

  // Draws one frame, copies the back buffer (top-down BGRA8), then presents.
  // Used by --screenshot; callers get exactly what the window shows.
  bool RenderAndCapture(std::vector<uint8_t>* bgra, uint32_t* width,
                        uint32_t* height);

  // Reserves `inset` pixels at the bottom of the client area for UI chrome;
  // fit/zoom/pan all operate on the remaining content rectangle.
  void SetBottomInset(float inset);
  float bottom_inset() const { return bottom_inset_; }
  // Title bar reservation. ContentRect() starts below it, so fit/zoom/pan all
  // operate on the image area only.
  void SetTopInset(float inset);
  float top_inset() const { return top_inset_; }

  void SetOverlay(OverlayFn overlay) { overlay_ = std::move(overlay); }
  ID2D1Factory1* d2d_factory() const { return d2d_factory_.Get(); }

  void SetTheme(bool dark);
  void SetCheckerboard(bool enabled);
  // 0-100: the checkerboard is blended towards the window background so a busy
  // pattern can be calmed down (規劃/22/07 §3.1 checker_opacity).
  void SetCheckerOpacity(int percent);
  // Nearest-neighbour magnification for pixel-art inspection (pixel_mode).
  // From 22/18 on this only applies at >= 100 %; shrinking stays cubic.
  void SetPixelMode(bool nearest);
  // Interpolation state machine (規劃/22/18 §3.1). Every pan/zoom/resize marks
  // the view interactive for kQualityUpgradeMs; the renderer then owns a
  // one-shot WM_TIMER on the window so the final frame is redrawn as cubic.
  void MarkInteractive();
  bool interactive() const;
  int interactive_remaining_ms() const;
  // Animation frames keep the cheap path while playing; a paused animation is
  // a static picture and upgrades to cubic (規劃/22/18 §3.4).
  void SetAnimating(bool animating);
  bool animating() const { return animating_; }
  // -1 = automatic; kFast/kGood/kPixel force a mode (--force-quality).
  void SetForcedQuality(int quality);
  int forced_quality() const { return forced_quality_; }
  // Display proxy switch (--proxy=off for A/B captures).
  void SetProxyEnabled(bool enabled);
  bool proxy_enabled() const { return proxy_enabled_; }
  bool proxy_active() const { return proxy_active_; }
  bool proxy_ready() const { return proxy_baked_; }
  // True while the uploaded bitmap is a display tier rather than the file's
  // full frame (規劃/25 §7.7). The app sets it right after every SetImage, so
  // the pure proxy policy can refuse to bake a proxy from a tier and the dump
  // can prove the input that decided it.
  void SetSourceIsTier(bool tier);
  bool source_is_tier() const { return source_is_tier_; }

  // Bird's eye view (v3.0, 規劃/26 §2.4). The app switches this on only while
  // the minimap is visible, so no extra VRAM is spent otherwise. The bake runs
  // once per frame change, before BeginDraw; `minimap_photo()` is the read-only
  // accessor the overlay uses while drawing.
  void SetMinimapEnabled(bool enabled);
  bool minimap_enabled() const { return minimap_enabled_; }
  // Never null while a readable picture exists and the minimap is enabled: a
  // small picture is returned as-is, a large one returns the baked copy. A
  // failed bake falls back to the full-size bitmap (slower but correct).
  ID2D1Bitmap* minimap_photo();
  bool minimap_ready() const { return minimap_baked_; }
  uint32_t minimap_width() const { return minimap_width_; }
  uint32_t minimap_height() const { return minimap_height_; }
  DrawQuality quality() const;
  // The quality the last DrawFrame actually used (a dump taken between paints
  // reads this rather than re-deriving from a stale timer).
  DrawQuality last_drawn_quality() const { return last_drawn_quality_; }
  // Fullscreen background override: 0 = theme, 1 = black, 2 = dimmed.
  void SetFullscreen(bool fullscreen);
  void SetFullscreenBackground(int mode);
  // High contrast follows the system window/selection colors and suppresses
  // the transparency checkerboard.
  void SetHighContrast(bool enabled);
  bool dark_theme() const { return dark_theme_; }
  bool checkerboard() const { return checkerboard_; }
  bool high_contrast() const { return high_contrast_; }

  // Background color as a GDI COLORREF, for the fast first paint before the
  // device exists (must match DrawFrame's Clear color exactly).
  COLORREF background_colorref() const;

  // View commands. Coordinates are client pixels.
  void FitToWindow();
  void ActualSize();

  // View rotation (規劃/22/05): display-only quarter turns, never written back
  // to the source file and never re-decoded. `steps` is normalised to 0..3 and
  // counts clockwise quarter turns.
  void RotateLeft();
  void RotateRight();
  void ResetRotation();
  void SetRotation(int steps);
  int rotation() const { return rotation_; }
  bool rotated_90() const;
  // Effective size used by fit/zoom: source size with width/height swapped for
  // odd rotations. image_width()/image_height() keep reporting the source.
  uint32_t display_width() const;
  uint32_t display_height() const;
  // Switches to a manual scale, centred on the current view so the image does
  // not jump (zoom menu: 50/100/200/400%).
  void SetZoom(float zoom);
  // Keyboard zoom (+/-): centred on the content, clamped to the derived range
  // (規劃/22/09 §6).
  void ZoomBy(float factor);
  void ZoomAt(float client_x, float client_y, int wheel_delta);
  void PanBy(float dx, float dy);
  // V3.1-06: brings the picture back to the middle of the content rectangle
  // without changing the zoom. Used when a pan press turns into a drag-out: the
  // file is leaving the window, and the half-panned frame behind it must not be
  // what the user comes back to.
  void CenterView();

  float zoom() const { return fit_ ? EffectiveScale() : zoom_; }
  bool fit() const { return fit_; }
  // Scale the image would use in fit mode, independent of the current mode.
  float FitScale() const;
  // Lower bound: min(10%, fit scale) so an 8K image may fit below 10%.
  float min_zoom() const;
  float max_zoom() const;
  uint32_t image_width() const { return image_width_; }
  uint32_t image_height() const { return image_height_; }
  // Logical (file) size of the current picture, upright. A display tier or a
  // two-stage preview carries the real file size in ImageInfo::full_*, while its
  // bitmap is smaller; zoom, fit, pan and the destination rectangle are all
  // expressed against these logical pixels and the bitmap is *stretched* into
  // that rectangle (= file pixels x zoom). Replacing the bitmap (tier -> full
  // frame)
  // therefore cannot move or resize the picture: only its sharpness changes.
  uint32_t logical_width() const { return logical_width_; }
  uint32_t logical_height() const { return logical_height_; }
  uint32_t client_width() const { return client_width_; }
  uint32_t client_height() const { return client_height_; }
  D2D1_RECT_F ContentRect() const;
  // Client-pixel centre of the displayed image. Two-stage loading uses it (and
  // the tests assert it) to prove that replacing a preview with the full frame
  // keeps the same picture point under the content centre (規劃/22/13 §3.4).
  void ViewCenter(float* x, float* y) const { ImageCenter(x, y); }

 private:
  bool CreateSwapChain();
  bool CreateTargetBitmap();
  void ReleaseTargetBitmap();
  void ReleaseDeviceResources();
  bool CreateCheckerBrush();
  bool DrawFrame();
  // Draws one frame with an explicit quality instead of the automatic policy;
  // --screenshot uses this to force kGood (規劃/22/18 §3.5).
  bool DrawFrameWithQuality(DrawQuality quality);
  bool CopyBackBuffer(std::vector<uint8_t>* bgra, uint32_t* width,
                      uint32_t* height);
  // Lazily bakes the 1/4-size proxy of the current frame (GPU, one pass).
  // Failure is not fatal: the original bitmap keeps being drawn.
  bool EnsureProxy();
  void ReleaseProxy();
  // Bird's eye bake (v3.0): a one-pass downscale to <= kMinimapBakeEdge.
  bool EnsureMinimap();
  void ReleaseMinimap();

  float EffectiveScale() const;
  float ComputeFitScale() const;
  void EffectiveOffset(float* x, float* y) const;
  // Centre of the unrotated image rectangle in client pixels; the view
  // rotation and the wheel-zoom inverse both pivot around it.
  void ImageCenter(float* x, float* y) const;
  void EnsureManualView();
  void DrawCheckerboard(const D2D1_RECT_F& rect);
  D2D1_COLOR_F BackgroundColor() const;
  uint32_t CheckerColor(bool light) const;

  HWND hwnd_ = nullptr;
  uint32_t client_width_ = 0;
  uint32_t client_height_ = 0;
  float bottom_inset_ = 0.0f;
  float top_inset_ = 0.0f;
  OverlayFn overlay_;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;
  Microsoft::WRL::ComPtr<ID2D1Device> d2d_device_;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_context_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_bitmap_;

  Microsoft::WRL::ComPtr<ID2D1Bitmap> image_bitmap_;
  // 1/4-size copy of a very large frame, baked on the GPU the first time it is
  // drawn at < 50 % (規劃/22/18 §3.3). Never exists in pixel mode.
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> proxy_bitmap_;
  uint32_t proxy_width_ = 0;
  uint32_t proxy_height_ = 0;
  bool proxy_baked_ = false;
  bool proxy_active_ = false;
  bool proxy_enabled_ = true;
  bool proxy_logged_ = false;
  bool source_is_tier_ = false;
  bool proxy_tier_logged_ = false;
  // Bird's eye photo (v3.0, 規劃/26 §2.4).
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> minimap_bitmap_;
  uint32_t minimap_width_ = 0;
  uint32_t minimap_height_ = 0;
  bool minimap_baked_ = false;
  bool minimap_enabled_ = false;
  // Logged once per baked size, not once per animation frame (every frame
  // re-bakes, but a 30 fps GIF must not flood the log with the same line).
  bool minimap_logged_ = false;
  uint32_t minimap_logged_width_ = 0;
  uint32_t minimap_logged_height_ = 0;
  Microsoft::WRL::ComPtr<ID2D1Bitmap> checker_bitmap_;
  Microsoft::WRL::ComPtr<ID2D1BitmapBrush> checker_brush_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background_brush_;

  uint32_t image_width_ = 0;
  uint32_t image_height_ = 0;
  // Upright file size (ImageInfo::full_* when the bitmap is a tier/preview,
  // otherwise the bitmap size). Geometry uses these; bitmap-sized operations
  // (proxy bake, minimap bake, bitmap reuse) keep using image_width_/height_.
  uint32_t logical_width_ = 0;
  uint32_t logical_height_ = 0;
  bool image_has_alpha_ = false;

  bool fit_ = true;
  float zoom_ = 1.0f;
  float offset_x_ = 0.0f;
  float offset_y_ = 0.0f;
  // Clockwise quarter turns (0..3). Not part of ImageInfo: the source stays
  // upright, only the view is rotated.
  int rotation_ = 0;

  bool dark_theme_ = true;
  bool checkerboard_ = true;
  bool high_contrast_ = false;
  int checker_opacity_ = 100;
  bool pixel_mode_ = false;
  bool animating_ = false;
  int forced_quality_ = kQualityAuto;
  DrawQuality last_drawn_quality_ = DrawQuality::kGood;
  bool last_proxy_active_ = false;
  bool quality_logged_ = false;
  // Timestamp (QPC ms) until which the view stays interactive; 0 means the
  // last interaction already expired.
  double interactive_until_ms_ = 0.0;
  bool fullscreen_ = false;
  int fullscreen_background_ = 0;
};

}  // namespace nv::render
