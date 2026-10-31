#include "ui/ui_root.h"

#include "ui/draw_util.h"
#include "ui/metrics.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace nv::ui {
namespace {

constexpr int kWeightRegular = 400;
constexpr int kWeightSemibold = 600;

LONG Pixels(float value) {
  return static_cast<LONG>(std::lround(value > 0.0f ? value : 0.0f));
}

}  // namespace

bool UiRoot::Create() {
  tokens_ = TokensFor(true, false);
  return ui_.Create();
}

void UiRoot::Shutdown() {
  chrome_.Shutdown();
  ui_.Shutdown();
}

void UiRoot::WarmUp() {
  if (!ui_.ready()) return;
  const int weights[] = {kWeightRegular, kWeightSemibold};
  for (const int weight : weights) {
    ui_.TextFormat(metrics::kTitleFontSize, weight, true, false);
    ui_.TextFormat(metrics::kTitleFontSize, weight, true, true);
    ui_.TextFormat(metrics::kBottomFontSize, weight, true, true);
  }
  // Prime the measure/layout cache with strings shaped like the real ones.
  ui_.MeasureText(L"ZoreView \u00D7 0000 \u00D7 0000 0000 KB 00 / 00 000%",
                  metrics::kTitleFontSize, kWeightRegular);
}

void UiRoot::SetTheme(bool dark_theme, bool high_contrast) {
  tokens_ = TokensFor(dark_theme, high_contrast);
  ui_.RefreshAccessibility();
}

void UiRoot::SetDpiScale(float scale) { ui_.SetDpiScale(scale); }

void UiRoot::SetClientSize(float width, float height) {
  client_width_ = std::max(0.0f, width);
  client_height_ = std::max(0.0f, height);
}

void UiRoot::SetHover(HitId id) { hover_ = id; }

void UiRoot::SetPressed(HitId id) { pressed_ = id; }

void UiRoot::ClearPointerState() {
  hover_ = HitId::kNone;
  pressed_ = HitId::kNone;
}

void UiRoot::SetEmptyFocus(int index) {
  empty_focus_ = std::clamp(index, 0, 2);
}

void UiRoot::CycleEmptyFocus(bool backward) {
  empty_focus_ = (empty_focus_ + (backward ? 2 : 1)) % 3;
}

