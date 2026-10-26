#!/usr/bin/env python3
"""P31b randomized reference checks for complete multi-request capture.

This producer is experimental and has no active raster source. The test proves
that accepted cells retain every recorded page request, not live GPU parity.
"""
import argparse
import random
import struct
import subprocess
from pathlib import Path

from compile_fragment_variants import shader_root, split_sections

INVALID = 0xFFFFFFFF
MAX_LAYERS = 8
CELL_PAGES = 4
MAX_PIXELS = 256


def inverse_edge(cell, viewport, grid):
    return (cell * viewport + grid - 1) // grid


def encode_cell(requests, counts, sw, sh, layers, gx, gy, gw, gh, page_count):
    x0 = inverse_edge(gx, sw, gw)
    x1 = min(inverse_edge(gx + 1, sw, gw), sw)
    y0 = inverse_edge(gy, sh, gh)
    y1 = min(inverse_edge(gy + 1, sh, gh), sh)
    if (x1 - x0) * (y1 - y0) > MAX_PIXELS:
        return [INVALID] * CELL_PAGES + [0] * CELL_PAGES + [INVALID]
    page_ids = [INVALID] * CELL_PAGES
    frequency = [0] * CELL_PAGES
    overflow = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = y * sw + x
            count = counts[index]
            if count > layers:
                overflow = INVALID
                continue
            for layer in range(count):
                page = requests[index][layer]
                if page >= page_count:
                    overflow = INVALID
                    continue
                if page in page_ids:
                    frequency[page_ids.index(page)] += 1
                elif INVALID in page_ids:
                    pos = page_ids.index(INVALID)
                    page_ids[pos] = page
                    frequency[pos] = 1
                elif overflow != INVALID:
                    overflow += 1
    return page_ids + frequency + [overflow]


def reference_case(rng):
    sw, sh = rng.randint(1, 32), rng.randint(1, 32)
    gw, gh = rng.randint(1, 12), rng.randint(1, 12)
    layers = rng.randint(1, MAX_LAYERS)
    page_count = rng.randint(1, 24)
    # Favor repeated material identities, but also stress >4-way collisions.
    palette = rng.sample(range(page_count), min(page_count, rng.randint(1, 8)))
    requests, counts = [], []
    for _ in range(sw * sh):
        n = rng.randrange(layers + 1)
        if rng.randrange(23) == 0:
            n = layers + 1  # recorded source-layer overflow
        counts.append(n)
        pages = [rng.choice(palette) for _ in range(layers)]
        if rng.randrange(41) == 0 and n > 0 and n <= layers:
            pages[0] = page_count  # invalid producer page; reject
        requests.append(pages)
    values = []
    for y in range(gh):
        for x in range(gw):
            cell = encode_cell(requests, counts, sw, sh, layers, x, y, gw, gh, page_count)
            values.append(cell)
            assert len(cell) == 9
            assert cell[8] == INVALID or 0 <= cell[8] <= MAX_PIXELS * layers
            for page, count in zip(cell[:4], cell[4:8]):
                assert (page == INVALID and count == 0) or (0 <= page < page_count and count > 0)
            if cell[8] != 0:
                continue  # CPU reducer must refuse capture; no partial admission
            x0, x1 = inverse_edge(x, sw, gw), min(inverse_edge(x + 1, sw, gw), sw)
            y0, y1 = inverse_edge(y, sh, gh), min(inverse_edge(y + 1, sh, gh), sh)
            direct = {}
            for py in range(y0, y1):
                for px in range(x0, x1):
                    i = py * sw + px
                    assert counts[i] <= layers
                    for page in requests[i][:counts[i]]:
                        assert page < page_count
                        direct[page] = direct.get(page, 0) + 1
            assert len(direct) <= CELL_PAGES
            observed = {p: c for p, c in zip(cell[:4], cell[4:8]) if p != INVALID}
            assert observed == direct
    return values


def check_spirv(blob):
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    assert words[0] == 0x07230203
    pos = 5
    instructions = 0
    atomics = 0
    while pos < len(words):
        length, opcode = words[pos] >> 16, words[pos] & 65535
        assert length > 0 and pos + length <= len(words)
        instructions += 1
        atomics += int(227 <= opcode <= 244)
        pos += length
    assert atomics == 0
    return instructions


def run(root, glslang, out):
    rd = shader_root(root)
    source = (rd / 'terrain_vt_feedback_multi_writer.glsl').read_text()
    sections = split_sections(source)
    assert 'reduce_multi' in sections['versions']
    out.mkdir(parents=True, exist_ok=True)
    shader = out / 'multi_writer.comp'
    result = out / 'multi_writer.spv'
    shader.write_text(sections['compute'].replace('#VERSION_DEFINES', ''))
    process = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.2',
                              '-S', 'comp', '--spirv-val', str(shader), '-o', str(result)],
                             capture_output=True, text=True, timeout=90)
    assert process.returncode == 0, process.stdout + process.stderr
    instructions = check_spirv(result.read_bytes())
    rng = random.Random(0x31B0)
    for _ in range(1100):
        reference_case(rng)
    assert encode_cell([[0] * 8] * 257, [1] * 257, 257, 1, 8, 0, 0, 1, 1, 10)[8] == INVALID
    too_many = [[i] * 8 for i in range(5)]
    assert encode_cell(too_many, [1] * 5, 5, 1, 8, 0, 0, 1, 1, 10)[8] == 1
    print(f'P31b PASS: 1100 randomized source grids, {instructions} static SPIR-V ops, zero atomics')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--glslang', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    run(args.root, args.glslang, args.out)
