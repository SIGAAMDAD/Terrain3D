#!/usr/bin/env python3
"""page and instance reduction offline shader contracts: large-page reduction and exclusive-command cull.

Compiler and CPU-reference tests, not a substitute for real Vulkan readback or
performance measurement. All random tests use a fixed seed.
"""
from __future__ import annotations

import argparse
import ast
from collections import Counter
import json
from pathlib import Path
import random
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
FORMATS = ("r32f", "rgba8", "r16ui", "r32ui")
MASK = 0xffffffff


def compile_variants(source: Path, names, glslang: Path, out: Path):
    head, body = source.read_text().split("#[compute]", 1)
    versions = {}
    for line in head.splitlines():
        if "=" in line:
            name, value = line.split("=", 1)
            versions[name.strip()] = ast.literal_eval(value.strip().rstrip(";"))
    out.mkdir(parents=True, exist_ok=True)
    output = {}
    for version in names:
        assert version in versions
        comp = out / f"{source.stem}_{version}.comp"
        spv = out / f"{source.stem}_{version}.spv"
        comp.write_text(body.replace("#VERSION_DEFINES", versions[version]))
        proc = subprocess.run([str(glslang), "-V", "--target-env", "vulkan1.2",
                               "-S", "comp", "--spirv-val", str(comp), "-o", str(spv)],
                              capture_output=True, text=True, timeout=60)
        assert proc.returncode == 0, (version, proc.stdout, proc.stderr)
        insn = words(spv)
        output[version] = dict(instructions=len(insn),
                               atomics=sum(1 for op, _ in insn if 227 <= op <= 244),
                               bindings=sorted((decor[34], decor[33]) for decor in decoration_map(insn).values()
                                               if 33 in decor and 34 in decor))
    return output


