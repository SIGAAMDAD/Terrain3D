#!/usr/bin/env python3
"""Compare paired CoD feedback captures; no claims without runtime measurements.

Each input is JSON with matching scene, resolution and settings and a `frames`
array of {cpu_ms, gpu_ms, feedback_readback_bytes, vt_page_misses,
vt_evictions}. Omit startup frames before exporting; the script compares the
provided matching workload without manufacturing GPU timing data.
"""
import argparse
import json
import math
from pathlib import Path

METRICS = ('cpu_ms', 'gpu_ms', 'feedback_readback_bytes', 'vt_page_misses', 'vt_evictions')


def percentile(values, pct):
    values = sorted(values)
    pos = (len(values) - 1) * pct / 100.0
    lo, hi = math.floor(pos), math.ceil(pos)
    return values[lo] + (values[hi] - values[lo]) * (pos - lo)


def load(path):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    frames = data['frames']
    if len(frames) < 1000:
        raise ValueError(f'{path}: requires at least 1000 steady-state frame samples')
    for frame in frames:
        if any(key not in frame for key in METRICS):
            raise ValueError(f'{path}: frame missing a required metric')
        if any(not isinstance(frame[key], (int,float)) or not math.isfinite(frame[key]) or frame[key] < 0 for key in METRICS):
            raise ValueError(f'{path}: invalid negative/nonfinite frame metric')
    return data


def compare(a, b):
    keys = ('scene', 'resolution', 'settings')
    for key in keys:
        if a.get(key) is None or a.get(key) != b.get(key):
            raise ValueError(f'mismatched or absent capture identity: {key}')
    print(f"Scene: {a['scene']} | resolution: {a['resolution']} | frames: {len(a['frames'])}/{len(b['frames'])}")
    print('Metric | Atomic median | Grid median | Atomic p95 | Grid p95 | Atomic p99 | Grid p99')
    print('--- | ---: | ---: | ---: | ---: | ---: | ---:')
    for key in METRICS:
        av = [f[key] for f in a['frames']]
        bv = [f[key] for f in b['frames']]
        vals = [percentile(x, p) for p in (50,95,99) for x in (av,bv)]
        print(f"{key} | " + ' | '.join(f'{v:.3f}' for v in vals))
    print('Measured distributions only. GPU timing must come from matching Godot captures.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('atomic_json')
    parser.add_argument('grid_json')
    args = parser.parse_args()
    try:
        compare(load(args.atomic_json), load(args.grid_json))
    except (ValueError, KeyError, json.JSONDecodeError, OSError) as exc:
        parser.error(str(exc))

if __name__ == '__main__':
    main()