const LayoutFrame& UiRoot::Update(const UiInputs& inputs) {
  inputs_ = inputs;
  const float dpi = ui_.dpi_scale();

  LayoutInputs layout_inputs;
  layout_inputs.has_image = inputs.has_image;
  layout_inputs.fullscreen = overlays_.fullscreen();
  layout_inputs.pill_visible = inputs.pill_visible;
  layout_inputs.toast_visible = overlays_.toast_visible();
  layout_inputs.strip_visible = inputs.strip_visible;
  layout_inputs.strip_shown = inputs.strip_shown;
  layout_inputs.strip_mode = inputs.strip_mode;
  layout_inputs.arrows_visible = inputs.arrows_visible;
  layout_inputs.arrows_opacity = inputs.arrows_opacity;
  layout_inputs.arrows_slide = inputs.arrows_slide;
  layout_inputs.percent_available = inputs.percent_available;
  layout_inputs.info_available = inputs.info_available;
  layout_inputs.rotate_available = inputs.rotate_available;
  layout_inputs.more_available = inputs.more_available;
  layout_inputs.drag_over = inputs.drag_over;
  layout_inputs.fs_reveal_opacity = inputs.fs_reveal_opacity;
  layout_inputs.minimap_visible = inputs.minimap.visible;
  layout_inputs.minimap_display_width = inputs.minimap.display_width;
  layout_inputs.minimap_display_height = inputs.minimap.display_height;
  layout_inputs.minimap_view_scale = inputs.minimap.view_scale;
  layout_inputs.minimap_view_center_x = inputs.minimap.view_center_x;
  layout_inputs.minimap_view_center_y = inputs.minimap.view_center_y;

  layout_ = ComputeLayout(layout_inputs, dpi,
                          Rect::FromXYWH(0.0f, 0.0f, client_width_,
                                         client_height_));

  // Hover tooltip of the thumbnail rail: only while the rail is interactive.
  // Geometry is a pure function so the flip/clamp rules are unit-tested without
  // a D3D device (規劃/22/08 §3.3).
  strip_tooltip_ = StripTooltipLayout{};
  if (layout_.strip_interactive && !inputs.strip_tooltip.empty() &&
      !layout_.strip.empty()) {
    const float text_width = ui_.MeasureText(inputs.strip_tooltip,
                                             metrics::kStripTooltipFontSize,
                                             kWeightRegular);
    strip_tooltip_ = ComputeStripTooltipLayout(
        inputs.strip_tooltip_anchor, layout_.client, text_width, dpi);
  }

  // Animation frame pill (規劃/22/12): one row on the bottom stack. The app
  // decides whether it has buttons; a file that could only be shown as its
  // first frame gets a centred label instead, so no dead control is drawn.
  anim_pill_ = AnimPillLayout{};
  anim_strings_ = AnimPillStrings{};
  if (layout_.pill_visible && !layout_.pill.empty() && inputs.anim.visible) {
    anim_pill_ = ComputeAnimPillLayout(layout_.pill, dpi,
                                       inputs.anim.show_buttons);
    anim_strings_.label = inputs.anim.label;
    anim_strings_.playing = inputs.anim.playing;
    anim_strings_.show_buttons = inputs.anim.show_buttons;
    anim_strings_.opacity = inputs.anim.opacity;
  }

  // Two-stage loading badge (規劃/22/13 §3.5): top-right of the image area. The
  // app owns the state and the fade; the UI measures the label and places the
  // capsule, which never participates in hit-testing.
  preview_badge_ = PreviewBadgeLayout{};
  preview_strings_ = PreviewBadgeStrings{};
  if (inputs.preview.visible && !inputs.preview.text.empty() &&
      !layout_.content.empty()) {
    const float text_width = ui_.MeasureText(
        inputs.preview.text, metrics::kPreviewBadgeFontSize, kWeightSemibold);
    preview_badge_ =
        ComputePreviewBadgeLayout(layout_.content, dpi, text_width);
    preview_strings_.text = inputs.preview.text;
    preview_strings_.failed = inputs.preview.failed;
    preview_strings_.opacity = inputs.preview.opacity;
  }

  std::array<float, kSegCount> widths{};
  const float title_font = metrics::kTitleFontSize;
  // A couple of pixels of slack per segment: DWrite trimming triggers on exact
  // fits (fractional advance widths), which would ellipsize text that fits.
  // An empty string measures 0: its segment is hidden instead of drawing a
  // separator with nothing between (the empty state has no dimensions yet).
  const float slack = 3.0f * dpi;
  const auto measure = [&](const std::wstring& text, int weight) {
    return text.empty() ? 0.0f
                        : ui_.MeasureText(text, title_font, weight) + slack;
  };
  widths[kSegFileName] = measure(inputs.strings.file_name, kWeightSemibold);
  widths[kSegDimension] = measure(inputs.strings.dimension, kWeightRegular);
  widths[kSegFileSize] = measure(inputs.strings.file_size, kWeightRegular);
  title_ = ComputeTitleLayout(layout_.title, dpi, true, widths);

  BottomInputs bottom_inputs;
  bottom_inputs.has_image = inputs.has_image;
  bottom_inputs.percent_available = inputs.percent_available;
  bottom_inputs.info_available = inputs.info_available;
  bottom_inputs.rotate_available = inputs.rotate_available;
  bottom_inputs.more_available = inputs.more_available;
  bottom_ = ComputeBottomLayout(layout_.bottom_bar, dpi, bottom_inputs);

  // One popup at a time, anchored to the part that owns it. The row list comes
  // from the app so the UI module stays free of command semantics.
  menu_items_.clear();
  menu_ = MenuLayout{};
  menu_hover_ = -1;
  const OverlayKind popup = overlays_.popup();
  if (popup != OverlayKind::kNone && !inputs.menu_items.empty()) {
    menu_items_ = inputs.menu_items;
    if (popup == OverlayKind::kContext) {
      // v3.0 (規劃/26 §2.9): a right-click menu hangs below/right of the pointer
      // and flips near an edge. Its bounds stop below the title bar: the menu is
      // painted in kTopPanels, and a panel overlapping the chrome would be
      // covered by the title bar drawn later.
      const float top = layout_.title.empty()
                            ? layout_.client.top
                            : std::max(layout_.client.top,
                                       layout_.title.bottom);
      const Rect bounds = Rect::FromLTRB(
          layout_.client.left, top, layout_.client.right, layout_.client.bottom);
      menu_ = ComputeMenuLayout(inputs.menu_anchor, bounds, menu_items_, dpi,
                                metrics::kMenuWidth,
                                MenuOpenSide::kBelowAnchor);
    } else if (!layout_.bottom_bar.empty()) {
      const bool percent = popup == OverlayKind::kPercent;
      menu_ = ComputeMenuLayout(
          bottom_.part[percent ? kPartPercent : kPartMore], layout_.client,
          menu_items_, dpi,
          percent ? metrics::kMenuWidthCompact : metrics::kMenuWidth);
    }
  }

  // Startup empty state: measure the format line against the card's inner
  // width, then lay the card out for the resulting line count. Two passes are
  // needed because the width comes from the card, whose height depends on the
  // line count (規劃/22/03 §3.3).
  empty_ = EmptyLayout{};
  empty_strings_ = EmptyStrings{};
  empty_strings_.drag_hint = inputs.empty.drag_hint;
  if (inputs.empty.visible) {
    const float padding = metrics::kEmptyCardPadding * dpi;
    EmptyInputs provisional;
    provisional.kind = inputs.empty.kind;
    provisional.format_lines = 1;
    provisional.has_note = inputs.empty.note;
    const EmptyLayout first =
        ComputeEmptyLayout(layout_.content, dpi, provisional);
    const float inner_width = std::max(
        0.0f, first.card.width() - 2.0f * padding);
    std::wstring lines[2];
    bool truncated = false;
    const int count = WrapFormatNames(
        ui_, inputs.empty.formats_prefix, inputs.empty.formats, inner_width,
        metrics::kEmptyFormatsFontSize, kWeightRegular, lines, &truncated);

    EmptyInputs measured = provisional;
    measured.format_lines = std::max(1, count);
    empty_ = ComputeEmptyLayout(layout_.content, dpi, measured);
    empty_strings_.title = inputs.empty.title;
    empty_strings_.format_lines[0] = lines[0];
    empty_strings_.format_lines[1] = lines[1];
    empty_strings_.format_line_count = empty_.format_lines;
    empty_strings_.format_truncated = truncated;
    empty_strings_.formats_more = inputs.empty.formats_more;
    empty_strings_.primary = inputs.empty.primary;
    empty_strings_.open_folder = inputs.empty.open_folder;
    empty_strings_.all_formats = inputs.empty.all_formats;
    empty_strings_.note = inputs.empty.note_text;
    if (!empty_.visible) {
      empty_focus_ = 0;
    } else {
      empty_focus_ = std::clamp(empty_focus_, 0, 2);
    }
  } else {
    empty_focus_ = 0;
  }

  // "All formats" modal card. Like the popup it is laid out only while open,
  // from the row count the app supplies.
  format_panel_ = FormatPanelLayout{};
  format_strings_ = FormatPanelStrings{};
  if (overlays_.modal() == OverlayKind::kFormats && !inputs.formats.rows.empty()) {
    format_panel_ = ComputeFormatPanelLayout(layout_.client, dpi,
                                             inputs.formats.rows.size(),
                                             format_first_row_);
    format_first_row_ = format_panel_.first_row;
    format_strings_.title = inputs.formats.title;
    format_strings_.footer = inputs.formats.footer;
    format_strings_.close = inputs.formats.close;
    format_strings_.rows = inputs.formats.rows;
  } else {
    // Reopening the card starts at the first family again.
    format_first_row_ = 0;
  }

  // --- Error card, confirmation card, loading state and toasts (22/11) -----
  error_card_ = ErrorCardLayout{};
  error_strings_ = ErrorCardStrings{};
  if (inputs.error.visible && !layout_.content.empty()) {
    std::vector<float> button_widths;
    button_widths.reserve(inputs.error.actions.size());
    for (const std::wstring& label : inputs.error.actions) {
      button_widths.push_back(
          ui_.MeasureText(label, metrics::kErrorBodyFontSize, kWeightRegular) +
          metrics::kErrorButtonPadding * 2.0f * dpi);
    }
    error_card_ = ComputeErrorCardLayout(layout_.content, dpi, button_widths,
                                         !inputs.error.detail.empty());
    error_strings_.title = inputs.error.title;
    error_strings_.message = inputs.error.message;
    error_strings_.detail = inputs.error.detail;
    error_strings_.buttons = inputs.error.actions;
    error_strings_.close = inputs.error.close;
  }

  confirm_card_ = ConfirmCardLayout{};
  confirm_strings_ = ConfirmCardStrings{};
  if (overlays_.IsModal(OverlayKind::kConfirm) && inputs.confirm.visible) {
    confirm_card_ = ComputeConfirmCardLayout(layout_.client, dpi,
                                             inputs.confirm.remember_visible);
    confirm_strings_.title = inputs.confirm.title;
    confirm_strings_.body = inputs.confirm.body;
    confirm_strings_.primary = inputs.confirm.primary;
    confirm_strings_.secondary = inputs.confirm.secondary;
    confirm_strings_.danger = inputs.confirm.danger;
    confirm_strings_.remember_visible = inputs.confirm.remember_visible;
    confirm_strings_.remember_checked = inputs.confirm.remember_checked;
    confirm_strings_.remember = inputs.confirm.remember;
  }

  // Rename overlay (V3.1-06 §06e): the confirmation card's geometry with an
  // input box; the app's EDIT child is placed over `rename_card_.input`.
  rename_card_ = RenameCardLayout{};
  rename_strings_ = RenameCardStrings{};
  if (overlays_.IsModal(OverlayKind::kRename) && inputs.rename.visible) {
    rename_card_ = ComputeRenameCardLayout(layout_.client, dpi);
    rename_strings_.title = inputs.rename.title;
    rename_strings_.hint = inputs.rename.hint;
    rename_strings_.ok = inputs.rename.ok;
    rename_strings_.cancel = inputs.rename.cancel;
    rename_strings_.placeholder = inputs.rename.placeholder;
    rename_strings_.error = inputs.rename.error;
  }

  loading_ = LoadingLayout{};
  loading_text_.clear();
  loading_phase_ = inputs.loading.progress;
  if (inputs.loading.visible && !layout_.content.empty()) {
    loading_ = ComputeLoadingLayout(layout_.content, dpi);
    loading_text_ = inputs.loading.text;
  }

  toast_rects_.clear();
  if (overlays_.toast_visible() && !layout_.toast.empty()) {
    const std::deque<ToastItem>& toasts = overlays_.toasts();
    size_t index = 0;
    for (auto it = toasts.rbegin(); it != toasts.rend() && index < kMaxToasts;
         ++it, ++index) {
      const float width =
          ui_.MeasureText(it->text, metrics::kBottomFontSize, kWeightRegular);
      toast_rects_.push_back(
          ComputeToastRect(layout_.toast, width, index, dpi));
    }
  }
  return layout_;
}