def words(path: Path):
    blob = path.read_bytes()
    assert len(blob) % 4 == 0
    data = struct.unpack("<" + "I" * (len(blob) // 4), blob)
    assert data[0] == 0x07230203
    pc = 5
    result = []
    while pc < len(data):
        op = data[pc] & 65535
        size = data[pc] >> 16
        assert 0 < size <= len(data) - pc
        result.append((op, data[pc + 1:pc + size]))
        pc += size
    return result


def decoration_map(insn):
    out = {}
    for op, args in insn:
        if op == 71 and len(args) >= 3 and args[1] in (33, 34):
            out.setdefault(args[0], {})[args[1]] = args[2]
    return out


def member_offsets(insn):
    return sorted(set((args[1], args[3]) for op, args in insn
                      if op == 72 and len(args) >= 4 and args[2] == 35))


def bit_value(x: int, y: int, fmt: str, salt: int):
    key = ((x * 0x9E3779B9) ^ (y * 0x85EBCA6B)) & MASK
    if fmt == "r16ui":
        channels = (salt & 0xffff, 0, 0, 0)
    elif fmt == "r32ui":
        channels = (salt & MASK, 0, 0, 0)
    elif fmt == "rgba8":
        channels = tuple(struct.unpack("<I", struct.pack("<f", ((salt >> (i * 7)) & 255) / 255.0))[0]
                         for i in range(4))
    else:
        channels = tuple((salt ^ (i * 0xA6E1E37D)) & MASK for i in range(4))
    for channel in channels:
        key ^= channel
    return key & MASK


def check_pages():
    rng = random.Random(0xB330)
    shapes = [(33, 33), (63, 64), (64, 63), (129, 65), (255, 127),
              (512, 64), (513, 17), (17, 513), (1024, 1024),
              (1024, 1), (1, 1024)]
    shapes.extend((rng.randrange(33, 270), rng.randrange(33, 270)) for _ in range(38))
    count = 0
    for fmt in FORMATS:
        for w, h in shapes:
            nx = (w + 15) // 16
            ny = (h + 15) // 16
            scratch = [0] * (nx * ny)
            xor_reference = 0
            or_reference = 0
            for y in range(h):
                for x in range(w):
                    salt = (x * 0x7F4A7C15 + y * 0x9E3779B9 + 0x71E2) & MASK
                    word = bit_value(x, y, fmt, salt)
                    xor_reference ^= word
                    scratch[(y // 16) * nx + x // 16] ^= word
                    # Every 97th pixel mismatches; include no-mismatch case below.
                    or_reference |= int((x + y * w) % 97 == 0)
            assert xor_reference == __import__('functools').reduce(lambda a, b: a ^ b, scratch, 0)
            # The compare shader produces one OR partial per tile, not the XOR partial.
            differs = [0] * len(scratch)
            for y in range(h):
                for x in range(w):
                    differs[(y // 16) * nx + x // 16] |= int((x + y * w) % 97 == 0)
            assert int(any(differs)) == or_reference
            assert int(any([0] * len(differs))) == 0
            # final stage iterates lane, lane+64, ... through the same cells.
            lanes = [0] * 64
            for i, item in enumerate(scratch):
                lanes[i & 63] ^= item
            for stride in (32, 16, 8, 4, 2, 1):
                for i in range(stride):
                    lanes[i] ^= lanes[i + stride]
            assert lanes[0] == xor_reference
            assert len(scratch) <= 4096
            count += 1
    return count


def check_exclusive():
    rng = random.Random(0xB329)
    cases = 0
    for _ in range(800):
        command_count = rng.randint(1, 20)
        owner_map = [rng.randrange(command_count) for __ in range(rng.randint(0, 28))]
        counts = Counter(owner_map)
        exclusive = all(n <= 1 for n in counts.values())
        assert exclusive == (len(set(owner_map)) == len(owner_map))
        seen = [0] * command_count
        for command in owner_map:
            seen[command] += 1
        assert exclusive == all(count <= 1 for count in seen)
        if not exclusive:
            # In the collision case, the shader must still reserve with atomicAdd.
            assert any(count >= 2 for count in seen)
            continue
        for command in owner_map:
            count = rng.randint(0, 320)
            visible = [rng.getrandbits(1) for _ in range(count)]
            atomic_chunks = []
            exclusive_chunks = []
            atomic_total = 0
            unique_total = 0
            for first in range(0, count, 64):
                accepted = [first + i for i in range(min(64, count - first)) if visible[first + i]]
                atomic_chunks.extend((atomic_total + offset, index) for offset, index in enumerate(accepted))
                exclusive_chunks.extend((unique_total + offset, index) for offset, index in enumerate(accepted))
                atomic_total += len(accepted)
                unique_total += len(accepted)
            assert atomic_total == unique_total
            assert atomic_chunks == exclusive_chunks
        cases += 1
    return cases


def contracts(root: Path):
    page = (root / "src/rendering/rd/terrain_pages/terrain_3d_page_compute.cpp").read_text()
    lib = (root / "src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp").read_text()
    inst = (root / "src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp").read_text()
    assert "T3D_PAGE_PARTIAL_CAPACITY" in page
    assert "compute_list_add_barrier( list );\n\tif ( partial )" in page
    assert "m_finalLease" in page
    assert "isPageReductionEnabledCold( void )" in lib
    assert "T3D_RD_SHADER_PAGE_COMPUTE_R32F_REDUCE" in lib
    assert "T3D_RD_SHADER_PAGE_COMPUTE_REDUCE_FINAL" in lib
    assert "isInstanceExclusiveEnabledCold( void )" in lib
    assert "isInstanceCommandOwnerEnabledCold( void )" in lib
    assert "return false;" in lib
    assert "ownership[index]++ != 0u" in inst
    assert "m_bMainCommandOwnershipExclusive" in inst
    assert "isInstanceExclusiveEnabledCold" in inst


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--glslang", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    checks = check_pages()
    exclusive = check_exclusive()
    contracts(args.root)
    page = args.root / "project/addons/terrain_3d/compute/terrain_page_storage.glsl"
    names = [name for fmt in FORMATS for name in (fmt, fmt + "_reduce", fmt + "_partial")]
    names.append("reduce_final")
    data = compile_variants(page, names, args.glslang, args.out)
    for fmt in FORMATS:
        assert data[fmt + "_partial"]["atomics"] == 0
        assert data[fmt + "_partial"]["bindings"] == [(0, 0), (0, 1), (0, 2), (0, 3)]
        assert data[fmt + "_reduce"]["bindings"] == data[fmt]["bindings"]
    assert data["reduce_final"]["atomics"] == 0
    assert data["reduce_final"]["bindings"] == [(0, 2), (0, 3)]
    for fmt in FORMATS:
        for a, b in ((fmt, fmt + "_reduce"), (fmt, fmt + "_partial"), (fmt, "reduce_final")):
            ia = member_offsets(words(args.out / f"{page.stem}_{a}.spv"))
            ib = member_offsets(words(args.out / f"{page.stem}_{b}.spv"))
            assert ia == ib, (a, b, ia, ib)
    inst = args.root / "project/addons/terrain_3d/rd/terrain_instance_cull.glsl"
    culls = compile_variants(inst, ("legacy", "grouped"), args.glslang, args.out)
    assert culls["legacy"]["atomics"] >= culls["grouped"]["atomics"]
    print(json.dumps(dict(page=data, cull=culls, page_trials=checks, exclusive_trials=exclusive), indent=2))
    (args.out / "page_instance_shader_report.json").write_text(json.dumps(dict(page=data,cull=culls), indent=2) + '\n')
    print(f"page and instance reduction contracts PASS: {checks} page format/extent trials and {exclusive} exclusive descriptor topologies")


if __name__ == "__main__":
    sys.exit(main())
