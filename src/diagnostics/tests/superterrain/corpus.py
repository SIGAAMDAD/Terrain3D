#!/usr/bin/env python3
"""Generate/verify fixed synthetic SOURCE-region golden inputs for ST01..ST04.

This is not a renderer golden-image test.  Values mirror the current Godot
R16_UNORM height, float-bit RF control and RGBA8 authored color contracts.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

SIDE = 128
MIN_HEIGHT, MAX_HEIGHT = -384.0, 768.0
HEADER = struct.Struct('<4sHHHHIIIQ')  # 32 bytes, no host-endian dependency
CORPUS = Path(__file__).with_name('corpus')
FIXTURES = (
    ('absent', 0, 'missing logical region: no payload, no residency'),
    ('empty_holes', 480, 'present region, all texels cut out'),
    ('flat', 481, 'flat zero elevation and implicit-safe authored controls'),
    ('negative_steep', 482, 'large negative slope and quantization floor'),
    ('cliff', 483, 'step discontinuity and mixed page fallback'),
    ('border_west', 527, 'west neighbor: right edge equals east left edge'),
    ('border_east', 528, 'east neighbor: left edge equals west right edge'),
    ('coverage_edges', 529, 'hole chessboard and texels at page boundaries'),
    ('alpha_extremes', 530, 'four authored-alpha quantization outcomes'),
    ('sparse_controls', 531, 'empty-looking controls, auto shader and isolated overrides'),
    ('wet_surface', 532, 'authored alpha plus wetness parameter permutation'),
    ('rotated_slopes', 533, 'UV rotation/scale fields, slope and auto shader'),
    ('distance_regimes', 534, 'same field observed from near, middle and far camera probes'),
)


def height_at(name, x, y):
    if name in ('absent', 'empty_holes', 'flat', 'alpha_extremes'):
        return 0.0
    if name == 'negative_steep':
        return min(MAX_HEIGHT, max(MIN_HEIGHT, -384.0 + x * 0.68 + y * 0.42))
    if name == 'cliff':
        return -240.0 if x < 64 else 510.0
    if name in ('border_west', 'border_east'):
        shared_world_x = x + (127 if name == 'border_east' else 0)
        return -80.0 + 0.5 * shared_world_x + 0.3 * y + 4.0 * math.sin(y / 8.0)
    if name == 'coverage_edges':
        return -45.0 + y * 0.1
    if name == 'sparse_controls':
        return 64.0 + 2.0 * math.sin(x / 15.0) * math.cos(y / 10.0)
    if name == 'wet_surface':
        return 21.0 + 0.4 * math.sin(x / 7.0)
    if name == 'rotated_slopes':
        return -70.0 + x * 1.45 + 17.0 * math.sin(y / 10.0)
    if name == 'distance_regimes':
        return 84.0 + 31.0 * math.sin(x / 22.0) * math.cos(y / 29.0)
    raise ValueError(name)


def height_u16(height):
    normalized = max(0.0, min(1.0, (height - MIN_HEIGHT) / (MAX_HEIGHT - MIN_HEIGHT)))
    return max(0, min(65535, int(math.floor(normalized * 65535.0 + 0.5))))


def control_at(name, x, y):
    base, overlay, blend, rotate, scale, hole, nav, auto = 1, 2, 64, 0, 0, 0, 0, 0
    if name == 'empty_holes':
        hole = 1
    if name == 'cliff':
        blend = 255 if x >= 64 else 0
    if name == 'coverage_edges':
        hole = int(((x // 8) + (y // 8)) % 2 == 0)
    if name == 'sparse_controls':
        base, overlay, blend, auto = 0, 0, 0, 1
        if (x, y) in {(0, 0), (63, 63), (64, 64), (127, 127)}:
            base, overlay, blend, auto = 31, 30, 255, 0
        hole = int(x == 0 and y >= 120)
    if name == 'wet_surface':
        blend = (x * 2) & 255
    if name == 'rotated_slopes':
        rotate, scale, auto = (x // 8) & 15, (y // 16) & 7, int((x + y) % 11 == 0)
        blend = y * 2
    if name == 'distance_regimes':
        base, overlay, blend = x // 32, y // 32, (x ^ y) & 255
    return ((base & 31) << 27) | ((overlay & 31) << 22) | ((blend & 255) << 14) | \
           ((rotate & 15) << 10) | ((scale & 7) << 7) | (hole << 2) | (nav << 1) | auto


def color_at(name, x, y):
    alpha = 128
    if name == 'alpha_extremes':
        alpha = (x // 32) * 85
    elif name == 'wet_surface':
        alpha = (x * 2) & 255
    elif name == 'rotated_slopes':
        alpha = y * 2
    return (x * 2 & 255, y * 2 & 255, ((x + y) * 3) & 255, alpha)


def derived_at(name, x, y):
    # Source footprint only: RG8 encodes lateral normal as [0,255].
    # Height continuity is tested from R16, not this lossy derived normal.
    dx = height_at(name, min(127, x + 1), y) - height_at(name, max(0, x - 1), y)
    dy = height_at(name, x, min(127, y + 1)) - height_at(name, x, max(0, y - 1))
    norm = math.sqrt(dx * dx + dy * dy + 4.0)
    return (max(0, min(255, int((-dx / norm * 0.5 + 0.5) * 255 + 0.5))),
            max(0, min(255, int((-dy / norm * 0.5 + 0.5) * 255 + 0.5))))


def construct(name, index):
    if name == 'absent':
        payload = b''
        flags = 0
    else:
        height = bytearray(SIDE * SIDE * 2)
        control = bytearray(SIDE * SIDE * 4)
        color = bytearray(SIDE * SIDE * 4)
        derived = bytearray(SIDE * SIDE * 2)
        for y in range(SIDE):
            for x in range(SIDE):
                i = y * SIDE + x
                struct.pack_into('<H', height, i * 2, height_u16(height_at(name, x, y)))
                struct.pack_into('<I', control, i * 4, control_at(name, x, y))
                color[i * 4:i * 4 + 4] = bytes(color_at(name, x, y))
                derived[i * 2:i * 2 + 2] = bytes(derived_at(name, x, y))
        payload = bytes(height + control + color + derived)
        flags = 3  # present + derived RG8
    sha = hashlib.sha256(payload).digest()
    signature = int.from_bytes(sha[:8], 'little')
    header = HEADER.pack(b'ST0R', 1, SIDE, index, flags, len(payload), zlib.crc32(payload), 0, signature)
    return header + payload


def describe(name, index, purpose, content):
    alpha = {'alpha_extremes': [0, 85, 170, 255], 'wet_surface': 'gradient',
             'rotated_slopes': 'row gradient'}.get(name, 128)
    return {'name': name, 'region_index': index, 'description': purpose,
            'source_schema': {'height': 'R16_UNORM global [-384,768] LE',
                              'control': 'RF32 bit-pattern LE', 'color': 'RGBA8',
                              'derived': 'RG8 XZ normal component'},
            'authored_variants': {'alpha': alpha, 'wetness_contribution': 1.0 if name == 'wet_surface' else 0.0,
                                  'projection_mode': 'varying' if name == 'rotated_slopes' else 'default',
                                  'detiling': name in ('wet_surface', 'rotated_slopes'),
                                  'camera_distance_m': [8, 150, 1100] if name == 'distance_regimes' else []},
            'bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest()}


def run(write):
    CORPUS.mkdir(parents=True, exist_ok=True)
    index = {'schema': 'ST00_SYNTHETIC_REGION_CORPUS_V1', 'side': SIDE, 'byte_order': 'little',
             'note': 'Synthetic SOURCE data, not a canonical material render or screenshot', 'fixtures': []}
    for name, region_index, purpose in FIXTURES:
        result = construct(name, region_index)
        file_path = CORPUS / (name + '.st00region')
        if write:
            file_path.write_bytes(result)
        elif not file_path.is_file() or file_path.read_bytes() != result:
            raise AssertionError('Corpus input drift: ' + str(file_path))
        index['fixtures'].append(describe(name, region_index, purpose, result))
    canonical = json.dumps(index, indent=2, sort_keys=True) + '\n'
    index_path = CORPUS / 'index.json'
    if write:
        index_path.write_text(canonical, encoding='utf-8')
    elif not index_path.is_file() or index_path.read_text(encoding='utf-8') != canonical:
        raise AssertionError('Corpus index drift')
    print(f'ST00 golden source corpus verified: {len(FIXTURES)} fixtures, '
          f'{sum(row["bytes"] for row in index["fixtures"])} binary bytes')
    return index


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true', help='Intentionally freeze corpus')
    args = parser.parse_args()
    run(args.write)
