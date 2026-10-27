#include "render/thumbnail_strip.h"

#include "render/palette.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nv::render {
namespace {

constexpr float kSideMargin = 2.0f;
constexpr size_t kEvictMargin = 32;

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

void ThumbnailStrip::SetItems(const std::vector<std::filesystem::path>& items) {
  bool same = items.size() == items_.size();
  if (same) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i] != items_[i]) {
        same = false;
        break;
      }
    }
  }
  if (same) {
    if (!items_.empty() && current_ != kNone && current_ >= items_.size()) {
      current_ = items_.size() - 1;
    }
    return;
  }

  items_ = items;
  images_.clear();
  bitmaps_.clear();
  failed_.clear();
  hover_ = kNone;
  scroll_x_ = 0.0f;
  if (items_.empty()) {
    current_ = 0;
  } else if (current_ != kNone && current_ >= items_.size()) {
    current_ = items_.size() - 1;
  }
}

void ThumbnailStrip::Clear() {
  items_.clear();
  images_.clear();
  bitmaps_.clear();
  failed_.clear();
  current_ = 0;
  hover_ = kNone;
  scroll_x_ = 0.0f;
}

void ThumbnailStrip::SetCurrent(size_t index) {
  if (items_.empty()) {
    current_ = 0;
    return;
  }
  current_ = index == kNone ? kNone : std::min(index, items_.size() - 1);
}

void ThumbnailStrip::SetDpiScale(float scale) {
  scale = std::clamp(scale, 0.5f, 4.0f);
  if (std::abs(scale - scale_) < 0.01f) return;
  scale_ = scale;
  bitmaps_.clear();  // Bitmaps are pixel sized; rebuild at the new scale.
  scroll_x_ = std::clamp(scroll_x_, 0.0f, MaxScroll());
}

void ThumbnailStrip::SetViewWidth(float width) {
  view_width_ = std::max(0.0f, width);
  scroll_x_ = std::clamp(scroll_x_, 0.0f, MaxScroll());
}

void ThumbnailStrip::ScrollBy(float dx) {
  scroll_x_ = std::clamp(scroll_x_ + dx, 0.0f, MaxScroll());
}

void ThumbnailStrip::ResetScroll() { scroll_x_ = 0.0f; }

void ThumbnailStrip::EnsureVisible(size_t index) {
  if (items_.empty()) return;
  index = std::min(index, items_.size() - 1);
  const float item_w = item_width();
  const float cell_left = padding() + static_cast<float>(index) * item_w;
  const float cell_right = cell_left + item_w;
  const float margin = padding();

  if (cell_left < scroll_x_ + margin) {
    scroll_x_ = cell_left - margin;
  } else if (cell_right > scroll_x_ + view_width_ - margin) {
    scroll_x_ = cell_right - view_width_ + margin;
  }
  scroll_x_ = std::clamp(scroll_x_, 0.0f, MaxScroll());
}

bool ThumbnailStrip::HitTest(float x, float y, float client_width,
                             float client_height, size_t* index) const {
  if (items_.empty() || index == nullptr) return false;
  if (x < 0.0f || x > client_width) return false;
  if (y < client_height - height() || y > client_height) return false;

  const float strip_top = client_height - height();
  const size_t first = FirstVisible();
  const size_t last = LastVisible();
  for (size_t i = first; i <= last && i < items_.size(); ++i) {
    const D2D1_RECT_F rect = ItemRect(i, strip_top);
    if (x >= rect.left - kSideMargin * scale_ &&
        x <= rect.right + kSideMargin * scale_ &&
        y >= rect.top - kSideMargin * scale_ &&
        y <= rect.bottom + kSideMargin * scale_) {
      *index = i;
      return true;
    }
  }
  return false;
}

bool ThumbnailStrip::SetHover(float x, float y, float client_width,
                              float client_height) {
  size_t hit = kNone;
  if (!HitTest(x, y, client_width, client_height, &hit)) hit = kNone;
  if (hit == hover_) return false;
  hover_ = hit;
  return true;
}