HitId UiRoot::HitTestTitleButtons(float x, float y) const {
  for (size_t i = 0; i < kTitleButtonCount; ++i) {
    if (title_.buttons[i].Contains(x, y)) {
      return static_cast<HitId>(static_cast<int>(HitId::kTitleMenu) +
                                static_cast<int>(i));
    }
  }
  return HitId::kNone;
}

HitId UiRoot::HitTest(float x, float y) const {
  // The confirmation card is a modal: it owns every click, including the scrim
  // (規劃/22/11 §4.3).
  if (overlays_.IsModal(OverlayKind::kConfirm) && confirm_card_.visible) {
    return HitTestConfirmCard(confirm_card_, x, y);
  }
  // The rename overlay is modal as well; its EDIT child owns the input box, so
  // this path only ever sees the buttons and the card surface.
  if (overlays_.IsModal(OverlayKind::kRename) && rename_card_.visible) {
    return HitTestRenameCard(rename_card_, x, y);
  }
  // The all-formats card is a modal: it owns the click inside its card and
  // swallows everything else (a modal never closes on an outside click,
  // 規劃/22/20 §3.5).
  if (overlays_.modal() == OverlayKind::kFormats) {
    return HitTestFormatPanel(format_panel_, x, y);
  }
  // Any other modal owns every click; a popup consumes clicks outside itself
  // so the main window never acts on them (規劃/22/20 §3.2). Inside an open
  // menu the click belongs to the menu surface, never to the chrome below it.
  if (overlays_.BlocksImageCommands()) return HitId::kNone;
  if (overlays_.popup() != OverlayKind::kNone) {
    return MenuContains(x, y) ? HitId::kMenuPanel : HitId::kNone;
  }
  // The error card is not modal: only its buttons hit, everything else falls
  // through to the image so panning/navigation keep working (規劃/22/11 坑 6).
  if (overlays_.error_open() && error_card_.visible) {
    const HitId hit = HitTestErrorCard(error_card_, x, y);
    if (hit != HitId::kNone) return hit;
  }
  // The info card is a separate top-level window (規劃/26 §2.1) and never
  // participates in this stack. The animation pill sits below the popups
  // (規劃/22/12 §3.1). Only its buttons hit; the gaps fall through to the image.
  if (const HitId hit = HitTestAnimPill(anim_pill_, x, y);
      hit != HitId::kNone) {
    return hit;
  }
  return HitTestFrame(layout_, title_, bottom_, empty_, x, y);
}

