#pragma once

// View rotation geometry (規劃/22/05). The view rotation is display-only: the
// decoded pixels stay upright, the renderer rotates the bitmap around its own
// centre, and fit/zoom/pan treat the rotated bounding box as the effective
// size. Everything here is a pure function so the pixel-free unit tests can
// cover the quarter-turn math without a D3D device.

namespace nv::render {

// Clockwise quarter turns, always normalised to 0..3.
inline int NormalizeRotation(int steps) {
  int value = steps % 4;
  if (value < 0) value += 4;
  return value;
}

inline int RotateSteps(int steps, int delta) {
  return NormalizeRotation(steps + delta);
}

// Odd quarter turns swap the effective width and height.
inline bool RotationSwapsAxes(int steps) {
  return (NormalizeRotation(steps) & 1) != 0;
}

inline int RotationDegrees(int steps) {
  return NormalizeRotation(steps) * 90;
}

// Affine transform used by Direct2D's row-vector convention
// (p' = p * M), built from exact 0/+-1 coefficients so four quarter turns
// return exactly to the original pixels (no sin/cos drift).
struct ViewMatrix {
  float m11 = 1.0f;
  float m12 = 0.0f;
  float m21 = 0.0f;
  float m22 = 1.0f;
  float dx = 0.0f;
  float dy = 0.0f;
};

// Rotates about (cx, cy), which is the centre of the image rectangle the
// renderer draws. Positive steps rotate clockwise on screen (y grows down).
inline ViewMatrix RotationAbout(float cx, float cy, int steps) {
  ViewMatrix matrix;
  switch (NormalizeRotation(steps)) {
    case 1:  // 90 degrees clockwise: (x,y) -> (-y, x)
      matrix.m11 = 0.0f;
      matrix.m12 = 1.0f;
      matrix.m21 = -1.0f;
      matrix.m22 = 0.0f;
      break;
    case 2:  // 180 degrees
      matrix.m11 = -1.0f;
      matrix.m12 = 0.0f;
      matrix.m21 = 0.0f;
      matrix.m22 = -1.0f;
      break;
    case 3:  // 270 degrees clockwise (90 counter-clockwise)
      matrix.m11 = 0.0f;
      matrix.m12 = -1.0f;
      matrix.m21 = 1.0f;
      matrix.m22 = 0.0f;
      break;
    default:
      break;
  }
  // Keep the rotation centre fixed: M' = T(-c) * R * T(c).
  matrix.dx = cx - (cx * matrix.m11 + cy * matrix.m21);
  matrix.dy = cy - (cx * matrix.m12 + cy * matrix.m22);
  return matrix;
}

// Maps a client point into the unrotated image space. Used by wheel zoom so
// the pixel under the cursor stays under the cursor after a rotation
// (規劃/22/05 §3.4, pitfall 4).
inline void UnrotatePoint(float cx, float cy, int steps, float* x, float* y) {
  const float px = *x - cx;
  const float py = *y - cy;
  float rx = px;
  float ry = py;
  switch (NormalizeRotation(steps)) {
    case 1:
      rx = py;
      ry = -px;
      break;
    case 2:
      rx = -px;
      ry = -py;
      break;
    case 3:
      rx = -py;
      ry = px;
      break;
    default:
      break;
  }
  *x = cx + rx;
  *y = cy + ry;
}

}  // namespace nv::render