void ThumbnailStrip::VisibleIndices(size_t margin, std::vector<size_t>* out) const {
  if (out == nullptr || items_.empty()) return;
  const size_t first = FirstVisible();
  const size_t last = LastVisible();
  const size_t begin = first > margin ? first - margin : 0;
  const size_t end = std::min(items_.size() - 1, last + margin);
  out->reserve(out->size() + (end - begin + 1));
  for (size_t i = begin; i <= end; ++i) out->push_back(i);
}

void ThumbnailStrip::SetThumbnail(size_t index, std::shared_ptr<Image> image) {
  if (index >= items_.size() || !image || image->Empty()) return;
  failed_.erase(index);
  bitmaps_.erase(index);
  images_[index] = std::move(image);
}

void ThumbnailStrip::SetFailed(size_t index) {
  if (index >= items_.size()) return;
  images_.erase(index);
  bitmaps_.erase(index);
  failed_.insert(index);
}

bool ThumbnailStrip::HasThumbnail(size_t index) const {
  return images_.find(index) != images_.end() ||
         bitmaps_.find(index) != bitmaps_.end();
}

bool ThumbnailStrip::HasFailed(size_t index) const {
  return failed_.find(index) != failed_.end();
}

void ThumbnailStrip::ReleaseDeviceResources() {
  bitmaps_.clear();
  background_brush_.Reset();
  placeholder_brush_.Reset();
  accent_brush_.Reset();
  border_brush_.Reset();
  fade_layer_.Reset();
}

float ThumbnailStrip::ContentWidth() const {
  return padding() * 2.0f +
         static_cast<float>(items_.size()) * item_width();
}

float ThumbnailStrip::MaxScroll() const {
  return std::max(0.0f, ContentWidth() - view_width_);
}

D2D1_RECT_F ThumbnailStrip::ItemRect(size_t index, float strip_top) const {
  const float item_w = item_width();
  const float thumb = thumb_size();
  const float cell_left =
      padding() + static_cast<float>(index) * item_w - scroll_x_;
  const float thumb_left = cell_left + (item_w - thumb) * 0.5f;
  const float thumb_top = strip_top + top_padding();
  return D2D1::RectF(thumb_left, thumb_top, thumb_left + thumb,
                     thumb_top + thumb);
}

D2D1_RECT_F ThumbnailStrip::CellRect(size_t index, float strip_top) const {
  if (index >= items_.size()) return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
  const float item_w = item_width();
  const float cell_left =
      padding() + static_cast<float>(index) * item_w - scroll_x_;
  return D2D1::RectF(cell_left, strip_top, cell_left + item_w,
                     strip_top + height());
}

size_t ThumbnailStrip::FirstVisible() const {
  if (items_.empty()) return 0;
  const float item_w = item_width();
  const float offset = padding() - scroll_x_;
  if (offset >= 0.0f) return 0;
  const float hidden = -offset;
  const size_t first = static_cast<size_t>(hidden / item_w);
  return std::min(first, items_.size() - 1);
}

size_t ThumbnailStrip::LastVisible() const {
  if (items_.empty()) return 0;
  const float item_w = item_width();
  const size_t span =
      static_cast<size_t>(std::ceil(view_width_ / std::max(1.0f, item_w))) + 2;
  return std::min(items_.size() - 1, FirstVisible() + span);
}

