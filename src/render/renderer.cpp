#include "render/renderer.h"

#include "platform/log.h"
#include "render/palette.h"
#include "render/view_transform.h"
#include "render/zoom_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv::render {
namespace {

// Temporary/always-on diagnostics for M1-2 startup work: each device-init step
// is logged so a slow driver or DLL load is visible without a profiler.
double QpcMs() {
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

void LogInitStep(const char* name, double start_ms) {
  char line[96]{};
  std::snprintf(line, sizeof(line), "renderer: %s=%.1fms", name,
                QpcMs() - start_ms);
  nv::platform::LogInfo(line);
}

void PremultiplyBgra(uint8_t* pixels, size_t count) {
  for (size_t i = 0; i + 3 < count; i += 4) {
    const uint32_t alpha = pixels[i + 3];
    if (alpha == 255) continue;
    pixels[i + 0] = static_cast<uint8_t>((pixels[i + 0] * alpha + 127) / 255);
    pixels[i + 1] = static_cast<uint8_t>((pixels[i + 1] * alpha + 127) / 255);
    pixels[i + 2] = static_cast<uint8_t>((pixels[i + 2] * alpha + 127) / 255);
  }
}

}  // namespace

Renderer::Renderer() = default;

Renderer::~Renderer() {
  Shutdown();
}

bool Renderer::Initialize(HWND hwnd) {
  return Initialize(hwnd, CreateDeviceBundle());
}

bool Renderer::Initialize(HWND hwnd, std::unique_ptr<DeviceBundle> bundle) {
  Shutdown();
  hwnd_ = hwnd;

  RECT client{};
  GetClientRect(hwnd_, &client);
  client_width_ = std::max<LONG>(1, client.right - client.left);
  client_height_ = std::max<LONG>(1, client.bottom - client.top);

  if (!bundle || !bundle->device || !bundle->d2d_context) return false;
  d3d_device_ = std::move(bundle->device);
  d3d_context_ = std::move(bundle->context);
  d2d_factory_ = std::move(bundle->d2d_factory);
  d2d_device_ = std::move(bundle->d2d_device);
  d2d_context_ = std::move(bundle->d2d_context);

  double step = QpcMs();
  if (!CreateSwapChain()) return false;
  LogInitStep("swapchain", step); step = QpcMs();

  if (!CreateTargetBitmap()) return false;
  LogInitStep("target", step); step = QpcMs();

  if (FAILED(d2d_context_->CreateSolidColorBrush(
          BackgroundColor(), background_brush_.GetAddressOf()))) {
    return false;
  }
  if (!CreateCheckerBrush()) return false;
  LogInitStep("brush", step);

  return true;
}

void Renderer::Shutdown() {
  if (hwnd_ != nullptr) KillTimer(hwnd_, kQualityUpgradeTimerId);
  interactive_until_ms_ = 0.0;
  ClearImage();
  overlay_ = nullptr;
  bottom_inset_ = 0.0f;
  top_inset_ = 0.0f;
  background_brush_.Reset();
  checker_brush_.Reset();
  checker_bitmap_.Reset();
  ReleaseTargetBitmap();
  d2d_context_.Reset();
  d2d_device_.Reset();
  d2d_factory_.Reset();
  swap_chain_.Reset();
  d3d_context_.Reset();
  d3d_device_.Reset();
  hwnd_ = nullptr;
}

std::unique_ptr<Renderer::DeviceBundle> Renderer::CreateDeviceBundle() {
  auto bundle = std::make_unique<DeviceBundle>();

  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

  double step = QpcMs();
  D3D_FEATURE_LEVEL level = static_cast<D3D_FEATURE_LEVEL>(0);
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
      static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
      bundle->device.GetAddressOf(), &level, bundle->context.GetAddressOf());
  LogInitStep("d3d_hardware", step);
  char detail[160]{};
  std::snprintf(detail, sizeof(detail),
                "renderer: device_hr=0x%08lX level=0x%X",
                static_cast<unsigned long>(hr), static_cast<unsigned>(level));
  nv::platform::LogInfo(detail);

  if (FAILED(hr)) {
    step = QpcMs();
    level = static_cast<D3D_FEATURE_LEVEL>(0);
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        bundle->device.GetAddressOf(), &level, bundle->context.GetAddressOf());
    LogInitStep("d3d_warp", step);
  }
  if (FAILED(hr)) return nullptr;

  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  if (SUCCEEDED(bundle->device.As(&dxgi_device)) &&
      SUCCEEDED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) {
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
      char name[128]{};
      std::snprintf(name, sizeof(name), "renderer: adapter=%ls vram=%lluMB",
                    desc.Description,
                    static_cast<unsigned long long>(desc.DedicatedVideoMemory /
                                                     (1024ull * 1024ull)));
      nv::platform::LogInfo(name);
    }
  }

  step = QpcMs();
  D2D1_FACTORY_OPTIONS options{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void**>(
                                   bundle->d2d_factory.GetAddressOf())))) {
    return nullptr;
  }
  LogInitStep("d2d_factory", step); step = QpcMs();

  if (FAILED(bundle->device.As(&dxgi_device))) return nullptr;
  if (FAILED(bundle->d2d_factory->CreateDevice(
          dxgi_device.Get(), bundle->d2d_device.GetAddressOf()))) {
    return nullptr;
  }
  if (FAILED(bundle->d2d_device->CreateDeviceContext(
          D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
          bundle->d2d_context.GetAddressOf()))) {
    return nullptr;
  }
  bundle->d2d_context->SetDpi(96.0f, 96.0f);
  bundle->d2d_context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  LogInitStep("d2d_device", step);

  return bundle;
}

