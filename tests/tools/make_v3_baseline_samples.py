#!/usr/bin/env python3
"""Generate the V3-01 baseline samples outside the repository.

The samples are deliberately not committed. The script writes the exact layout
that new/tests/tools/run_v3_baseline.ps1 expects:

  <out>/single24_12/24mp_12mb.jpg   6000x4000, ~12 MB
  <out>/single24_25/24mp_25mb.jpg   6000x4000, ~25 MB
  <out>/flip24/flip01..12.jpg       same family, ~12 MB
  <out>/single8k/8k.jpg             7680x4320 (copied from tests/output/8k.jpg)
  <out>/flip8k/01..10.jpg           ten copies of the 8K sample
  <out>/watch2000/w0000..w1999.bmp  2000 tiny BMPs for the watcher scenario

Usage:
  python make_v3_baseline_samples.py C:\temp\nv-v3-samples
  python make_v3_baseline_samples.py C:\temp\nv-v3-samples --eightk path\\8k.jpg
"""

import argparse
import hashlib
import os
import shutil
import time
from pathlib import Path

import numpy as np
from PIL import Image

TARGET_12 = 12 * 1024 * 1024
TARGET_25 = 25 * 1024 * 1024


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def save_jpeg(image, path, quality):
    start = time.time()
    image.save(path, "JPEG", quality=quality, subsampling=0, optimize=True)
    size = os.path.getsize(path)
    print(f"  q={quality} {size / 1048576:.2f} MB in {time.time() - start:.2f}s")
    return size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out", help="sample directory (created)")
    parser.add_argument(
        "--eightk",
        default=str(Path(__file__).resolve().parent.parent / "output" / "8k.jpg"),
        help="existing 7680x4320 JPEG (default: new/tests/output/8k.jpg)",
    )
    args = parser.parse_args()

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name in ("single24_12", "single24_25", "flip24", "single8k", "flip8k", "watch2000"):
        target = out / name
        if target.exists():
            shutil.rmtree(target)
        target.mkdir()

    width, height = 6000, 4000
    print("24 MP source")
    rng = np.random.default_rng(20261026)
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    base = np.empty((height, width, 3), dtype=np.float32)
    base[..., 0] = 120 + 45 * np.sin(xx / 520.0) + 25 * np.cos(yy / 310.0)
    base[..., 1] = 115 + 40 * np.sin((xx + yy) / 610.0) + 30 * np.cos(xx / 740.0)
    base[..., 2] = 105 + 50 * np.sin(yy / 430.0) + 20 * np.cos((xx - yy) / 830.0)
    noise = rng.normal(0.0, 9.0, (height, width, 3)).astype(np.float32)
    pixels = np.clip(base + noise, 0, 255).astype(np.uint8)
    image = Image.fromarray(pixels, "RGB")
    del base, noise, yy, xx

    candidates = {}
    for quality in (85, 88, 90, 92, 95, 97, 98):
        path = out / f"candidate_q{quality}.jpg"
        candidates[quality] = save_jpeg(image, path, quality)

    q12 = min(candidates, key=lambda q: abs(candidates[q] - TARGET_12))
    q25 = min(candidates, key=lambda q: abs(candidates[q] - TARGET_25))
    shutil.move(str(out / f"candidate_q{q12}.jpg"), str(out / "single24_12" / "24mp_12mb.jpg"))
    shutil.move(str(out / f"candidate_q{q25}.jpg"), str(out / "single24_25" / "24mp_25mb.jpg"))
    for quality in candidates:
        path = out / f"candidate_q{quality}.jpg"
        if path.exists():
            path.unlink()

    print(f"24 MP chosen: 12 MB target q={q12}, 25 MB target q={q25}")
    for i in range(12):
        variant = np.roll(pixels, i * 4, axis=2)
        variant = np.clip(variant.astype(np.int16) + (i % 5) * 3, 0, 255).astype(np.uint8)
        Image.fromarray(variant, "RGB").save(
            out / "flip24" / f"flip{i + 1:02d}.jpg",
            "JPEG", quality=q12, subsampling=0, optimize=True)
    del pixels

    eightk = Path(args.eightk).resolve()
    if not eightk.exists():
        raise SystemExit(f"8K sample not found: {eightk}")
    shutil.copyfile(eightk, out / "single8k" / "8k.jpg")
    for i in range(1, 11):
        shutil.copyfile(eightk, out / "flip8k" / f"{i:02d}.jpg")

    for i in range(2000):
        color = (40 + (i * 7) % 180, 80 + (i * 11) % 150, 60 + (i * 13) % 170)
        Image.new("RGB", (64, 64), color).save(out / "watch2000" / f"w{i:04d}.bmp")

    print("samples")
    for rel in (
        "single24_12/24mp_12mb.jpg",
        "single24_25/24mp_25mb.jpg",
        "single8k/8k.jpg",
        "flip24/flip01.jpg",
        "flip8k/01.jpg",
        "watch2000/w0000.bmp",
    ):
        path = out / rel
        with Image.open(path) as probe:
            dimensions = f"{probe.width}x{probe.height}"
        print(f"  {rel} {path.stat().st_size} {dimensions} {sha256(path)}")


if __name__ == "__main__":
    main()