void ThumbnailStrip::EnsureBrushes(ID2D1DeviceContext* context, bool dark_theme,
                                   bool high_contrast) {
  if (context == nullptr) return;
  if (background_brush_ && brushes_dark_ == dark_theme &&
      brushes_high_contrast_ == high_contrast) {
    return;
  }

  brushes_dark_ = dark_theme;
  brushes_high_contrast_ = high_contrast;
  background_brush_.Reset();
  placeholder_brush_.Reset();
  accent_brush_.Reset();
  border_brush_.Reset();

  D2D1_COLOR_F background = dark_theme
                                ? D2D1::ColorF(0.09f, 0.095f, 0.105f)
                                : D2D1::ColorF(0.90f, 0.91f, 0.92f);
  D2D1_COLOR_F placeholder = dark_theme
                                 ? D2D1::ColorF(0.14f, 0.15f, 0.17f)
                                 : D2D1::ColorF(0.83f, 0.84f, 0.86f);
  D2D1_COLOR_F accent = D2D1::ColorF(0.30f, 0.60f, 1.0f);
  D2D1_COLOR_F border = dark_theme ? D2D1::ColorF(0.24f, 0.25f, 0.28f)
                                   : D2D1::ColorF(0.72f, 0.74f, 0.77f);
  if (high_contrast) {
    // Follow the user's high-contrast palette instead of the app theme.
    background = ColorFromColorRef(GetSysColor(COLOR_3DFACE));
    placeholder = ColorFromColorRef(GetSysColor(COLOR_APPWORKSPACE));
    accent = ColorFromColorRef(GetSysColor(COLOR_HIGHLIGHT));
    border = ColorFromColorRef(GetSysColor(COLOR_WINDOWTEXT));
  }

  context->CreateSolidColorBrush(background, background_brush_.GetAddressOf());
  context->CreateSolidColorBrush(placeholder,
                                 placeholder_brush_.GetAddressOf());
  context->CreateSolidColorBrush(accent, accent_brush_.GetAddressOf());
  context->CreateSolidColorBrush(border, border_brush_.GetAddressOf());
}

ID2D1Bitmap* ThumbnailStrip::BitmapFor(ID2D1DeviceContext* context,
                                       size_t index) {
  if (context == nullptr) return nullptr;

  const auto existing = bitmaps_.find(index);
  if (existing != bitmaps_.end()) return existing->second.Get();

  const auto image_it = images_.find(index);
  if (image_it == images_.end() || !image_it->second) return nullptr;
  const Image& image = *image_it->second;
  if (image.Empty()) return nullptr;

  std::vector<uint8_t> pixels = image.pixels;
  if (image.info.has_alpha) PremultiplyBgra(pixels.data(), pixels.size());

  const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.0f, 96.0f);

  ComPtr<ID2D1Bitmap> bitmap;
  if (FAILED(context->CreateBitmap(D2D1::SizeU(image.info.width,
                                                image.info.height),
                                   pixels.data(), image.info.stride, &props,
                                   bitmap.GetAddressOf()))) {
    return nullptr;
  }
  auto& slot = bitmaps_[index];
  slot = std::move(bitmap);
  return slot.Get();
}