bool Renderer::CreateSwapChain() {
  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(d3d_device_.As(&dxgi_device))) return false;

  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) return false;

  ComPtr<IDXGIFactory2> factory;
  if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return false;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = client_width_;
  desc.Height = client_height_;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = 0;

  if (FAILED(factory->CreateSwapChainForHwnd(
          d3d_device_.Get(), hwnd_, &desc, nullptr, nullptr,
          swap_chain_.GetAddressOf()))) {
    return false;
  }

  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  return true;
}

bool Renderer::CreateTargetBitmap() {
  ComPtr<IDXGISurface> surface;
  if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf())))) {
    return false;
  }

  const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
      96.0f, 96.0f);

  if (FAILED(d2d_context_->CreateBitmapFromDxgiSurface(
          surface.Get(), &props, target_bitmap_.GetAddressOf()))) {
    return false;
  }

  d2d_context_->SetTarget(target_bitmap_.Get());
  return true;
}

void Renderer::ReleaseTargetBitmap() {
  if (d2d_context_) d2d_context_->SetTarget(nullptr);
  target_bitmap_.Reset();
}

bool Renderer::CreateCheckerBrush() {
  constexpr uint32_t kSize = 16;
  constexpr uint32_t kCell = 8;
  std::array<uint32_t, kSize * kSize> pixels{};
  for (uint32_t y = 0; y < kSize; ++y) {
    for (uint32_t x = 0; x < kSize; ++x) {
      const bool light = ((x / kCell) + (y / kCell)) % 2 == 0;
      pixels[y * kSize + x] = CheckerColor(light);
    }
  }

  const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
      96.0f, 96.0f);
  if (FAILED(d2d_context_->CreateBitmap(D2D1::SizeU(kSize, kSize), pixels.data(),
                                        kSize * 4, &props,
                                        checker_bitmap_.GetAddressOf()))) {
    return false;
  }

  const D2D1_BITMAP_BRUSH_PROPERTIES brush_props = D2D1::BitmapBrushProperties(
      D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
      D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
  return SUCCEEDED(d2d_context_->CreateBitmapBrush(
      checker_bitmap_.Get(), &brush_props, nullptr,
      checker_brush_.GetAddressOf()));
}

bool Renderer::SetImage(const Image& image, bool reset_view,
                        bool preserve_view) {
  return SetImage(image.info, image.pixels, reset_view, preserve_view);
}

