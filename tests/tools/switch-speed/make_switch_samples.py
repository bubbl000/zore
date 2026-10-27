#!/usr/bin/env python3
"""Build the V3-07 switch-speed samples (never committed).

Source photos: 25 landscape Canon EOS R6 Mark II CR3 files (5999x3999 after
LibRaw + orientation), decoded once with zoreview_cli --decode, then
re-encoded into three 31-file sets:

  <out>/24mp12/img00..img30.jpg   6000x4000, ~12 MiB
  <out>/24mp25/img00..img30.jpg   6000x4000, ~25 MiB
  <out>/8k/img00..img30.jpg       7680x4320 (1.28x upscale from 6000x4000)

img00 is the start anchor; every measured pass uses img01..img30, so a
20-key or 30-key sequence never runs off the end of the folder.

31 slots come from 25 distinct photos plus 6 horizontally mirrored copies
(img26..img30 mirror img01..img05). Mirroring keeps bytes/pixels different
while size and encoding stay identical.

Usage:
  python make_switch_samples.py --out C:\\temp\\nv-v3-07-samples
"""

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

SRC_DIR = Path(r"E:\修图\lily\8.2莉莉 腾\r")
DEFAULT_CLI = Path(__file__).resolve().parents[3] / "build" / "Release" / "zoreview_cli.exe"

SETS = {
    # target bytes are "aim for", but JPEG q99 -> q100 has a big cliff, so the
    # closest reachable size is used and reported per file.
    "24mp12": dict(width=6000, height=4000, target=12 * 1024 * 1024, grain=0.0),
    "24mp25": dict(width=6000, height=4000, target=25 * 1024 * 1024, grain=4.0),
    "8k": dict(width=7680, height=4320, target=12 * 1024 * 1024, grain=0.0),
}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def probe(cli, path):
    out = subprocess.run([str(cli), "--probe", str(path)],
                         capture_output=True, text=True, encoding="utf-8",
                         errors="replace")
    fields = {}
    for line in out.stdout.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            fields[key.strip()] = value.strip()
    return fields


def decode_to_rgb(cli, path, tmp_bgra):
    if tmp_bgra.exists():
        tmp_bgra.unlink()
    out = subprocess.run([str(cli), "--decode", str(path), "--out", str(tmp_bgra)],
                         capture_output=True, text=True, encoding="utf-8",
                         errors="replace")
    size = None
    for line in out.stdout.splitlines():
        if line.startswith("size:"):
            size = line.split(":", 1)[1].strip()
    if size is None or not tmp_bgra.exists():
        raise RuntimeError(f"decode failed for {path}: {out.stdout} {out.stderr}")
    width, height = (int(v) for v in size.lower().split("x"))
    raw = np.fromfile(tmp_bgra, dtype=np.uint8)
    if raw.size != width * height * 4:
        raise RuntimeError(f"unexpected bgra size {raw.size} for {width}x{height}")
    bgra = raw.reshape(height, width, 4)
    return Image.fromarray(bgra[:, :, 2::-1].copy(), "RGB")


