#include "ui/layout.h"

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

float Scale(float dpi_scale) { return dpi_scale > 0.0f ? dpi_scale : 1.0f; }

Rect Centered(const Rect& area, float width, float height, float bottom_gap) {
  const float w = std::min(width, std::max(0.0f, area.width()));
  const float x = area.left + (area.width() - w) * 0.5f;
  const float bottom = area.bottom - bottom_gap;
  return Rect::FromXYWH(x, bottom - height, w, height);
}

}  // namespace

const wchar_t* StripModeName(StripMode mode) {
  switch (mode) {
    case StripMode::kPinned:
      return L"pinned";
    case StripMode::kAuto:
      return L"auto";
    case StripMode::kOff:
      return L"off";
  }
  return L"pinned";
}

bool ParseStripMode(std::wstring_view text, StripMode* mode) {
  if (mode == nullptr) return false;
  if (text == L"pinned") {
    *mode = StripMode::kPinned;
    return true;
  }
  if (text == L"auto") {
    *mode = StripMode::kAuto;
    return true;
  }
  if (text == L"off") {
    *mode = StripMode::kOff;
    return true;
  }
  return false;
}

LayoutFrame ComputeLayout(const LayoutInputs& inputs, float dpi_scale,
                          Rect client) {
  const float s = Scale(dpi_scale);

  LayoutFrame frame;
  frame.client = client;
  frame.dpi_scale = s;
  frame.fullscreen = inputs.fullscreen;
  frame.show_title = !inputs.fullscreen;
  frame.show_bottom = !inputs.fullscreen;

  const float title_h =
      frame.show_title ? metrics::kTitleHeight * s : 0.0f;
  const float bar_h = frame.show_bottom ? metrics::kBottomHeight * s : 0.0f;

  // v3.0 (規劃/26 §2.7): fullscreen keeps the reveal strip in the top-right
  // corner. The rectangles exist for the whole fullscreen session (they host
  // the reveal hot zone); `fullscreen_reveal_visible` only says whether they are
  // painted and hit-testable right now.
  frame.fullscreen_reveal_visible =
      inputs.fullscreen && inputs.fs_reveal_opacity > 0.001f;
  frame.fs_reveal_opacity =
      inputs.fullscreen ? std::clamp(inputs.fs_reveal_opacity, 0.0f, 1.0f) : 0.0f;
  if (inputs.fullscreen) {
    const float size = metrics::kFullscreenButton * s;
    const float margin = metrics::kFullscreenMargin * s;
    const float gap = metrics::kFullscreenGap * s;
    const float top = client.top + margin;
    const float right = client.right - margin;
    frame.fs_close = Rect::FromLTRB(right - size, top, right, top + size);
    frame.fs_exit =
        Rect::FromLTRB(right - size - gap - size, top, right - size - gap,
                       top + size);
  }

  // Fullscreen hides the rail as well as the chrome; since v3.0 the rail is
  // reveal-only there too (規劃/26 §2.7 reverses pitfall 146).
  const bool strip_available = inputs.strip_visible &&
                               inputs.strip_mode != StripMode::kOff;
  frame.strip_counts_in_content = strip_available && !inputs.fullscreen &&
                                  inputs.strip_mode == StripMode::kPinned;
  const float pinned_strip_h =
      frame.strip_counts_in_content ? metrics::kStripHeight * s : 0.0f;

  frame.title =
      Rect::FromLTRB(client.left, client.top, client.right, client.top + title_h);
  frame.bottom_bar = Rect::FromLTRB(client.left, client.bottom - bar_h,
                                    client.right, client.bottom);
  frame.content = Rect::FromLTRB(client.left, client.top + title_h, client.right,
                                 client.bottom - bar_h - pinned_strip_h);

  // The rail sits directly above the bottom bar in both pinned and auto mode;
  // the difference is only whether the content rectangle was shrunk for it.
  if (strip_available) {
    const float strip_h = metrics::kStripHeight * s;
    frame.strip = Rect::FromLTRB(client.left, client.bottom - bar_h - strip_h,
                                 client.right, client.bottom - bar_h);
  }
  // Auto mode keeps the rectangle (it hosts the reveal hot zone) but only the
  // faded-in rail owns clicks. Pinned is always interactive; off has no rect.
  // In fullscreen (v3.0) every mode is reveal-only, so only `strip_shown`
  // counts — a pinned rail starts hidden there.
  frame.strip_interactive =
      strip_available &&
      (inputs.fullscreen
           ? inputs.strip_shown
           : (inputs.strip_mode == StripMode::kPinned ||
              (inputs.strip_mode == StripMode::kAuto && inputs.strip_shown)));

  // Bottom stack: the auto rail overlays the content, so panels must move up
  // while it is faded in (once, at the start of the fade - never per frame).
  const bool auto_shown = strip_available && inputs.strip_mode == StripMode::kAuto &&
                          inputs.strip_shown;
  const float auto_extra = auto_shown ? metrics::kStripHeight * s : 0.0f;
  const float stack_top =
      frame.show_bottom ? client.bottom - bar_h - pinned_strip_h - auto_extra
                        : client.bottom;

  frame.pill_visible = inputs.pill_visible && inputs.has_image;
  if (frame.pill_visible) {
    const float gap = metrics::kPillGap * s;
    const float width = std::min(metrics::kAnimPillWidth * s, client.width());
    frame.pill = Centered(Rect::FromLTRB(client.left, 0.0f, client.right,
                                         stack_top),
                          width, metrics::kPillHeight * s, gap);
  }

  frame.toast_visible = inputs.toast_visible;
  if (frame.toast_visible) {
    const float pill_extra =
        frame.pill_visible ? (metrics::kPillHeight + metrics::kPillGap) * s : 0.0f;
    const float gap = metrics::kStackGap * s + pill_extra;
    frame.toast = Centered(Rect::FromLTRB(client.left, 0.0f, client.right,
                                         stack_top),
                           360.0f * s, metrics::kToastHeight * s, gap);
  }

  // v3.0 (規劃/26 §2.1): the info card is a separate top-level window, so the
  // main window layout no longer reserves a rectangle for it.

  // Image-area arrows: vertically centred inside the content rectangle. They
  // are laid out only while visible (more than one file), so a single-image
  // folder has neither a rectangle nor a hit target (規劃/22/09 §5.1).
  frame.arrows_visible = inputs.arrows_visible && !frame.content.empty();
  if (frame.arrows_visible) {
    const float arrow = metrics::kArrowSize * s;
    const float arrow_gap = metrics::kArrowMargin * s;
    const float center_y = (frame.content.top + frame.content.bottom) * 0.5f -
                           arrow * 0.5f;
    frame.arrows_left = Rect::FromXYWH(frame.content.left + arrow_gap, center_y,
                                       arrow, arrow);
    frame.arrows_right = Rect::FromXYWH(frame.content.right - arrow_gap - arrow,
                                        center_y, arrow, arrow);
  }

  // Drag hint: centred in the content (V3.1-06 §06a). It used to hang near the
  // top like a banner, which read as "another card" instead of "drop here".
  frame.drag_over = inputs.drag_over;
  if (inputs.drag_over && !frame.content.empty()) {
    const float hint_w =
        std::min(metrics::kEmptyHintWidth * s, frame.content.width());
    const float hint_h =
        std::min(metrics::kEmptyHintHeight * s, frame.content.height());
    frame.drag_hint = Rect::FromXYWH(
        frame.content.left + (frame.content.width() - hint_w) * 0.5f,
        frame.content.top + (frame.content.height() - hint_h) * 0.5f, hint_w,
        hint_h);
  }

  // v3.0 (規劃/26 §2.4): bird's eye view. It floats above the rail slot or the
  // bottom bar, so it is laid out after those rectangles are known. Everything
  // is pure geometry; the app only supplies the renderer's view numbers.
  frame.minimap = MinimapLayout{};
  if (inputs.minimap_visible) {
    MinimapInputs minimap;
    minimap.client = client;
    minimap.content = frame.content;
    minimap.strip = frame.strip;
    minimap.bottom_bar = frame.bottom_bar;
    minimap.fullscreen = inputs.fullscreen;
    minimap.dpi_scale = s;
    minimap.display_width = inputs.minimap_display_width;
    minimap.display_height = inputs.minimap_display_height;
    minimap.view_scale = inputs.minimap_view_scale;
    minimap.view_center_x = inputs.minimap_view_center_x;
    minimap.view_center_y = inputs.minimap_view_center_y;
    frame.minimap = ComputeMinimapLayout(minimap);
  }
  return frame;
}