bool Renderer::SetImage(const ImageInfo& info,
                        const std::vector<uint8_t>& image_pixels,
                        bool reset_view, bool preserve_view) {
  if (image_pixels.empty() || info.width == 0 || info.height == 0 ||
      info.format == PixelFormat::kUnknown || d2d_context_ == nullptr) {
    return false;
  }

  // Capture the picture point currently under the centre of the content area
  // before the dimensions change. Only a manual view needs it: in fit mode the
  // new scale is derived from the new size and the picture stays centred
  // (規劃/22/13 §3.4). The point is stored as a fraction of the *logical* size,
  // so a tier/preview being replaced by the full frame lands on the same spot
  // (and, because both carry the same file size, the offsets do not move at all).
  bool remap = false;
  float anchor_x = 0.0f;
  float anchor_y = 0.0f;
  float anchor_fx = 0.0f;
  float anchor_fy = 0.0f;
  if (!reset_view && preserve_view && image_bitmap_ && logical_width_ > 0 &&
      logical_height_ > 0 && !fit_ && zoom_ > 0.0f) {
    const D2D1_RECT_F content = ContentRect();
    anchor_x = (content.left + content.right) * 0.5f;
    anchor_y = (content.top + content.bottom) * 0.5f;
    anchor_fx = (anchor_x - offset_x_) /
                (static_cast<float>(logical_width_) * zoom_);
    anchor_fy = (anchor_y - offset_y_) /
                (static_cast<float>(logical_height_) * zoom_);
    remap = true;
  }
  // V3.1-08: geometry follows the *file* size. A tier or a two-stage preview
  // reports it through ImageInfo::full_*, so every frame of one file shares the
  // same logical rectangle and swapping the bitmap cannot move the picture.
  const uint32_t new_logical_width =
      info.full_width != 0 ? info.full_width : info.width;
  const uint32_t new_logical_height =
      info.full_height != 0 ? info.full_height : info.height;

  // Only alpha images need a premultiplied copy; for the common opaque case
  // this avoids a full-frame memcpy (33 MB for 4K) on every image change.
  ReleaseProxy();
  // v3.0 (規劃/26 §2.4): the bird's eye copy belongs to the previous frame; a
  // navigation/animation frame always re-bakes it before the next draw.
  ReleaseMinimap();
  const uint8_t* pixels = image_pixels.data();
  std::vector<uint8_t> premultiplied;
  if (info.has_alpha) {
    premultiplied = image_pixels;
    PremultiplyBgra(premultiplied.data(), premultiplied.size());
    pixels = premultiplied.data();
  }

  const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.0f, 96.0f);

  // Release the previous texture before allocating the new one. With 8K frames
  // the old and new bitmaps are 127 MB each, and keeping both alive during the
  // upload pushed the peak well above the 8K memory budget.
  const double upload_start = QpcMs();
  if (image_bitmap_ && image_width_ == info.width &&
      image_height_ == info.height) {
    // Reusing the bitmap avoids a second 127 MB D2D allocation on every
    // navigation. Recreating it left the previous allocation pending in the
    // driver and drove peak private bytes to ~685 MB on consecutive 8K frames.
    const D2D1_RECT_U rect =
        D2D1::RectU(0, 0, info.width, info.height);
    if (SUCCEEDED(
            image_bitmap_->CopyFromMemory(&rect, pixels, info.stride))) {
      LogInitStep("copy_image", upload_start);
      image_has_alpha_ = info.has_alpha;
      logical_width_ = new_logical_width;
      logical_height_ = new_logical_height;
      if (reset_view) {
        fit_ = true;
        zoom_ = 1.0f;
        offset_x_ = 0.0f;
        offset_y_ = 0.0f;
      } else if (remap) {
        offset_x_ = anchor_x -
                    anchor_fx * static_cast<float>(logical_width_) * zoom_;
        offset_y_ = anchor_y -
                    anchor_fy * static_cast<float>(logical_height_) * zoom_;
      }
      return true;
    }
  }

  image_bitmap_.Reset();

  ComPtr<ID2D1Bitmap> bitmap;
  if (FAILED(d2d_context_->CreateBitmap(
          D2D1::SizeU(info.width, info.height), pixels,
          info.stride, &props, bitmap.GetAddressOf()))) {
    return false;
  }
  LogInitStep("set_image", upload_start);

  {
    const double mb = static_cast<double>(info.stride) *
                      static_cast<double>(info.height) /
                      (1024.0 * 1024.0);
    char line[128]{};
    std::snprintf(line, sizeof(line), "renderer: bitmap %ux%u bgra=%.1fMB",
                  info.width, info.height, mb);
    nv::platform::LogInfo(line);
  }

  image_bitmap_ = std::move(bitmap);
  image_width_ = info.width;
  image_height_ = info.height;
  logical_width_ = new_logical_width;
  logical_height_ = new_logical_height;
  image_has_alpha_ = info.has_alpha;
  if (reset_view) {
    fit_ = true;
    zoom_ = 1.0f;
    offset_x_ = 0.0f;
    offset_y_ = 0.0f;
  } else if (remap) {
    // Keep the zoom and put the same picture point back under the centre. The
    // rotation is untouched: the view spin is orthogonal to the replacement
    // (規劃/22/13 坑 13). With the logical size unchanged (tier -> full frame of
    // the same file) this is a no-op, which is exactly the "no jump" guarantee.
    offset_x_ = anchor_x -
                anchor_fx * static_cast<float>(logical_width_) * zoom_;
    offset_y_ = anchor_y -
                anchor_fy * static_cast<float>(logical_height_) * zoom_;
  }
  return true;
}

void Renderer::ClearImage() {
  ReleaseProxy();
  ReleaseMinimap();
  image_bitmap_.Reset();
  image_width_ = 0;
  image_height_ = 0;
  logical_width_ = 0;
  logical_height_ = 0;
  image_has_alpha_ = false;
  // The empty state has no view; a fresh image starts upright even when
  // keep_rotation is on (there is nothing to keep across the empty state).
  rotation_ = 0;
}

void Renderer::Resize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) return;
  client_width_ = width;
  client_height_ = height;
  if (!swap_chain_ || !d2d_context_) return;

  // A resize is an interaction: keep linear while the user drags the window,
  // then upgrade once the size settles (規劃/22/18 §3.2).
  MarkInteractive();
  ReleaseTargetBitmap();
  if (FAILED(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
    return;
  }
  CreateTargetBitmap();
}

void Renderer::SetBottomInset(float inset) {
  const float limit = static_cast<float>(client_height_) * 0.5f;
  bottom_inset_ = std::clamp(inset, 0.0f, std::max(0.0f, limit));
}

void Renderer::SetTopInset(float inset) {
  const float limit = static_cast<float>(client_height_) * 0.5f;
  top_inset_ = std::clamp(inset, 0.0f, std::max(0.0f, limit));
}

D2D1_RECT_F Renderer::ContentRect() const {
  const float top = top_inset_;
  return D2D1::RectF(0.0f, top, static_cast<float>(client_width_),
                     std::max(top + 1.0f,
                              static_cast<float>(client_height_) -
                                  bottom_inset_));
}

bool Renderer::Render() {
  if (!DrawFrame()) return false;
  // Present without waiting for vblank: DWM composes the flip-model swap chain,
  // so this does not tear, and waiting would add up to a refresh interval to
  // the first frame and to every interactive repaint.
  return SUCCEEDED(swap_chain_->Present(0, 0));
}