int UiRoot::ToastAt(float x, float y) const {
  for (size_t i = 0; i < toast_rects_.size(); ++i) {
    if (toast_rects_[i].Contains(x, y)) return static_cast<int>(i);
  }
  return -1;
}

bool UiRoot::MenuContains(float x, float y) const {
  return menu_.visible && menu_.panel.Contains(x, y);
}

int UiRoot::MenuRowAt(float x, float y) const {
  return nv::ui::MenuRowAt(menu_, x, y);
}

int UiRoot::MenuItemAt(float x, float y) const {
  return nv::ui::MenuItemAt(menu_, menu_items_, x, y);
}

void UiRoot::ScrollFormats(int rows) {
  if (rows == 0 || overlays_.modal() != OverlayKind::kFormats) return;
  const long long next =
      std::clamp<long long>(static_cast<long long>(format_first_row_) + rows, 0,
                            static_cast<long long>(format_panel_.max_first_row));
  format_first_row_ = static_cast<size_t>(next);
}

bool UiRoot::UpdateMenuHover(float x, float y) {
  if (!menu_.visible) {
    if (menu_hover_ == -1) return false;
    menu_hover_ = -1;
    return true;
  }
  const int row = MenuItemAt(x, y);
  if (row == menu_hover_) return false;
  menu_hover_ = row;
  return true;
}

