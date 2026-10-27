#!/usr/bin/env python3
"""Build the §7 markdown tables from aggregate.csv (medians over 3 repeats)."""

import argparse
import collections
import csv
import statistics

SETS = ["24mp12", "24mp25", "8k"]
APPS = [("ours", "A ZoreView"), ("replica", "B 复刻 0.9"), ("2345", "C 2345 看图王")]


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8-sig") as handle:
        for row in csv.DictReader(handle):
            for key in ("median_ms", "p95_ms", "min_ms", "max_ms", "t3_ms", "t4_ms",
                        "peak_private_mb", "peak_ws_mb", "decoded", "t1_median_ms"):
                if row.get(key) not in (None, ""):
                    row[key] = float(row[key])
            rows.append(row)
    return rows


def clamp(value):
    return None if value is None else max(0.0, value)


def cell(values):
    vals = []
    for v in values:
        if v in (None, ""):
            continue
        try:
            vals.append(max(0.0, float(v)))
        except (TypeError, ValueError):
            continue
    if not vals:
        return "-"
    if len(vals) == 1:
        return f"{vals[0]:.0f}"
    return f"{statistics.median(vals):.0f} ({min(vals):.0f}-{max(vals):.0f})"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True)
    args = parser.parse_args()
    rows = load(args.csv)

    def pick(app, set_name, scenario):
        return [r for r in rows if r["app"] == app and r["set"] == set_name
                and r["scenario"].upper() == scenario]

    for scenario, label in (("S1", "S1 慢速冷开（20 次前进，1.2 s）T2 清晰帧"),
                            ("S2", "S2 慢速回看（20 次后退，1.2 s）T2 清晰帧")):
        print(f"\n### {label} — ms，median（3 次范围）\n")
        header = "| 软件 | " + " | ".join(SETS) + " | 峰值 private MB |"
        print(header)
        print("| --- | " + " | ".join("---" for _ in SETS) + " | --- |")
        for app, name in APPS:
            cells = []
            peaks = []
            for s in SETS:
                sel = pick(app, s, scenario)
                cells.append(cell([r["median_ms"] for r in sel]))
                peaks += [r["peak_private_mb"] for r in sel if r.get("peak_private_mb")]
            print(f"| {name} | " + " | ".join(cells) + f" | {cell(peaks)} |")

    for scenario, label in (("S3", "S3 快速连点 20 张（200 ms/张）"),
                            ("S4", "S4 快速连点 30 张（200 ms/张）")):
        print(f"\n### {label} — T3 停手→清晰 / T4 首按→清晰，ms，median（3 次范围）\n")
        print("| 软件 | 指标 | " + " | ".join(SETS) + " |")
        print("| --- | --- | " + " | ".join("---" for _ in SETS) + " |")
        for app, name in APPS:
            for metric, key in (("T3", "t3_ms"), ("T4", "t4_ms")):
                cells = [cell([r.get(key) for r in pick(app, s, scenario)]) for s in SETS]
                print(f"| {name} | {metric} | " + " | ".join(cells) + " |")

    # hit mix + decode/cancel counts
    print("\n### 每轮计数（3 次中位；S1/S2 共用同一进程）\n")
    print("| 软件 | 样本组 | decoded | cancelled | tier_mem | tier_disk | S1 命中构成 |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for app, name in APPS:
        for s in SETS:
            sel = pick(app, s, "S1")
            if not sel:
                continue
            decoded = [r.get("decoded") for r in sel if r.get("decoded") not in (None, "")]
            cancelled = [r.get("cancelled") for r in sel if r.get("cancelled") not in (None, "")]
            sources = (sel[0].get("sources") or "").split("|")[:20]
            mix = collections.Counter(sources)
            mix_text = ", ".join(f"{k}={v}" for k, v in mix.most_common()) if sources[0] else "-"
            print(f"| {name} | {s} | {cell(decoded)} | {cell(cancelled)} | "
                  f"{cell([r.get('tier_mem') for r in sel])} | "
                  f"{cell([r.get('tier_disk') for r in sel])} | {mix_text} |")


if __name__ == "__main__":
    main()
