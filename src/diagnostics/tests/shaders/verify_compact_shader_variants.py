#!/usr/bin/env python3
"""BR10-BR13: compile all opt-in compact fragment variants and the compute producer."""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('--glslang', default='glslangValidator')
    parser.add_argument('--compute-only', action='store_true', help='fragment matrix already compiled')
    args = parser.parse_args()
    root = args.root.resolve()
    rd = root / 'project/addons/terrain_3d/rd'
    sys.path.insert(0, str(root / 'src/diagnostics/tests/shaders'))
    from verify_fragment_shader_gate import check
    from compile_fragment_variants import split_sections
    count = 0
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        for name in (() if args.compute_only else ('terrain_clipmap_near', 'terrain_clipmap_middle', 'terrain_clipmap_far',
                     'terrain_world_raster', 'terrain_instances')):
            source = (rd / (name + '.glsl')).read_text()
            for version in re.findall(r'(?m)^(\w+_compact)\s*=', source):
                check(root, name + '.glsl', version, (), 'cpp', args.glslang, None, tmp)
                count += 1
        compute = split_sections((rd / 'terrain_cluster_compact.glsl').read_text())['compute']
        shader = tmp / 'br21.comp'
        shader.write_text(compute.replace('#VERSION_DEFINES', ''))
        process = subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.2',
                                  '--spirv-val', '-S', 'comp', str(shader), '-o', str(tmp / 'br21.spv')],
                                 capture_output=True, text=True)
        if process.returncode:
            raise RuntimeError(process.stdout + process.stderr)
    print(f'BR21 glslang Vulkan SPIR-V: {count} compact raster + 1 compute PASS')


if __name__ == '__main__':
    main()
