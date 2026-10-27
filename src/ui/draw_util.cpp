#include "ui/draw_util.h"

#include <algorithm>
#include <cmath>

namespace nv::ui::draw {

void FillRect(ID2D1DeviceContext* context, UiContext& ui, const Rect& rect,
              uint32_t argb) {
  if (context == nullptr || rect.empty()) return;
  ID2D1SolidColorBrush* brush = ui.Brush(context, argb);
  if (brush == nullptr) return;
  context->FillRectangle(ToD2D(rect), brush);
}

void FillRoundRect(ID2D1DeviceContext* context, UiContext& ui, const Rect& rect,
                   float radius, uint32_t argb) {
  if (context == nullptr || rect.empty()) return;
  ID2D1SolidColorBrush* brush = ui.Brush(context, argb);
  if (brush == nullptr) return;
  const float r = std::min(radius, std::min(rect.width(), rect.height()) * 0.5f);
  context->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), r, r), brush);
}

void StrokeRoundRect(ID2D1DeviceContext* context, UiContext& ui,
                     const Rect& rect, float radius, float stroke_width,
                     uint32_t argb) {
  if (context == nullptr || rect.empty() || stroke_width <= 0.0f) return;
  ID2D1SolidColorBrush* brush = ui.Brush(context, argb);
  if (brush == nullptr) return;
  const float r = std::min(radius, std::min(rect.width(), rect.height()) * 0.5f);
  context->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(rect), r, r), brush,
                                stroke_width);
}

void DrawText(ID2D1DeviceContext* context, UiContext& ui, std::wstring_view text,
              float size_dip, int weight, const Rect& rect, uint32_t argb,
              bool ellipsis, bool center) {
  if (context == nullptr || text.empty() || rect.empty()) return;

  IDWriteTextFormat* format = ui.TextFormat(size_dip, weight, true, center);
  if (format == nullptr) return;
  ID2D1SolidColorBrush* brush = ui.Brush(context, argb);
  if (brush == nullptr) return;

  const float width = std::max(1.0f, rect.width());
  const float height = std::max(1.0f, rect.height());
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(ui.dwrite()->CreateTextLayout(text.data(),
                                           static_cast<UINT32>(text.size()),
                                           format, width, height, &layout)) ||
      !layout) {
    return;
  }

  if (ellipsis) {
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    IDWriteInlineObject* sign = ui.Ellipsis(format);
    layout->SetTrimming(&trimming, sign);
  }

  context->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(),
                          brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace nv::ui::draw