TitleLayout ComputeTitleLayout(const Rect& bar, float dpi_scale, bool show_icon,
                               const std::array<float, kSegCount>& text_widths) {
  const float s = Scale(dpi_scale);

  TitleLayout title;
  title.bar = bar;
  title.show_icon = show_icon;
  title.compact = bar.width() / s < metrics::kCompactWidth;
  title.button_width =
      (title.compact ? metrics::kTitleButtonWidthCompact
                     : metrics::kTitleButtonWidth) *
      s;
  if (bar.empty() || title.button_width <= 0.0f) return title;

  for (size_t i = 0; i < kTitleButtonCount; ++i) {
    const float left = bar.right - static_cast<float>(kTitleButtonCount - i) *
                                       title.button_width;
    title.buttons[i] =
        Rect::FromLTRB(left, bar.top, left + title.button_width, bar.bottom);
  }

  float text_left = bar.left + metrics::kTitlePaddingLeft * s;
  if (show_icon) {
    const float icon = metrics::kTitleIconSize * s;
    title.icon = Rect::FromXYWH(text_left, bar.top + (bar.height() - icon) * 0.5f,
                                icon, icon);
    text_left += icon + metrics::kTitleIconGap * s;
  }

  const float text_right =
      title.buttons[0].left - metrics::kTitlePaddingRight * s;
  const float available = std::max(0.0f, text_right - text_left);
  title.text_area =
      Rect::FromLTRB(text_left, bar.top, std::max(text_left, text_right),
                     bar.bottom);

  const float gap = metrics::kTitleSegmentGap * s;
  std::array<bool, kSegCount> visible{};
  for (size_t i = 0; i < kSegCount; ++i) {
    // An empty string means the segment has nothing to show (no image yet):
    // it must not leave a separator dot behind (規劃/22/03 §3).
    visible[i] = text_widths[i] > 0.0f;
  }
  const auto required_width = [&]() {
    float total = 0.0f;
    int count = 0;
    for (size_t i = 0; i < kSegCount; ++i) {
      if (!visible[i]) continue;
      total += std::max(0.0f, text_widths[i]);
      ++count;
    }
    if (count > 1) total += gap * static_cast<float>(count - 1);
    return total;
  };

  // Drop order: file size (3), dimension (2). The file name (1) is never
  // dropped. (v3.0: the index and the percentage left the title bar, so the
  // old index/percent rungs are gone — 規劃/26 §2.2.)
  for (const TitleSegment segment : {kSegFileSize, kSegDimension}) {
    if (required_width() <= available) break;
    visible[segment] = false;
  }

  float x = text_left;
  for (size_t i = 0; i < kSegCount; ++i) {
    if (!visible[i]) continue;
    float width = std::max(0.0f, text_widths[i]);
    if (x + width > text_right) width = std::max(0.0f, text_right - x);
    title.segment[i] = Rect::FromLTRB(x, bar.top, x + width, bar.bottom);
    title.segment_visible[i] = true;
    x += width + gap;
  }
  return title;
}

