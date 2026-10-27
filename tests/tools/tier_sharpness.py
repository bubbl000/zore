"""Sharpness / error A/B for the V3-04 display tier (規劃/25 §7.4b, §7.9-7).

Both screenshots show the same picture at fit, so the displayed geometry is
identical. They differ only in the source pixels the renderer had:

  off  full decode (6000x4000 BGRA) drawn with HIGH_QUALITY_CUBIC      -> "全解 + CUBIC"
  on   WIC Fant decode at the tier edge (~content size) drawn with the
       same cubic pass (scale ~1.0)                                   -> "按档解码 + Fant"

The reference is a Pillow LANCZOS downscale of the source at the same displayed
size, the same reference run_quality_check uses for 22/18. Prints machine
readable lines:

  geometry=<dw>x<dh>
  off_error=<mean abs diff>  off_gradient=<mean |dx|+|dy|>
  on_error=<mean abs diff>   on_gradient=<mean |dx|+|dy|>
  error_ratio=<on/off>       gradient_ratio=<on/off>
  off_shift=<dx>,<dy>        on_shift=<dx>,<dy>

A ratio near 1.0 means "the tier is as close to the reference as the full
frame"; a gradient ratio well below 1.0 means the tier is visibly softer.

Usage:
  python tier_sharpness.py <source> <off.png> <on.png> <left> <top> <w> <h>
"""

import sys

import numpy as np
from PIL import Image

CROP_W, CROP_H = 600, 400
SEARCH = 2


def gradient_energy(gray: np.ndarray) -> float:
    dx = np.abs(np.diff(gray.astype(np.int16), axis=1)).mean()
    dy = np.abs(np.diff(gray.astype(np.int16), axis=0)).mean()
    return float(dx + dy)


def main() -> int:
    source_path, off_path, on_path = sys.argv[1:4]
    left, top, cw, ch = (float(v) for v in sys.argv[4:8])

    src = Image.open(source_path).convert("RGB")
    width, height = src.size
    scale = min(cw / width, ch / height)
    dw, dh = round(width * scale), round(height * scale)
    if dw <= 0 or dh <= 0:
        print("geometry=0x0")
        return 2
    reference_image = src.resize((dw, dh), Image.LANCZOS)
    ox = left + (cw - dw) / 2.0
    oy = top + (ch - dh) / 2.0
    print(f"geometry={dw}x{dh}")

    rw, rh = min(CROP_W, dw), min(CROP_H, dh)
    rx = (dw - rw) // 2
    ry = (dh - rh) // 2
    reference = np.asarray(
        reference_image.crop((rx, ry, rx + rw, ry + rh)), dtype=np.int16)

    def score(path: str):
        shot = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
        best = None
        for dy in range(-SEARCH, SEARCH + 1):
            for dx in range(-SEARCH, SEARCH + 1):
                x = int(round(ox)) + rx + dx
                y = int(round(oy)) + ry + dy
                if x < 0 or y < 0 or x + rw > shot.shape[1] or y + rh > shot.shape[0]:
                    continue
                crop = shot[y:y + rh, x:x + rw]
                diff = float(np.abs(crop - reference).mean())
                if best is None or diff < best[0]:
                    best = (diff, dx, dy, crop)
        return best

    off = score(off_path)
    on = score(on_path)
    if off is None or on is None:
        print("error=crop_outside_screenshot")
        return 3

    off_gradient = gradient_energy(off[3])
    on_gradient = gradient_energy(on[3])
    print(f"off_error={off[0]:.4f}")
    print(f"on_error={on[0]:.4f}")
    print(f"off_gradient={off_gradient:.4f}")
    print(f"on_gradient={on_gradient:.4f}")
    print(f"error_ratio={on[0] / off[0] if off[0] else 0.0:.4f}")
    print(
        f"gradient_ratio={on_gradient / off_gradient if off_gradient else 0.0:.4f}")
    print(f"off_shift={off[1]},{off[2]}")
    print(f"on_shift={on[1]},{on[2]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