bool UiRoot::StepMenuHover(int delta) {
  if (!menu_.visible || menu_items_.empty()) return false;
  const int next = NextSelectableRow(menu_items_, menu_hover_, delta);
  if (next == menu_hover_) return false;
  menu_hover_ = next;
  return true;
}

EscapeAction UiRoot::HandleEscape() { return overlays_.Escape(); }

ChromeState UiRoot::MakeChromeState() {
  ChromeState state;
  state.app_logo = app_logo_.Get(draw_context_);
  state.layout = &layout_;
  state.title = &title_;
  state.bottom = &bottom_;
  state.menu = &menu_;
  state.menu_items = &menu_items_;
  state.colors = &tokens_.color;
  state.strings = &inputs_.strings;
  state.hover = hover_;
  state.pressed = pressed_;
  state.menu_hover = menu_hover_;
  state.active = inputs_.window_active;
  state.has_image = inputs_.has_image;
  state.maximized = inputs_.maximized;
  state.fullscreen = overlays_.fullscreen();
  return state;
}

void UiRoot::DrawToasts(ID2D1DeviceContext* context) {
  if (context == nullptr || !layout_.toast_visible || toast_rects_.empty()) {
    return;
  }
  // Newest first: the drawing helper stacks rects[0] at the bottom of the
  // bottom stack (規劃/22/11 §4.1).
  std::vector<ToastItem> items;
  const std::deque<ToastItem>& toasts = overlays_.toasts();
  items.reserve(toasts.size());
  for (auto it = toasts.rbegin(); it != toasts.rend() && items.size() < kMaxToasts;
       ++it) {
    items.push_back(*it);
  }
  nv::ui::DrawToasts(context, ui_, items, toast_rects_, tokens_.color, -1);
}

