#pragma once

#include "core/image.h"

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nv::render {

// Bottom thumbnail rail.
//
// The strip owns its layout, scroll offset, hover state and device-dependent
// bitmaps. It only materializes bitmaps for the visible window (plus a margin),
// so a folder with thousands of images costs a bounded amount of memory.
//
// All sizes are logical pixels at 96 DPI and are scaled by SetDpiScale().
class ThumbnailStrip {
 public:
  static constexpr size_t kNone = static_cast<size_t>(-1);

  ThumbnailStrip() = default;
  ~ThumbnailStrip() = default;

  ThumbnailStrip(const ThumbnailStrip&) = delete;
  ThumbnailStrip& operator=(const ThumbnailStrip&) = delete;

  // Replaces the item list. Identical lists keep already-decoded thumbnails.
  void SetItems(const std::vector<std::filesystem::path>& items);
  void Clear();

  bool visible() const { return !items_.empty(); }
  size_t item_count() const { return items_.size(); }
  bool empty() const { return items_.empty(); }

  size_t current() const { return current_; }
  void SetCurrent(size_t index);

  void SetDpiScale(float scale);
  float dpi_scale() const { return scale_; }
  float height() const { return 76.0f * scale_; }

  void SetViewWidth(float width);
  float scroll() const { return scroll_x_; }
  void ScrollBy(float dx);
  void EnsureVisible(size_t index);
  void ResetScroll();

  // Client coordinates in physical pixels. Returns true when the point is
  // over an item; `index` receives that item.
  bool HitTest(float x, float y, float client_width, float client_height,
               size_t* index) const;

  // Updates the hovered item; returns true when it changed.
  bool SetHover(float x, float y, float client_width, float client_height);
  size_t hovered() const { return hover_; }

  // Appends indices that intersect the visible window into `out`.
  void VisibleIndices(size_t margin, std::vector<size_t>* out) const;

  void SetThumbnail(size_t index, std::shared_ptr<Image> image);
  void SetFailed(size_t index);
  bool HasThumbnail(size_t index) const;
  bool HasFailed(size_t index) const;

  // Draws the rail inside `bounds` (the bottom `height()` pixels of the
  // client area). Requires the renderer's D2D device context. In high-contrast
  // mode the system window/highlight colors replace the theme palette.
  // `opacity` implements the auto-mode fade (規劃/22/08 §3.5); 1.0 draws the
  // rail unchanged.
  void Render(ID2D1DeviceContext* context, const D2D1_RECT_F& bounds,
              bool dark_theme, bool high_contrast = false, float opacity = 1.0f);

  // Slot rectangle (thumbnail cell, not just the bitmap) in client pixels.
  // Used as the hover-tooltip anchor; empty when the index is out of range.
  D2D1_RECT_F CellRect(size_t index, float strip_top) const;

  // Drops device-dependent bitmaps and brushes (device loss, shutdown).
  void ReleaseDeviceResources();

 private:
  float item_width() const { return 68.0f * scale_; }
  float thumb_size() const { return 60.0f * scale_; }
  float padding() const { return 10.0f * scale_; }
  float top_padding() const { return 8.0f * scale_; }

  float ContentWidth() const;
  float MaxScroll() const;
  D2D1_RECT_F ItemRect(size_t index, float strip_top) const;
  size_t FirstVisible() const;
  size_t LastVisible() const;

  void EnsureBrushes(ID2D1DeviceContext* context, bool dark_theme,
                     bool high_contrast);
  ID2D1Bitmap* BitmapFor(ID2D1DeviceContext* context, size_t index);
  void EvictOutside(size_t first, size_t last);

  std::vector<std::filesystem::path> items_;
  size_t current_ = 0;
  size_t hover_ = kNone;
  float scroll_x_ = 0.0f;
  float view_width_ = 0.0f;
  float scale_ = 1.0f;

  std::unordered_map<size_t, std::shared_ptr<Image>> images_;
  std::unordered_map<size_t, Microsoft::WRL::ComPtr<ID2D1Bitmap>> bitmaps_;
  std::unordered_set<size_t> failed_;

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background_brush_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> placeholder_brush_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent_brush_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_brush_;
  // Fade layer for auto mode; device-derived, released with the bitmaps.
  Microsoft::WRL::ComPtr<ID2D1Layer> fade_layer_;
  bool brushes_dark_ = false;
  bool brushes_high_contrast_ = false;
};

}  // namespace nv::render
