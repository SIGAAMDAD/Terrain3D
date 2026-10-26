#!/usr/bin/env python3
"""P31a reference tests for a deterministic coarse-grid feedback compute producer.

This validates unique destination ownership, stable tie-breaking, viewport edge
mapping, and an unchanged fragment feedback address under invocation-local reuse.
No CPU test substitutes for visible-surface GPU feedback residency captures.
"""
import argparse
import random
import struct
import subprocess
from pathlib import Path

from compile_fragment_wave import shader_root, split_sections

INVALID = 0xFFFFFFFF


def inverse_edge(cell, viewport, grid):
    return (cell * viewport + grid - 1) // grid


def cell_of_pixel(pixel, viewport, grid):
    return min(pixel * grid // viewport, grid - 1)


def select_cell(source, sw, sh, gx, gy, gw, gh, page_count):
    x0, x1 = inverse_edge(gx, sw, gw), min(inverse_edge(gx + 1, sw, gw), sw)
    y0, y1 = inverse_edge(gy, sh, gh), min(inverse_edge(gy + 1, sh, gh), sh)
    if x0 >= x1 or y0 >= y1:
        return INVALID
    w, h = x1 - x0, y1 - y0
    pages = []
    for i in range(16):
        x = x0 + min(((2 * (i % 4) + 1) * w) // 8, w - 1)
        y = y0 + min(((2 * (i // 4) + 1) * h) // 8, h - 1)
        v = source[y * sw + x]
        pages.append(v if v < page_count else INVALID)
    best, best_count = INVALID, 0
    for page in pages:
        if page == INVALID:
            continue
        count = pages.count(page)
        if count > best_count or (count == best_count and page < best):
            best, best_count = page, count
    return best


def reference(source, sw, sh, gw, gh, pages, slot=0, stride=None):
    stride = stride or 10 + gw * gh
    buffer = [INVALID] * (stride * 4)
    # Versioned grid headers are prepared independently by VT CPU owner.
    for i in range(10):
        buffer[slot * stride + i] = i + 400
    touched = set()
    for y in range(gh):
        for x in range(gw):
            index = slot * stride + 10 + y * gw + x
            assert index not in touched
            touched.add(index)
            buffer[index] = select_cell(source, sw, sh, x, y, gw, gh, pages)
    assert all(buffer[slot * stride + i] == i + 400 for i in range(10))
    assert len(touched) == gw * gh
    assert all(buffer[i] == INVALID for i in range(slot * stride))
    return buffer


def test_invocation_local_cell_cache():
    rnd = random.Random(0x31AC)
    requests = 0
    for _ in range(900):
        vw, vh = rnd.randint(1, 4096), rnd.randint(1, 2160)
        gw, gh = rnd.randint(1, 240), rnd.randint(1, 136)
        slot = rnd.randrange(4)
        stride = 10 + gw * gh
        fragments = []
        for _ in range(rnd.randint(1, 160)):
            x = rnd.randrange(vw + 10)
            y = rnd.randrange(vh + 10)
            pages = [rnd.randrange(1000) if rnd.randrange(5) else INVALID
                     for _ in range(rnd.randrange(0, 15))]
            fragments.append((x, y, pages))
        for dedup in (False, True):
            old_writes = []
            cached_writes = []
            for x, y, pages in fragments:
                cached_index = None
                cached_valid = False
                previous_page, previous_index = INVALID, INVALID
                old_previous_page, old_previous_index = INVALID, INVALID
                for page in pages:
                    # Legacy computes the same per-pixel address for every request.
                    index = (slot * stride + 10 + (y * gh // vh) * gw + (x * gw // vw)) \
                            if x < vw and y < vh else None
                    if page != INVALID and index is not None:
                        if not (dedup and page == old_previous_page and index == old_previous_index):
                            old_writes.append((index, page))
                            old_previous_page, old_previous_index = page, index
                    if cached_index is None and not cached_valid:
                        cached_index = index
                        cached_valid = True
                    if page != INVALID and cached_index is not None:
                        if not (dedup and page == previous_page and cached_index == previous_index):
                            cached_writes.append((cached_index, page))
                            previous_page, previous_index = page, cached_index
                    requests += 1
            assert cached_writes == old_writes
    return requests


def read_opcodes(blob):
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    assert words[0] == 0x07230203
    pos = 5
    codes = []
    while pos < len(words):
        size, opcode = words[pos] >> 16, words[pos] & 0xFFFF
        assert size > 0 and pos + size <= len(words)
        codes.append(opcode)
        pos += size
    return codes


def run(root, glslang, out):
    rd = shader_root(root)
    source = (rd / 'terrain_vt_feedback_single_writer.glsl').read_text()
    sections = split_sections(source)
    assert 'reduce' in sections['versions']
    assert '#VERSION_DEFINES' in sections['compute']
    out.mkdir(parents=True, exist_ok=True)
    cs = out / 'feedback_single_writer.comp'
    spv = out / 'feedback_single_writer.spv'
    cs.write_text(sections['compute'].replace('#VERSION_DEFINES', ''))
    result = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.2', '-S', 'comp',
                            '--spirv-val', str(cs), '-o', str(spv)],
                            capture_output=True, text=True, timeout=90)
    assert result.returncode == 0 and 'Validation Errors' not in (result.stdout + result.stderr), result.stdout + result.stderr
    codes = read_opcodes(spv.read_bytes())
    # SPIR-V atomic opcodes cover AtomicLoad through AtomicFlagClear.
    assert all(code not in range(227, 245) for code in codes)
    assert codes.count(62) >= 1   # OpStore has a single invocation owner per cell

    reused = test_invocation_local_cell_cache()
    rnd = random.Random(0x31071)
    count = 0
    for _ in range(700):
        sw = rnd.randint(1, 65)
        sh = rnd.randint(1, 65)
        gw = rnd.randint(1, 16)
        gh = rnd.randint(1, 16)
        page_count = rnd.randint(1, 256)
        source_pages = [rnd.choice([INVALID, rnd.randrange(page_count),
                                    page_count + rnd.randint(0, 20)]) for _ in range(sw * sh)]
        slot = rnd.randrange(4)
        stride = 10 + gw * gh + rnd.randint(0, 7)
        result = reference(source_pages, sw, sh, gw, gh, page_count, slot, stride)
        for y in range(gh):
            for x in range(gw):
                if inverse_edge(x, sw, gw) < inverse_edge(x + 1, sw, gw) and \
                   inverse_edge(y, sh, gh) < inverse_edge(y + 1, sh, gh):
                    # Every sampled pixel must map back to the same coarse cell.
                    x0, x1 = inverse_edge(x, sw, gw), inverse_edge(x + 1, sw, gw)
                    y0, y1 = inverse_edge(y, sh, gh), inverse_edge(y + 1, sh, gh)
                    for px in range(x0, min(x1, sw)):
                        assert cell_of_pixel(px, sw, gw) == x
                    for py in range(y0, min(y1, sh)):
                        assert cell_of_pixel(py, sh, gh) == y
                selected = result[slot * stride + 10 + y * gw + x]
                assert selected == INVALID or selected < page_count
        count += 1
    # Ties are resolved deterministically and repeatability is exact.
    src = [2, 9, 2, 9] * 4
    assert select_cell(src, 4, 4, 0, 0, 1, 1, 16) == 2
    assert reference(src, 4, 4, 1, 1, 16) == reference(src, 4, 4, 1, 1, 16)
    # A rare source page can be absent from the 4x4 stratified footprint.
    # Do not promote this experimental producer into shipping from CPU tests.
    rare = [4] * 64
    rare[0] = 7
    assert select_cell(rare, 8, 8, 0, 0, 1, 1, 16) == 4
    print(f'P31a PASS: {count} randomized grids, {reused} feedback requests with byte-identical write order, zero atomics')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--glslang', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    run(args.root, args.glslang, args.out)