void UiRoot::Draw(ID2D1DeviceContext* context, nv::render::OverlayLayer layer) {
  draw_context_ = context;
  if (context == nullptr) return;
  switch (layer) {
    case nv::render::OverlayLayer::kImageArea:
      // V3.1-09: the empty card is painted first and the drag wash *over* it, so
      // the highlight is the topmost layer even when the "drop an image here"
      // card is on screen. It used to sit under the card, which made a drag feel
      // like it was highlighting the window only (user report).
      // A failed load shows the error card instead of the startup card: the
      // failure is the state, not "nothing was opened" (規劃/22/11 §4.2).
      if (empty_.visible && !error_card_.visible) {
        DrawEmptyCard(context, ui_, chrome_.icons(), empty_,
                      empty_strings_, inputs_.empty.kind, tokens_.color,
                      ui_.dpi_scale(), hover_, pressed_, empty_focus_,
                      inputs_.drag_over);
      }
      if (inputs_.drag_over) {
        DrawDragWash(context, ui_, layout_.content, tokens_.color);
      }
      if (loading_.visible) {
        DrawLoadingState(context, ui_, loading_, loading_text_, loading_phase_,
                         tokens_.color);
      }
      // The badge sits over the image but below the error card: a failed full
      // decode keeps the preview visible and adds the card on top (規劃/22/13
      // §3.2 step 4).
      DrawPreviewBadge(context, ui_, preview_badge_, preview_strings_,
                       tokens_.color, tokens_.high_contrast, ui_.dpi_scale());
      if (error_card_.visible) {
        DrawErrorCard(context, ui_, chrome_.icons(), error_card_,
                      error_strings_, tokens_.color, ui_.dpi_scale(), hover_,
                      pressed_, inputs_.error.default_action);
      }
      chrome_.DrawImageArrows(context, ui_, MakeChromeState());
      if (inputs_.drag_over) {
        DrawDragHint(context, ui_, chrome_.icons(), layout_,
                     inputs_.empty.drag_hint, tokens_.color);
      }
      break;
    case nv::render::OverlayLayer::kBottomPanels:
      // Tooltip sits above the rail but below the bottom bar (規劃/22/08 §3.3).
      DrawStripTooltip(context, ui_, strip_tooltip_, inputs_.strip_tooltip,
                       tokens_.color, ui_.dpi_scale());
      chrome_.DrawBottomBar(context, ui_, MakeChromeState());
      break;
    case nv::render::OverlayLayer::kTopPanels:
      // Bottom stack first, then the popup menu, then the toasts, then the modal
      // cards on top of everything in this layer (規劃/22/02 §3.3, 22/20 §3.1).
      // V3.1-06 (§06d): toasts moved *after* the menu — a copy confirmation
      // belongs to the frame after the menu closed, not underneath it.
      DrawAnimPill(context, ui_, chrome_.icons(), anim_pill_, anim_strings_,
                   tokens_.color, tokens_.high_contrast, ui_.dpi_scale(), hover_,
                   pressed_);
      chrome_.DrawMenu(context, ui_, MakeChromeState());
      DrawToasts(context);
      if (overlays_.modal() == OverlayKind::kFormats) {
        DrawFormatPanel(context, ui_, format_panel_, format_strings_,
                        tokens_.color, ui_.dpi_scale(), hover_, pressed_);
      }
      if (confirm_card_.visible) {
        DrawConfirmCard(context, ui_, chrome_.icons(), confirm_card_,
                        confirm_strings_, tokens_.color, ui_.dpi_scale(),
                        hover_, pressed_);
      }
      if (rename_card_.visible) {
        // The EDIT child owns the text; the card draws only the frame and labels.
        DrawRenameCard(context, ui_, rename_card_, rename_strings_, tokens_.color,
                       ui_.dpi_scale(), hover_, pressed_, false);
      }
      break;
    case nv::render::OverlayLayer::kChrome:
      chrome_.DrawTitleBar(context, ui_, MakeChromeState());
      // v3.0 (規劃/26 §2.7): the reveal is the only chrome in fullscreen, so it
      // is drawn on the same layer and after the (empty) title bar.
      chrome_.DrawFullscreenReveal(context, ui_, MakeChromeState());
      // V3.1-06 (§06a): the whole-client drag border is gone. Only the canvas
      // highlights on a drag-over; the title bar/bottom bar/frame must not
      // change a single pixel (asserted by run_empty_state_check).
      break;
  }
}

