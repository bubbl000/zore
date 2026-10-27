#!/usr/bin/env python3
"""Turn one V3-07 screen recording into T1/T2/T3/T4 numbers.

The recording is the 1440x960 top-left region of the desktop at ~58-60 fps
(ddagrab + x264 -qp 0). run.json has the key timestamps and the client rect;
the sync marker (24x24 at 0,0) flashed white once per injected key, so every
key has a video anchor frame.

Frame/PTS alignment: frames are piped with -vsync 0 so the count matches
ffprobe's PTS list 1:1 (the default CFR conversion duplicates frames).

ROI: computed per video from the bbox of pixels that differ between two
settled images, ignoring the top/bottom 60 px of the client (title bar /
status bar text changes with the file name and index and must not count as
"the picture appeared").

Segmentation: "stable runs" (>= MIN_RUN identical consecutive frames) rather
than fixed key windows, because a fast decode can finish one frame before the
marker flash.

Outputs: frames.csv, keys.csv, summary.json (all next to the video).
"""

import argparse
import csv
import json
import os
import subprocess
import sys

import numpy as np

FFMPEG = r"E:\02-AI\ffmpeg7.1.1\bin\ffmpeg.exe"
FFPROBE = r"E:\02-AI\ffmpeg7.1.1\bin\ffprobe.exe"

STABLE_MEAN = 0.5
STABLE_P99 = 6.0
CHANGE_RATIO = 0.005
CHANGE_DELTA = 8
MIN_RUN = 6
CHROME_MARGIN = 60      # px in video (physical) at top/bottom of the client


def probe_pts(video):
    out = subprocess.run(
        [FFPROBE, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "frame=pts_time", "-of", "csv=p=0", video],
        capture_output=True, text=True)
    pts = []
    for line in out.stdout.splitlines():
        line = line.strip().rstrip(",")
        if not line:
            continue
        try:
            pts.append(float(line))
        except ValueError:
            pass
    return pts


def frame_size(video):
    out = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=width,height", "-of", "csv=p=0", video],
                         capture_output=True, text=True)
    width, height = out.stdout.strip().split(",")[:2]
    return int(width), int(height)


