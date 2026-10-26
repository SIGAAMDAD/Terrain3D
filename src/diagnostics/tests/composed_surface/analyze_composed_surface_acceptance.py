#!/usr/bin/env python3
"""CS08: evaluate real Vulkan A/B frame captures, never create missing data.

Required CSV columns: case,resolution,warm_state,shader_path,frame_gpu_usec,
terrain_depth_usec,terrain_dnr_usec,terrain_color_usec,visibility_usec,
transfer_usec,upload_bytes,vt_misses.
Optional columns: composed_gpu_reserved_bytes,composed_physical_pages,
composed_dirty_queue_depth,composed_evictions,composed_stale_discarded,
composed_fallback_patches,composed_upload_calls,composed_read_bytes,
composed_recompositions,composed_cache_enabled,resource_rebuilds,
feedback_readback_bytes. Missing columns remain unavailable (not zero).

CSV is a source of hardware readings, not a simulated test. Compare identical
scene, driver, build, GPU, framebuffer, editor mode and shader configuration.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

GROUP_KEYS = ('case', 'resolution', 'warm_state')
PATH_KEY = 'shader_path'
MEASUREMENTS = (
    'frame_gpu_usec', 'terrain_depth_usec', 'terrain_dnr_usec',
    'terrain_color_usec', 'visibility_usec', 'transfer_usec',
    'upload_bytes', 'vt_misses', 'composed_gpu_reserved_bytes',
    'composed_physical_pages', 'composed_dirty_queue_depth',
    'composed_evictions', 'composed_stale_discarded',
    'composed_fallback_patches', 'composed_upload_calls',
    'composed_read_bytes', 'composed_recompositions',
    'resource_rebuilds', 'feedback_readback_bytes',
)
REQUIRED = set(GROUP_KEYS + (PATH_KEY,)) | set(MEASUREMENTS[:8])


def percentile(values: list[float], percentage: float) -> float:
    values = sorted(values)
    at = (len(values) - 1) * percentage / 100.0
    i = int(math.floor(at))
    return values[i] + (values[min(i + 1, len(values) - 1)] - values[i]) * (at - i)


def read_frames(path: Path):
    groups = defaultdict(list)
    with path.open(newline='', encoding='utf-8') as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or not REQUIRED.issubset(reader.fieldnames):
            raise ValueError(f'missing required columns: {sorted(REQUIRED - set(reader.fieldnames or []))}')
        for line, row in enumerate(reader, 2):
            cohort = tuple((row.get(key) or '').strip() for key in GROUP_KEYS)
            shader_path = (row[PATH_KEY] or '').strip()
            if not all(cohort) or not shader_path:
                raise ValueError(f'blank comparison key on line {line}')
            metrics = {}
            for key in MEASUREMENTS:
                cell = (row.get(key) or '').strip()
                if cell:
                    value = float(cell)
                    if not math.isfinite(value) or value < 0:
                        raise ValueError(f'invalid {key} on line {line}')
                    metrics[key] = value
            groups[(cohort, shader_path)].append(metrics)
    return groups


def summarize(group: list[dict], key: str):
    values = [sample[key] for sample in group if key in sample]
    if not values:
        return None
    return {'n': len(values), 'p50': statistics.median(values),
            'p95': percentile(values, 95), 'p99': percentile(values, 99),
            'max': max(values)}


def compare(groups, baseline='canonical_vt_v1', candidate='composed_surface', min_frames=30):
    results = []
    cohorts = sorted({cohort for cohort, _ in groups})
    for cohort in cohorts:
        old, new = groups.get((cohort, baseline)), groups.get((cohort, candidate))
        if not old or not new:
            results.append({'case': cohort, 'decision': 'NO_MATCHED_CAPTURE'})
            continue
        data = {'case': cohort, 'baseline_frames': len(old), 'candidate_frames': len(new),
                'metrics': {}}
        for key in MEASUREMENTS:
            old_summary, new_summary = summarize(old, key), summarize(new, key)
            if old_summary and new_summary:
                diff = new_summary['p50'] - old_summary['p50']
                data['metrics'][key] = {
                    'baseline': old_summary, 'candidate': new_summary,
                    'p50_delta': diff,
                    'p50_percent': 100.0 * diff / old_summary['p50'] if old_summary['p50'] else None,
                }
        # Color + DNR is the intended material cost. The frame and upload
        # comparison gates check that moving overhead did not erase the gain.
        for name, group in [('baseline', old), ('candidate', new)]:
            complete = [sample['terrain_color_usec'] + sample['terrain_dnr_usec'] for sample in group
                        if 'terrain_color_usec' in sample and 'terrain_dnr_usec' in sample]
            data[name + '_material'] = summarize([{'material_usec': v} for v in complete], 'material_usec')
        enough = min(len(old), len(new)) >= min_frames
        baseline_material = data['baseline_material']
        candidate_material = data['candidate_material']
        baseline_frame = summarize(old, 'frame_gpu_usec')
        candidate_frame = summarize(new, 'frame_gpu_usec')
        baseline_upload = summarize(old, 'upload_bytes')
        candidate_upload = summarize(new, 'upload_bytes')
        timing_groups = (baseline_material, candidate_material, baseline_frame,
                         candidate_frame, baseline_upload, candidate_upload)
        if not enough or any(s is None or s['n'] < min_frames for s in timing_groups):
            data['decision'] = 'INSUFFICIENT_MEASUREMENTS'
        elif (candidate_material['p50'] < baseline_material['p50'] and
              candidate_frame['p95'] <= baseline_frame['p95'] and
              candidate_upload['p95'] <= baseline_upload['p95']):
            data['decision'] = 'PASS_TIMING_GATES_ONLY'
        else:
            data['decision'] = 'FAIL_TIMING_GATES'
        results.append(data)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_csv', type=Path)
    parser.add_argument('--baseline', default='canonical_vt_v1')
    parser.add_argument('--candidate', default='composed_surface')
    parser.add_argument('--min-frames', type=int, default=30)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.min_frames < 2 or args.baseline == args.candidate:
        parser.error('min-frames must be >=2 and comparison paths must differ')
    result = {'measurement_source': str(args.capture_csv),
              'note': 'Timing gates only; visual parity, residency, Vulkan and 10k-frame plateau remain separate.',
              'comparisons': compare(read_frames(args.capture_csv), args.baseline,
                                     args.candidate, args.min_frames)}
    serialized = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(serialized + '\n', encoding='utf-8')
    else:
        print(serialized)


if __name__ == '__main__':
    main()
