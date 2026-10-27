#include "ui/toast.h"

#include "ui/draw_util.h"
#include "ui/metrics.h"
#include "ui/ui_context.h"

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

constexpr int kWeightRegular = 400;
constexpr float kToastMinWidth = 180.0f;
constexpr float kToastMaxWidth = 480.0f;
constexpr float kToastPadding = 14.0f;
constexpr float kToastGap = 8.0f;

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

}  // namespace

Rect ComputeToastRect(const Rect& anchor, float text_width_px, size_t index,
                      float dpi_scale) {
  if (anchor.empty()) return {};
  const float s = Scale(dpi_scale);
  const float wanted = text_width_px + 2.0f * kToastPadding * s;
  const float width = std::clamp(wanted, kToastMinWidth * s, kToastMaxWidth * s);
  const float height = metrics::kToastHeight * s;
  const float step = height + kToastGap * s;
  const float center = (anchor.left + anchor.right) * 0.5f;
  const float bottom = anchor.bottom - static_cast<float>(index) * step;
  return Rect::FromXYWH(center - width * 0.5f, bottom - height, width, height);
}

void DrawToasts(ID2D1DeviceContext* context, UiContext& ui,
                const std::vector<ToastItem>& items,
                const std::vector<Rect>& rects, const ColorTokens& colors,
                int hovered_index) {
  if (context == nullptr) return;
  const size_t count = std::min({items.size(), rects.size(), kMaxToasts});
  for (size_t i = 0; i < count; ++i) {
    const Rect& rect = rects[i];
    if (rect.empty()) continue;
    const float s = rect.height() / metrics::kToastHeight;
    const uint32_t fill = (i == static_cast<size_t>(hovered_index))
                              ? WithAlpha(colors.bg_panel, 1.0f)
                              : WithAlpha(colors.bg_panel, 0.96f);
    draw::FillRoundRect(context, ui, rect, metrics::kRadiusControl * s, fill);
    draw::StrokeRoundRect(context, ui, rect, metrics::kRadiusControl * s,
                          std::max(1.0f, s), colors.border_strong);
    if (items[i].danger) {
      // Danger marker: a 3 DIP red bar inside the left edge.
      const Rect bar = Rect::FromLTRB(
          rect.left + 1.5f * s, rect.top + 8.0f * s, rect.left + 4.5f * s,
          rect.bottom - 8.0f * s);
      draw::FillRoundRect(context, ui, bar, 1.5f * s, colors.danger);
    }
    draw::DrawText(context, ui, items[i].text, metrics::kBottomFontSize,
                   kWeightRegular, rect.Inset(kToastPadding * s, 0.0f),
                   items[i].danger ? colors.danger : colors.text_primary, true,
                   true);
  }
}

}  // namespace nv::ui
