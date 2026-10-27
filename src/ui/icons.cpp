#include "ui/icons.h"

#include "ui/ui_context.h"

#include <algorithm>
#include <cmath>

namespace nv::ui {
namespace {

constexpr float kGrid = 20.0f;

// Thin wrapper over ID2D1GeometrySink so an icon reads as a list of strokes.
struct Builder {
  ID2D1GeometrySink* sink = nullptr;

  void Move(float x, float y) {
    sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
  }
  void Line(float x, float y) { sink->AddLine(D2D1::Point2F(x, y)); }
  void Arc(float x, float y, float rx, float ry, bool clockwise, bool large) {
    D2D1_ARC_SEGMENT arc{};
    arc.point = D2D1::Point2F(x, y);
    arc.size = D2D1::SizeF(rx, ry);
    arc.rotationAngle = 0.0f;
    arc.sweepDirection = clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                   : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    arc.arcSize = large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
    sink->AddArc(&arc);
  }
  void End(bool closed = false) {
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
  }

  // Polyline through the given points; the first point starts the figure.
  void Poly(std::initializer_list<D2D1_POINT_2F> points, bool closed = false) {
    bool first = true;
    for (const D2D1_POINT_2F& point : points) {
      if (first) {
        Move(point.x, point.y);
        first = false;
      } else {
        Line(point.x, point.y);
      }
    }
    End(closed);
  }
};

void Circle(Builder& b, float cx, float cy, float r) {
  b.Move(cx + r, cy);
  b.Arc(cx - r, cy, r, r, true, false);
  b.Arc(cx + r, cy, r, r, true, false);
  b.End(true);
}

void BuildIcon(Builder& b, IconId id) {
  switch (id) {
    case IconId::kMenu:
      b.Poly({{4, 6}, {16, 6}});
      b.Poly({{4, 10}, {16, 10}});
      b.Poly({{4, 14}, {16, 14}});
      break;

    case IconId::kMinimize:
      b.Poly({{5, 13}, {15, 13}});
      break;

    case IconId::kMaximize:
      b.Poly({{5, 5}, {15, 5}, {15, 15}, {5, 15}}, true);
      break;

    case IconId::kRestore:
      b.Poly({{7, 4}, {16, 4}, {16, 13}}, false);
      b.Poly({{4, 7}, {13, 7}, {13, 16}, {4, 16}}, true);
      break;

    case IconId::kClose:
      b.Poly({{5, 5}, {15, 15}});
      b.Poly({{15, 5}, {5, 15}});
      break;

    case IconId::kFullscreen:
      b.Poly({{8, 3}, {3, 3}, {3, 8}});
      b.Poly({{12, 3}, {17, 3}, {17, 8}});
      b.Poly({{3, 12}, {3, 17}, {8, 17}});
      b.Poly({{17, 12}, {17, 17}, {12, 17}});
      break;

    case IconId::kExitFullscreen:
      b.Poly({{3, 8}, {8, 8}, {8, 3}});
      b.Poly({{17, 8}, {12, 8}, {12, 3}});
      b.Poly({{8, 17}, {8, 12}, {3, 12}});
      b.Poly({{12, 17}, {12, 12}, {17, 12}});
      break;

    case IconId::kFit:
      // Four arrows pointing outwards from the centre.
      b.Poly({{9, 9}, {4, 4}});
      b.Poly({{4, 4}, {4, 8}});
      b.Poly({{4, 4}, {8, 4}});
      b.Poly({{11, 9}, {16, 4}});
      b.Poly({{16, 4}, {12, 4}});
      b.Poly({{16, 4}, {16, 8}});
      b.Poly({{9, 11}, {4, 16}});
      b.Poly({{4, 16}, {4, 12}});
      b.Poly({{4, 16}, {8, 16}});
      b.Poly({{11, 11}, {16, 16}});
      b.Poly({{16, 16}, {12, 16}});
      b.Poly({{16, 16}, {16, 12}});
      break;

    case IconId::kActual:
      b.Poly({{5, 5}, {15, 5}, {15, 15}, {5, 15}}, true);
      b.Poly({{10, 7}, {10, 13}});
      break;

    case IconId::kRotateLeft:
      // Two arcs pinned to the circle centred (10,10). A single large arc only
      // fixes the endpoints and radii, so Direct2D is free to pick the other
      // candidate centre (16,4); the glyph then sat ~4.75 DIP up and outwards
      // (v3.0, 規劃/26 §2.3). The diagonal point (4,10) makes the centre unique.
      b.Move(16, 10);
      b.Arc(4, 10, 6, 6, true, false);    // right -> bottom -> left
      b.Arc(10, 4, 6, 6, true, false);    // left -> top
      b.End();
      b.Poly({{10, 4}, {13, 4}});
      b.Poly({{10, 4}, {10, 7}});
      break;

    case IconId::kRotateRight:
      b.Move(4, 10);
      b.Arc(16, 10, 6, 6, false, false);  // left -> bottom -> right
      b.Arc(10, 4, 6, 6, false, false);   // right -> top
      b.End();
      b.Poly({{10, 4}, {7, 4}});
      b.Poly({{10, 4}, {10, 7}});
      break;

    case IconId::kChevronDown:
      b.Poly({{6, 8}, {10, 12}, {14, 8}});
      break;

    case IconId::kChevronLeft:
      b.Poly({{12, 5}, {7, 10}, {12, 15}});
      break;

    case IconId::kChevronRight:
      b.Poly({{8, 5}, {13, 10}, {8, 15}});
      break;

    case IconId::kCheck:
      b.Poly({{4, 10}, {8, 14}, {16, 6}});
      break;

    case IconId::kPlay:
      // Outline triangle, consistent with the stroked icon set.
      b.Poly({{7, 4.5f}, {15.5f, 10}, {7, 15.5f}}, true);
      break;

    case IconId::kPause:
      b.Poly({{8, 5}, {8, 15}});
      b.Poly({{12, 5}, {12, 15}});
      break;

    case IconId::kInfo: {
      Circle(b, 10, 10, 7);
      b.Poly({{10, 9}, {10, 14}});
      Circle(b, 10, 6.2f, 0.7f);
      break;
    }

    case IconId::kFolder:
      b.Poly({{3, 6}, {8, 6}, {9.5f, 8}, {17, 8}, {17, 15}, {3, 15}}, true);
      break;

    case IconId::kImage:
      b.Poly({{3, 4}, {17, 4}, {17, 16}, {3, 16}}, true);
      Circle(b, 7, 8, 1.3f);
      b.Poly({{4, 15}, {9, 9.5f}, {12, 12.5f}, {14, 10.5f}, {16.5f, 15}});
      break;

    case IconId::kWarning:
      // Triangle with an exclamation mark: the error card's only decoration.
      b.Poly({{10, 3}, {17.5f, 16}, {2.5f, 16}}, true);
      b.Poly({{10, 7.5f}, {10, 11.5f}});
      Circle(b, 10, 14, 0.7f);
      break;
  }
}

}  // namespace

void IconSet::Shutdown() {
  geometries_.clear();
  stroke_style_.Reset();
  factory_.Reset();
}

bool IconSet::EnsureStrokeStyle(ID2D1Factory1* factory) {
  if (stroke_style_ && factory_.Get() == factory) return true;
  factory_ = factory;
  geometries_.clear();
  stroke_style_.Reset();
  if (factory == nullptr) return false;

  D2D1_STROKE_STYLE_PROPERTIES properties{};
  properties.startCap = D2D1_CAP_STYLE_ROUND;
  properties.endCap = D2D1_CAP_STYLE_ROUND;
  properties.dashCap = D2D1_CAP_STYLE_ROUND;
  properties.lineJoin = D2D1_LINE_JOIN_ROUND;
  properties.miterLimit = 1.0f;
  return SUCCEEDED(
      factory->CreateStrokeStyle(properties, nullptr, 0, &stroke_style_));
}

ID2D1PathGeometry* IconSet::Geometry(ID2D1Factory1* factory, IconId id) {
  auto it = geometries_.find(id);
  if (it != geometries_.end()) return it->second.Get();

  Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
  if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry) return nullptr;
  Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(geometry->Open(&sink)) || !sink) return nullptr;

  sink->SetFillMode(D2D1_FILL_MODE_WINDING);
  Builder builder{sink.Get()};
  BuildIcon(builder, id);
  if (FAILED(sink->Close())) return nullptr;

  auto inserted = geometries_.emplace(id, std::move(geometry));
  return inserted.first->second.Get();
}

