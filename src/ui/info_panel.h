#pragma once

// Image information card: geometry, hit-testing and the formatted line model.
//
// v3.0 (規劃/26 §2.1) moved the card out of the main window into its own
// top-level window (ui/info_window.*), so this module no longer draws. It keeps
// the pure layout ("where is every line / button") and the strings the app
// fills in, which the window renders and hit-tests.
//
// A line is either a group header or a label/value row; both use the same slot
// height so the panel height stays a pure function of the line count.

#include "ui/hit.h"
#include "ui/rect.h"

#include <string>
#include <vector>

namespace nv::ui {

struct InfoLine {
  std::wstring label;
  std::wstring value;
  bool header = false;
};

struct InfoPanelStrings {
  std::wstring title;
  std::wstring copy_all;
  std::wstring close;
  std::vector<InfoLine> lines;
};

struct InfoPanelLayout {
  Rect panel;
  Rect header;
  Rect close;
  Rect body;
  Rect footer;
  Rect copy_all;
  Rect scrollbar_track;
  Rect scrollbar_thumb;
  // One rectangle per line, in panel coordinates. Lines outside the body are
  // still laid out (drawing clips them) so hit-testing and drawing agree.
  std::vector<Rect> line;

  float content_height = 0.0f;
  float max_scroll = 0.0f;
  float scroll = 0.0f;
  float dpi_scale = 1.0f;
  int first_visible = 0;
  int last_visible = -1;
  bool visible = false;
  bool scrollable = false;
};

// Client size (physical pixels) of the standalone info window. The width is the
// card width; the height follows the row count and is capped to a share of the
// monitor work area, which is what makes the body scroll instead of growing off
// screen. A zero work-area height means "no cap" (used by the unit tests).
struct InfoWindowSize {
  float width = 0.0f;
  float height = 0.0f;
};

InfoWindowSize ComputeInfoWindowSize(size_t row_count, float dpi_scale,
                                     float work_area_width,
                                     float work_area_height);

// `panel` is the window's client rectangle, `copy_all_width` the measured button
// width in pixels, `scroll` the persistent offset (clamped here).
InfoPanelLayout ComputeInfoPanelLayout(const Rect& panel, float dpi_scale,
                                       const std::vector<InfoLine>& lines,
                                       float scroll, float copy_all_width);

// Only the two buttons are hit ids; the panel body returns kInfoPanel so a
// click inside it never drags the window (規劃/26 §2.1).
HitId HitTestInfoPanel(const InfoPanelLayout& layout, float x, float y);

// Zero-based line index under the point, or -1 (headers, footer, outside).
// Used by the context menu's "copy this value" (規劃/22/06 §4.3).
int InfoLineAt(const InfoPanelLayout& layout, const std::vector<InfoLine>& lines,
               float x, float y);

}  // namespace nv::ui