void UiRoot::PaintGdiBackground(HDC dc) const {
  if (dc == nullptr) return;
  const auto fill = [dc](const Rect& rect, uint32_t argb) {
    if (rect.empty()) return;
    HBRUSH brush = CreateSolidBrush(ArgbToColorRef(argb));
    if (brush == nullptr) return;
    RECT native{Pixels(rect.left), Pixels(rect.top), Pixels(rect.right),
                Pixels(rect.bottom)};
    FillRect(dc, &native, brush);
    DeleteObject(brush);
  };
  fill(layout_.title, tokens_.color.bg_panel);
  // Only a pinned rail owns a band in the GDI first frame; the auto rail is an
  // overlay that starts faded out and must not leave an empty strip band.
  if (layout_.strip_counts_in_content) {
    fill(layout_.strip, tokens_.color.bg_canvas);
  }
  fill(layout_.bottom_bar, tokens_.color.bg_panel);

  // Simplified empty card for the GDI first frame: same rectangles as the D2D
  // card, only the box, the title and the primary button (規劃/22/03 §6). The
  // window therefore shows content immediately instead of a flat rectangle.
  if (!empty_.visible) return;
  fill(empty_.card, tokens_.color.bg_panel);
  fill(empty_.primary, tokens_.color.accent);
  if (empty_strings_.title.empty()) return;

  const float s = layout_.dpi_scale > 0.0f ? layout_.dpi_scale : 1.0f;
  HFONT font = CreateFontW(
      -static_cast<int>(Pixels(metrics::kEmptyTitleFontSize * s)), 0, 0, 0,
      FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  if (font != nullptr) {
    HGDIOBJ previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ArgbToColorRef(tokens_.color.text_primary));
    RECT native{Pixels(empty_.title.left), Pixels(empty_.title.top),
                Pixels(empty_.title.right), Pixels(empty_.title.bottom)};
    DrawTextW(dc, empty_strings_.title.c_str(),
              static_cast<int>(empty_strings_.title.size()), &native,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, previous);
    DeleteObject(font);
  }
}

}  // namespace nv::ui