def read_gray(video, x, y, w, h, scale):
    vf = f"crop={w}:{h}:{x}:{y}"
    if scale != 1:
        vf += f",scale=iw/{scale}:ih/{scale}:flags=area"
    vf += ",format=gray"
    cmd = [FFMPEG, "-v", "error", "-vsync", "0", "-i", video, "-vf", vf,
           "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    fw, fh = w // scale, h // scale
    size = fw * fh
    frames = []
    while True:
        buf = proc.stdout.read(size)
        if len(buf) < size:
            break
        frames.append(np.frombuffer(buf, dtype=np.uint8).reshape(fh, fw))
    proc.wait()
    return frames


def read_marker(video):
    frames = read_gray(video, 0, 0, 24, 24, 1)
    return np.array([float(f.mean()) for f in frames])


def runs_of_true(flags):
    runs = []
    start = None
    for index, flag in enumerate(flags):
        if flag and start is None:
            start = index
        elif not flag and start is not None:
            runs.append((start, index - 1))
            start = None
    if start is not None:
        runs.append((start, len(flags) - 1))
    return runs


def diff_stats(a, b):
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return float(d.mean()), float(np.percentile(d, 99)), float((d > CHANGE_DELTA).mean())


def same_image(a, b):
    mean, p99, ratio = diff_stats(a, b)
    return mean <= STABLE_MEAN and p99 <= STABLE_P99 and ratio < 0.002


def stable_runs(frames):
    runs = []
    start = 0
    for i in range(1, len(frames)):
        if not same_image(frames[i], frames[i - 1]):
            if i - start >= MIN_RUN:
                runs.append((start, i - 1))
            start = i
    if len(frames) - start >= MIN_RUN:
        runs.append((start, len(frames) - 1))
    return runs


def auto_roi(frames, runs, scale):
    """bbox of pixels that differ between settled, *different* images.

    Uses the union over every consecutive run pair whose change is large, so a
    wrap-around (last image == first image) cannot collapse the bbox.
    """
    h, w = frames[0].shape
    margin = max(1, CHROME_MARGIN // scale)
    union = np.zeros((h, w), dtype=bool)
    pairs = 0
    for i in range(len(runs) - 1):
        a = frames[runs[i][1]]
        b = frames[runs[i + 1][0] + min(3, runs[i + 1][1] - runs[i + 1][0])]
        mask = np.abs(a.astype(np.int16) - b.astype(np.int16)) > CHANGE_DELTA
        if mask.mean() < 0.03:
            continue
        union |= mask
        pairs += 1
    if pairs == 0:
        return [0, 0, w * scale, h * scale]
    union[:margin, :] = False
    union[h - margin:, :] = False
    union[:, :2] = False
    union[:, w - 2:] = False
    rows = np.where(union.any(axis=1))[0]
    cols = np.where(union.any(axis=0))[0]
    if rows.size == 0 or cols.size == 0:
        return [0, 0, w * scale, h * scale]
    y0, y1 = int(rows[0]), int(rows[-1]) + 1
    x0, x1 = int(cols[0]), int(cols[-1]) + 1
    return [x0 * scale, y0 * scale, (x1 - x0) * scale, (y1 - y0) * scale]


def analyze(video, run, out, roi_override=None, scale=2):
    """Analyze one recording; returns (summary dict, keys list)."""
    split = run.get("slow_split")
    notes = list(run.get("notes", []))
    os.makedirs(out, exist_ok=True)

    vw, vh = frame_size(video)
    client = run.get("client_rect") or [0, 0, vw, vh]
    cx, cy, cw, ch = client
    cx = max(0, cx)
    cy = max(0, cy)
    cw = min(cw, vw - cx)
    ch = min(ch, vh - cy)
    cw -= cw % scale
    ch -= ch % scale

    pts = probe_pts(video)
    client_frames = read_gray(video, cx, cy, cw, ch, scale)
    marker = read_marker(video)

    n = min(len(client_frames), len(marker), len(pts) if pts else len(client_frames))
    if pts and len(pts) != len(client_frames):
        notes.append(f"pts/frames mismatch {len(pts)}/{len(client_frames)}")
    times = pts[:n] if pts and len(pts) == len(client_frames) else [i / 60.0 for i in range(n)]
    frames_full = client_frames[:n]
    white = marker[:n] > 100.0
    anchors = [start for start, _ in runs_of_true(white)]
    if len(anchors) != run["key_count"]:
        notes.append(f"anchors {len(anchors)} expected {run['key_count']}")

    runs_full = stable_runs(frames_full)
    if roi_override:
        roi = list(roi_override)
    elif run.get("roi") and run["roi"][2] > 0:
        roi = list(run["roi"])
    else:
        rx, ry, rw, rh = auto_roi(frames_full, runs_full, scale)
        roi = [cx + rx, cy + ry, rw, rh]
    rx, ry, rw, rh = roi[0] - cx, roi[1] - cy, roi[2], roi[3]
    if rx < 0 or ry < 0 or rw <= 0 or rh <= 0:
        raise SystemExit(f"bad roi {roi} for client {client}")
    sx, sy = rx // scale, ry // scale
    sw, sh = rw // scale, rh // scale
    frames = [f[sy:sy + sh, sx:sx + sw] for f in frames_full]

    runs = stable_runs(frames)

    stats_prev = [None] * n
    for i in range(1, n):
        stats_prev[i] = diff_stats(frames[i], frames[i - 1])

    with open(os.path.join(out, "frames.csv"), "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["frame", "pts", "marker_white", "dmean", "dp99", "changed_ratio"])
        for i in range(n):
            st = stats_prev[i] or (0.0, 0.0, 0.0)
            writer.writerow([i, f"{times[i]:.4f}", int(white[i]),
                             f"{st[0]:.3f}", f"{st[1]:.2f}", f"{st[2]:.5f}"])

    def ms(frame, anchor):
        return (times[frame] - times[anchor]) * 1000.0

    keys = []
    if run["mode"] == "slow":
        prev_ref = frames[0]
        for k, anchor in enumerate(anchors):
            limit = anchors[k + 1] - 3 if k + 1 < len(anchors) else n - 1
            limit = max(limit, anchor)
            covering = None
            for r in runs:
                if r[0] <= limit and r[1] >= limit - 2:
                    covering = r
            t2 = covering[0] if (covering is not None and covering[0] >= anchor - 3) else None
            t1 = None
            search_end = t2 if t2 is not None else limit
            for j in range(max(0, anchor - 3), min(search_end + 1, n)):
                if diff_stats(frames[j], prev_ref)[2] >= CHANGE_RATIO:
                    t1 = j
                    break
            if t1 is None:
                t1 = t2
            keys.append({
                "key": k,
                "anchor_frame": anchor,
                "t1_frame": t1,
                "t1_ms": None if t1 is None else round(ms(t1, anchor), 1),
                "t2_frame": t2,
                "t2_ms": None if t2 is None else round(ms(t2, anchor), 1),
                "pre_marker": bool(t2 is not None and t2 < anchor),
                "overlap": bool(t2 is None),
                "timeout": bool(t2 is None or ms(t2, anchor) > 3000.0),
            })
            if covering is not None:
                prev_ref = frames[covering[1]]
            elif t2 is not None:
                prev_ref = frames[t2]
    else:
        final_run = None
        for r in runs:
            if r[1] >= n - 2:
                final_run = r
        t3 = t4 = None
        if final_run is not None and anchors:
            t3 = ms(final_run[0], anchors[-1])
            t4 = ms(final_run[0], anchors[0])
        keys.append({
            "key": "all",
            "anchor_frame": anchors[0] if anchors else None,
            "last_anchor_frame": anchors[-1] if anchors else None,
            "final_run": final_run,
            "t3_ms": None if t3 is None else round(t3, 1),
            "t4_ms": None if t4 is None else round(t4, 1),
        })

    anchor_err = []
    if run.get("keys") and anchors:
        offsets = []
        for key, anchor in zip(run["keys"], anchors):
            global_ms = key.get("t0_global_ms", key.get("t0_ms", 0.0))
            offsets.append((times[anchor] - global_ms / 1000.0) * 1000.0)
        median = float(np.median(offsets))
        anchor_err = [round(v - median, 1) for v in offsets]

    fps = (n - 1) / (times[-1] - times[0]) if n > 1 else 60.0
    summary = {
        "video": os.path.abspath(video),
        "app": run["app"],
        "sample_set": run["sample_set"],
        "scenario": run["scenario"],
        "repeat": run["repeat"],
        "roi": roi,
        "client": [cx, cy, cw, ch],
        "fps": round(fps, 3),
        "frames": n,
        "anchors": len(anchors),
        "stable_runs": len(runs),
        "anchor_err_ms": anchor_err,
        "keys": keys,
        "notes": notes,
    }
    with open(os.path.join(out, "keys.csv"), "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        if run["mode"] == "slow":
            writer.writerow(["key", "anchor_frame", "t1_frame", "t1_ms", "t2_frame", "t2_ms",
                             "pre_marker", "overlap", "timeout"])
            for row in keys:
                writer.writerow([row["key"], row["anchor_frame"], row["t1_frame"],
                                 row["t1_ms"], row["t2_frame"], row["t2_ms"],
                                 row["pre_marker"], row["overlap"], row["timeout"]])
        else:
            writer.writerow(["key", "anchor_frame", "last_anchor_frame", "t3_ms", "t4_ms"])
            for row in keys:
                writer.writerow([row["key"], row["anchor_frame"], row["last_anchor_frame"],
                                 row["t3_ms"], row["t4_ms"]])
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, ensure_ascii=False)
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", required=True)
    parser.add_argument("--run", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--roi", default="", help="x,y,w,h video (screen) px")
    parser.add_argument("--scale", type=int, default=2)
    args = parser.parse_args()

    with open(args.run, "r", encoding="utf-8-sig") as handle:
        run = json.load(handle)
    roi = [int(v) for v in args.roi.split(",")] if args.roi else None
    summary = analyze(args.video, run, args.out, roi, args.scale)
    keys = summary["keys"]
    split = run.get("slow_split")
    if run["mode"] == "slow":
        groups = [(keys[:split], "S1"), (keys[split:], "S2")] if split else [(keys, run["scenario"])]
        for part, name in groups:
            t1 = [k["t1_ms"] for k in part if k["t1_ms"] is not None]
            t2 = [k["t2_ms"] for k in part if k["t2_ms"] is not None]
            t1s = f"{np.median(t1):.1f}" if t1 else "-"
            t2s = f"{np.median(t2):.1f}" if t2 else "-"
            print(f"{run['app']} {run['sample_set']} {name} r{run['repeat']}: "
                  f"T1 median={t1s} T2 median={t2s} n={len(t1)}/{len(t2)} runs={summary['stable_runs']}")
    else:
        print(f"{run['app']} {run['sample_set']} {run['scenario']} r{run['repeat']}: "
              f"T3={keys[0]['t3_ms']} T4={keys[0]['t4_ms']} runs={summary['stable_runs']}")


if __name__ == "__main__":
    sys.exit(main())
