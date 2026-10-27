#include "ui/strip_tooltip.h"

#include "ui/draw_util.h"
#include "ui/metrics.h"

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

}  // namespace

StripTooltipLayout ComputeStripTooltipLayout(const Rect& anchor,
                                             const Rect& client,
                                             float text_width, float dpi_scale) {
  StripTooltipLayout layout;
  if (client.empty() || anchor.empty()) return layout;
  const float s = Scale(dpi_scale);

  const float margin = metrics::kStripTooltipMargin * s;
  const float padding = metrics::kStripTooltipPadding * s;
  const float gap = metrics::kStripTooltipGap * s;
  const float height = metrics::kStripTooltipHeight * s;

  const float available = std::max(0.0f, client.width() - 2.0f * margin);
  if (available <= 0.0f) return layout;
  const float max_width =
      std::min(metrics::kStripTooltipMaxWidth * s, available);
  // `std::clamp` needs low <= high: derive the lower bound from the available
  // width so a very narrow window still produces a positive panel.
  const float min_width = std::min(metrics::kStripTooltipMinWidth * s, max_width);
  const float wanted = std::max(0.0f, text_width) + 2.0f * padding;
  const float width = std::clamp(wanted, min_width, max_width);
  if (width <= 0.0f) return layout;

  // Horizontally centred on the slot, then clamped into the client.
  float left = anchor.left + (anchor.width() - width) * 0.5f;
  left = std::clamp(left, client.left + margin,
                    std::max(client.left + margin, client.right - margin - width));

  // Preferred position is above the slot; below is the fallback when the top of
  // the window would clip it (規劃/22/08 坑 5).
  float top = anchor.top - gap - height;
  layout.above = true;
  if (top < client.top + margin) {
    top = anchor.bottom + gap;
    layout.above = false;
  }
  const float max_top = std::max(client.top + margin, client.bottom - margin - height);
  if (top > max_top) top = max_top;
  if (top < client.top + margin) top = client.top + margin;

  layout.panel = Rect::FromLTRB(left, top, left + width, top + height);
  layout.visible = true;
  return layout;
}

void DrawStripTooltip(ID2D1DeviceContext* context, UiContext& ui,
                      const StripTooltipLayout& layout, std::wstring_view text,
                      const ColorTokens& colors, float dpi_scale) {
  if (context == nullptr || !layout.visible || text.empty()) return;
  const float s = Scale(dpi_scale);
  const float radius = metrics::kStripTooltipRadius * s;

  // Soft shadow: one offset translucent rounded rectangle behind the panel.
  const Rect shadow = Rect::FromLTRB(layout.panel.left, layout.panel.top + 2.0f * s,
                                     layout.panel.right, layout.panel.bottom + 2.0f * s);
  draw::FillRoundRect(context, ui, shadow, radius, 0x55000000);
  draw::FillRoundRect(context, ui, layout.panel, radius, colors.bg_panel);
  draw::StrokeRoundRect(context, ui, layout.panel, radius, std::max(1.0f, s),
                        colors.border_subtle);

  const float padding = metrics::kStripTooltipPadding * s;
  const Rect inner = Rect::FromLTRB(layout.panel.left + padding, layout.panel.top,
                                    layout.panel.right - padding,
                                    layout.panel.bottom);
  draw::DrawText(context, ui, text, metrics::kStripTooltipFontSize, 400, inner,
                 colors.text_primary, true, false);
}

}  // namespace nv::ui
