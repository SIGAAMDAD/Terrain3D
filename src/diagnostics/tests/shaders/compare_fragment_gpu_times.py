#!/usr/bin/env python3
"""P00: compare measured GPU-pass CSV captures using exact cohort identity.

Uses CS00's capture CSV schema. Blank time fields are unknown, never 0.
No speedup gate is inferred; user-supplied tolerance gates comparisons.
"""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

COHORT = ('case', 'resolution', 'warm_state', 'shader_path')
METRICS = ('frame_gpu_usec', 'terrain_depth_usec', 'terrain_dnr_usec',
           'terrain_color_usec', 'visibility_usec', 'transfer_usec')


def load(path):
    frames = defaultdict(lambda: defaultdict(list))
    with path.open(newline='', encoding='utf-8') as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or not set(COHORT + METRICS).issubset(reader.fieldnames):
            raise ValueError(f'{path}: missing required headers')
        for line, row in enumerate(reader, 2):
            if any(not row[name] for name in COHORT):
                raise ValueError(f'{path}:{line}: incomplete cohort')
            cohort = tuple(row[name] for name in COHORT)
            for metric in METRICS:
                if row[metric] and row[metric].strip():
                    value = float(row[metric])
                    if not math.isfinite(value) or value < 0:
                        raise ValueError(f'{path}:{line}: invalid {metric}')
                    frames[cohort][metric].append(value)
    if not frames:
        raise ValueError(f'{path}: no captured frames')
    return frames


def percentile(vals, p):
    vals = sorted(vals)
    idx = (len(vals) - 1) * p / 100
    lo = math.floor(idx)
    return vals[lo] + (vals[min(lo + 1, len(vals)-1)]-vals[lo]) * (idx - lo)


def compare(base, new, max_regression_percent):
    old, current = load(base), load(new)
    if set(old) != set(current):
        raise ValueError('baseline and candidate require identical GPU timing cohorts')
    rows = []
    accepted = True
    for cohort in sorted(old):
        for metric in METRICS:
            a, b = old[cohort][metric], current[cohort][metric]
            if not a and not b:
                continue
            if len(a) < 5 or len(b) < 5:
                raise ValueError(f'{cohort}:{metric}: need at least 5 real frames per side')
            detail = {'cohort': dict(zip(COHORT, cohort)), 'metric': metric,
                      'reference_frames': len(a), 'candidate_frames': len(b)}
            for name, func in [('p50', statistics.median), ('p95', lambda v: percentile(v, 95)),
                               ('p99', lambda v: percentile(v, 99))]:
                old_time = func(a)
                new_time = func(b)
                delta_percent = 0.0 if old_time == new_time == 0 else (
                    ((new_time - old_time) * 100.0 / old_time) if old_time else None)
                detail[name] = {'reference_usec': old_time, 'candidate_usec': new_time,
                                'delta_percent': delta_percent}
                if delta_percent is None:
                    if new_time > 0:
                        accepted = False
                elif delta_percent > max_regression_percent:
                    accepted = False
            rows.append(detail)
    return {'accepted': accepted, 'max_regression_percent': max_regression_percent, 'comparisons': rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--max-regression-percent', type=float, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if not math.isfinite(args.max_regression_percent) or args.max_regression_percent < 0:
        parser.error('max regression must be finite and nonnegative')
    try:
        report = compare(args.reference, args.candidate, args.max_regression_percent)
    except (ValueError, OSError) as error:
        parser.exit(2, f'P00 GPU capture comparison failed: {error}\n')
    result = json.dumps(report, indent=2) + '\n'
    if args.out:
        args.out.write_text(result, encoding='utf-8')
    print(result, end='')
    return 0 if report['accepted'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
