#!/usr/bin/env python3
"""Export three RGBA32F page-channel arrays to the CS21 float capture contract.

The arrays are produced by t3d_comp_surf_static_baseline_evaluate_linear. The normal
array contains normalized XYZ, and the ORM array is AO/R/M/AO influence.
The raw capture is diagnostic-only; calling this tool never approves a .t3sc.
"""
import argparse
import json
import math
from pathlib import Path
import struct

PHYSICAL_TILE = 136
COUNT = PHYSICAL_TILE * PHYSICAL_TILE
LAYOUT = {'albedo': 3, 'normal': 3, 'orm': 4}


def export(output, channels, metadata):
    output.mkdir(parents=True, exist_ok=True)
    result = {'width': PHYSICAL_TILE, 'height': PHYSICAL_TILE,
              'capture_stage': 'uncompressed_baker', 'channels': {}, **metadata}
    payloads = {}
    for name, n in LAYOUT.items():
        source = Path(channels[name])
        raw = source.read_bytes()
        if len(raw) != COUNT * 4 * 4:
            raise ValueError(f'{name}: expected {COUNT * 16} RGBA32F bytes')
        values = struct.unpack(f'<{COUNT*4}f', raw)
        if not all(math.isfinite(v) for v in values):
            raise ValueError(f'{name}: non-finite source sample')
        payload = []
        for i in range(COUNT):
            payload.extend(values[i*4:i*4+n])
        payloads[name] = struct.pack(f'<{COUNT*n}f', *payload)
        result['channels'][name] = {'components': n, 'file': name + '.f32le'}
    for name in LAYOUT:
        (output / (name + '.f32le')).write_bytes(payloads[name])
    (output / 'capture.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in LAYOUT:
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--region-key', required=True)
    parser.add_argument('--mip', type=int, required=True)
    parser.add_argument('--source-fingerprint', required=True)
    parser.add_argument('--composition-revision', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.mip < 0 or args.composition_revision < 0:
        parser.error('mip and composition revision must be nonnegative')
    metadata = {'region_key': args.region_key, 'mip': args.mip,
                'source_fingerprint': args.source_fingerprint,
                'composition_revision': args.composition_revision}
    try:
        result = export(args.output, {k: getattr(args, k) for k in LAYOUT}, metadata)
    except (ValueError, OSError) as error:
        parser.exit(2, f'P09 uncompressed reference export failed: {error}\n')
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
