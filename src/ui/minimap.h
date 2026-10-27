#pragma once

#include "ui/rect.h"

#include <cstdint>

// Forward declarations only: this header is part of the pure layout layer, so
// the pixel-free unit tests can include it without a D2D device.
struct ID2D1Bitmap;
struct ID2D1DeviceContext;

namespace nv::ui {

class UiContext;
struct ColorTokens;

// Bird's eye view (v3.0, 規劃/26 §2.4).
//
// The minimap is an in-window overlay (never a second top-level window): a small
// copy of the whole picture with the visible part of the content area outlined.
// It floats directly above the thumbnail rail (above the bottom bar when the rail
// is off, at the bottom edge in fullscreen) and is drawn above the rail but
// below the toast/popup layer.
//
// All geometry lives in this one pure function so drawing, hit-testing, dragging
// and --layout-dump read the same numbers. Display width/height are the rotated
// (post-view-rotation) source pixels, which is what the renderer draws; the
// caller passes them already swapped for an odd quarter turn.
struct MinimapInputs {
  Rect client;
  Rect content;
  Rect strip;
  Rect bottom_bar;
  bool fullscreen = false;
  float dpi_scale = 1.0f;
  // Rotated source size in pixels.
  uint32_t display_width = 0;
  uint32_t display_height = 0;
  // Client pixels per display pixel (the renderer's effective scale) and the
  // client-pixel centre of the picture (Renderer::ViewCenter).
  float view_scale = 1.0f;
  float view_center_x = 0.0f;
  float view_center_y = 0.0f;
};

struct MinimapLayout {
  bool visible = false;
  Rect panel;
  Rect canvas;
  // Where the photo is painted: the canvas, because the canvas was fitted to the
  // display aspect ratio. Kept separate so drawing never re-derives it.
  Rect image;
  // Visible part of the picture, in client pixels.
  Rect viewport;
  // Same region in display-space source pixels (what --layout-dump reports so a
  // rotation test can assert the mapping without pixels).
  Rect view_display;
  uint32_t display_width = 0;
  uint32_t display_height = 0;
  // Display pixels per canvas pixel: the drag conversion factor.
  float display_per_canvas = 1.0f;
};

MinimapLayout ComputeMinimapLayout(const MinimapInputs& inputs);

// Client-pixel delta for Renderer::PanBy() when the frame is dragged by
// (dx, dy) canvas pixels. Negative: dragging the frame right reveals more of the
// right side, i.e. the picture moves left. Never touches zoom, so a drag can
// never make the picture grow (the 0.9 pitfall this port avoids by construction).
void MinimapDragPan(const MinimapLayout& layout, float view_scale, float dx,
                    float dy, float* pan_x, float* pan_y);

// Display-space point under a client point inside the canvas. False when the
// point is outside the photo (used to distinguish "clicked the blank margin").
bool MinimapDisplayPoint(const MinimapLayout& layout, float x, float y,
                         float* display_x, float* display_y);

// Draws the panel, the photo (rotated with the view) and the viewport frame.
// `photo` may be null: the panel and frame are still drawn, which is what the
// device-loss / bake-failure fallback path shows.
void DrawMinimap(ID2D1DeviceContext* context, UiContext& ui,
                 const MinimapLayout& layout, ID2D1Bitmap* photo, int rotation,
                 const ColorTokens& colors);

}  // namespace nv::ui
