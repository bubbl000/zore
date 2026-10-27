#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>

namespace nv::ui {

// Device-independent text + device-dependent brush cache for the custom chrome.
//
// DWrite was removed in M1-2 to save startup time; v2.0 needs real text, so the
// factory is created once during startup (before D2D, ~2 ms) and every
// IDWriteTextFormat is cached per (size, weight, alignment, DPI).
class UiContext {
 public:
  UiContext();
  ~UiContext();

  UiContext(const UiContext&) = delete;
  UiContext& operator=(const UiContext&) = delete;

  // Creates the DWrite factory and resolves the UI font family. Cheap enough to
  // call before the window exists; does not touch D3D/D2D.
  bool Create();
  void Shutdown();

  bool ready() const { return dwrite_ != nullptr; }
  IDWriteFactory* dwrite() const { return dwrite_.Get(); }

  // The renderer owns the D2D factory; icons need it to build path geometry.
  // Cleared by Shutdown().
  void SetD2DFactory(ID2D1Factory1* factory) { d2d_factory_ = factory; }
  ID2D1Factory1* d2d_factory() const { return d2d_factory_.Get(); }

  void SetDpiScale(float scale);
  float dpi_scale() const { return dpi_scale_; }

  // `weight` is a DWRITE_FONT_WEIGHT value. `center` selects centred paragraph
  // alignment (used for single-line labels in fixed-height rows).
  IDWriteTextFormat* TextFormat(float size_dip, int weight, bool center_vertical,
                                bool center_horizontal = false);
  // Trimming sign that matches a cached format; owns nothing the caller keeps.
  IDWriteInlineObject* Ellipsis(IDWriteTextFormat* format);

  // Layout width in pixels of a single line, cached by (text, size, weight).
  float MeasureText(std::wstring_view text, float size_dip, int weight);

  // Solid brush for 0xAARRGGBB. Requires the device context that will draw.
  ID2D1SolidColorBrush* Brush(ID2D1DeviceContext* context, uint32_t argb);

  // Drops every brush after a device reset / device loss.
  void ResetDeviceResources();

  // SPI_GETCLIENTAREAANIMATION == 0 means "reduce motion": all fades become
  // instant (規劃/22/20 §5).
  void RefreshAccessibility();
  bool reduce_motion() const { return reduce_motion_; }

  const std::wstring& font_family() const { return font_family_; }

 private:
  struct FormatEntry {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
  };
  struct FormatKey {
    int size_x4 = 0;
    int weight = 400;
    bool center_v = false;
    bool center_h = false;
    bool operator<(const FormatKey& other) const {
      return std::tie(size_x4, weight, center_v, center_h) <
             std::tie(other.size_x4, other.weight, other.center_v,
                      other.center_h);
    }
  };
  struct MeasureKey {
    std::wstring text;
    int size_x4 = 0;
    int weight = 400;
    bool operator<(const MeasureKey& other) const {
      return std::tie(text, size_x4, weight) <
             std::tie(other.text, other.size_x4, other.weight);
    }
  };

  void ClearTextCache();

  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> d2d_factory_;  // borrowed
  std::wstring font_family_ = L"Segoe UI";
  std::map<FormatKey, FormatEntry> formats_;
  std::map<MeasureKey, float> measures_;
  std::map<uint32_t, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushes_;
  ID2D1DeviceContext* brush_context_ = nullptr;
  float dpi_scale_ = 1.0f;
  bool reduce_motion_ = false;
};

}  // namespace nv::ui
