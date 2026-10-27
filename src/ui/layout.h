#pragma once

#include "ui/hit.h"
#include "ui/metrics.h"
#include "ui/minimap.h"
#include "ui/rect.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nv::ui {

// How the thumbnail rail participates in layout (規劃/22/08).
//   pinned - always visible and subtracted from the image content rectangle
//   auto   - overlays the image, fades in/out; the content rectangle never moves
//   off    - hidden
enum class StripMode { kPinned, kAuto, kOff };

const wchar_t* StripModeName(StripMode mode);
bool ParseStripMode(std::wstring_view text, StripMode* mode);

// Everything ComputeLayout needs. Geometry only: no filenames, no images.
struct LayoutInputs {
  bool has_image = false;
  bool fullscreen = false;

  bool pill_visible = false;
  bool toast_visible = false;
  // An OLE drag hovers the window: the whole client gets an accent wash and a
  // hint pill near the top of the content (規劃/22/03 §4.2).
  bool drag_over = false;

  bool strip_visible = false;  // the app has a non-empty folder list
  bool strip_shown = false;    // auto mode only: the fade is currently visible
  StripMode strip_mode = StripMode::kPinned;
  // Image-area previous/next circles. False without an image or with a single
  // file, so the rectangles stay empty and nothing can be clicked (22/09 §5.1).
  bool arrows_visible = false;

  // Capabilities. A control that is not implemented yet (S1/S2) is still laid
  // out, but it is drawn disabled and skipped by hit-testing.
  bool percent_available = false;
  bool info_available = false;
  bool rotate_available = false;
  bool more_available = false;

  // v3.0 (規劃/26 §2.7): the fullscreen reveal strip. The app owns the 3 s idle
  // timer and the 160 ms fade; the layout only positions the two buttons and
  // carries the current opacity.
  float fs_reveal_opacity = 0.0f;

  // v3.0 (規劃/26 §2.4): bird's eye view. The app supplies the renderer's
  // post-rotation display size, effective scale and picture centre; the layout
  // derives the panel/canvas/photo/viewport rectangles from them.
  bool minimap_visible = false;
  uint32_t minimap_display_width = 0;
  uint32_t minimap_display_height = 0;
  float minimap_view_scale = 1.0f;
  float minimap_view_center_x = 0.0f;
  float minimap_view_center_y = 0.0f;
};

// All rectangles are client pixels. Drawing and hit-testing both read this one
// frame, so a control can never be visible somewhere it is not clickable.
struct LayoutFrame {
  Rect client;
  Rect title;
  Rect content;
  Rect bottom_bar;
  Rect strip;
  Rect pill;
  Rect toast;
  Rect arrows_left;
  Rect arrows_right;
  Rect drag_hint;
  // Fullscreen reveal buttons (v3.0, 規劃/26 §2.7). Empty unless fullscreen and
  // the reveal is visible; the hit test reads `fullscreen_reveal_visible`.
  Rect fs_exit;
  Rect fs_close;
  // Bird's eye view (v3.0, 規劃/26 §2.4). Same frame as everything else, so the
  // four rectangles reported by --layout-dump are exactly what is painted and
  // hit-tested.
  MinimapLayout minimap;

  bool strip_counts_in_content = false;
  // True when the rail is painted and may be clicked. In auto mode the
  // rectangle always exists (it hosts the 8 DIP reveal hot zone), but the rail
  // only owns clicks while it is faded in (規劃/22/08 坑 3).
  bool strip_interactive = false;
  bool pill_visible = false;
  bool toast_visible = false;
  bool drag_over = false;
  bool arrows_visible = false;
  bool show_title = true;
  bool show_bottom = true;
  bool fullscreen = false;
  bool fullscreen_reveal_visible = false;
  float fs_reveal_opacity = 0.0f;
  float dpi_scale = 1.0f;
};

