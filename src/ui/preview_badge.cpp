#include "ui/preview_badge.h"

#include "ui/draw_util.h"
#include "ui/metrics.h"
#include "ui/ui_context.h"

#include <algorithm>

namespace nv::ui {
namespace {

constexpr int kWeightSemibold = 600;

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

}  // namespace

PreviewBadgeLayout ComputePreviewBadgeLayout(const Rect& content,
                                             float dpi_scale,
                                             float text_width) {
  PreviewBadgeLayout layout;
  if (content.empty()) return layout;
  const float s = Scale(dpi_scale);
  const float margin = metrics::kPreviewBadgeMargin * s;
  const float height = metrics::kPreviewBadgeHeight * s;
  // A very long localized label is clamped instead of eating the image area;
  // the text is then clipped by the draw helper.
  const float width = std::clamp(
      std::max(text_width, 0.0f) + metrics::kPreviewBadgePadding * 2.0f * s,
      metrics::kPreviewBadgeMinWidth * s,
      metrics::kPreviewBadgeMaxWidth * s);
  // The badge is laid out inside the content rectangle, so a full-screen or a
  // pinned-rail layout still puts it below the title bar and above the bar.
  if (content.width() < width + margin * 2.0f ||
      content.height() < height + margin * 2.0f) {
    return layout;
  }
  const float right = content.right - margin;
  const float top = content.top + margin;
  layout.badge = Rect::FromLTRB(right - width, top, right, top + height);
  layout.visible = true;
  return layout;
}

void DrawPreviewBadge(ID2D1DeviceContext* context, UiContext& ui,
                      const PreviewBadgeLayout& layout,
                      const PreviewBadgeStrings& strings,
                      const ColorTokens& colors, bool high_contrast,
                      float dpi_scale) {
  if (context == nullptr || !layout.visible || strings.text.empty()) return;
  const float s = Scale(dpi_scale);
  // High contrast keeps the badge fully opaque with system colors, like the
  // animation pill (規劃/22/12 坑 16).
  const float opacity =
      high_contrast ? 1.0f : std::clamp(strings.opacity, 0.0f, 1.0f);
  if (opacity <= 0.01f) return;

  const float radius = metrics::kPreviewBadgeHeight * 0.5f * s;
  draw::FillRoundRect(context, ui, layout.badge, radius,
                      WithAlpha(colors.bg_panel, 0.85f * opacity));
  draw::StrokeRoundRect(context, ui, layout.badge, radius, std::max(1.0f, s),
                        WithAlpha(colors.border_subtle, 0.85f * opacity));

  const uint32_t text_color =
      strings.failed ? colors.danger : colors.text_primary;
  const Rect inner = Rect::FromLTRB(
      layout.badge.left + metrics::kPreviewBadgePadding * s, layout.badge.top,
      layout.badge.right - metrics::kPreviewBadgePadding * s,
      layout.badge.bottom);
  draw::DrawText(context, ui, strings.text, metrics::kPreviewBadgeFontSize,
                 kWeightSemibold, inner, WithAlpha(text_color, opacity), true,
                 true);
}

}  // namespace nv::ui
