#!/usr/bin/env python3
"""Compare same-footprint raster and composed material exports in LINEAR space.

Manifest JSON example:
{"width":128,"height":128,"region_key":"0x123","mip":0,
 "source_fingerprint":"0x456","composition_revision":7,
 "channels":{"albedo":{"file":"albedo.f32le","components":3},
             "normal":{"file":"normal.f32le","components":3},
             "orm":{"file":"orm.f32le","components":4}}}

Data is row-major little-endian float32 (per-pixel interleaved components).
This compares uncompressed shader outputs before BC encoding. A separate
post-compression comparison is needed to measure BC quantization artifacts.
"""
import argparse
import json
import math
import struct
from pathlib import Path

CHANNELS = {"albedo": 3, "normal": 3, "orm": 4}
METADATA = ("width", "height", "region_key", "mip", "source_fingerprint", "composition_revision")


def load_manifest(path):
    obj = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(obj, dict) or any(key not in obj for key in METADATA):
        raise ValueError("capture manifest is missing required metadata")
    w, h = obj["width"], obj["height"]
    if type(w) is not int or type(h) is not int or not (1 <= w <= 16384 and 1 <= h <= 16384):
        raise ValueError("invalid capture dimensions")
    channels = obj.get("channels")
    if not isinstance(channels, dict) or set(channels) != set(CHANNELS):
        raise ValueError("albedo, normal, orm must each be present")
    for key, count in CHANNELS.items():
        record = channels[key]
        if not isinstance(record, dict) or record.get("components") != count:
            raise ValueError(f"incorrect {key} channel layout")
        filename = record.get("file")
        if not isinstance(filename, str) or not filename or Path(filename).name != filename:
            raise ValueError("channel filenames must be local simple names")
    return obj


def read_plane(manifest_path, manifest, name):
    record = manifest["channels"][name]
    path = manifest_path.parent / record["file"]
    length = manifest["width"] * manifest["height"] * record["components"]
    if path.stat().st_size != length * 4:
        raise ValueError(f"{name} has unexpected byte length")
    data = path.read_bytes()
    values = struct.unpack(f"<{length}f", data)
    if not all(map(math.isfinite, values)):
        raise ValueError(f"{name} contains a non-finite float")
    return values


def compare(reference, candidate, limits):
    a = load_manifest(reference)
    b = load_manifest(candidate)
    for key in METADATA:
        if a[key] != b[key]:
            raise ValueError(f"mismatched capture identity: {key}")
    results = {}
    passed = True
    for name, components in CHANNELS.items():
        old = read_plane(reference, a, name)
        new = read_plane(candidate, b, name)
        differences = [abs(x - y) for x, y in zip(old, new)]
        max_abs = max(differences)
        mae = sum(differences) / len(differences)
        rmse = math.sqrt(sum(x * x for x in differences) / len(differences))
        channel_limit = limits[name]
        violations = sum(x > channel_limit["max_abs"] for x in differences)
        ok = max_abs <= channel_limit["max_abs"] and mae <= channel_limit["mae"]
        results[name] = {
            "components": components,
            "samples": len(differences),
            "max_abs": max_abs,
            "mae": mae,
            "rmse": rmse,
            "violations": violations,
            "accepted": ok,
        }
        passed &= ok
    return {"accepted": passed, "metadata": {key: a[key] for key in METADATA},
            "channels": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--max-abs", type=float, required=True)
    parser.add_argument("--max-mae", type=float, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if any(not math.isfinite(x) or x < 0 for x in (args.max_abs, args.max_mae)):
        parser.error("tolerances must be finite and nonnegative")
    limits = {key: {"max_abs": args.max_abs, "mae": args.max_mae} for key in CHANNELS}
    report = compare(args.reference, args.candidate, limits)
    output = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(output + "\n", encoding="utf-8")
    print(output)
    return 0 if report["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
