#!/usr/bin/env python3
"""P00: compare real Vulkan fragment attachment captures, including exact coverage/pages.

Both JSON manifests must refer to attachments captured on the *same* GPU,
scene, camera and material/VT state. Manifest example is documented in
FRAGMENT_SHADER_CAPTURE.md. Required thresholds are authored by the operator
from the measured baseline; this tool never silently tolerates differences.
"""
import argparse
import json
import math
from pathlib import Path
import struct

REQUIRED = {'hdr_color': ('f32le', 4), 'dnr': ('f32le', 4),
            'depth': ('f32le', 1), 'coverage': ('u8', 1),
            'vt_page_ids': ('u32le', 1)}
IDENTITY = ('renderer', 'device', 'driver', 'godot_build', 'scene_sha256',
            'assets_sha256', 'camera_state_sha256', 'material_state_sha256',
            'vt_state_sha256', 'variant', 'capture_case', 'state',
            'frame_number', 'width', 'height')


def load(path):
    info = json.loads(path.read_text(encoding='utf-8'))
    if info.get('schema') != 1 or not isinstance(info.get('identity'), dict):
        raise ValueError(f'{path}: expected schema 1 and identity object')
    who = info['identity']
    for field in IDENTITY:
        if field not in who or who[field] in ('', None):
            raise ValueError(f'{path}: missing identity {field}')
    if who['renderer'] != 'Vulkan' or type(who['width']) is not int or type(who['height']) is not int \
            or not (1 <= who['width'] <= 16384 and 1 <= who['height'] <= 16384):
        raise ValueError(f'{path}: invalid renderer/extent')
    planes = info.get('attachments')
    if not isinstance(planes, dict) or set(planes) != set(REQUIRED):
        raise ValueError(f'{path}: all five required attachments must be present')
    for name, (dtype, components) in REQUIRED.items():
        spec = planes[name]
        if not isinstance(spec, dict) or spec.get('type') != dtype or spec.get('components') != components:
            raise ValueError(f'{path}: invalid {name} type/shape')
        filename = spec.get('file', '')
        if not isinstance(filename, str) or Path(filename).name != filename or not filename:
            raise ValueError(f'{path}: invalid {name} filename')
        expected = who['width'] * who['height'] * components * {'f32le': 4, 'u8': 1, 'u32le': 4}[dtype]
        if (path.parent / filename).stat().st_size != expected:
            raise ValueError(f'{path}: {name} requires {expected} bytes')
    return info


def compare(reference, candidate, thresholds):
    before, after = load(reference), load(candidate)
    if before['identity'] != after['identity']:
        mismatches = sorted(field for field in set(before['identity']) | set(after['identity'])
                            if before['identity'].get(field) != after['identity'].get(field))
        raise ValueError(f'capture identities differ: {mismatches}')
    if set(thresholds) != {'hdr_color', 'dnr', 'depth'}:
        raise ValueError('thresholds must declare exactly hdr_color, dnr and depth')
    for name in thresholds:
        limit = thresholds[name]
        if set(limit) != {'max_abs', 'mae'} or any(
                type(value) not in (int, float) or not math.isfinite(value) or value < 0
                for value in limit.values()):
            raise ValueError(f'invalid {name} thresholds')
    result = {}
    passed = True
    for name, (dtype, components) in REQUIRED.items():
        length = before['identity']['width'] * before['identity']['height'] * components
        a_path = reference.parent / before['attachments'][name]['file']
        b_path = candidate.parent / after['attachments'][name]['file']
        max_delta, sum_delta, differences, count = 0.0, 0.0, 0, 0
        with a_path.open('rb') as a, b_path.open('rb') as b:
            while count < length:
                n = min(16384, length - count)
                unit = 1 if dtype == 'u8' else 4
                left = a.read(n * unit)
                right = b.read(n * unit)
                if dtype == 'u8':
                    xs, ys = left, right
                else:
                    fmt = f'<{n}{"f" if dtype == "f32le" else "I"}'
                    xs, ys = struct.unpack(fmt, left), struct.unpack(fmt, right)
                for x, y in zip(xs, ys):
                    if dtype == 'f32le' and (not math.isfinite(x) or not math.isfinite(y)):
                        raise ValueError(f'{name}: non-finite attachment value at sample {count}')
                    d = abs(x - y)
                    max_delta = max(max_delta, d)
                    sum_delta += d
                    differences += d != 0
                    count += 1
        mae = sum_delta / length
        if dtype == 'f32le':
            accepted = max_delta <= thresholds[name]['max_abs'] and mae <= thresholds[name]['mae']
        else:
            accepted = differences == 0
        passed &= accepted
        result[name] = {'accepted': accepted, 'max_abs': max_delta, 'mae': mae,
                        'different_elements': differences, 'elements': length,
                        'exact': dtype != 'f32le'}
    return {'accepted': passed, 'identity': before['identity'], 'attachments': result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--limits', type=Path, required=True,
                        help='JSON: {"hdr_color":{"max_abs":0,...},"dnr":{...},"depth":{...}}')
    parser.add_argument('--out', type=Path)
    options = parser.parse_args()
    try:
        limits = json.loads(options.limits.read_text(encoding='utf-8'))
        report = compare(options.reference, options.candidate, limits)
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        parser.exit(2, f'P00 capture comparison failed: {error}\n')
    message = json.dumps(report, indent=2) + '\n'
    if options.out:
        options.out.write_text(message, encoding='utf-8')
    print(message, end='')
    return 0 if report['accepted'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
