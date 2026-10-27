#!/usr/bin/env python3
"""Batch-analyze every V3-07 run and print the S1-S4 tables.

Reads <out>/runs/<app>-<set>-<scenario>-r<repeat>/run.json, runs the video
analyzer (slow first: its auto-ROI is reused by s3/s4 of the same app), then
aggregates per-run metrics plus the app-side counters (decode count, cancels,
cache-hit mix) into aggregate.csv and a markdown summary.
"""

import argparse
import collections
import csv
import glob
import json
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze_switch_video as asv  # noqa: E402

APPS = ["ours", "replica", "2345"]
SETS = ["24mp12", "24mp25", "8k"]


def load_json(path):
    with open(path, "r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def stats(values):
    vals = [v for v in values if v is not None]
    if not vals:
        return dict(n=0, median=None, p95=None, min=None, max=None)
    vals.sort()
    return dict(n=len(vals), median=round(statistics.median(vals), 1),
                p95=round(float(__import__("numpy").percentile(vals, 95)), 1),
                min=round(vals[0], 1), max=round(vals[-1], 1))


def ours_counters(run_dir, run):
    """decode/cancel counts + per-key hit source from the app log."""
    log_path = os.path.join(run_dir, "app.log")
    text = ""
    if os.path.exists(log_path):
        text = open(log_path, "r", encoding="utf-8", errors="replace").read()
    counters = {
        "decoded": text.count("decoded:"),
        "cancelled": text.count("load: cancelled path="),
        "mem_hit": text.count("memory cache hit:"),
        "disk_hit": text.count("disk cache hit:"),
        "tier_mem": len(re.findall(r"display cache hit:.*source=mem", text)),
        "tier_disk": len(re.findall(r"display cache hit:.*source=disk", text)),
        "preview": text.count("preview: shown"),
    }
    sources = []
    events = run.get("log_events") or []
    keys = run.get("keys") or []
    for i, key in enumerate(keys):
        start = key.get("t0_global_ms", 0)
        end = keys[i + 1].get("t0_global_ms", start + 2000) if i + 1 < len(keys) else start + 2000
        hit = "unknown"
        for ev in events:
            if not (start <= ev["t_ms"] < end + 2):
                continue
            line = ev["line"]
            if "display cache hit" in line:
                hit = "tier_mem" if "source=mem" in line else "tier_disk"
            elif "memory cache hit" in line:
                hit = "full_mem"
            elif "disk cache hit" in line:
                hit = "full_disk"
            elif "preview: shown" in line:
                hit = "preview"
            elif "load: done" in line:
                hit = "decode"
            elif "load: cancelled" in line:
                hit = "cancelled"
            else:
                continue
            break
        sources.append(hit)
    counters["sources"] = sources
    return counters


def replica_counters(run_dir, run):
    trace_path = os.path.join(run_dir, "trace.log")
    lines = []
    if os.path.exists(trace_path):
        lines = open(trace_path, "r", encoding="utf-8", errors="replace").read().splitlines()
    stamp = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d\d\d) ")
    events = []
    for line in lines:
        m = stamp.match(line)
        if not m:
            continue
        ms = ((int(m.group(1)) * 60 + int(m.group(2))) * 60 + int(m.group(3))) * 1000 + int(m.group(4))
        events.append((ms, line[m.end():]))
    steps = [i for i, (_, text) in enumerate(events) if text.startswith("ui: step-key")]
    sources = []
    latencies = []
    for i, step in enumerate(steps):
        limit = steps[i + 1] if i + 1 < len(steps) else len(events)
        hit = "unknown"
        latency = None
        for j in range(step, limit):
            text = events[j][1]
            if "display-cache HIT" in text:
                hit = "tier_mem"
            elif "display-cache MISS" in text:
                hit = "tier_miss"
            elif text.startswith("ui: apply") and latency is None:
                latency = events[j][0] - events[step][0]
                break
        sources.append(hit)
        latencies.append(latency)
    text_all = "\n".join(line for _, line in events)
    return {
        "decoded": text_all.count("ui: full-decode"),
        "cancelled": None,
        "tier_mem": text_all.count("display-cache HIT"),
        "tier_disk": 0,
        "tier_miss": text_all.count("display-cache MISS"),
        "sources": sources,
        "step_to_apply_ms": latencies,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--reuse", action="store_true", help="skip runs that already have summary.json")
    args = parser.parse_args()

    rows = []
    run_dirs = sorted(glob.glob(os.path.join(args.out, "runs", "*")))
    rois = {}
    # slow runs first so their ROI can be reused by the fast scenarios
    ordered = sorted(run_dirs, key=lambda p: (0 if "slow" in os.path.basename(p) else 1, p))
    for run_dir in ordered:
        run_path = os.path.join(run_dir, "run.json")
        if not os.path.exists(run_path):
            continue
        run = load_json(run_path)
        video = run.get("video")
        if not video or not os.path.exists(video):
            video = os.path.join(args.out, "videos", os.path.basename(run_dir) + ".mkv")
        if not os.path.exists(video):
            print(f"skip (no video): {run_dir}")
            continue
        roi = rois.get(run["app"])
        summary_path = os.path.join(run_dir, "summary.json")
        if args.reuse and os.path.exists(summary_path):
            summary = load_json(summary_path)
        else:
            summary = asv.analyze(video, run, run_dir, roi)
        if run["mode"] == "slow":
            rois[run["app"]] = summary["roi"]
        keys = summary["keys"]
        if run["app"] == "ours":
            counters = ours_counters(run_dir, run)
        elif run["app"] == "replica":
            counters = replica_counters(run_dir, run)
        else:
            counters = {"decoded": None, "cancelled": None,
                        "tier_mem": None, "tier_disk": None, "sources": []}
        if run["mode"] == "slow":
            s1 = keys[:20]
            s2 = keys[20:]
            rows.append(dict(app=run["app"], set=run["sample_set"], scenario="S1",
                             repeat=run["repeat"], stats=stats([k["t2_ms"] for k in s1]),
                             t1=stats([k["t1_ms"] for k in s1]),
                             timeouts=sum(1 for k in s1 if k["timeout"]),
                             peak_private=run.get("peak_private_mb"),
                             peak_ws=run.get("peak_ws_mb"), **counters))
            rows.append(dict(app=run["app"], set=run["sample_set"], scenario="S2",
                             repeat=run["repeat"], stats=stats([k["t2_ms"] for k in s2]),
                             t1=stats([k["t1_ms"] for k in s2]),
                             timeouts=sum(1 for k in s2 if k["timeout"]),
                             peak_private=run.get("peak_private_mb"),
                             peak_ws=run.get("peak_ws_mb"), **counters))
        else:
            row = dict(app=run["app"], set=run["sample_set"], scenario=run["scenario"],
                       repeat=run["repeat"], t3=keys[0]["t3_ms"], t4=keys[0]["t4_ms"],
                       peak_private=run.get("peak_private_mb"),
                       peak_ws=run.get("peak_ws_mb"), **counters)
            row["stats"] = dict(n=1, median=keys[0]["t3_ms"], p95=keys[0]["t3_ms"],
                                min=keys[0]["t3_ms"], max=keys[0]["t3_ms"])
            rows.append(row)

    path = os.path.join(args.out, "aggregate.csv")
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["app", "set", "scenario", "repeat", "n", "median_ms", "p95_ms",
                         "min_ms", "max_ms", "t1_median_ms", "timeouts", "t3_ms", "t4_ms",
                         "peak_private_mb", "peak_ws_mb", "decoded", "cancelled",
                         "tier_mem", "tier_disk", "sources"])
        for row in rows:
            st = row["stats"]
            writer.writerow([row["app"], row["set"], row["scenario"], row["repeat"],
                             st["n"], st["median"], st["p95"], st["min"], st["max"],
                             (row.get("t1") or {}).get("median"), row.get("timeouts"),
                             row.get("t3"), row.get("t4"), row.get("peak_private"),
                             row.get("peak_ws"), row.get("decoded"), row.get("cancelled"),
                             row.get("tier_mem"), row.get("tier_disk"),
                             "|".join(row.get("sources") or [])])
    print(f"wrote {path}")

    # quick pivot for eyeballing
    for scenario in ("S1", "S2", "S3", "S4"):
        print(f"\n== {scenario} ==")
        print("set      app      medians by repeat        peak_priv")
        for s in SETS:
            for app in APPS:
                sel = [r for r in rows if r["app"] == app and r["set"] == s and r["scenario"] == scenario]
                if not sel:
                    continue
                if scenario in ("S1", "S2"):
                    vals = [r["stats"]["median"] for r in sel]
                else:
                    vals = [r["t3"] for r in sel]
                priv = [r["peak_private"] for r in sel]
                print(f"{s:8} {app:8} {vals}  {priv}")


if __name__ == "__main__":
    sys.exit(main())
