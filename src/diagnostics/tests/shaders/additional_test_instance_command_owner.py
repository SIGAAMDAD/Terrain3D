#!/usr/bin/env python3
"""Wave 6 P29b: test CPU-built command CSR and atomics-free command-owned GPU culling.

Tests equivalent accepted records/counts against the existing descriptor-local
append algorithm. GPU execution ordering within shared commands can differ.
These tests do not claim Vulkan device execution or measured performance.
"""
from __future__ import annotations
from collections import Counter
from pathlib import Path
import argparse
import json
import random
import struct
import subprocess
import sys

from compile_siggraph_compute import emit_compute

ROOT = Path(__file__).resolve().parents[4]


def build_pass(commands, descriptors):
    offsets = [0] * (commands + 1)
    for cmd, _ in descriptors:
        if cmd >= commands or cmd < 0:
            raise ValueError('out-of-range command index')
        offsets[cmd + 1] += 1
    for cmd in range(commands):
        offsets[cmd + 1] += offsets[cmd]
    cursor = offsets[:-1].copy()
    indices = [0] * len(descriptors)
    for di, (cmd, _) in enumerate(descriptors):
        indices[cursor[cmd]] = di
        cursor[cmd] += 1
    return offsets + indices


def old_reference(commands, descriptors):
    output = [[] for _ in range(commands)]
    for cmd, records in descriptors:
        # The legacy culler reserves globally once for each nonempty 64-lane chunk.
        for base in range(0, len(records), 64):
            output[cmd].extend(record for record in records[base:base + 64] if record[1])
    return output


def new_reference(commands, descriptors, csr):
    offsets = csr[:commands + 1]
    indices = csr[commands + 1:]
    output = [[] for _ in range(commands)]
    for cmd in range(commands):
        for cursor in range(offsets[cmd], offsets[cmd + 1]):
            di = indices[cursor]
            assert descriptors[di][0] == cmd
            records = descriptors[di][1]
            for base in range(0, len(records), 64):
                output[cmd].extend(record for record in records[base:base + 64] if record[1])
    return output


def test():
    rng = random.Random(0xB629)
    counts = [0, 1, 2, 3, 7, 8, 63, 64, 65, 127, 256]
    cases = 0
    for pass_index in range(2):
        for commands in counts:
            for rep in range(50):
                descriptor_count = 0 if not commands else rng.randrange(0, 220)
                descriptors = []
                for di in range(descriptor_count):
                    command = rng.randrange(commands)
                    instances = [((di, lane, pass_index), rng.randrange(7) != 0)
                                 for lane in range(rng.choice([0, 1, 63, 64, 65, 127, 128, 129, 256]))]
                    descriptors.append((command, instances))
                csr = build_pass(commands, descriptors)
                assert len(csr) == commands + descriptor_count + 1
                assert csr[0] == 0 and csr[commands] == descriptor_count
                old = old_reference(commands, descriptors)
                new = new_reference(commands, descriptors, csr)
                assert [len(x) for x in old] == [len(x) for x in new]
                for cmd in range(commands):
                    assert Counter(map(str, old[cmd])) == Counter(map(str, new[cmd]))
                    assert old[cmd] == new[cmd]  # Same deterministic descriptor order
                cases += 1
    try:
        build_pass(1, [(1, [])])
        raise AssertionError('failed to reject invalid command')
    except ValueError:
        pass
    for main_commands, shadow_commands in [(0, 0), (1, 2), (64, 33), (1024, 1024)]:
        a = [(i % main_commands, []) for i in range(main_commands * 2)] if main_commands else []
        b = [(i % shadow_commands, []) for i in range(shadow_commands)] if shadow_commands else []
        packed = build_pass(main_commands, a) + build_pass(shadow_commands, b)
        shadow_base = main_commands + len(a) + 1
        assert packed[shadow_base:shadow_base + shadow_commands + 1] == build_pass(shadow_commands, b)[:shadow_commands + 1]
        assert len(packed) <= (4 * max(main_commands, shadow_commands, len(a), len(b)) + 2)
    return cases


def opcodes(path):
    blob = path.read_bytes()
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    assert words[0] == 0x07230203
    at = 5
    result = []
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 65535
        assert count > 0
        result.append(op)
        at += count
    return result


def descriptor_bindings(path):
    blob = path.read_bytes()
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    at = 5
    binding_ids = {}
    set_ids = {}
    while at < len(words):
        size, op = words[at] >> 16, words[at] & 65535
        if op == 71 and size >= 4:
            target, decoration, value = words[at + 1:at + 4]
            if decoration == 33:
                binding_ids[target] = value
            if decoration == 34:
                set_ids[target] = value
        at += size
    return {binding for target, binding in binding_ids.items() if set_ids.get(target) == 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--glslang', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    cases = test()
    print(f'PASS: {cases} command-owned CSR and visibility parity cases')
    if args.glslang:
        assert args.out is not None
        args.out.mkdir(parents=True, exist_ok=True)
        folder = args.root / 'project/addons/terrain_3d/rd'
        src = folder / 'terrain_instance_cull.glsl'
        assert 'command_owner' in src.read_text()
        stats = {}
        for version in ('legacy', 'grouped', 'command_owner'):
            glsl = args.out / f'instance_{version}.comp'
            spirv = args.out / f'instance_{version}.spv'
            glsl.write_text(emit_compute(folder, 'terrain_instance_cull.glsl', version))
            proc = subprocess.run([str(args.glslang), '-V', '--target-env', 'vulkan1.2',
                                   '-S', 'comp', '--spirv-val', str(glsl), '-o', str(spirv)],
                                  capture_output=True, text=True, timeout=60)
            assert proc.returncode == 0 and 'Validation Errors' not in (proc.stdout + proc.stderr), proc.stderr
            insn = opcodes(spirv)
            # OpAtomicIAdd = 234, OpAtomic* = 227..244 in the SPIR-V core grammar.
            atomic_count = sum(1 for op in insn if 227 <= op <= 244)
            stats[version] = {'instructions': len(insn), 'atomic_sites': atomic_count,
                              'set0_bindings': sorted(descriptor_bindings(spirv))}
        assert stats['command_owner']['atomic_sites'] == 0
        assert 13 in stats['command_owner']['set0_bindings']
        assert 13 not in stats['grouped']['set0_bindings']
        assert 13 not in stats['legacy']['set0_bindings']
        assert stats['grouped']['atomic_sites'] > 0
        (args.out / 'wave6_metrics.json').write_text(json.dumps(stats, indent=2) + '\n')
        print('PASS: command-owned shader has zero atomic instructions', stats)


if __name__ == '__main__':
    sys.exit(main())
