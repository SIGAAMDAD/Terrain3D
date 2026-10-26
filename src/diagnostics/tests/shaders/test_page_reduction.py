#!/usr/bin/env python3
"""P30 small-page hash/compare exactness and Vulkan SPIR-V contract tests.

This validates integer reduction equivalence and compiler structure; it does
not substitute for GPU readback equality or a device-level performance test.
"""

from __future__ import annotations

from collections import Counter
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
import sys

from compile_fragment_variants import ROOT, split_sections, variants

FORMATS = ('r32f', 'rgba8', 'r16ui', 'r32ui')
MASK = 0xffffffff
LOCAL_SIZE = 8
ATOMIC_MIN = 227
ATOMIC_MAX = 244


def words_in_spirv(path):
    blob = path.read_bytes()
    w = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    assert w[0] == 0x07230203
    entries = []
    i = 5
    while i < len(w):
        size = w[i] >> 16
        assert 0 < size <= len(w) - i
        entries.append((w[i] & 65535, w[i + 1:i + size]))
        i += size
    return entries


def layout(spv):
    decorations = {}
    offsets = []
    for op, args in words_in_spirv(spv):
        if op == 71 and len(args) > 2 and args[1] in (33, 34):
            decorations.setdefault(args[0], {})[args[1]] = args[2]
        if op == 72 and len(args) > 3 and args[2] == 35:
            offsets.append((args[1], args[3]))
    bindings = sorted((decor[34], decor[33]) for decor in decorations.values()
                      if 33 in decor and 34 in decor)
    return bindings, sorted(offsets)


def compile_shaders(root, glslang, out):
    src = root / 'project/addons/terrain_3d/compute/terrain_page_storage.glsl'
    sections = split_sections(src.read_text(encoding='utf-8'))
    opts = variants(sections)
    out.mkdir(parents=True, exist_ok=True)
    report = {}
    for fmt in FORMATS:
        for ver in (fmt, fmt + '_reduce'):
            shader = out / f'page_{ver}.comp'
            spv = out / f'page_{ver}.spv'
            shader.write_text(sections['compute'].replace('#VERSION_DEFINES', opts[ver]), encoding='utf-8')
            p = subprocess.run([str(glslang), '-V', '--target-env', 'vulkan1.2', '-S', 'comp',
                                '--spirv-val', str(shader), '-o', str(spv)],
                               text=True, capture_output=True, timeout=60)
            output = p.stdout + p.stderr
            assert p.returncode == 0 and 'Validation Errors' not in output, (ver, output)
            insn = words_in_spirv(spv)
            atomics = Counter(op for op, _ in insn if ATOMIC_MIN <= op <= ATOMIC_MAX)
            report[ver] = {'instructions': len(insn), 'atomics': sum(atomics.values()),
                           'opcodes': dict(atomics), 'bytes': spv.stat().st_size}
        assert layout(out / f'page_{fmt}.spv') == layout(out / f'page_{fmt}_reduce.spv'), fmt
        assert layout(out / f'page_{fmt}.spv')[0] == [(0, 0), (0, 1), (0, 2)], fmt
        assert report[fmt + '_reduce']['atomics'] == 0, fmt
        assert report[fmt]['atomics'] >= 2, fmt
    return report


def hash_word(x, y, bits):
    k = ((x * 0x9E3779B9) ^ (y * 0x85EBCA6B)) & MASK
    for word in bits:
        k ^= word
    return k & MASK


def reference(width, height, target, other):
    hashed = 0
    different = 0
    for y in range(height):
        for x in range(width):
            index = y * width + x
            hashed ^= hash_word(x, y, target[index])
            different |= int(target[index] != other[index])
    return hashed, different


def reduced(width, height, target, other):
    h = [0] * 64
    d = [0] * 64
    for lane in range(64):
        lx, ly = lane % LOCAL_SIZE, lane // LOCAL_SIZE
        for y in range(ly, height, LOCAL_SIZE):
            for x in range(lx, width, LOCAL_SIZE):
                index = y * width + x
                h[lane] ^= hash_word(x, y, target[index])
                d[lane] |= int(target[index] != other[index])
    for stride in (32, 16, 8, 4, 2, 1):
        for lane in range(stride):
            h[lane] ^= h[lane + stride]
            d[lane] |= d[lane + stride]
    return h[0], d[0]


def check_reduction():
    rng = random.Random(0x2A30)
    extents = [(1, 1), (1, 7), (7, 1), (7, 11), (8, 8), (9, 9),
               (16, 16), (32, 32), (5, 32), (32, 5), (31, 32), (32, 31)]
    extents += [(rng.randrange(1, 33), rng.randrange(1, 33)) for _ in range(140)]
    for fmt in FORMATS:
        for width, height in extents:
            count = width * height
            target = []
            other = []
            for i in range(count):
                if fmt == 'r16ui':
                    v = (rng.randrange(65536), 0, 0, 0)
                elif fmt == 'r32ui':
                    v = (rng.getrandbits(32), 0, 0, 0)
                elif fmt == 'rgba8':
                    # model the *returned float bits* after normalized texel conversion
                    v = tuple(struct.unpack('<I', struct.pack('<f', rng.randrange(256) / 255.0))[0]
                              for _ in range(4))
                else:
                    v = tuple(rng.getrandbits(32) for _ in range(4))
                    if i % 29 == 0:
                        v = (0x7fc00001, 0x80000000, 0, 0x7f800000)
                target.append(v)
                other.append(v if rng.randrange(7) else (v[0] ^ 1, *v[1:]))
            assert reference(width, height, target, other) == reduced(width, height, target, other)
            assert reference(width, height, target, target) == reduced(width, height, target, target)
    return len(extents) * len(FORMATS)


def verify_source_contract(root):
    shader = (root / 'project/addons/terrain_3d/compute/terrain_page_storage.glsl').read_text()
    page = (root / 'src/rendering/rd/terrain_pages/terrain_3d_page_compute.cpp').read_text()
    cat = (root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp').read_text()
    lib = (root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp').read_text()
    assert 'result.hash_xor = pc.operation == 2u ? s_page_partials[0] : 0u;' in shader
    assert 'width <= 32u && height <= 32u' in page
    assert 'reduce ? 1u : ( partial ? ( width + 15u ) / 16u : ( width + 7u ) / 8u )' in page
    assert '&& !reduce && !partial' in page
    assert 'T3D_RD_SHADER_PAGE_COMPUTE_R32F_REDUCE' in cat
    assert 'TERRAIN3D_GPU_PAGE_REDUCE' in lib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--glslang', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    amount = check_reduction()
    verify_source_contract(args.root)
    data = compile_shaders(args.root, args.glslang, args.out)
    print(json.dumps(data, indent=2))
    print(f'P30 page reduction: {amount} format/extent trials plus shader ABI PASS')
    (args.out / 'gpu_page_reduction.json').write_text(json.dumps(data, indent=2) + '\n')


if __name__ == '__main__':
    sys.exit(main())