void IconSet::Draw(ID2D1DeviceContext* context, UiContext& ui, IconId id,
                   const Rect& box, uint32_t argb, float stroke_px) {
  if (context == nullptr || box.empty()) return;
  ID2D1Factory1* factory = ui.d2d_factory();
  if (factory == nullptr || !EnsureStrokeStyle(factory)) return;

  ID2D1PathGeometry* geometry = Geometry(factory, id);
  if (geometry == nullptr) return;
  ID2D1SolidColorBrush* brush = ui.Brush(context, argb);
  if (brush == nullptr) return;

  const float size = std::min(box.width(), box.height());
  const float scale = size / kGrid;
  if (scale <= 0.0f) return;

  const float offset_x = box.left + (box.width() - size) * 0.5f;
  const float offset_y = box.top + (box.height() - size) * 0.5f;

  // D2D uses row vectors, so the left-hand matrix is applied first: scale the
  // 20x20 grid about the origin, then translate it onto the box. The reverse
  // order scales the offset as well and paints the icon far away from its box
  // - invisible whenever the icon is exactly 20 px (scale 1), which is why it
  // went unnoticed until a 17.5 px menu check exposed it.
  context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                        D2D1::Matrix3x2F::Translation(offset_x, offset_y));
  context->DrawGeometry(geometry, brush,
                        std::max(0.1f, stroke_px) / scale, stroke_style_.Get());
  context->SetTransform(D2D1::Matrix3x2F::Identity());
}

}  // namespace nv::ui
