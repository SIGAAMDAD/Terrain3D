#!/usr/bin/env python3
"""CS00 GPU capture summarizer. Never imputes missing measurements.

Input: CSV exported from Vulkan profiling with columns:
case,resolution,warm_state,shader_path,frame_gpu_usec,terrain_depth_usec,
terrain_dnr_usec,terrain_color_usec,visibility_usec,transfer_usec,upload_bytes,
vt_misses. Each row is one measured frame. Any blank metric remains unknown.
"""
import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path

GROUP = ('case', 'resolution', 'warm_state', 'shader_path')
METRICS = ('frame_gpu_usec', 'terrain_depth_usec', 'terrain_dnr_usec',
           'terrain_color_usec', 'visibility_usec', 'transfer_usec',
           'upload_bytes', 'vt_misses')


def percentile(samples, percent):
    samples = sorted(samples)
    f = (len(samples) - 1) * percent / 100
    a = math.floor(f)
    return samples[a] + (samples[min(a + 1, len(samples) - 1)] - samples[a]) * (f - a)


def summarize(path):
    cohorts = defaultdict(lambda: defaultdict(list))
    with path.open(encoding='utf-8', newline='') as stream:
        reader = csv.DictReader(stream)
        required = set(GROUP) | set(METRICS)
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError(f'capture CSV must contain: {sorted(required)}')
        for row in reader:
            if not all(row.get(key, '').strip() for key in GROUP):
                raise ValueError('all cohort keys are required')
            key = tuple(row[name] for name in GROUP)
            for metric in METRICS:
                if row.get(metric, '').strip():
                    value = float(row[metric])
                    if not math.isfinite(value) or value < 0:
                        raise ValueError(f'non-finite or negative metric: {metric}')
                    cohorts[key][metric].append(value)
    for key in sorted(cohorts):
        print(' | '.join(key))
        for metric in METRICS:
            sample = cohorts[key][metric]
            if not sample:
                continue
            print(f'  {metric}: n={len(sample)} p50={statistics.median(sample):.3f} '
                  f'p95={percentile(sample,95):.3f} p99={percentile(sample,99):.3f}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_csv', type=Path)
    args = parser.parse_args()
    summarize(args.capture_csv)


if __name__ == '__main__':
    main()