bool Renderer::RenderAndCapture(std::vector<uint8_t>* bgra, uint32_t* width,
                                uint32_t* height) {
  // Screenshots must match the final static visual, not whatever interaction
  // timer happens to be running (規劃/22/18 §3.5). That is the automatic policy
  // with interaction and animation treated as finished, so pixel mode still
  // captures nearest-neighbour blocks. --force-quality wins so a script can
  // capture the fast path deliberately.
  DrawQuality quality = DrawQuality::kGood;
  if (forced_quality_ != kQualityAuto) {
    quality = DrawQualityFromValue(forced_quality_);
  } else {
    DrawQualityInputs inputs;
    inputs.pixel_mode = pixel_mode_;
    inputs.interactive = false;
    inputs.animating = false;
    inputs.scale = EffectiveScale();
    quality = SelectDrawQuality(inputs);
  }
  if (!DrawFrameWithQuality(quality)) return false;
  const bool captured = CopyBackBuffer(bgra, width, height);
  swap_chain_->Present(1, 0);
  return captured;
}

DrawQuality Renderer::quality() const {
  if (forced_quality_ != kQualityAuto) {
    return DrawQualityFromValue(forced_quality_);
  }
  DrawQualityInputs inputs;
  inputs.pixel_mode = pixel_mode_;
  inputs.interactive =
      interactive_until_ms_ > 0.0 && QpcMs() < interactive_until_ms_;
  inputs.animating = animating_;
  inputs.scale = EffectiveScale();
  return SelectDrawQuality(inputs);
}

void Renderer::MarkInteractive() {
  interactive_until_ms_ = QpcMs() + static_cast<double>(kQualityUpgradeMs);
  // One-shot upgrade timer. Re-arming it on every move would consume a message
  // per pixel; the timer only decides the transition back to cubic, and the
  // timestamp above decides when that is allowed.
  if (hwnd_ != nullptr) {
    SetTimer(hwnd_, kQualityUpgradeTimerId,
             static_cast<UINT>(kQualityUpgradeMs + 16), nullptr);
  }
}

bool Renderer::interactive() const {
  return interactive_until_ms_ > 0.0 && QpcMs() < interactive_until_ms_;
}

int Renderer::interactive_remaining_ms() const {
  if (interactive_until_ms_ <= 0.0) return 0;
  const double remaining = interactive_until_ms_ - QpcMs();
  return remaining > 0.0 ? static_cast<int>(remaining) : 0;
}

void Renderer::SetAnimating(bool animating) { animating_ = animating; }

