#!/usr/bin/env python3
"""V3.1-02 blur accounting: how many burst frames were a low-res placeholder?

2345 buys "every key press changes the screen" by showing a low-resolution
upscale first; the price is that those frames are blurry. V3.1-02 adds the same
mechanism to ZoreView, so the check must prove the *price* stayed acceptable.

The measurement pairs frames of the *same picture* only:

  * every distinct stable run is represented by its middle frame;
  * two runs are "the same picture" when their 32x32 low-frequency samples
    correlate at >= 0.98 (the same test that identified 2345's 0.990 pair in
    加载逻辑对比 §7.4);
  * a run is blurry when a sharper run of the same picture exists and its
    gradient energy is below 0.8x that sharper run.

Gradient energy alone is *not* comparable across different photos (one 22 MP
burst run measured 0.59-1.01 across different images), which is why the pairing
step exists: an unpaired run is a picture that was only ever shown sharp, not a
blurry frame.

Usage:
  python sharpness_split.py --out <switch-speed output dir> --set 24mp25
"""

import argparse
import glob
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze_switch_video as asv  # noqa: E402
from burst_shown import runs_from_frames  # noqa: E402

BLUR_RATIO = 0.8
SAME_PICTURE_CORR = 0.98
SAMPLE = 32


def gradient_energy(frame):
    """Mean |dx| + |dy| over a grayscale frame; a blurry upscale has less."""
    g = frame.astype(np.int16)
    dx = np.abs(np.diff(g, axis=1)).mean()
    dy = np.abs(np.diff(g, axis=0)).mean()
    return float(dx + dy)


def sample32(frame):
    h, w = frame.shape
    ys = np.linspace(0, h, SAMPLE + 1).astype(int)
    xs = np.linspace(0, w, SAMPLE + 1).astype(int)
    out = np.zeros((SAMPLE, SAMPLE), dtype=np.float64)
    for i in range(SAMPLE):
        row = frame[ys[i]:max(ys[i] + 1, ys[i + 1])]
        for j in range(SAMPLE):
            block = row[:, xs[j]:max(xs[j] + 1, xs[j + 1])]
            out[i, j] = float(block.mean())
    return out


def correlation(a, b):
    av = a - a.mean()
    bv = b - b.mean()
    denom = float(np.sqrt((av * av).sum() * (bv * bv).sum()))
    if denom <= 1e-9:
        return 0.0
    return float((av * bv).sum() / denom)


def load_json(path):
    with open(path, "r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def measure(run_dir):
    run_path = os.path.join(run_dir, "run.json")
    summary_path = os.path.join(run_dir, "summary.json")
    frames_path = os.path.join(run_dir, "frames.csv")
    if not (os.path.exists(run_path) and os.path.exists(summary_path) and
            os.path.exists(frames_path)):
        return None
    run = load_json(run_path)
    summary = load_json(summary_path)
    video = run.get("video")
    if not video or not os.path.exists(video):
        return None
    client = run.get("client_rect") or [0, 0, 0, 0]
    roi = summary.get("roi")
    if not roi or roi[2] <= 0 or roi[3] <= 0:
        return None
    # Same crop the analyzer used for frames.csv (client-relative ROI, 2x scale).
    rx = int(roi[0]) - int(client[0])
    ry = int(roi[1]) - int(client[1])
    frames = asv.read_gray(video, int(client[0]) + rx, int(client[1]) + ry,
                           int(roi[2]), int(roi[3]), 2)
    runs = runs_from_frames(frames_path)
    first = summary["keys"][0].get("anchor_frame")
    final = summary["keys"][0].get("final_run")
    if first is None or not final:
        return None
    final_start = final[0]
    if final_start >= len(frames):
        return None

    shown = []
    for start, end in runs:
        if not (first <= start <= final_start):
            continue
        start = max(0, min(start, len(frames) - 1))
        end = max(start, min(end, len(frames) - 1))
        middle = (start + end) // 2
        shown.append({
            "frame": middle,
            "energy": gradient_energy(frames[middle]),
            "sample": sample32(frames[middle]),
        })
    if len(shown) < 2:
        return {"pairs": 0, "blurry": 0, "blur_ratio": None, "details": []}

    pairs = 0
    blurry = 0
    details = []
    for i, run_i in enumerate(shown):
        best = None
        for j, run_j in enumerate(shown):
            if j == i or run_j["energy"] <= run_i["energy"]:
                continue
            if correlation(run_i["sample"], run_j["sample"]) < SAME_PICTURE_CORR:
                continue
            if best is None or run_j["energy"] > best["energy"]:
                best = run_j
        if best is None:
            continue
        ratio = run_i["energy"] / best["energy"]
        pairs += 1
        if ratio < BLUR_RATIO:
            blurry += 1
        details.append(round(ratio, 2))
    return {
        "pairs": pairs,
        "blurry": blurry,
        "blur_ratio": (blurry / pairs) if pairs else None,
        "details": details,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--app", default="ours")
    parser.add_argument("--set", default="")
    parser.add_argument("--scenarios", default="s3,s4")
    args = parser.parse_args()

    scenarios = [s.strip() for s in args.scenarios.split(",") if s.strip()]
    prefix = args.app + "-" + (args.set + "-" if args.set else "")
    rows = []
    for scenario in scenarios:
        for run_dir in sorted(glob.glob(os.path.join(
                args.out, "runs", prefix + scenario + "-r*"))):
            result = measure(run_dir)
            if result is None:
                continue
            rows.append((os.path.basename(run_dir), result))

    if not rows:
        print(f"no measured runs under {args.out}")
        return 1

    print("| run | same-picture pairs | blurry | blur ratio | ratios |")
    print("| --- | --- | --- | --- | --- |")
    worst = 0.0
    for name, result in rows:
        ratio = result["blur_ratio"]
        if ratio is not None and ratio > worst:
            worst = ratio
        print(f"| {name} | {result['pairs']} | {result['blurry']} | "
              f"{'-' if ratio is None else f'{ratio:.0%}'} | {result['details']} |")

    print()
    if worst <= 0.5:
        print(f"PASS: worst blur ratio {worst:.0%} <= 50%")
        return 0
    print(f"FAIL: worst blur ratio {worst:.0%} > 50%")
    return 1


if __name__ == "__main__":
    sys.exit(main())
