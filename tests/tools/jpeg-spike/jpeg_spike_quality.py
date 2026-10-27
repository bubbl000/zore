"""Quality check for the V3.1-05 spike: compares two raw BGRA dumps.

Usage:
  python jpeg_spike_quality.py --width 1280 --height 853 --ref wic.bgra --test turbo.bgra

Reports:
  * mean/max absolute channel difference on a strided sample
  * gradient energy (mean |dx| + |dy| per channel) of each input and their ratio

The gradient ratio is the same idea as tests/tools/switch-speed/sharpness_split.py
(sharpness measured as gradient energy), applied to a still frame instead of a
video segment. 1.00 means "as sharp as the reference"; <1 means softer.
"""

import argparse
import struct


def read_bgra(path, width, height):
    with open(path, "rb") as handle:
        data = handle.read()
    expected = width * height * 4
    if len(data) != expected:
        raise SystemExit(
            "size mismatch: {} has {} bytes, expected {}".format(
                path, len(data), expected
            )
        )
    return data


def channel(data, width, height, index, stride_pixels):
    total = 0
    count = 0
    row_bytes = width * 4
    for y in range(0, height, 1):
        base = y * row_bytes
        for x in range(0, width, stride_pixels):
            total += data[base + x * 4 + index]
            count += 1
    return total / count if count else 0.0


def gradient_energy(data, width, height, index, stride_pixels):
    row_bytes = width * 4
    total = 0.0
    count = 0
    for y in range(1, height - 1):
        base = y * row_bytes
        prev = base - row_bytes
        for x in range(stride_pixels, width - stride_pixels, stride_pixels):
            value = data[base + x * 4 + index]
            dx = abs(value - data[base + (x - stride_pixels) * 4 + index])
            dy = abs(value - data[prev + x * 4 + index])
            total += dx + dy
            count += 1
    return total / count if count else 0.0


def mean_abs_diff(a, b, stride_pixels):
    total = 0
    count = 0
    max_diff = 0
    step = 4 * stride_pixels
    for i in range(0, len(a) - 3, step):
        for c in range(3):
            d = abs(a[i + c] - b[i + c])
            total += d
            if d > max_diff:
                max_diff = d
            count += 1
    return total / count if count else 0.0, max_diff


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--test", required=True)
    parser.add_argument("--stride", type=int, default=3)
    parser.add_argument("--label", default="")
    args = parser.parse_args()

    ref = read_bgra(args.ref, args.width, args.height)
    test = read_bgra(args.test, args.width, args.height)

    diff, max_diff = mean_abs_diff(ref, test, args.stride)
    print(
        "{} {}x{} mean_abs={:.3f} max_abs={}".format(
            args.label or "compare", args.width, args.height, diff, max_diff
        )
    )

    for index, name in ((2, "R"), (1, "G"), (0, "B")):
        ref_energy = gradient_energy(
            ref, args.width, args.height, index, args.stride
        )
        test_energy = gradient_energy(
            test, args.width, args.height, index, args.stride
        )
        ratio = test_energy / ref_energy if ref_energy else 0.0
        print(
            "  ch={} ref_grad={:.3f} test_grad={:.3f} ratio={:.3f}".format(
                name, ref_energy, test_energy, ratio
            )
        )
    print(
        "  mean_channel ref=({:.1f},{:.1f},{:.1f}) test=({:.1f},{:.1f},{:.1f})".format(
            channel(ref, args.width, args.height, 2, 11),
            channel(ref, args.width, args.height, 1, 11),
            channel(ref, args.width, args.height, 0, 11),
            channel(test, args.width, args.height, 2, 11),
            channel(test, args.width, args.height, 1, 11),
            channel(test, args.width, args.height, 0, 11),
        )
    )


if __name__ == "__main__":
    main()