void Renderer::SetForcedQuality(int quality) {
  const int normalized =
      quality == kQualityAuto ? kQualityAuto
                              : DrawQualityValue(
                                    DrawQualityFromValue(quality));
  if (forced_quality_ == normalized) return;
  forced_quality_ = normalized;
  // A forced mode must be visible on the very next frame.
  if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::SetProxyEnabled(bool enabled) {
  if (proxy_enabled_ == enabled) return;
  proxy_enabled_ = enabled;
  if (!enabled) ReleaseProxy();
  if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::SetSourceIsTier(bool tier) {
  if (source_is_tier_ == tier) return;
  source_is_tier_ = tier;
  proxy_tier_logged_ = false;
  // A proxy baked from the previous (full) frame is not a copy of this one.
  // §7.6 upgrades and tier hits both go through SetImage, which already
  // releases it; this keeps the invariant for any direct setter call too.
  if (tier) ReleaseProxy();
  if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Renderer::ReleaseProxy() {
  proxy_bitmap_.Reset();
  proxy_width_ = 0;
  proxy_height_ = 0;
  proxy_baked_ = false;
  proxy_active_ = false;
  proxy_logged_ = false;
  proxy_tier_logged_ = false;
}

void Renderer::SetMinimapEnabled(bool enabled) {
  if (minimap_enabled_ == enabled) return;
  minimap_enabled_ = enabled;
  // Free the derived copy as soon as the panel is off; the next enable bakes
  // again on the next frame.
  if (!enabled) ReleaseMinimap();
}

void Renderer::ReleaseMinimap() {
  minimap_bitmap_.Reset();
  minimap_width_ = 0;
  minimap_height_ = 0;
  minimap_baked_ = false;
}

ID2D1Bitmap* Renderer::minimap_photo() {
  if (!minimap_enabled_ || !image_bitmap_ || image_width_ == 0 ||
      image_height_ == 0) {
    return nullptr;
  }
  // A picture that already fits the bake budget needs no copy at all.
  if (std::max(image_width_, image_height_) <= kMinimapBakeEdge) {
    return image_bitmap_.Get();
  }
  return minimap_baked_ ? minimap_bitmap_.Get() : image_bitmap_.Get();
}

bool Renderer::EnsureMinimap() {
  if (!minimap_enabled_ || !d2d_context_ || !image_bitmap_ ||
      image_width_ == 0 || image_height_ == 0) {
    return false;
  }
  const uint32_t edge = std::max(image_width_, image_height_);
  if (edge <= kMinimapBakeEdge) return false;

  const float factor =
      static_cast<float>(kMinimapBakeEdge) / static_cast<float>(edge);
  const uint32_t width =
      std::max<uint32_t>(1, static_cast<uint32_t>(
                                std::lround(image_width_ * factor)));
  const uint32_t height =
      std::max<uint32_t>(1, static_cast<uint32_t>(
                                std::lround(image_height_ * factor)));

  if (!minimap_bitmap_ || minimap_width_ != width ||
      minimap_height_ != height) {
    minimap_bitmap_.Reset();
    minimap_baked_ = false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (FAILED(d2d_context_->CreateBitmap(
            D2D1::SizeU(width, height), nullptr, 0, &props,
            minimap_bitmap_.GetAddressOf()))) {
      return false;
    }
    minimap_width_ = width;
    minimap_height_ = height;
  }
  if (minimap_baked_) return true;

  const double start = QpcMs();
  d2d_context_->SetTarget(minimap_bitmap_.Get());
  d2d_context_->BeginDraw();
  d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
  d2d_context_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  d2d_context_->DrawBitmap(
      image_bitmap_.Get(),
      D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                  static_cast<float>(height)),
      1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
  const bool baked = SUCCEEDED(d2d_context_->EndDraw());
  d2d_context_->SetTarget(target_bitmap_.Get());

  if (!baked) {
    ReleaseMinimap();
    nv::platform::LogError("render: minimap bake failed");
    return false;
  }
  minimap_baked_ = true;
  if (!minimap_logged_ || minimap_logged_width_ != width ||
      minimap_logged_height_ != height) {
    minimap_logged_ = true;
    minimap_logged_width_ = width;
    minimap_logged_height_ = height;
    char line[128]{};
    std::snprintf(line, sizeof(line),
                  "renderer: minimap %ux%u bgra=%.1fMB bake=%.1fms", width,
                  height,
                  static_cast<double>(width) * static_cast<double>(height) *
                      4.0 / (1024.0 * 1024.0),
                  QpcMs() - start);
    nv::platform::LogInfo(line);
  }
  return true;
}

bool Renderer::EnsureProxy() {
  if (!d2d_context_ || !image_bitmap_ || image_width_ == 0 ||
      image_height_ == 0) {
    return false;
  }
  const uint32_t width = ProxyEdge(image_width_);
  const uint32_t height = ProxyEdge(image_height_);
  if (width < 2 || height < 2) return false;

  if (!proxy_bitmap_ || proxy_width_ != width || proxy_height_ != height) {
    proxy_bitmap_.Reset();
    proxy_baked_ = false;
    proxy_logged_ = false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (FAILED(d2d_context_->CreateBitmap(
            D2D1::SizeU(width, height), nullptr, 0, &props,
            proxy_bitmap_.GetAddressOf()))) {
      return false;
    }
    proxy_width_ = width;
    proxy_height_ = height;
  }
  if (proxy_baked_) return true;

  // Bake on the GPU: the source never leaves the device, and the pass costs
  // ~8 MB of output instead of a second full-frame texture (規劃/22/18 §3.3).
  const double start = QpcMs();
  d2d_context_->SetTarget(proxy_bitmap_.Get());
  d2d_context_->BeginDraw();
  d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
  d2d_context_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  d2d_context_->DrawBitmap(
      image_bitmap_.Get(),
      D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                  static_cast<float>(height)),
      1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
  const bool baked = SUCCEEDED(d2d_context_->EndDraw());
  d2d_context_->SetTarget(target_bitmap_.Get());

  if (!baked) {
    ReleaseProxy();
    nv::platform::LogError("render: proxy bake failed");
    return false;
  }
  proxy_baked_ = true;
  if (!proxy_logged_) {
    proxy_logged_ = true;
    char line[128]{};
    std::snprintf(line, sizeof(line),
                  "renderer: proxy %ux%u bgra=%.1fMB bake=%.1fms", width,
                  height,
                  static_cast<double>(width) * static_cast<double>(height) *
                      4.0 / (1024.0 * 1024.0),
                  QpcMs() - start);
    nv::platform::LogInfo(line);
  }
  return true;
}

bool Renderer::DrawFrame() { return DrawFrameWithQuality(quality()); }

bool Renderer::DrawFrameWithQuality(DrawQuality quality) {
  if (!d2d_context_ || !swap_chain_ || !target_bitmap_) return false;

  const D2D1_RECT_F content = ContentRect();
  const float scale = EffectiveScale();

  // The proxy is baked before BeginDraw because it re-targets the same device
  // context. Baking happens on the first eligible static frame; the result is
  // then used only by interactive frames (see below), so the static picture
  // always keeps the direct cubic quality (measured: direct cubic has a lower
  // error against a Lanczos reference than proxy + cubic).
  ProxyInputs proxy_inputs;
  proxy_inputs.image_width = image_width_;
  proxy_inputs.image_height = image_height_;
  proxy_inputs.content_width = static_cast<uint32_t>(
      std::max(0.0f, content.right - content.left));
  proxy_inputs.content_height = static_cast<uint32_t>(
      std::max(0.0f, content.bottom - content.top));
  proxy_inputs.scale = scale;
  proxy_inputs.pixel_mode = quality == DrawQuality::kPixel;
  proxy_inputs.enabled = proxy_enabled_;
  proxy_inputs.source_is_tier = source_is_tier_;
  const bool proxy_candidate = image_bitmap_ && ShouldUseProxy(proxy_inputs);
  if (proxy_candidate && !proxy_baked_) EnsureProxy();
  // §7.7 evidence: a tier source at < 50 % is the one case where the old policy
  // would have baked a 1/4 copy of an already-small frame. Log it once per
  // frame change so a test can prove the flag reached the renderer.
  if (source_is_tier_ && !proxy_tier_logged_ && scale < 0.5f) {
    proxy_tier_logged_ = true;
    nv::platform::LogInfo("renderer: proxy skipped (display tier source)");
  }

  // v3.0 (規劃/26 §2.4): the bird's eye copy is baked here, before BeginDraw,
  // because it re-targets this same device context. The overlay below only ever
  // reads the finished bitmap.
  if (minimap_enabled_ && !minimap_baked_) EnsureMinimap();

  // The 1/4 proxy exists to make pan/zoom and animation frames cheaper; a
  // static frame draws the original with cubic because that is measurably
  // sharper than a second resample of the proxy.
  const bool use_proxy =
      proxy_candidate && proxy_baked_ && quality == DrawQuality::kFast;
  ID2D1Bitmap* source = image_bitmap_.Get();
  if (use_proxy) source = proxy_bitmap_.Get();
  proxy_active_ = use_proxy;

  const D2D1_INTERPOLATION_MODE interpolation =
      quality == DrawQuality::kPixel
          ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
          : (quality == DrawQuality::kGood
                 ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                 : D2D1_INTERPOLATION_MODE_LINEAR);

  if (!quality_logged_ || quality != last_drawn_quality_ ||
      proxy_active_ != last_proxy_active_) {
    quality_logged_ = true;
    char line[128]{};
    std::snprintf(line, sizeof(line),
                  "render: quality=%s proxy=%d scale=%.3f",
                  DrawQualityName(quality), proxy_active_ ? 1 : 0,
                  static_cast<double>(scale));
    nv::platform::LogInfo(line);
  }
  last_drawn_quality_ = quality;
  last_proxy_active_ = proxy_active_;

  d2d_context_->BeginDraw();
  d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
  d2d_context_->Clear(BackgroundColor());

  if (image_bitmap_ && image_width_ > 0 && image_height_ > 0) {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    EffectiveOffset(&offset_x, &offset_y);

    const D2D1_RECT_F dest = D2D1::RectF(
        offset_x, offset_y,
        offset_x + static_cast<float>(logical_width_) * scale,
        offset_y + static_cast<float>(logical_height_) * scale);

    d2d_context_->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_ALIASED);
    // Push the clip while the transform is identity: the content rectangle is
    // in client pixels and must stay axis-aligned on screen even when the
    // bitmap below is rotated (規劃/22/05 §3.3).
    if (rotation_ != 0) {
      const float cx = (dest.left + dest.right) * 0.5f;
      const float cy = (dest.top + dest.bottom) * 0.5f;
      const ViewMatrix matrix = RotationAbout(cx, cy, rotation_);
      d2d_context_->SetTransform(
          D2D1::Matrix3x2F(matrix.m11, matrix.m12, matrix.m21, matrix.m22,
                           matrix.dx, matrix.dy));
    }
    if (image_has_alpha_ && checkerboard_ && !high_contrast_) {
      DrawCheckerboard(dest);
    }
    d2d_context_->DrawBitmap(source, &dest, 1.0f, interpolation, nullptr);
    // Reset before the overlay: the title/bottom bars and arrows are screen
    // space UI and must never rotate with the picture (規劃/22/05 坑 1).
    if (rotation_ != 0) {
      d2d_context_->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    d2d_context_->PopAxisAlignedClip();
  }

  if (overlay_) {
    // Fixed layer order: image-area overlays, bottom panels, top panels, then
    // the title bar last so content can never cover it (規劃/22/01 §3.1).
    overlay_(d2d_context_.Get(), content, dark_theme_, high_contrast_,
             OverlayLayer::kImageArea);
    overlay_(d2d_context_.Get(), content, dark_theme_, high_contrast_,
             OverlayLayer::kBottomPanels);
    overlay_(d2d_context_.Get(), content, dark_theme_, high_contrast_,
             OverlayLayer::kTopPanels);
    overlay_(d2d_context_.Get(), content, dark_theme_, high_contrast_,
             OverlayLayer::kChrome);
  }

  return SUCCEEDED(d2d_context_->EndDraw());
}

bool Renderer::CopyBackBuffer(std::vector<uint8_t>* bgra, uint32_t* width,
                              uint32_t* height) {
  if (bgra == nullptr || width == nullptr || height == nullptr) return false;

  ComPtr<ID3D11Texture2D> back_buffer;
  if (FAILED(swap_chain_->GetBuffer(
          0, IID_PPV_ARGS(back_buffer.GetAddressOf())))) {
    return false;
  }

  D3D11_TEXTURE2D_DESC desc{};
  back_buffer->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  desc.MipLevels = 1;
  desc.ArraySize = 1;

  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(d3d_device_->CreateTexture2D(&desc, nullptr,
                                          staging.GetAddressOf()))) {
    return false;
  }

  d3d_context_->CopyResource(staging.Get(), back_buffer.Get());

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(d3d_context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }

  const uint32_t row_bytes = desc.Width * 4u;
  bgra->resize(static_cast<size_t>(row_bytes) * desc.Height);
  for (uint32_t y = 0; y < desc.Height; ++y) {
    std::memcpy(bgra->data() + static_cast<size_t>(y) * row_bytes,
                static_cast<const uint8_t*>(mapped.pData) +
                    static_cast<size_t>(y) * mapped.RowPitch,
                row_bytes);
  }
  d3d_context_->Unmap(staging.Get(), 0);

  *width = desc.Width;
  *height = desc.Height;
  return true;
}

void Renderer::SetTheme(bool dark) {
  if (dark_theme_ == dark) return;
  dark_theme_ = dark;
  if (!d2d_context_) return;

  background_brush_.Reset();
  d2d_context_->CreateSolidColorBrush(BackgroundColor(),
                                      background_brush_.GetAddressOf());

  checker_brush_.Reset();
  checker_bitmap_.Reset();
  CreateCheckerBrush();
}

void Renderer::SetCheckerboard(bool enabled) {
  checkerboard_ = enabled;
}

void Renderer::SetCheckerOpacity(int percent) {
  const int clamped = std::clamp(percent, 0, 100);
  if (checker_opacity_ == clamped) return;
  checker_opacity_ = clamped;
  if (!d2d_context_) return;
  checker_brush_.Reset();
  checker_bitmap_.Reset();
  CreateCheckerBrush();
}

void Renderer::SetPixelMode(bool nearest) { pixel_mode_ = nearest; }

void Renderer::SetFullscreen(bool fullscreen) {
  if (fullscreen_ == fullscreen) return;
  fullscreen_ = fullscreen;
  if (!d2d_context_) return;
  background_brush_.Reset();
  d2d_context_->CreateSolidColorBrush(BackgroundColor(),
                                      background_brush_.GetAddressOf());
}

void Renderer::SetFullscreenBackground(int mode) {
  const int clamped = std::clamp(mode, 0, 2);
  if (fullscreen_background_ == clamped) return;
  fullscreen_background_ = clamped;
  if (!d2d_context_ || !fullscreen_) return;
  background_brush_.Reset();
  d2d_context_->CreateSolidColorBrush(BackgroundColor(),
                                      background_brush_.GetAddressOf());
}

void Renderer::SetHighContrast(bool enabled) {
  if (high_contrast_ == enabled) return;
  high_contrast_ = enabled;
  if (!d2d_context_) return;

  background_brush_.Reset();
  d2d_context_->CreateSolidColorBrush(BackgroundColor(),
                                      background_brush_.GetAddressOf());

  checker_brush_.Reset();
  checker_bitmap_.Reset();
  CreateCheckerBrush();
}

void Renderer::FitToWindow() {
  fit_ = true;
  MarkInteractive();
}

void Renderer::RotateLeft() { SetRotation(rotation_ + 3); }

void Renderer::RotateRight() { SetRotation(rotation_ + 1); }

void Renderer::ResetRotation() { SetRotation(0); }

void Renderer::SetRotation(int steps) {
  const int next = NormalizeRotation(steps);
  if (next == rotation_) return;
  rotation_ = next;
  MarkInteractive();
  // In fit mode the scale is derived from the effective size, so nothing else
  // changes. In manual mode the zoom and the image centre are kept: the
  // rotation pivots around that centre, so the picture spins in place.
}

bool Renderer::rotated_90() const { return RotationSwapsAxes(rotation_); }

uint32_t Renderer::display_width() const {
  return RotationSwapsAxes(rotation_) ? logical_height_ : logical_width_;
}

uint32_t Renderer::display_height() const {
  return RotationSwapsAxes(rotation_) ? logical_width_ : logical_height_;
}

void Renderer::ActualSize() {
  fit_ = false;
  zoom_ = 1.0f;
  const D2D1_RECT_F content = ContentRect();
  offset_x_ = (content.right - static_cast<float>(logical_width_)) * 0.5f;
  offset_y_ = (content.bottom - static_cast<float>(logical_height_)) * 0.5f;
  MarkInteractive();
}

void Renderer::SetZoom(float zoom) {
  if (image_width_ == 0 || image_height_ == 0) return;
  // Leaving fit mode keeps the current view centre: the zoom menu must not jump
  // the image to a corner (規劃/22/02 §3.3 zoom menu).
  EnsureManualView();
  const float fit_scale = ComputeFitScale();
  const float new_zoom = ClampZoomFor(zoom, fit_scale);
  if (new_zoom == zoom_) return;

  const D2D1_RECT_F content = ContentRect();
  float ax = (content.left + content.right) * 0.5f;
  float ay = (content.top + content.bottom) * 0.5f;
  // Zoom pivots in the unrotated image space: map the anchor back so a rotated
  // view stays centred on the same picture point (規劃/22/05 §3.4).
  float cx = 0.0f;
  float cy = 0.0f;
  ImageCenter(&cx, &cy);
  UnrotatePoint(cx, cy, rotation_, &ax, &ay);

  const float ratio = new_zoom / zoom_;
  offset_x_ = ax - (ax - offset_x_) * ratio;
  offset_y_ = ay - (ay - offset_y_) * ratio;
  zoom_ = new_zoom;
  MarkInteractive();
}

void Renderer::ZoomBy(float factor) {
  if (image_width_ == 0 || image_height_ == 0) return;
  if (!(factor > 0.0f)) return;
  EnsureManualView();
  SetZoom(zoom_ * factor);
}

void Renderer::ZoomAt(float client_x, float client_y, int wheel_delta) {
  if (image_width_ == 0 || image_height_ == 0) return;
  EnsureManualView();

  const float old_zoom = zoom_;
  const float factor = std::pow(1.2f, static_cast<float>(wheel_delta) / 120.0f);
  const float new_zoom =
      ClampZoomFor(old_zoom * factor, ComputeFitScale());
  if (new_zoom == old_zoom) return;

  // The cursor is over the rotated view; the anchor formula works in the
  // unrotated image space, so undo the rotation first (規劃/22/05 坑 4).
  float cx = 0.0f;
  float cy = 0.0f;
  ImageCenter(&cx, &cy);
  float ax = client_x;
  float ay = client_y;
  UnrotatePoint(cx, cy, rotation_, &ax, &ay);

  const float ratio = new_zoom / old_zoom;
  offset_x_ = ax - (ax - offset_x_) * ratio;
  offset_y_ = ay - (ay - offset_y_) * ratio;
  zoom_ = new_zoom;
  MarkInteractive();
}

void Renderer::PanBy(float dx, float dy) {
  if (image_width_ == 0 || image_height_ == 0) return;
  EnsureManualView();
  offset_x_ += dx;
  offset_y_ += dy;
  MarkInteractive();
}

void Renderer::CenterView() {
  if (image_width_ == 0 || image_height_ == 0) return;
  if (fit_) return;  // fit is centred by definition (EffectiveOffset)
  const D2D1_RECT_F content = ContentRect();
  const float scale = EffectiveScale();
  offset_x_ =
      content.left + (content.right - content.left -
                      static_cast<float>(logical_width_) * scale) *
                         0.5f;
  offset_y_ =
      content.top + (content.bottom - content.top -
                     static_cast<float>(logical_height_) * scale) *
                        0.5f;
  MarkInteractive();
}

float Renderer::EffectiveScale() const {
  if (!fit_) return zoom_;
  return ComputeFitScale();
}

float Renderer::ComputeFitScale() const {
  if (image_width_ == 0 || image_height_ == 0 || client_width_ == 0) {
    return 1.0f;
  }
  const D2D1_RECT_F content = ContentRect();
  // A quarter-turned view fits the swapped bounding box, not the source size
  // (規劃/22/05 坑 2).
  return FitScaleFor(content.right - content.left, content.bottom - content.top,
                     static_cast<float>(display_width()),
                     static_cast<float>(display_height()));
}

float Renderer::FitScale() const { return ComputeFitScale(); }

float Renderer::min_zoom() const { return MinZoomFor(ComputeFitScale()); }

float Renderer::max_zoom() const { return kZoomCeiling; }

void Renderer::EffectiveOffset(float* x, float* y) const {
  if (!fit_) {
    *x = offset_x_;
    *y = offset_y_;
    return;
  }

  const D2D1_RECT_F content = ContentRect();
  const float scale = EffectiveScale();
  *x = content.left + (content.right - content.left -
                       static_cast<float>(logical_width_) * scale) * 0.5f;
  *y = content.top + (content.bottom - content.top -
                      static_cast<float>(logical_height_) * scale) * 0.5f;
}

void Renderer::EnsureManualView() {
  if (!fit_) return;
  EffectiveOffset(&offset_x_, &offset_y_);
  zoom_ = EffectiveScale();
  fit_ = false;
}

void Renderer::ImageCenter(float* x, float* y) const {
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  EffectiveOffset(&offset_x, &offset_y);
  const float scale = EffectiveScale();
  *x = offset_x + static_cast<float>(logical_width_) * scale * 0.5f;
  *y = offset_y + static_cast<float>(logical_height_) * scale * 0.5f;
}

void Renderer::DrawCheckerboard(const D2D1_RECT_F& rect) {
  if (!checker_brush_) return;
  d2d_context_->FillRectangle(rect, checker_brush_.Get());
}

D2D1_COLOR_F Renderer::BackgroundColor() const {
  if (high_contrast_) {
    return ColorFromColorRef(GetSysColor(COLOR_WINDOW));
  }
  if (fullscreen_) {
    if (fullscreen_background_ == 1) return D2D1::ColorF(0.0f, 0.0f, 0.0f);
    if (fullscreen_background_ == 2) {
      return D2D1::ColorF(0.03f, 0.035f, 0.04f);
    }
  }
  return dark_theme_ ? D2D1::ColorF(0.07f, 0.08f, 0.09f)
                     : D2D1::ColorF(0.94f, 0.95f, 0.96f);
}

COLORREF Renderer::background_colorref() const {
  const D2D1_COLOR_F color = BackgroundColor();
  return ColorRefFromFloat(color.r, color.g, color.b);
}

uint32_t Renderer::CheckerColor(bool light) const {
  // 0xAARRGGBB in memory is B,G,R,A for BGRA.
  uint32_t color = 0;
  if (dark_theme_) {
    color = light ? 0xFF2C2E32u : 0xFF24262Au;
  } else {
    color = light ? 0xFFE6E8EBu : 0xFFD8DBDFu;
  }
  if (checker_opacity_ >= 100) return color;

  // Blend towards the window background: 0% = invisible, 100% = full contrast.
  const D2D1_COLOR_F background = BackgroundColor();
  const int bg_r = static_cast<int>(background.r * 255.0f + 0.5f);
  const int bg_g = static_cast<int>(background.g * 255.0f + 0.5f);
  const int bg_b = static_cast<int>(background.b * 255.0f + 0.5f);
  const int alpha = checker_opacity_;
  const auto blend = [alpha](int base, int bg) {
    return (base * alpha + bg * (100 - alpha) + 50) / 100;
  };
  const int r = blend(static_cast<int>((color >> 16) & 0xFFu), bg_r);
  const int g = blend(static_cast<int>((color >> 8) & 0xFFu), bg_g);
  const int b = blend(static_cast<int>(color & 0xFFu), bg_b);
  return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

}  // namespace nv::render
