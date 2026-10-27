"""Quality reference for tests/run_quality_check.ps1 (規劃/22/18 §6).

Compares the image area of two zoreview screenshots (linear and cubic) against
a Pillow LANCZOS downscale of the source at the same fit geometry. Prints
machine-readable lines:

    geometry=<dw>x<dh>
    linear=<mean abs diff>
    cubic=<mean abs diff>
    improvement=<percent>
    cubic_shift=<dx>,<dy>

A small +-2 px search is allowed because the renderer positions the image with
float coordinates and the screenshot crop is integral; the reported shift is
part of the evidence.

Usage:
  python quality_compare.py <source.png> <linear.png> <cubic.png> \
      <content_left> <content_top> <content_w> <content_h>
"""

import sys

import numpy as np
from PIL import Image


def main() -> int:
    source_path, linear_path, cubic_path = sys.argv[1:4]
    left, top, cw, ch = (float(v) for v in sys.argv[4:8])

    src = Image.open(source_path).convert("RGB")
    width, height = src.size
    scale = min(cw / width, ch / height)
    dw, dh = round(width * scale), round(height * scale)
    if dw <= 0 or dh <= 0:
        print("geometry=0x0")
        return 2
    ref = src.resize((dw, dh), Image.LANCZOS)
    ox = left + (cw - dw) / 2.0
    oy = top + (ch - dh) / 2.0
    print(f"geometry={dw}x{dh}")

    rw, rh = min(600, dw), min(400, dh)
    rx = (dw - rw) // 2
    ry = (dh - rh) // 2
    reference = np.asarray(ref.crop((rx, ry, rx + rw, ry + rh)), dtype=np.int16)

    def score(path: str):
        shot = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
        best = None
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                x = int(round(ox)) + rx + dx
                y = int(round(oy)) + ry + dy
                if x < 0 or y < 0 or x + rw > shot.shape[1] or y + rh > shot.shape[0]:
                    continue
                crop = shot[y:y + rh, x:x + rw]
                diff = float(np.abs(crop - reference).mean())
                if best is None or diff < best[0]:
                    best = (diff, dx, dy)
        if best is None:
            return None
        return best

    linear = score(linear_path)
    cubic = score(cubic_path)
    if linear is None or cubic is None:
        print("error=crop_outside_screenshot")
        return 3
    print(f"linear={linear[0]:.4f}")
    print(f"cubic={cubic[0]:.4f}")
    print(f"cubic_shift={cubic[1]},{cubic[2]}")
    improvement = 100.0 * (linear[0] - cubic[0]) / linear[0] if linear[0] else 0.0
    print(f"improvement={improvement:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
