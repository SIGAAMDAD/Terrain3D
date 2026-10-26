#!/usr/bin/env python3
"""Strict v2/v3 offline capture comparator regression tests."""
import struct
import tempfile
from pathlib import Path
from compare_feedback_v2_v3 import compare, read_capture

HEADER = struct.Struct('<8IQ')
RECORD = struct.Struct('<9I')
INVALID = 0xFFFFFFFF


def write_pair(root, old, new, version=3):
    # 2x2 coarse grid, identical slot identity in both readback captures.
    common = (0x47465456, 2, 1, 101, 7, 3, 4, 4, 0x1122334455667788)
    (root/'v2.bin').write_bytes(HEADER.pack(*common) + struct.pack('<4I', *old))
    v3_header = list(common)
    v3_header[1] = version
    v3_header[7] = 36
    (root/'v3.bin').write_bytes(HEADER.pack(*v3_header) + b''.join(RECORD.pack(*rec) for rec in new))
    return root/'v2.bin', root/'v3.bin'


def record(*pages):
    assert len(pages) <= 4
    return [*pages, *([INVALID] * (4-len(pages))),
            *([1] * len(pages)), *([0] * (4-len(pages))), 0]


with tempfile.TemporaryDirectory() as dirname:
    root = Path(dirname)
    baseline, candidate = write_pair(root, [1, 2, INVALID, 3],
                                      [record(1, 4), record(2, 5), record(), record(3)])
    success = compare(baseline, candidate, 2, 2, 12)
    assert success['accepted'] and success['dropped_legacy_cells'] == 0
    assert success['version2_unique_pages'] == 3 and success['version3_unique_pages'] == 5
    baseline, candidate = write_pair(root, [1, 2, INVALID, 3],
                                      [record(1), record(5), record(), record(3)])
    failure = compare(baseline, candidate, 2, 2, 12)
    assert not failure['accepted'] and failure['dropped_legacy_cells'] == 1
    old = [record(1), record(2), record(), record(3)]
    old[2][8] = 1
    baseline, candidate = write_pair(root, [1, 2, INVALID, 3], old)
    try:
        read_capture(candidate, 3, 2, 2, 12)
        raise AssertionError('overflow incorrectly accepted')
    except ValueError:
        pass
    old[2][8] = 0
    baseline, candidate = write_pair(root, [1, 2, INVALID, 3], old, version=2)
    try:
        compare(baseline, candidate, 2, 2, 12)
        raise AssertionError('wrong version incorrectly accepted')
    except ValueError:
        pass
    old[2][8] = 0
    baseline, candidate = write_pair(root, [1, 2, INVALID, 3], old)
    with candidate.open('ab') as out:
        out.write(b'x')
    try:
        compare(baseline, candidate, 2, 2, 12)
        raise AssertionError('bad size incorrectly accepted')
    except ValueError:
        pass
print('P31b capture comparator: PASS')