LayoutFrame ComputeLayout(const LayoutInputs& inputs, float dpi_scale,
                          Rect client);

// --- Startup empty state (規劃/22/03) --------------------------------------

// Which empty state the content area shows. kNone means an image (or a decode
// in flight) owns the area and the card must not be drawn (坑 5/6).
enum class EmptyKind {
  kNone = 0,
  kEmpty,        // nothing opened yet
  kFolder,       // a folder was opened but contains no supported image
  kUnsupported,  // the last drop was not an image; transient note
};

const wchar_t* EmptyKindName(EmptyKind kind);

struct EmptyInputs {
  EmptyKind kind = EmptyKind::kNone;
  // Format line count, measured by UiRoot (1..kEmptyFormatsMaxLines).
  int format_lines = 1;
  // Whether a second explanatory line (unsupported / folder hint) is drawn.
  bool has_note = false;
};

// Card rectangles. Everything is centred inside `content`, which already
// excludes the title bar, the bottom bar and a pinned rail.
struct EmptyLayout {
  Rect card;
  Rect icon;
  Rect title;
  Rect formats;
  Rect formats_more;
  Rect note;
  Rect primary;
  Rect open_folder;
  Rect all_formats;

  int format_lines = 0;
  bool visible = false;
  // Small windows lose the decorative icon instead of overflowing the card.
  bool compact = false;
};

EmptyLayout ComputeEmptyLayout(const Rect& content, float dpi_scale,
                                const EmptyInputs& inputs);

// Buttons only; returns kNone elsewhere.
HitId HitTestEmpty(const EmptyLayout& layout, float x, float y);

// --- Title bar -------------------------------------------------------------

// v3.0 (規劃/26 §2.2): the title bar keeps only the three identity segments.
// The index and the zoom percentage live in the bottom bar, where they are
// always visible; repeating them here made the caption unreadable on small
// windows.
enum TitleSegment {
  kSegFileName = 0,
  kSegDimension,
  kSegFileSize,
  kSegCount,
};

struct TitleLayout {
  Rect bar;
  Rect icon;
  Rect text_area;
  std::array<Rect, kTitleButtonCount> buttons{};
  std::array<Rect, kSegCount> segment{};
  std::array<bool, kSegCount> segment_visible{};
  float button_width = 0.0f;
  bool show_icon = false;
  bool compact = false;
};

// `text_widths` are measured pixels (DIP * dpi_scale) for the three segments.
// Narrow windows drop segments in reverse priority order: file size, dimension.
// The file name is never dropped; it is ellipsized instead.
TitleLayout ComputeTitleLayout(const Rect& bar, float dpi_scale, bool show_icon,
                               const std::array<float, kSegCount>& text_widths);

// --- Bottom bar ------------------------------------------------------------

enum BottomPart {
  kPartInfo = 0,
  kPartPercent,
  kPartIndex,
  kPartFit,
  kPartActual,
  kPartRotateLeft,
  kPartRotateRight,
  kPartMore,
  kPartCount,
};

struct BottomInputs {
  bool has_image = false;
  bool percent_available = false;
  bool info_available = false;
  bool rotate_available = false;
  bool more_available = false;
};

struct BottomLayout {
  Rect bar;
  std::array<Rect, kPartCount> part{};
  std::array<bool, kPartCount> enabled{};
  float dpi_scale = 1.0f;
};

BottomLayout ComputeBottomLayout(const Rect& bar, float dpi_scale,
                                 const BottomInputs& inputs);

// --- Hit testing -----------------------------------------------------------

// Ordered hit stack for one frame: title buttons, bottom bar parts, empty-state
// buttons, thumbnail strip, image area. Modal/popup layers are handled by
// OverlayStack before this is consulted.
HitId HitTestFrame(const LayoutFrame& frame, const TitleLayout& title,
                   const BottomLayout& bottom, const EmptyLayout& empty,
                   float x, float y);

}  // namespace nv::ui
