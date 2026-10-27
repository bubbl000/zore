#pragma once

namespace nv::render {

// The renderer draws one overlay layer at a time in this order. The UI code
// maps its panels onto these layers, so "the title bar is painted last" is
// enforced by the renderer instead of by the order of calls in main.cpp.
enum class OverlayLayer {
  kImageArea = 0,   // arrows, empty-state card, error card
  kBottomPanels,    // thumbnail rail, bottom bar
  kTopPanels,       // more/percent popups, info panel, toasts, modals
  kChrome,          // title bar
};

}  // namespace nv::render
