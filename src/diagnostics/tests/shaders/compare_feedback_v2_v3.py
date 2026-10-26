#!/usr/bin/env python3
"""P31b strict offline capture comparator (slot-sized GPU readback binary files).

Reports exact per-cell version-2 demand recall by a complete version-3 capture.
Only captured data is compared; this cannot prove a GPU timing improvement.
"""
import argparse
import json
import struct
from pathlib import Path

MAGIC = 0x47465456
INVALID = 0xFFFFFFFF
HEADER = struct.Struct('<8IQ')
RECORD = struct.Struct('<9I')


def read_capture(path, version, width, height, page_count):
    payload = Path(path).read_bytes()
    cells = width * height
    stride = 4 if version == 2 else RECORD.size
    if len(payload) != HEADER.size + cells * stride:
        raise ValueError('bad capture length')
    magic, found_version, slot, frame, epoch, database, device, words, viewport = HEADER.unpack_from(payload)
    if magic != MAGIC or found_version != version or slot >= 4 or words != cells * (1 if version == 2 else 9):
        raise ValueError('bad capture header')
    identity = (slot, frame, epoch, database, device, viewport)
    result = []
    for i in range(cells):
        offset = HEADER.size + i * stride
        if version == 2:
            page, = struct.unpack_from('<I', payload, offset)
            result.append(set() if page >= page_count else {page})
            continue
        *raw, overflow = RECORD.unpack_from(payload, offset)
        if overflow != 0:
            raise ValueError(f'incomplete cell {i}: overflow {overflow}')
        pages, weights = raw[:4], raw[4:]
        accepted = set()
        total = 0
        for page, weight in zip(pages, weights):
            if page == INVALID:
                if weight != 0:
                    raise ValueError(f'malformed empty slot in cell {i}')
                continue
            if page >= page_count or weight <= 0 or page in accepted:
                raise ValueError(f'malformed page record in cell {i}')
            accepted.add(page)
            total += weight
        if total > 256 * 8:
            raise ValueError(f'impossible source sample count in cell {i}')
        result.append(accepted)
    return identity, result


def compare(reference, candidate, width, height, page_count):
    refid, old = read_capture(reference, 2, width, height, page_count)
    new_id, new = read_capture(candidate, 3, width, height, page_count)
    if refid != new_id:
        raise ValueError('capture identity or epoch mismatch')
    lost = []
    for i, (before, after) in enumerate(zip(old, new)):
        if not before.issubset(after):
            lost.append(i)
    return dict(accepted=not lost, cells=len(old),
                cells_with_legacy_demand=sum(bool(x) for x in old),
                cells_with_multi_demand=sum(bool(x) for x in new),
                version2_unique_pages=len(set.union(set(), *old)),
                version3_unique_pages=len(set.union(set(), *new)),
                dropped_legacy_cells=len(lost),
                first_dropped_cell_indices=lost[:16])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--width', type=int, required=True)
    parser.add_argument('--height', type=int, required=True)
    parser.add_argument('--pages', type=int, required=True)
    args = parser.parse_args()
    if args.width < 1 or args.height < 1 or args.width > 240 or args.height > 136 or args.pages < 1:
        parser.error('invalid grid dimensions or page count')
    try:
        result = compare(args.baseline, args.candidate, args.width, args.height, args.pages)
    except ValueError as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['accepted'] else 1)


if __name__ == '__main__':
    main()
