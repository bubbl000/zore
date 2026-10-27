#include "ui/chrome.h"

#include "platform/i18n.h"
#include "ui/draw_util.h"
#include "ui/metrics.h"
#include "ui/ui_context.h"

#include <algorithm>

namespace nv::ui {
namespace {

constexpr int kWeightRegular = 400;
constexpr int kWeightSemibold = 600;

HitId ButtonId(size_t index) {
  return static_cast<HitId>(static_cast<int>(HitId::kTitleMenu) +
                            static_cast<int>(index));
}

HitId BottomId(BottomPart part) {
  return static_cast<HitId>(static_cast<int>(HitId::kBottomInfo) +
                            static_cast<int>(part));
}

// Title bar button icons in TitleLayout.buttons order.
IconId TitleButtonIcon(size_t index, bool maximized, bool fullscreen) {
  switch (index) {
    case 0:
      return IconId::kMenu;
    case 1:
      return fullscreen ? IconId::kExitFullscreen : IconId::kFullscreen;
    case 2:
      return IconId::kMinimize;
    case 3:
      return maximized ? IconId::kRestore : IconId::kMaximize;
    default:
      return IconId::kClose;
  }
}

}  // namespace

void Chrome::Shutdown() { icons_.Shutdown(); }

void Chrome::DrawTitleButton(ID2D1DeviceContext* context, UiContext& ui,
                             const ChromeState& state, HitId id, IconId icon) {
  if (state.title == nullptr || state.colors == nullptr) return;
  const int index = TitleButtonIndex(id);
  if (index < 0 || static_cast<size_t>(index) >= kTitleButtonCount) return;

  const ColorTokens& colors = *state.colors;
  const float s = state.layout != nullptr ? state.layout->dpi_scale : 1.0f;
  const Rect button = state.title->buttons[static_cast<size_t>(index)];
  if (button.empty()) return;

  const bool is_close = id == HitId::kTitleClose;
  const bool hovered = state.hover == id;
  const bool pressed = state.pressed == id;

  const Rect inner = button.Inset(2.0f * s, 3.0f * s);
  if (pressed) {
    draw::FillRoundRect(context, ui, inner, metrics::kRadiusControl * s,
                        is_close ? colors.danger : colors.bg_pressed);
  } else if (hovered) {
    draw::FillRoundRect(context, ui, inner, metrics::kRadiusControl * s,
                        is_close ? colors.danger : colors.bg_hover);
  }

  uint32_t icon_color = state.active ? colors.text_primary : colors.text_inactive;
  if (is_close && (hovered || pressed)) icon_color = 0xFFFFFFFFu;

  const float size = metrics::kTitleButtonIconSize * s;
  const Rect box = Rect::FromXYWH(button.left + (button.width() - size) * 0.5f,
                                  button.top + (button.height() - size) * 0.5f,
                                  size, size);
  icons_.Draw(context, ui, icon, box, icon_color,
              metrics::kTitleButtonIconStroke * s);
}

void Chrome::DrawTitleBar(ID2D1DeviceContext* context, UiContext& ui,
                          const ChromeState& state) {
  if (context == nullptr || state.layout == nullptr || state.title == nullptr ||
      state.colors == nullptr) {
    return;
  }
  const LayoutFrame& layout = *state.layout;
  const TitleLayout& title = *state.title;
  const ColorTokens& colors = *state.colors;
  const float s = layout.dpi_scale;
  if (title.bar.empty()) return;

  draw::FillRect(context, ui, title.bar,
                 state.active ? colors.bg_panel : colors.bg_panel_inactive);

  // One-pixel separator under the bar, drawn inside the bar so the content
  // rectangle keeps its exact height.
  draw::FillRect(context,
                 ui, Rect::FromLTRB(title.bar.left, title.bar.bottom - 1.0f,
                                    title.bar.right, title.bar.bottom),
                 colors.border_subtle);

  if (title.show_icon && !title.icon.empty()) {
    if (state.app_logo != nullptr) {
      // The logo artwork already carries its own rounded tile and colours, so
      // it is drawn as-is (high quality, since it is downscaled from 64 px).
      const D2D1_RECT_F dest = D2D1::RectF(
          title.icon.left, title.icon.top, title.icon.right, title.icon.bottom);
      context->DrawBitmap(state.app_logo, &dest, 1.0f,
                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    } else {
      icons_.Draw(context, ui, IconId::kImage, title.icon, colors.accent,
                  1.2f * s);
    }
  }

  if (state.strings != nullptr) {
    const ChromeStrings& strings = *state.strings;
    const std::wstring* texts[kSegCount] = {
        &strings.file_name, &strings.dimension, &strings.file_size};
    const float font = metrics::kTitleFontSize;  // DIP: DrawText scales by DPI

    bool have_previous = false;
    Rect previous_rect{};
    for (size_t i = 0; i < kSegCount; ++i) {
      if (!title.segment_visible[i] || title.segment[i].empty()) continue;
      if (have_previous) {
        const float dot_x = (previous_rect.right + title.segment[i].left) * 0.5f;
        const Rect dot = Rect::FromXYWH(dot_x - 1.0f, title.bar.top,
                                        2.0f, title.bar.height());
        draw::DrawText(context, ui, L"\u00B7", font, kWeightRegular, dot,
                       colors.text_muted, false, true);
      }
      // Only the file name is allowed to be ellipsized: the numeric segments
      // are dropped before they would ever need trimming.
      draw::DrawText(context, ui, *texts[i], font,
                     i == kSegFileName ? kWeightSemibold : kWeightRegular,
                     title.segment[i],
                     i == kSegFileName
                         ? (state.active ? colors.text_primary
                                         : colors.text_inactive)
                         : colors.text_secondary,
                     i == kSegFileName, false);
      previous_rect = title.segment[i];
      have_previous = true;
    }
  }

  for (size_t i = 0; i < kTitleButtonCount; ++i) {
    const HitId id = ButtonId(i);
    DrawTitleButton(context, ui, state, id,
                    TitleButtonIcon(i, state.maximized, state.fullscreen));
  }
}

void Chrome::DrawBottomPart(ID2D1DeviceContext* context, UiContext& ui,
                            const ChromeState& state, BottomPart part,
                            bool enabled) {
  if (state.bottom == nullptr || state.colors == nullptr) return;
  const ColorTokens& colors = *state.colors;
  const float s = state.bottom->dpi_scale;
  const Rect rect = state.bottom->part[part];
  if (rect.empty()) return;

  const HitId id = BottomId(part);
  const bool hovered = enabled && state.hover == id;
  const bool pressed = enabled && state.pressed == id;
  const Rect inner = rect.Inset(2.0f * s, 6.0f * s);
  if (pressed) {
    draw::FillRoundRect(context, ui, inner, metrics::kRadiusControl * s,
                        colors.bg_pressed);
  } else if (hovered) {
    draw::FillRoundRect(context, ui, inner, metrics::kRadiusControl * s,
                        colors.bg_hover);
  }

  const uint32_t text_color =
      enabled ? colors.text_primary : colors.text_muted;
  const uint32_t icon_color = enabled ? colors.text_secondary : colors.text_muted;
  const float icon_size = metrics::kBottomIconSize * s;
  const float font = metrics::kBottomFontSize;  // DIP: DrawText scales by DPI

  switch (part) {
    case kPartInfo: {
      const Rect box = Rect::FromXYWH(
          rect.left + (rect.width() - icon_size) * 0.5f,
          rect.top + (rect.height() - icon_size) * 0.5f, icon_size, icon_size);
      icons_.Draw(context, ui, IconId::kInfo, box,
                  enabled ? colors.accent : colors.text_muted, 1.3f * s);
      break;
    }
    case kPartPercent:
      if (state.strings != nullptr) {
        draw::DrawText(context, ui, state.strings->percent, font,
                       kWeightSemibold, rect, text_color, true, true);
      }
      break;
    case kPartIndex:
      if (state.strings != nullptr) {
        draw::DrawText(context, ui, state.strings->index, font,
                       kWeightRegular, rect, colors.text_secondary, true, true);
      }
      break;
    case kPartFit: {
      const Rect box = Rect::FromXYWH(rect.left + 6.0f * s,
                                      rect.top + (rect.height() - icon_size) * 0.5f,
                                      icon_size, icon_size);
      icons_.Draw(context, ui, IconId::kFit, box, icon_color, 1.3f * s);
      draw::DrawText(context, ui, nv::i18n::Text(nv::i18n::StringId::kUiFit),
                     font, kWeightRegular,
                     Rect::FromLTRB(box.right + 6.0f * s, rect.top, rect.right,
                                    rect.bottom),
                     text_color, true, false);
      break;
    }
    case kPartActual:
      draw::DrawText(context, ui, L"1:1", font, kWeightSemibold, rect,
                     text_color, false, true);
      break;
    case kPartRotateLeft:
    case kPartRotateRight: {
      const Rect box = Rect::FromXYWH(
          rect.left + (rect.width() - icon_size) * 0.5f,
          rect.top + (rect.height() - icon_size) * 0.5f, icon_size, icon_size);
      icons_.Draw(context, ui,
                  part == kPartRotateLeft ? IconId::kRotateLeft
                                          : IconId::kRotateRight,
                  box, icon_color, 1.3f * s);
      // v3.0 (規劃/26 §2.3): the accent dot that marked "rotated" is gone —
      // it read as a stray pixel and the rotated picture already says so.
      break;
    }
    case kPartMore: {
      const Rect chevron =
          Rect::FromXYWH(rect.right - 18.0f * s,
                         rect.top + (rect.height() - icon_size) * 0.5f,
                         icon_size, icon_size);
      draw::DrawText(context, ui, nv::i18n::Text(nv::i18n::StringId::kUiMore),
                     font, kWeightRegular,
                     Rect::FromLTRB(rect.left + 12.0f * s, rect.top,
                                    chevron.left, rect.bottom),
                     text_color, true, false);
      icons_.Draw(context, ui, IconId::kChevronDown, chevron, icon_color,
                  1.3f * s);
      break;
    }
    default:
      break;
  }
}

void Chrome::DrawBottomBar(ID2D1DeviceContext* context, UiContext& ui,
                           const ChromeState& state) {
  if (context == nullptr || state.layout == nullptr || state.bottom == nullptr ||
      state.colors == nullptr) {
    return;
  }
  const ColorTokens& colors = *state.colors;
  const BottomLayout& bottom = *state.bottom;
  if (bottom.bar.empty()) return;

  draw::FillRect(context, ui, bottom.bar, colors.bg_panel);
  draw::FillRect(context, ui,
                 Rect::FromLTRB(bottom.bar.left, bottom.bar.top,
                                bottom.bar.right, bottom.bar.top + 1.0f),
                 colors.border_subtle);

  for (size_t i = 0; i < kPartCount; ++i) {
    DrawBottomPart(context, ui, state, static_cast<BottomPart>(i),
                   bottom.enabled[i]);
  }
}

void Chrome::DrawImageArrows(ID2D1DeviceContext* context, UiContext& ui,
                             const ChromeState& state) {
  if (context == nullptr || state.layout == nullptr || state.colors == nullptr) {
    return;
  }
  const LayoutFrame& layout = *state.layout;
  if (!layout.arrows_visible) return;
  const ColorTokens& colors = *state.colors;
  const float s = layout.dpi_scale;
  const struct {
    HitId id;
    const Rect* rect;
    IconId icon;
  } kArrows[] = {
      {HitId::kArrowLeft, &layout.arrows_left, IconId::kChevronLeft},
      {HitId::kArrowRight, &layout.arrows_right, IconId::kChevronRight},
  };

  const float radius = metrics::kArrowSize * s * 0.5f;
  for (const auto& arrow : kArrows) {
    if (arrow.rect->empty()) continue;
    const bool hovered = state.hover == arrow.id;
    const float opacity = hovered ? 1.0f : metrics::kArrowRestOpacity;
    // A small filled disc keeps the chevron readable over bright photos; the
    // border turns accent on hover without moving the geometry.
    draw::FillRoundRect(context, ui, *arrow.rect, radius,
                        WithAlpha(colors.bg_panel, 0.78f * opacity));
    draw::StrokeRoundRect(context, ui, *arrow.rect, radius, 1.0f * s,
                          WithAlpha(hovered ? colors.accent
                                            : colors.border_subtle,
                                    opacity));
    const float icon = metrics::kArrowIconSize * s;
    const Rect box = Rect::FromXYWH(
        arrow.rect->left + (arrow.rect->width() - icon) * 0.5f,
        arrow.rect->top + (arrow.rect->height() - icon) * 0.5f, icon, icon);
    icons_.Draw(context, ui, arrow.icon, box,
                WithAlpha(hovered ? colors.accent_hover : colors.text_secondary,
                          opacity),
                metrics::kArrowIconStroke * s);
  }
}

void Chrome::DrawToggle(ID2D1DeviceContext* context, UiContext& ui,
                        const Rect& box, bool on, bool enabled,
                        const ColorTokens& colors, float s) {
  if (box.empty()) return;
  const float radius = box.height() * 0.5f;
  const uint32_t track =
      !enabled ? colors.border_subtle : (on ? colors.accent : colors.border_strong);
  draw::FillRoundRect(context, ui, box, radius, track);

  const float inset = metrics::kMenuToggleInset * s;
  const float knob = metrics::kMenuToggleKnob * s;
  const float knob_y = box.top + (box.height() - knob) * 0.5f;
  const float knob_x = on ? box.right - inset - knob : box.left + inset;
  draw::FillRoundRect(context, ui, Rect::FromXYWH(knob_x, knob_y, knob, knob),
                      knob * 0.5f,
                      enabled ? 0xFFFFFFFFu : colors.text_muted);
}

void Chrome::DrawMenuRow(ID2D1DeviceContext* context, UiContext& ui,
                         const ChromeState& state, const MenuItem& item,
                         const Rect& row, bool hovered) {
  const ColorTokens& colors = *state.colors;
  const float s = state.layout->dpi_scale;
  const float padding = metrics::kMenuPadding * s;

  if (item.kind == MenuItemKind::kSeparator) {
    const float y = (row.top + row.bottom) * 0.5f;
    draw::FillRect(context, ui,
                   Rect::FromLTRB(row.left, y, row.right,
                                  y + metrics::kMenuSeparatorHeight * s),
                   colors.border_subtle);
    return;
  }

  if (item.kind == MenuItemKind::kHeader) {
    draw::DrawText(context, ui, item.label, metrics::kMenuFontSize - 1.0f,
                   kWeightSemibold,
                   Rect::FromLTRB(row.left + padding, row.top, row.right,
                                  row.bottom),
                   colors.text_muted, true, false);
    return;
  }

  if (hovered && item.enabled) {
    draw::FillRoundRect(context, ui, row, metrics::kRadiusControl * s * 0.6f,
                        colors.bg_hover);
  }

  const uint32_t text_color =
      item.enabled ? colors.text_primary : colors.text_muted;
  float text_left = row.left + padding;
  if (item.kind == MenuItemKind::kRadio) {
    // The check mark sits in the left gutter so labels of every row align.
    const float check = metrics::kMenuCheckSize * s;
    if (item.checked) {
      const Rect box = Rect::FromXYWH(text_left, row.top + (row.height() - check) * 0.5f,
                                      check, check);
      icons_.Draw(context, ui, IconId::kCheck, box, colors.accent, 1.6f * s);
    }
    text_left += check + 6.0f * s;
  }

  float text_right = row.right - padding;
  Rect toggle{};
  if (item.kind == MenuItemKind::kToggle) {
    const float width = metrics::kMenuToggleWidth * s;
    const float height = metrics::kMenuToggleHeight * s;
    toggle = Rect::FromXYWH(row.right - padding - width,
                            row.top + (row.height() - height) * 0.5f, width,
                            height);
    text_right = toggle.left - 8.0f * s;
  }

  draw::DrawText(context, ui, item.label, metrics::kMenuFontSize,
                 kWeightRegular,
                 Rect::FromLTRB(text_left, row.top, text_right, row.bottom),
                 text_color, true, false);

  if (item.kind == MenuItemKind::kToggle) {
    DrawToggle(context, ui, toggle, item.checked, item.enabled, colors, s);
  }
}

void Chrome::DrawMenu(ID2D1DeviceContext* context, UiContext& ui,
                      const ChromeState& state) {
  if (context == nullptr || state.layout == nullptr || state.colors == nullptr ||
      state.menu == nullptr || state.menu_items == nullptr) {
    return;
  }
  const MenuLayout& menu = *state.menu;
  if (!menu.visible || menu.panel.empty()) return;

  const ColorTokens& colors = *state.colors;
  const float s = menu.dpi_scale;

  // A slightly translucent panel reads as an overlay above the image without
  // hiding it completely (瑕忓妰/22/02 搂3.3).
  draw::FillRoundRect(context, ui, menu.panel, metrics::kRadiusControl * s,
                      WithAlpha(colors.bg_panel, 0.98f));
  draw::StrokeRoundRect(context, ui, menu.panel, metrics::kRadiusControl * s,
                        1.0f, colors.border_strong);

  const std::vector<MenuItem>& items = *state.menu_items;
  for (size_t i = 0; i < items.size() && i < menu.rows.size(); ++i) {
    const bool hovered = state.menu_hover == static_cast<int>(i);
    DrawMenuRow(context, ui, state, items[i], menu.rows[i], hovered);
  }
}

void Chrome::DrawFullscreenReveal(ID2D1DeviceContext* context, UiContext& ui,
                                  const ChromeState& state) {
  if (context == nullptr || state.layout == nullptr || state.colors == nullptr) {
    return;
  }
  const LayoutFrame& layout = *state.layout;
  if (!layout.fullscreen_reveal_visible) return;
  const ColorTokens& colors = *state.colors;
  const float s = layout.dpi_scale;
  const float alpha = layout.fs_reveal_opacity;

  const struct {
    const Rect* rect;
    HitId id;
    IconId icon;
    bool is_close;
  } buttons[] = {
      {&layout.fs_exit, HitId::kFullscreenExit, IconId::kExitFullscreen, false},
      {&layout.fs_close, HitId::kFullscreenClose, IconId::kClose, true},
  };
  for (const auto& button : buttons) {
    if (button.rect->empty()) continue;
    const bool hovered = state.hover == button.id;
    const bool pressed = state.pressed == button.id;
    // A translucent plate keeps the icon readable on a bright picture; the
    // hover/pressed wash matches the title-bar buttons.
    draw::FillRoundRect(context, ui, *button.rect, metrics::kRadiusControl * s,
                        WithAlpha(colors.bg_panel,
                                  alpha * (pressed ? 0.96f
                                                   : (hovered ? 0.92f : 0.78f))));
    if (hovered || pressed) {
      draw::FillRoundRect(context, ui, button.rect->Inset(2.0f * s, 2.0f * s),
                          metrics::kRadiusControl * s,
                          WithAlpha(button.is_close ? colors.danger
                                                    : colors.bg_hover,
                                    alpha));
    }
    const float size = metrics::kTitleButtonIconSize * s;
    const Rect box = Rect::FromXYWH(
        button.rect->left + (button.rect->width() - size) * 0.5f,
        button.rect->top + (button.rect->height() - size) * 0.5f, size, size);
    const uint32_t color =
        (button.is_close && (hovered || pressed)) ? 0xFFFFFFFFu
                                                  : colors.text_primary;
    icons_.Draw(context, ui, button.icon, box, WithAlpha(color, alpha),
                metrics::kTitleButtonIconStroke * s);
  }
}

}  // namespace nv::ui
