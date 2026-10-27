#pragma once

namespace nv::tests {

// Pure-function checks for the v2.0 UI skeleton: layout rectangles (including
// the BottomStack formulas), the title-bar segment priority, the overlay /
// escape stack and the window state machine. No window, device or DPI context
// is created, so these run in milliseconds and pin down the geometry that the
// drawing and hit-testing code shares.
int RunUiLayoutTests();

}  // namespace nv::tests