BottomLayout ComputeBottomLayout(const Rect& bar, float dpi_scale,
                                 const BottomInputs& inputs) {
  const float s = Scale(dpi_scale);

  BottomLayout bottom;
  bottom.bar = bar;
  bottom.dpi_scale = s;
  if (bar.empty()) return bottom;

  const float pad = metrics::kBottomPadding * s;
  float x = bar.left + pad;
  const auto place = [&](BottomPart part, float width_dip, bool enabled) {
    const float width = width_dip * s;
    bottom.part[part] = Rect::FromLTRB(x, bar.top, x + width, bar.bottom);
    bottom.enabled[part] = enabled;
    x += width;
  };

  place(kPartInfo, metrics::kPartInfo, inputs.has_image && inputs.info_available);
  place(kPartPercent, metrics::kPartPercent,
        inputs.has_image && inputs.percent_available);
  place(kPartIndex, metrics::kPartIndex, false);  // display only

  // Right cluster is laid out from the right edge inwards: 更多, ↻, ↺, 1:1, 适应.
  float right = bar.right - pad;
  const auto place_right = [&](BottomPart part, float width_dip, bool enabled) {
    const float width = width_dip * s;
    bottom.part[part] = Rect::FromLTRB(right - width, bar.top, right, bar.bottom);
    bottom.enabled[part] = enabled;
    right -= width;
  };
  place_right(kPartMore, metrics::kPartMore, inputs.more_available);
  place_right(kPartRotateRight, metrics::kPartRotate,
              inputs.has_image && inputs.rotate_available);
  place_right(kPartRotateLeft, metrics::kPartRotate,
              inputs.has_image && inputs.rotate_available);
  place_right(kPartActual, metrics::kPartActual, inputs.has_image);
  place_right(kPartFit, metrics::kPartFit, inputs.has_image);
  return bottom;
}

