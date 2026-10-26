#!/usr/bin/env python3
"""P09 fail-closed three-stage composed material fidelity gate.

Compare same-footprint captures from (1) live raster composition, (2) the
uncompressed float V3 CPU/reference baker, and (3) compressed-then-decoded
BC output. Stage one measures source-semantic errors; stage two isolates BC
encoding and reconstruction errors. The byte encoder remains off-by-default.

Every stage must identify the same source snapshot and page. No capture file
is bundled and a missing stage is always a failure, not an empty success.
"""
import argparse
import json
import math
from pathlib import Path

from compare_cs14_material_parity import CHANNELS, METADATA, load_manifest, read_plane

STAGES = ('raster_source', 'uncompressed_baker', 'decoded_bc')
LIMIT_GROUPS = ('source_to_baker', 'baker_to_decoded_bc')


def validate_limits(limits):
    if not isinstance(limits, dict) or set(limits) != set(LIMIT_GROUPS):
        raise ValueError('limits must define source_to_baker and baker_to_decoded_bc')
    for step in LIMIT_GROUPS:
        obj = limits[step]
        if not isinstance(obj, dict) or set(obj) != set(CHANNELS):
            raise ValueError(f'{step}: require albedo, normal and orm thresholds')
        for name in CHANNELS:
            target = {'max_abs', 'mae', 'max_degrees', 'mean_degrees'} if name == 'normal' else {'max_abs', 'mae'}
            if not isinstance(obj[name], dict) or set(obj[name]) != target:
                raise ValueError(f'{step}:{name}: wrong threshold names')
            if any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 for v in obj[name].values()):
                raise ValueError(f'{step}:{name}: non-finite or negative tolerance')


def normal_angles(first, second):
    maximum, total, count = 0.0, 0.0, 0
    if len(first) != len(second) or len(first) % 3:
        raise ValueError('invalid normal vector plane')
    for i in range(0, len(first), 3):
        a = first[i:i+3]
        b = second[i:i+3]
        na = math.sqrt(sum(v*v for v in a))
        nb = math.sqrt(sum(v*v for v in b))
        if na < 1.0e-7 or nb < 1.0e-7:
            raise ValueError(f'degenerate normal at pixel {i // 3}')
        dot = sum(x*y for x, y in zip(a, b)) / (na * nb)
        angle = 0.0 if a == b else math.degrees(math.acos(max(-1.0, min(1.0, dot))))
        maximum = max(maximum, angle)
        total += angle
        count += 1
    return maximum, total / count


def compare_pair(first_path, second_path, limits):
    first, second = load_manifest(first_path), load_manifest(second_path)
    mismatches = [field for field in METADATA if first[field] != second[field]]
    if mismatches:
        raise ValueError('different composed page identity: ' + ', '.join(mismatches))
    results = {}
    accepted = True
    for name, components in CHANNELS.items():
        a = read_plane(first_path, first, name)
        b = read_plane(second_path, second, name)
        deltas = [abs(x-y) for x, y in zip(a, b)]
        maximum = max(deltas)
        mae = sum(deltas) / len(deltas)
        limit = limits[name]
        passed = maximum <= limit['max_abs'] and mae <= limit['mae']
        details = {'max_abs': maximum, 'mae': mae, 'elements': len(deltas),
                   'over_max_abs_count': sum(delta > limit['max_abs'] for delta in deltas)}
        if name == 'normal':
            max_angle, mean_angle = normal_angles(a, b)
            details.update({'max_degrees': max_angle, 'mean_degrees': mean_angle})
            passed &= max_angle <= limit['max_degrees'] and mean_angle <= limit['mean_degrees']
        details['accepted'] = passed
        accepted &= passed
        results[name] = details
    return {'accepted': accepted, 'channels': results,
            'page': {name: first[name] for name in METADATA}}


def compare_stages(source_path, baker_path, decoded_path, limits):
    validate_limits(limits)
    paths = (source_path.resolve(), baker_path.resolve(), decoded_path.resolve())
    if len(set(paths)) != 3:
        raise ValueError('three independent stage manifests are required')
    for path, expected in zip(paths, STAGES):
        actual = load_manifest(path)
        if actual.get('capture_stage') != expected:
            raise ValueError(f'{path}: expected capture_stage={expected}')
    first = compare_pair(paths[0], paths[1], limits['source_to_baker'])
    second = compare_pair(paths[1], paths[2], limits['baker_to_decoded_bc'])
    return {'accepted': first['accepted'] and second['accepted'],
            'source_to_baker': first, 'baker_to_decoded_bc': second,
            'note': 'Diagnostic comparison only; it never enables or publishes composed-surface pages.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='raster source capture manifest')
    parser.add_argument('baker', type=Path, help='uncompressed composer capture manifest')
    parser.add_argument('decoded_bc', type=Path, help='decoded compressed texture manifest')
    parser.add_argument('--limits', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    try:
        limits = json.loads(args.limits.read_text(encoding='utf-8'))
        report = compare_stages(args.source, args.baker, args.decoded_bc, limits)
    except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as error:
        parser.exit(2, f'P09 composed-surface stage comparison failed: {error}\n')
    result = json.dumps(report, indent=2, sort_keys=True) + '\n'
    if args.out:
        args.out.write_text(result, encoding='utf-8')
    print(result, end='')
    return 0 if report['accepted'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
