#!/usr/bin/env python3
"""Count how many distinct images actually reached the screen during a burst.

Reconstructs the stable runs from frames.csv (same thresholds as the analyzer)
and counts runs that start between the first key and the final settle. A viewer
that cancels aggressively can win on T3/T4 while showing only 2-3 of the 20
requested images; this number makes that visible.
"""

import argparse
import csv
import glob
import json
import os

STABLE_MEAN = 0.5
STABLE_P99 = 6.0
RATIO = 0.002
MIN_RUN = 6


def runs_from_frames(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            rows.append((float(row["dmean"]), float(row["dp99"]), float(row["changed_ratio"])))
    same = [False] * len(rows)
    for i in range(1, len(rows)):
        dmean, dp99, ratio = rows[i]
        same[i] = dmean <= STABLE_MEAN and dp99 <= STABLE_P99 and ratio < RATIO
    runs = []
    start = 0
    for i in range(1, len(rows)):
        if not same[i]:
            if i - start >= MIN_RUN:
                runs.append((start, i - 1))
            start = i
    if len(rows) - start >= MIN_RUN:
        runs.append((start, len(rows) - 1))
    return runs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    print("| app | set | scenario | distinct images shown | runs |")
    print("| --- | --- | --- | --- | --- |")
    for app in ("ours", "replica", "2345"):
        for set_name in ("24mp12", "24mp25", "8k"):
            for scenario in ("s3", "s4"):
                counts = []
                run_counts = []
                for repeat in (1, 2, 3):
                    d = os.path.join(args.out, "runs", f"{app}-{set_name}-{scenario}-r{repeat}")
                    fpath = os.path.join(d, "frames.csv")
                    spath = os.path.join(d, "summary.json")
                    if not (os.path.exists(fpath) and os.path.exists(spath)):
                        continue
                    summary = json.load(open(spath, "r", encoding="utf-8"))
                    final = summary["keys"][0].get("final_run")
                    first = summary["keys"][0].get("anchor_frame")
                    if not final or first is None:
                        continue
                    runs = runs_from_frames(fpath)
                    run_counts.append(len(runs))
                    counts.append(sum(1 for r in runs if first <= r[0] <= final[0]))
                if counts:
                    print(f"| {app} | {set_name} | {scenario} | {counts} | {run_counts} |")


if __name__ == "__main__":
    main()
