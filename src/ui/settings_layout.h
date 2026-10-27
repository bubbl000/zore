#pragma once

#include "ui/rect.h"

#include <string>
#include <vector>

namespace nv::ui {

// The three settings pages (規劃/22/07 §4.2).
enum class SettingsPage {
  kGeneral = 0,
  kView,
  kAssociation,
};

inline constexpr int kSettingsPageCount = 3;

const wchar_t* SettingsPageName(SettingsPage page);

// Row template used by the pure layout. The app builds one spec per row; the
// layout module never interprets the setting behind it.
enum class SettingsRowKind {
  kGroup,   // group caption, no control
  kRadio,   // label + N mutually exclusive options
  kCheck,   // label + checkbox at the right edge
  kSlider,  // label + track + value text
  kNote,    // muted note line (association page)
};

struct SettingsRowSpec {
  SettingsRowKind kind = SettingsRowKind::kCheck;
  // kRadio: measured option widths in pixels, in display order.
  std::vector<float> option_widths;
  bool enabled = true;
};

struct SettingsRowLayout {
  SettingsRowKind kind = SettingsRowKind::kCheck;
  Rect row;
  Rect label;
  // kCheck: the 18 DIP box; kSlider: the track; kRadio: unused.
  Rect control;
  // kRadio option boxes, left to right (each includes its 18 DIP indicator).
  std::vector<Rect> options;
  // kSlider value text area ("60%").
  Rect value;
};

struct SettingsLayout {
  Rect client;
  Rect title;
  Rect close_button;
  Rect nav;
  Rect content;
  Rect body;   // full scrollable body, may extend past `content`
  Rect footer;
  Rect reset;
  Rect open_config;
  Rect close;
  Rect scrollbar;
  std::vector<Rect> nav_rows;
  std::vector<SettingsRowLayout> rows;
  float dpi_scale = 1.0f;
  float scroll = 0.0f;
  float max_scroll = 0.0f;
  float body_height = 0.0f;
};

struct SettingsLayoutInputs {
  SettingsPage page = SettingsPage::kGeneral;
  std::vector<SettingsRowSpec> rows;
  float scroll = 0.0f;
};

SettingsLayout ComputeSettingsLayout(const SettingsLayoutInputs& inputs,
                                     float dpi_scale, const Rect& client);

// Row under the pointer inside the scrolled viewport, or -1. Rows clipped by the
// viewport edges are still returned only when the point is inside `content`.
int SettingsRowAt(const SettingsLayout& layout, float x, float y);

// Navigation entry under the pointer (0..2), or -1.
int SettingsNavAt(const SettingsLayout& layout, float x, float y);

// --- Association page -------------------------------------------------------
// 39 extensions in a 4-column checkbox grid with its own scroll offset.
struct AssociationCell {
  Rect rect;
  int index = 0;
};

struct AssociationLayout {
  Rect header;
  Rect select_all;
  Rect list;
  Rect scrollbar;
  std::vector<AssociationCell> cells;
  Rect register_button;
  Rect unregister_button;
  Rect default_button;
  Rect status_primary;
  Rect status_secondary;
  float dpi_scale = 1.0f;
  float scroll = 0.0f;
  float max_scroll = 0.0f;
  float content_height = 0.0f;
  float row_height = 0.0f;
};

AssociationLayout ComputeAssociationLayout(float dpi_scale,
                                           const Rect& content, int count,
                                           int columns, float scroll);

int AssociationCellAt(const AssociationLayout& layout, float x, float y);

}  // namespace nv::ui
