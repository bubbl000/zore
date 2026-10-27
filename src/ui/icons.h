#pragma once

#include "ui/rect.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>

namespace nv::ui {

class UiContext;

// Vector icons on a 20x20 grid, drawn as stroked geometry (no bitmaps: the
// planning explicitly rejects the four-resolution PNG sets the reference app
// ships, 規劃/22/01 §5.2).
enum class IconId {
  kMenu,
  kFullscreen,
  kExitFullscreen,
  kMinimize,
  kMaximize,
  kRestore,
  kClose,
  kInfo,
  kFit,
  kActual,
  kRotateLeft,
  kRotateRight,
  kChevronDown,
  kCheck,
  kChevronLeft,
  kChevronRight,
  // Animation frame control (規劃/22/12).
  kPlay,
  kPause,
  kFolder,
  kImage,
  // Error card marker (規劃/22/11).
  kWarning,
};

class IconSet {
 public:
  void Shutdown();

  // `box` is the icon's target rectangle in client pixels; the 20x20 grid is
  // scaled to fit and centred. `stroke_px` is the stroke width in pixels.
  void Draw(ID2D1DeviceContext* context, UiContext& ui, IconId id,
            const Rect& box, uint32_t argb, float stroke_px);

 private:
  ID2D1PathGeometry* Geometry(ID2D1Factory1* factory, IconId id);
  bool EnsureStrokeStyle(ID2D1Factory1* factory);

  Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;  // cache owner
  std::map<IconId, Microsoft::WRL::ComPtr<ID2D1PathGeometry>> geometries_;
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> stroke_style_;
};

}  // namespace nv::ui
