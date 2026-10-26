#!/usr/bin/env python3
"""F00/T00 deterministic offline summary of real Terrain3D profiler captures.

Input is produced by project/addons/terrain_3d/tools/streaming_baseline_capture.gd.
No synthetic data are substituted for missing GPU measurements.
"""
import argparse
import csv
import json
import math
from pathlib import Path

SCENARIOS = ('still', 'orbit', 'flythrough', 'teleport', 'hlod')
VT_FIELDS = (
    'vt_page_hits', 'vt_page_misses', 'vt_parent_fallback_hits',
    'vt_tail_fallback_hits', 'vt_request_attempts', 'vt_request_unique',
    'vt_request_hash_overflow', 'vt_upload_bytes_frame', 'vt_upload_bytes_total',
    'vt_upload_page_sets_frame', 'vt_evictions_frame', 'vt_evictions_total',
    'vt_thrash_count', 'vt_physical_capacity', 'vt_physical_resident',
    'vt_physical_retiring', 'vt_io_jobs_pending',
    'vt_feedback_accepted_requests', 'vt_prediction_incomplete_fallbacks',
    'vt_prediction_attempts',
)
# These are cumulative counts in the renderer; report the delta over a capture.
VT_CUMULATIVE = {
    'vt_page_hits', 'vt_page_misses', 'vt_parent_fallback_hits',
    'vt_tail_fallback_hits', 'vt_request_attempts', 'vt_request_unique',
    'vt_request_hash_overflow', 'vt_upload_bytes_total',
    'vt_evictions_total', 'vt_thrash_count',
}
FIELDS = (
    'physics_total_usec', 'pre_hot_usec', 'data_residency_usec', 'hlod_streaming_usec',
    'node_streaming_usec', 'clipmap_usec', 'streaming_usec', 'gpu_upload_bytes',
    'gpu_missing_required', 'gpu_evictions', 'gpu_requested_pages', 'gpu_prepared_queue',
    'gpu_prepare_latency_usec', 'gpu_prepare_end_to_end_usec', 'gpu_prepare_file_read_usec',
    'gpu_runtime_prepares', 'gpu_cooked_misses', 'clipmap_near_updates',
    'clipmap_middle_updates', 'clipmap_far_updates', 'clipmap_teleports',
    'cpu_load_bytes', 'instance_load_bytes', 'node_loads', 'node_load_failures',
)


def percentile(data, fraction):
    ordered = sorted(data)
    if not ordered:
        return None
    pos = (len(ordered) - 1) * fraction
    lower = int(pos)
    upper = min(len(ordered) - 1, lower + 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (pos - lower)


def summarize(capture):
    if capture.get('schema') != 't3d_streaming_baseline_v1':
        raise ValueError('unsupported baseline capture schema')
    if capture.get('scenario') not in SCENARIOS:
        raise ValueError('unrecognized scenario')
    if not capture.get('scene_id') or not isinstance(capture.get('settings'), dict) or not capture['settings']:
        raise ValueError('scene ID and camera/streaming settings are required')
    samples = capture.get('samples')
    if not isinstance(samples, list) or not samples:
        raise ValueError('empty baseline; run a scene before exporting')
    sequences = [s.get('sequence') for s in samples]
    if any(not isinstance(x, int) or x < 0 for x in sequences) or sorted(set(sequences)) != sequences:
        raise ValueError('non-increasing or invalid frame sequence')
    result = {key: capture[key] for key in ('schema', 'scenario', 'scene_id', 'settings')}
    result['sample_count'] = len(samples)
    result['lost_samples'] = int(capture.get('lost_samples', 0))
    if result['lost_samples'] < 0:
        raise ValueError('invalid lost_samples')
    result['complete_capture'] = result['lost_samples'] == 0
    result['first_sequence'], result['last_sequence'] = sequences[0], sequences[-1]
    result['metrics'] = {}
    result['native_aggregate'] = capture.get('aggregate', {})
    for field in FIELDS:
        valid = [float(sample[field]) for sample in samples if isinstance(sample.get(field), (int, float)) and math.isfinite(sample[field])]
        if not valid:
            result['metrics'][field] = None
            continue
        result['metrics'][field] = {
            'count': len(valid), 'total': sum(valid), 'mean': sum(valid) / len(valid),
            'min': min(valid), 'max': max(valid),
            'p50': percentile(valid, .50), 'p95': percentile(valid, .95), 'p99': percentile(valid, .99),
        }
    vt_samples = capture.get('vt_samples', [])
    if not isinstance(vt_samples, list):
        raise ValueError('vt_samples must be a list')
    vt_sequences = [s.get('sequence') for s in vt_samples]
    if any(not isinstance(x, int) or x < 0 for x in vt_sequences) or sorted(set(vt_sequences)) != vt_sequences:
        raise ValueError('invalid VT capture sequences')
    result['vt_samples'] = len(vt_samples)
    result['vt_metrics'] = {}
    for field in VT_FIELDS:
        valid = [float(s[field]) for s in vt_samples
                 if isinstance(s.get(field), (int, float)) and math.isfinite(s[field])]
        if not valid:
            result['vt_metrics'][field] = None
            continue
        result['vt_metrics'][field] = {
            'count': len(valid), 'first': valid[0], 'last': valid[-1],
            'delta': (valid[-1] - valid[0]) if field in VT_CUMULATIVE else None,
            'p50': percentile(valid, .50), 'p95': percentile(valid, .95),
            'peak': max(valid),
        }
    result['missing_external_metrics'] = ['gpu_frame_time_usec', 'gpu_vram_bytes', 'vt_physical_page_miss_age']
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--csv', type=Path)
    a = p.parse_args()
    result = summarize(json.loads(a.capture.read_text(encoding='utf-8')))
    a.output.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    if a.csv:
        with a.csv.open('w', encoding='utf-8', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(('metric', 'count', 'total', 'mean', 'min', 'max', 'p50', 'p95', 'p99'))
            for key, val in result['metrics'].items():
                writer.writerow([key] + ([val.get(x) for x in ('count', 'total', 'mean', 'min', 'max', 'p50', 'p95', 'p99')] if val else [''] * 8))
            # Separate counter/gauge output retains its original units.
            for key, val in result['vt_metrics'].items():
                writer.writerow(['vt:' + key] + ([val.get('count'), val.get('delta'), '', val.get('first'),
                    val.get('peak'), val.get('p50'), val.get('p95'), val.get('last')] if val else [''] * 8))


if __name__ == '__main__':
    main()
