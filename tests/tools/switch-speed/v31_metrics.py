#!/usr/bin/env python3
"""V3.1 switch-check metrics: distinct frames, cancels, T3, peak private.

Reads the run directories produced by run_switch_speed.ps1 (+ analyze_all.py)
and reports exactly the numbers v3.1/01 and v3.1/00 use for acceptance:

  distinct   how many different pictures reached the screen during the burst
             (burst_shown.py logic: stable runs between the first key anchor and
             the final stable run) -- the metric that exposed 1-2 frames on the
             22 MB group;
  cancelled  "load: cancelled path=" lines (each one is a key press that showed
             nothing of its own);
  T3         last key -> final sharp frame (video analyzer);
  T4         first key -> final sharp frame;
  peak_private_mb  sampled by the harness.

V3.1-01 adds four log counters the V3-04 run did not need: prefetched,
merged (current merged into an in-flight prefetch), promoted, burst on/off.

Usage:
  python v31_metrics.py --out <dir> [--set 24mp25] [--repeats 3]
"""

import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from burst_shown import runs_from_frames  # noqa: E402

TARGETS = {
    "distinct_min": 8,
    "cancelled_max": 6,
    "t3_max_ms": 250.0,
    "peak_private_max_mb": 480.0,
    "prefetched_min": 15,
}


def load_json(path):
    with open(path, "r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def log_text(run_dir):
    path = os.path.join(run_dir, "app.log")
    if not os.path.exists(path):
        return ""
    return open(path, "r", encoding="utf-8", errors="replace").read()


def distinct_images(run_dir):
    fpath = os.path.join(run_dir, "frames.csv")
    spath = os.path.join(run_dir, "summary.json")
    if not (os.path.exists(fpath) and os.path.exists(spath)):
        return None
    summary = load_json(spath)
    final = summary["keys"][0].get("final_run")
    first = summary["keys"][0].get("anchor_frame")
    if not final or first is None:
        return None
    runs = runs_from_frames(fpath)
    return sum(1 for r in runs if first <= r[0] <= final[0])


def median(values):
    vals = sorted(v for v in values if v is not None)
    if not vals:
        return None
    mid = len(vals) // 2
    if len(vals) % 2:
        return round(vals[mid], 1)
    return round((vals[mid - 1] + vals[mid]) / 2.0, 1)


def lowest(values):
    vals = [v for v in values if v is not None]
    return min(vals) if vals else None


def highest(values):
    vals = [v for v in values if v is not None]
    return max(vals) if vals else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--set", default="24mp25")
    parser.add_argument("--app", default="ours")
    parser.add_argument("--scenarios", default="s3,s4")
    parser.add_argument("--markdown", default="", help="also write the table here")
    args = parser.parse_args()

    scenarios = [s.strip() for s in args.scenarios.split(",") if s.strip()]
    rows = []
    for scenario in scenarios:
        for run_dir in sorted(glob.glob(os.path.join(
                args.out, "runs", f"{args.app}-{args.set}-{scenario}-r*"))):
            run_json = os.path.join(run_dir, "run.json")
            if not os.path.exists(run_json):
                continue
            run = load_json(run_json)
            text = log_text(run_dir)
            row = {
                "scenario": scenario,
                "repeat": run.get("repeat"),
                "distinct": distinct_images(run_dir),
                "cancelled": text.count("load: cancelled path="),
                "prefetched": text.count("prefetched:"),
                "merged": text.count("current merged into in-flight prefetch"),
                "promoted": text.count("prefetch promoted to current"),
                "decoded": text.count("decoded:"),
                "tier_mem": text.count("display cache hit: "),
                "full_hit": text.count("memory cache hit:") + text.count("disk cache hit:"),
                "burst": text.count("decode: burst=on"),
                "peak_private_mb": run.get("peak_private_mb"),
                "t3_ms": None,
                "t4_ms": None,
            }
            summary_path = os.path.join(run_dir, "summary.json")
            if os.path.exists(summary_path):
                summary = load_json(summary_path)
                keys = summary.get("keys") or []
                if keys:
                    row["t3_ms"] = keys[0].get("t3_ms")
                    row["t4_ms"] = keys[0].get("t4_ms")
            rows.append(row)

    if not rows:
        print(f"no runs found under {args.out} for {args.app}/{args.set}")
        return 1

    header = ("| scenario | r | distinct | cancelled | prefetched | merged | "
              "promoted | decoded | tier_hit | full_hit | burst_on | T3 ms | peak MB |")
    table = [header, "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for row in rows:
        table.append("| {scenario} | {repeat} | {distinct} | {cancelled} | {prefetched} | "
                     "{merged} | {promoted} | {decoded} | {tier_mem} | {full_hit} | {burst} | "
                     "{t3_ms} | {peak_private_mb} |".format(**row))

    failures = []
    by_scenario = {}
    for row in rows:
        by_scenario.setdefault(row["scenario"], []).append(row)
        if row["distinct"] is not None and row["distinct"] < TARGETS["distinct_min"]:
            failures.append(f"{row['scenario']}r{row['repeat']}: distinct {row['distinct']} < "
                            f"{TARGETS['distinct_min']}")
        if row["cancelled"] > TARGETS["cancelled_max"]:
            failures.append(f"{row['scenario']}r{row['repeat']}: cancelled {row['cancelled']} > "
                            f"{TARGETS['cancelled_max']}")
        if row["t3_ms"] is not None and row["t3_ms"] > TARGETS["t3_max_ms"]:
            failures.append(f"{row['scenario']}r{row['repeat']}: T3 {row['t3_ms']} > "
                            f"{TARGETS['t3_max_ms']}")
        if row["peak_private_mb"] is not None and row["peak_private_mb"] > TARGETS["peak_private_max_mb"]:
            failures.append(f"{row['scenario']}r{row['repeat']}: private "
                            f"{row['peak_private_mb']} MB > {TARGETS['peak_private_max_mb']}")

    summary_rows = [f"| {scenario} | {median([r['t3_ms'] for r in group])} | "
                    f"{lowest([r['t3_ms'] for r in group])} | "
                    f"{median([r['distinct'] for r in group])} | "
                    f"{lowest([r['distinct'] for r in group])} | "
                    f"{median([r['cancelled'] for r in group])} | "
                    f"{highest([r['peak_private_mb'] for r in group])} |"
                    for scenario, group in by_scenario.items()]
    table.append("")
    table.append("| scenario | T3 median | T3 min | distinct median | distinct min | "
                 "cancelled median | peak MB max |")
    table.append("| --- | --- | --- | --- | --- | --- | --- |")
    table.extend(summary_rows)
    text_out = "\n".join(table)
    print(text_out)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as handle:
            handle.write(text_out + "\n")
    if failures:
        print("\nFAIL")
        for failure in failures:
            print("  " + failure)
        return 1
    print("\nPASS (distinct >= %d, cancelled <= %d, T3 <= %.0f ms, private <= %.0f MB)" % (
        TARGETS["distinct_min"], TARGETS["cancelled_max"], TARGETS["t3_max_ms"],
        TARGETS["peak_private_max_mb"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