def encode_to_target(image, target, scratch):
    """Pick the JPEG quality whose size is closest to target.

    q99 -> q100 in libjpeg is a cliff (roughly +50%), so an exact hit is often
    impossible; the closest reachable size wins.
    """
    low, high = 92, 100
    best = None
    tried = set()
    for _ in range(7):
        quality = (low + high) // 2
        if quality in tried:
            break
        tried.add(quality)
        image.save(scratch, "JPEG", quality=quality, subsampling=0, optimize=False)
        size = scratch.stat().st_size
        if best is None or abs(size - target) < abs(best[1] - target):
            best = (quality, size)
        if size < target:
            low = quality + 1
        else:
            high = quality - 1
        if low > high:
            break
    # Re-encode with the winning quality (the loop left another q on disk).
    image.save(scratch, "JPEG", quality=best[0], subsampling=0, optimize=False)
    return best[0], scratch.stat().st_size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--cli", default=str(DEFAULT_CLI))
    parser.add_argument("--src", default=str(SRC_DIR))
    args = parser.parse_args()

    cli = Path(args.cli)
    if not cli.exists():
        raise SystemExit(f"cli not found: {cli}")
    out = Path(args.out).resolve()
    masters = out / "masters"
    if masters.exists():
        shutil.rmtree(masters)
    masters.mkdir(parents=True)

    src = Path(args.src)
    candidates = []
    for path in sorted(src.glob("*.CR3")):
        fields = probe(cli, path)
        size = fields.get("size", "")
        if "x" not in size:
            continue
        width, height = (int(v) for v in size.lower().split("x"))
        if width > height:
            candidates.append((path, width, height))
    print(f"landscape sources: {len(candidates)}", flush=True)
    if len(candidates) < 25:
        raise SystemExit("need at least 25 landscape CR3 files")

    tmp_bgra = out / "decode.bgra"
    master_paths = []
    for index, (path, width, height) in enumerate(candidates[:25]):
        rgb = decode_to_rgb(cli, path, tmp_bgra)
        master = masters / f"m{index:02d}.png"
        rgb.save(master, "PNG", compress_level=1)
        master_paths.append(master)
        print(f"master {index:02d} {path.name} {rgb.width}x{rgb.height}", flush=True)
    if tmp_bgra.exists():
        tmp_bgra.unlink()

    # 31 slots: 25 photos + mirrored copies of the first six.
    images = []
    for index in range(31):
        source = master_paths[index] if index < 25 else master_paths[index - 25]
        with Image.open(source) as handle:
            image = handle.convert("RGB")
        if index >= 25:
            image = image.transpose(Image.FLIP_LEFT_RIGHT)
        images.append(image)
        print(f"slot img{index:02d} <- {source.name}{' (mirror)' if index >= 25 else ''}", flush=True)

    manifest = {"source_dir": str(src), "sets": {}, "created": None}
    scratch = out / "scratch.jpg"
    for name, spec in SETS.items():
        target_dir = out / name
        if target_dir.exists():
            shutil.rmtree(target_dir)
        target_dir.mkdir()
        entries = []
        for index, image in enumerate(images):
            if spec["width"] == 7680:
                resized = image.resize((7680, 5120), Image.LANCZOS)
                top = (5120 - spec["height"]) // 2
                resized = resized.crop((0, top, spec["width"], top + spec["height"]))
            else:
                resized = image.resize((spec["width"], spec["height"]), Image.LANCZOS)
            if spec["grain"]:
                rng = np.random.default_rng(1000 + index)
                arr = np.asarray(resized).astype(np.int16)
                noise = rng.normal(0.0, spec["grain"], arr.shape).astype(np.int16)
                resized = Image.fromarray(np.clip(arr + noise, 0, 255).astype(np.uint8), "RGB")
            quality, size = encode_to_target(resized, spec["target"], scratch)
            dest = target_dir / f"img{index:02d}.jpg"
            shutil.move(str(scratch), str(dest))
            entries.append({
                "file": dest.name,
                "bytes": size,
                "width": spec["width"],
                "height": spec["height"],
                "quality": quality,
                "sha256": sha256(dest),
            })
            print(f"{name}/{dest.name} q={quality} {size/1048576:.2f} MiB", flush=True)
        manifest["sets"][name] = {
            "target_bytes": spec["target"],
            "width": spec["width"],
            "height": spec["height"],
            "files": entries,
        }

    # Drop the masters: they are ~1 GB and only needed during generation.
    shutil.rmtree(masters)
    manifest_path = out / "samples.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"wrote {manifest_path}", flush=True)

    for name, data in manifest["sets"].items():
        sizes = [f["bytes"] for f in data["files"]]
        print(f"{name}: {len(sizes)} files, min={min(sizes)/1048576:.2f} "
              f"max={max(sizes)/1048576:.2f} MiB", flush=True)


if __name__ == "__main__":
    sys.exit(main())