HitId HitTestFrame(const LayoutFrame& frame, const TitleLayout& title,
                   const BottomLayout& bottom, const EmptyLayout& empty,
                   float x, float y) {
  // v3.0 (規劃/26 §2.7): the fullscreen reveal floats above everything — it is
  // the only chrome in fullscreen and must never be shadowed by the image.
  if (frame.fullscreen_reveal_visible) {
    if (frame.fs_close.Contains(x, y)) return HitId::kFullscreenClose;
    if (frame.fs_exit.Contains(x, y)) return HitId::kFullscreenExit;
  }
  for (size_t i = 0; i < kTitleButtonCount; ++i) {
    if (title.buttons[i].Contains(x, y)) {
      return static_cast<HitId>(static_cast<int>(HitId::kTitleMenu) +
                                static_cast<int>(i));
    }
  }
  for (size_t i = 0; i < kPartCount; ++i) {
    if (!bottom.enabled[i]) continue;
    if (bottom.part[i].Contains(x, y)) {
      return static_cast<HitId>(static_cast<int>(HitId::kBottomInfo) +
                                static_cast<int>(i));
    }
  }
  // The empty card sits in the content area, so its buttons must be found
  // before the image (規劃/22/03 坑 11: a click on "Choose image" must never
  // start a pan).
  if (const HitId empty_hit = HitTestEmpty(empty, x, y);
      empty_hit != HitId::kNone) {
    return empty_hit;
  }
  // Arrows sit above the image: hitting them must not start a pan (坑 6).
  if (frame.arrows_visible) {
    if (frame.arrows_left.Contains(x, y)) return HitId::kArrowLeft;
    if (frame.arrows_right.Contains(x, y)) return HitId::kArrowRight;
  }
  // v3.0 (規劃/26 §2.4): the bird's eye panel is above the rail and the image;
  // hitting it must be a minimap drag, never a picture pan or a rail click.
  if (frame.minimap.visible && frame.minimap.panel.Contains(x, y)) {
    return HitId::kMinimap;
  }
  if (frame.strip_interactive && !frame.strip.empty() &&
      frame.strip.Contains(x, y)) {
    return HitId::kStrip;
  }
  if (frame.content.Contains(x, y)) return HitId::kImage;
  return HitId::kNone;
}

const wchar_t* EmptyKindName(EmptyKind kind) {
  switch (kind) {
    case EmptyKind::kNone:
      return L"none";
    case EmptyKind::kEmpty:
      return L"empty";
    case EmptyKind::kFolder:
      return L"folder";
    case EmptyKind::kUnsupported:
      return L"unsupported";
  }
  return L"none";
}

