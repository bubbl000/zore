#pragma once

#include <algorithm>

namespace nv::ui {

// Pixel-space rectangle in client coordinates.
//
// Deliberately independent of Direct2D: the whole layout is a pure function of
// (state, DPI, client size), so unit tests can assert every rectangle without a
// device, a window or a GPU. Drawing converts to D2D1_RECT_F at the edge.
struct Rect {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;

  static Rect FromLTRB(float l, float t, float r, float b) {
    return Rect{l, t, r, b};
  }
  static Rect FromXYWH(float x, float y, float w, float h) {
    return Rect{x, y, x + w, y + h};
  }

  float width() const { return right - left; }
  float height() const { return bottom - top; }
  bool empty() const { return right <= left || bottom <= top; }

  bool Contains(float x, float y) const {
    return x >= left && x < right && y >= top && y < bottom;
  }

  Rect Offset(float dx, float dy) const {
    return Rect{left + dx, top + dy, right + dx, bottom + dy};
  }
  Rect Inset(float dx, float dy) const {
    return Rect{left + dx, top + dy, right - dx, bottom - dy};
  }
  Rect Inset(float l, float t, float r, float b) const {
    return Rect{left + l, top + t, right - r, bottom - b};
  }
  Rect Intersect(const Rect& other) const {
    return Rect{std::max(left, other.left), std::max(top, other.top),
                std::min(right, other.right), std::min(bottom, other.bottom)};
  }

  bool operator==(const Rect&) const = default;
};

// Tolerance used by layout tests: every rectangle is a scaled DIP value, so the
// comparisons must survive float multiplication (125% of 76 = 95 exactly, but
// 150% of 76.5 is not representable).
inline bool NearEqual(float a, float b, float epsilon = 0.02f) {
  return (a > b ? a - b : b - a) <= epsilon;
}

inline bool NearEqual(const Rect& a, const Rect& b,
                      float epsilon = 0.02f) {
  return NearEqual(a.left, b.left, epsilon) &&
         NearEqual(a.top, b.top, epsilon) &&
         NearEqual(a.right, b.right, epsilon) &&
         NearEqual(a.bottom, b.bottom, epsilon);
}

}  // namespace nv::ui