void ThumbnailStrip::EvictOutside(size_t first, size_t last) {
  const size_t begin = first > kEvictMargin ? first - kEvictMargin : 0;
  const size_t end = std::min(items_.size() - 1, last + kEvictMargin);

  for (auto it = bitmaps_.begin(); it != bitmaps_.end();) {
    if (it->first < begin || it->first > end) {
      it = bitmaps_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = images_.begin(); it != images_.end();) {
    if (it->first < begin || it->first > end) {
      it = images_.erase(it);
    } else {
      ++it;
    }
  }
}

void ThumbnailStrip::Render(ID2D1DeviceContext* context,
                            const D2D1_RECT_F& bounds, bool dark_theme,
                            bool high_contrast, float opacity) {
  if (context == nullptr || items_.empty()) return;
  opacity = std::clamp(opacity, 0.0f, 1.0f);
  if (opacity <= 0.01f) return;

  view_width_ = std::max(0.0f, bounds.right - bounds.left);
  scroll_x_ = std::clamp(scroll_x_, 0.0f, MaxScroll());
  EnsureBrushes(context, dark_theme, high_contrast);

  // Auto-mode fade: a single D2D layer applies the opacity to the whole rail
  // (background, cells, selected border and scroll indicator) instead of
  // rebuilding every brush (規劃/22/08 §3.5).
  bool layered = opacity < 0.999f;
  if (layered && !fade_layer_) {
    layered = SUCCEEDED(
        context->CreateLayer(nullptr, fade_layer_.ReleaseAndGetAddressOf()));
  }
  if (layered) {
    const D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters(
        bounds, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(),
        opacity);
    context->PushLayer(params, fade_layer_.Get());
  }

  const float scale = scale_;
  context->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
  if (background_brush_) {
    context->FillRectangle(bounds, background_brush_.Get());
  }
  if (border_brush_) {
    context->DrawLine(D2D1::Point2F(bounds.left, bounds.top + 0.5f),
                      D2D1::Point2F(bounds.right, bounds.top + 0.5f),
                      border_brush_.Get(), std::max(1.0f, scale));
  }

  const size_t first = FirstVisible();
  const size_t last = LastVisible();
  const float corner = 4.0f * scale;
  const float inset = 3.0f * scale;

  for (size_t i = first; i <= last && i < items_.size(); ++i) {
    const D2D1_RECT_F thumb = ItemRect(i, bounds.top);
    if (thumb.right < bounds.left || thumb.left > bounds.right) continue;

    const D2D1_ROUNDED_RECT cell = D2D1::RoundedRect(
        D2D1::RectF(thumb.left - inset, thumb.top - inset,
                    thumb.right + inset, thumb.bottom + inset),
        corner, corner);

    if (placeholder_brush_) {
      context->FillRoundedRectangle(cell, placeholder_brush_.Get());
    }
    if (ID2D1Bitmap* bitmap = BitmapFor(context, i)) {
      const D2D1_SIZE_F bitmap_size = bitmap->GetSize();
      if (bitmap_size.width > 0.0f && bitmap_size.height > 0.0f) {
        // Letterbox: previews keep their aspect ratio inside the square cell.
        const float box_width = thumb.right - thumb.left;
        const float box_height = thumb.bottom - thumb.top;
        const float fit_scale =
            std::min(box_width / bitmap_size.width,
                     box_height / bitmap_size.height);
        const float draw_width = bitmap_size.width * fit_scale;
        const float draw_height = bitmap_size.height * fit_scale;
        const D2D1_RECT_F dest = D2D1::RectF(
            thumb.left + (box_width - draw_width) * 0.5f,
            thumb.top + (box_height - draw_height) * 0.5f,
            thumb.left + (box_width + draw_width) * 0.5f,
            thumb.top + (box_height + draw_height) * 0.5f);
        context->DrawBitmap(bitmap, &dest, 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
      }
    }
    if (i == current_ && accent_brush_) {
      context->DrawRoundedRectangle(cell, accent_brush_.Get(),
                                    std::max(1.5f, 2.0f * scale), nullptr);
    } else if (i == hover_ && border_brush_) {
      context->DrawRoundedRectangle(cell, border_brush_.Get(),
                                    std::max(1.0f, scale), nullptr);
    }
  }

  // Scroll indicator: only meaningful when the rail overflows.
  if (accent_brush_ && ContentWidth() > view_width_ + 0.5f) {
    const float track_y = bounds.bottom - 3.0f * scale;
    const float ratio = view_width_ / ContentWidth();
    const float thumb_width =
        std::max(24.0f * scale, view_width_ * ratio);
    const float travel = std::max(0.0f, view_width_ - thumb_width);
    const float t = MaxScroll() > 0.0f ? scroll_x_ / MaxScroll() : 0.0f;
    const float x = bounds.left + t * travel;
    context->FillRectangle(
        D2D1::RectF(x, track_y, x + thumb_width, track_y + 3.0f * scale),
        accent_brush_.Get());
  }

  context->PopAxisAlignedClip();
  if (layered) context->PopLayer();
  EvictOutside(first, last);
}

}  // namespace nv::render