EmptyLayout ComputeEmptyLayout(const Rect& content, float dpi_scale,
                                const EmptyInputs& inputs) {
  const float s = Scale(dpi_scale);
  EmptyLayout layout;
  if (inputs.kind == EmptyKind::kNone || content.empty()) return layout;

  const float margin = metrics::kEmptyCardMargin * s;
  const float padding = metrics::kEmptyCardPadding * s;
  const float width = std::min(
      metrics::kEmptyCardMaxWidth * s,
      std::max(0.0f, content.width() - 2.0f * margin));
  if (width < 200.0f * s) return layout;

  const int lines =
      std::clamp(inputs.format_lines, 1,
                 static_cast<int>(metrics::kEmptyFormatsMaxLines));
  const auto height_for = [&](bool with_icon) {
    float h = padding * 2.0f;
    if (with_icon) {
      h += metrics::kEmptyIconSize * s + metrics::kEmptyIconGap * s;
    }
    h += metrics::kEmptyTitleHeight * s + metrics::kEmptyTitleGap * s;
    h += static_cast<float>(lines) * metrics::kEmptyFormatLineHeight * s;
    h += metrics::kEmptyMoreGap * s + metrics::kEmptyMoreHeight * s;
    if (inputs.has_note) {
      h += metrics::kEmptyNoteGap * s + metrics::kEmptyFormatLineHeight * s;
    }
    h += metrics::kEmptySectionGap * s + metrics::kEmptyButtonHeight * s;
    h += metrics::kEmptyFooterGap * s + metrics::kEmptyFooterHeight * s;
    return h;
  };

  const float available = std::max(0.0f, content.height());
  bool with_icon = true;
  float height = height_for(true);
  // A 480 DIP tall window with the pinned rail leaves ~328 DIP for the card;
  // dropping the icon keeps every control usable instead of clipping the
  // primary button (規劃/22/03 §3.1).
  if (height > available) {
    with_icon = false;
    height = height_for(false);
  }
  if (height > available) return layout;

  layout.compact = !with_icon;
  layout.format_lines = lines;
  const float left = content.left + (content.width() - width) * 0.5f;
  const float top = content.top + (available - height) * 0.5f;
  layout.card = Rect::FromXYWH(left, top, width, height);

  const float inner_left = layout.card.left + padding;
  const float inner_right = layout.card.right - padding;
  const float inner_width = std::max(0.0f, inner_right - inner_left);
  float y = layout.card.top + padding;

  if (with_icon) {
    const float icon = metrics::kEmptyIconSize * s;
    layout.icon = Rect::FromXYWH(layout.card.left + (width - icon) * 0.5f, y,
                                 icon, icon);
    y += icon + metrics::kEmptyIconGap * s;
  }
  layout.title = Rect::FromLTRB(inner_left, y, inner_right,
                                y + metrics::kEmptyTitleHeight * s);
  y += metrics::kEmptyTitleHeight * s + metrics::kEmptyTitleGap * s;

  const float line_h = metrics::kEmptyFormatLineHeight * s;
  layout.formats = Rect::FromLTRB(inner_left, y, inner_right,
                                  y + static_cast<float>(lines) * line_h);
  y += static_cast<float>(lines) * line_h;
  y += metrics::kEmptyMoreGap * s;
  layout.formats_more =
      Rect::FromLTRB(inner_left, y, inner_right,
                     y + metrics::kEmptyMoreHeight * s);
  y += metrics::kEmptyMoreHeight * s;

  if (inputs.has_note) {
    y += metrics::kEmptyNoteGap * s;
    layout.note = Rect::FromLTRB(inner_left, y, inner_right, y + line_h);
    y += line_h;
  }

  y += metrics::kEmptySectionGap * s;
  const float button_w = metrics::kEmptyButtonWidth * s;
  const float button_h = metrics::kEmptyButtonHeight * s;
  layout.primary = Rect::FromXYWH(layout.card.left + (width - button_w) * 0.5f,
                                  y, button_w, button_h);
  y += button_h + metrics::kEmptyFooterGap * s;

  const float footer_h = metrics::kEmptyFooterHeight * s;
  const float footer_w = std::min(metrics::kEmptyFooterWidth * s,
                                  std::max(0.0f, inner_width));
  const float footer_left = layout.card.left + (width - footer_w) * 0.5f;
  const float item_w = std::min(metrics::kEmptyFooterButtonWidth * s,
                                std::max(0.0f, (footer_w -
                                                metrics::kEmptyFooterButtonGap * s) *
                                                   0.5f));
  layout.open_folder =
      Rect::FromXYWH(footer_left, y, item_w, footer_h);
  layout.all_formats = Rect::FromXYWH(footer_left + footer_w - item_w, y, item_w,
                                      footer_h);

  layout.visible = true;
  return layout;
}

HitId HitTestEmpty(const EmptyLayout& layout, float x, float y) {
  if (!layout.visible) return HitId::kNone;
  if (layout.primary.Contains(x, y)) return HitId::kEmptyPrimary;
  if (layout.open_folder.Contains(x, y)) return HitId::kEmptyOpenFolder;
  if (layout.all_formats.Contains(x, y)) return HitId::kEmptyAllFormats;
  return HitId::kNone;
}

}  // namespace nv::ui
