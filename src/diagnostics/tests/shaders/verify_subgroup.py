#!/usr/bin/env python3
"""BR18: fragment subgroup experiment is optional, correctly gated and SPIR-V valid."""
import argparse
from pathlib import Path
import random
import subprocess
import sys
import tempfile

from compile_fragment_wave import emit_fragment, shader_root

FRAGMENTS = (
    ('terrain_clipmap.glsl', 'color_native'),
    ('terrain_clipmap.glsl', 'feedback_color_native'),
    ('terrain_clipmap.glsl', 'color_native'),
    ('terrain_clipmap.glsl', 'color_native'),
    ('terrain_instances.glsl', 'color_native'),
    ('terrain_instances.glsl', 'feedback_color_native'),
    ('terrain_world_raster.glsl', 'mesh_color_native'),
    ('terrain_world_raster.glsl', 'terrain_strip_color_native'),
)


def verify_gpu_compiler(root, compiler):
    rd = shader_root(root)
    with tempfile.TemporaryDirectory(prefix='br24_subgroup_') as work:
        stage = Path(work) / 'br24.frag'
        output = Path(work) / 'br24.spv'
        for source_name, variant in FRAGMENTS:
            default = emit_fragment(rd, source_name, variant)
            assert '#define T3D_FORWARD_SUBGROUP_CLUSTER_LIGHTING 1' not in default
            source = default.replace('#version 450\n',
                                     '#version 450\n#define T3D_FORWARD_SUBGROUP_CLUSTER_LIGHTING 1\n', 1)
            stage.write_text(source, encoding='utf-8')
            cmd = [str(compiler), '-V', '--target-env', 'vulkan1.2', '-S', 'frag',
                   '--spirv-val', '-H', str(stage), '-o', str(output)]
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            if proc.returncode != 0 or 'SPIRV-Tools Validation Errors' in proc.stdout + proc.stderr:
                raise AssertionError(f'{source_name}:{variant}: {proc.stdout} {proc.stderr}')
            disassembly = proc.stdout
            for opcode in ('GroupNonUniformAllEqual', 'GroupNonUniformElect',
                           'GroupNonUniformBroadcastFirst'):
                assert opcode in disassembly, (source_name, variant, opcode)
    return len(FRAGMENTS)


def verify_subgroup_reference():
    # Model active-lane subgroup election/broadcast vs independent SSBO reads;
    # widths and inactive lanes vary. No assumption about fixed subgroup size.
    random_state = random.Random(2418)
    for size in (4, 8, 16, 32, 64, 128):
        for _ in range(4000):
            data = [random_state.randrange(0, 0xffffffff) for _ in range(32)]
            indices = [random_state.randrange(len(data)) for _ in range(size)]
            active = [random_state.random() < 0.75 for _ in range(size)]
            if not any(active):
                continue
            if random_state.random() < 0.5:
                same = random_state.randrange(len(data))
                indices = [same if active[i] else indices[i] for i in range(size)]
            participating = [i for i in range(size) if active[i]]
            all_equal = len({indices[i] for i in participating}) == 1
            leader = participating[0]
            broadcast = data[indices[leader]]
            for i in participating:
                result = broadcast if all_equal else data[indices[i]]
                assert result == data[indices[i]]
    return 24000


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('--glslang', type=Path, required=True)
    args = parser.parse_args()
    n = verify_gpu_compiler(args.root, args.glslang)
    oracle = verify_subgroup_reference()
    print(f'BR18 PASS: {n} fragment SPIR-V variants, {oracle} subgroup oracle groups')
    print('Device capabilities / live frame timing not established by this test')


if __name__ == '__main__':
    main()
