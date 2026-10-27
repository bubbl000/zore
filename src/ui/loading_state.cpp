#include "ui/loading_state.h"

#include "ui/draw_util.h"
#include "ui/metrics.h"
#include "ui/ui_context.h"

#include <algorithm>

namespace nv::ui {
namespace {

constexpr int kWeightRegular = 400;

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

}  // namespace

LoadingLayout ComputeLoadingLayout(const Rect& content, float dpi_scale) {
  const float s = Scale(dpi_scale);
  LoadingLayout layout;
  if (content.empty()) return layout;
  const float bar_w = std::min(metrics::kLoadingBarWidth * s, content.width());
  const float bar_h = std::max(1.0f, metrics::kLoadingBarHeight * s);
  const float label_h = metrics::kLoadingLabelHeight * s;
  const float gap = metrics::kLoadingLabelGap * s;
  const float chip_w = std::min((metrics::kLoadingBarWidth + 80.0f) * s,
                                content.width());
  const float chip_h = std::max(bar_h + gap + label_h + 32.0f * s,
                                metrics::kLoadingChipHeight * s);
  layout.chip = Rect::FromXYWH(
      content.left + (content.width() - chip_w) * 0.5f,
      content.top + (content.height() - chip_h) * 0.5f, chip_w, chip_h);
  layout.bar = Rect::FromXYWH(
      content.left + (content.width() - bar_w) * 0.5f,
      layout.chip.top + (chip_h - (bar_h + gap + label_h)) * 0.5f, bar_w, bar_h);
  const float label_w = std::min(metrics::kLoadingLabelWidth * s,
                                 content.width());
  layout.label = Rect::FromXYWH(
      content.left + (content.width() - label_w) * 0.5f, layout.bar.bottom + gap,
      label_w, label_h);
  layout.visible = true;
  return layout;
}

void DrawLoadingState(ID2D1DeviceContext* context, UiContext& ui,
                      const LoadingLayout& layout, const std::wstring& text,
                      float phase, const ColorTokens& colors) {
  if (context == nullptr || !layout.visible) return;
  const float radius = layout.bar.height() * 0.5f;

  // Backing chip: the indicator can sit over a bright photograph, where plain
  // muted text and a 1 px track would be unreadable.
  draw::FillRoundRect(context, ui, layout.chip, metrics::kRadiusControl * 2.0f,
                      WithAlpha(colors.bg_panel, 0.92f));
  draw::StrokeRoundRect(context, ui, layout.chip,
                        metrics::kRadiusControl * 2.0f,
                        std::max(1.0f, layout.chip.height() / 68.0f),
                        colors.border_subtle);

  // Track, then one accent segment (40% of the bar) moving left-to-right and
  // back; `phase` wraps at 1.0 in the app's animation timer.
  draw::FillRoundRect(context, ui, layout.bar, radius,
                      WithAlpha(colors.border_strong, 0.75f));
  const float segment = layout.bar.width() * 0.4f;
  const float travel = std::max(0.0f, layout.bar.width() - segment);
  const float t = std::clamp(phase, 0.0f, 1.0f);
  const float eased = t <= 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
  const Rect fill = Rect::FromLTRB(layout.bar.left + travel * eased,
                                   layout.bar.top,
                                   layout.bar.left + travel * eased + segment,
                                   layout.bar.bottom);
  draw::FillRoundRect(context, ui, fill, radius,
                      WithAlpha(colors.accent, 0.85f));
  draw::DrawText(context, ui, text, metrics::kBottomFontSize, kWeightRegular,
                 layout.label, colors.text_secondary, false, true);
}

}  // namespace nv::ui
