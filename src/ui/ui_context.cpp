#include "ui/ui_context.h"

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

int ToSizeX4(float size_dip, float dpi_scale) {
  const float pixels = std::clamp(size_dip * dpi_scale, 1.0f, 512.0f);
  return static_cast<int>(std::lround(pixels * 4.0f));
}

}  // namespace

UiContext::UiContext() = default;
UiContext::~UiContext() { Shutdown(); }

bool UiContext::Create() {
  if (dwrite_) return true;
  HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(
                                       dwrite_.GetAddressOf()));
  if (FAILED(hr)) return false;

  // Segoe UI Variable exists on Windows 11; fall back to Segoe UI on Windows 10.
  Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
  if (SUCCEEDED(dwrite_->GetSystemFontCollection(&collection, FALSE)) &&
      collection) {
    const wchar_t* candidates[] = {L"Segoe UI Variable Text", L"Segoe UI",
                                  L"Microsoft YaHei UI"};
    for (const wchar_t* candidate : candidates) {
      UINT32 index = 0;
      BOOL exists = FALSE;
      if (SUCCEEDED(collection->FindFamilyName(candidate, &index, &exists)) &&
          exists) {
        font_family_ = candidate;
        break;
      }
    }
  }
  RefreshAccessibility();
  return true;
}

void UiContext::Shutdown() {
  brushes_.clear();
  brush_context_ = nullptr;
  ClearTextCache();
  measures_.clear();
  d2d_factory_.Reset();
  dwrite_.Reset();
}

void UiContext::SetDpiScale(float scale) {
  const float clamped = scale > 0.0f ? scale : 1.0f;
  if (std::fabs(clamped - dpi_scale_) < 0.001f) return;
  dpi_scale_ = clamped;
  // Text formats bake the DPI into the pixel size, so they must be rebuilt.
  ClearTextCache();
  measures_.clear();
}

void UiContext::ClearTextCache() { formats_.clear(); }

IDWriteTextFormat* UiContext::TextFormat(float size_dip, int weight,
                                         bool center_vertical,
                                         bool center_horizontal) {
  if (!dwrite_) return nullptr;

  FormatKey key{ToSizeX4(size_dip, dpi_scale_), weight, center_vertical,
                center_horizontal};
  auto it = formats_.find(key);
  if (it != formats_.end()) return it->second.format.Get();

  const float pixel_size = static_cast<float>(key.size_x4) / 4.0f;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
  if (FAILED(dwrite_->CreateTextFormat(
          font_family_.c_str(), nullptr,
          static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, pixel_size, L"", &format)) ||
      !format) {
    return nullptr;
  }
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format->SetTextAlignment(center_horizontal ? DWRITE_TEXT_ALIGNMENT_CENTER
                                             : DWRITE_TEXT_ALIGNMENT_LEADING);
  format->SetParagraphAlignment(center_vertical
                                    ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                    : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

  FormatEntry entry;
  entry.format = format;
  dwrite_->CreateEllipsisTrimmingSign(format.Get(), &entry.ellipsis);
  auto [inserted, ok] = formats_.emplace(key, std::move(entry));
  (void)ok;
  return inserted->second.format.Get();
}

IDWriteInlineObject* UiContext::Ellipsis(IDWriteTextFormat* format) {
  if (format == nullptr) return nullptr;
  for (auto& [key, entry] : formats_) {
    (void)key;
    if (entry.format.Get() == format) return entry.ellipsis.Get();
  }
  return nullptr;
}

float UiContext::MeasureText(std::wstring_view text, float size_dip, int weight) {
  if (!dwrite_ || text.empty()) return 0.0f;

  MeasureKey key{std::wstring(text), ToSizeX4(size_dip, dpi_scale_), weight};
  auto cached = measures_.find(key);
  if (cached != measures_.end()) return cached->second;

  IDWriteTextFormat* format = TextFormat(size_dip, weight, false);
  if (format == nullptr) return 0.0f;

  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite_->CreateTextLayout(text.data(),
                                       static_cast<UINT32>(text.size()), format,
                                       4096.0f, 4096.0f, &layout)) ||
      !layout) {
    return 0.0f;
  }
  DWRITE_TEXT_METRICS metrics{};
  if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
  const float width = metrics.widthIncludingTrailingWhitespace;
  measures_.emplace(std::move(key), width);
  return width;
}

ID2D1SolidColorBrush* UiContext::Brush(ID2D1DeviceContext* context,
                                       uint32_t argb) {
  if (context == nullptr) return nullptr;
  if (brush_context_ != context) {
    // Brushes are bound to the device context that created them.
    brushes_.clear();
    brush_context_ = context;
  }
  auto it = brushes_.find(argb);
  if (it != brushes_.end()) return it->second.Get();

  const D2D1_COLOR_F color = D2D1::ColorF(
      static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
      static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
      static_cast<float>(argb & 0xFFu) / 255.0f,
      static_cast<float>((argb >> 24) & 0xFFu) / 255.0f);
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(context->CreateSolidColorBrush(color, &brush)) || !brush) {
    return nullptr;
  }
  auto inserted = brushes_.emplace(argb, std::move(brush));
  return inserted.first->second.Get();
}

void UiContext::ResetDeviceResources() {
  brushes_.clear();
  brush_context_ = nullptr;
}

void UiContext::RefreshAccessibility() {
  BOOL animations = TRUE;
  if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) {
    reduce_motion_ = animations == FALSE;
  }
}

}  // namespace nv::ui
