#!/usr/bin/env python3
"""Compile and validate RDShaderFile compute stages used by visibility/culling.

Runs glslang --spirv-val on RESET/CULL/FINALIZE and instance culling.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys

from compile_fragment_variants import INCLUDE, ROOT, count_spirv, shader_root, split_sections, variants

FILES = ('terrain_world_visibility.glsl', 'terrain_instance_cull.glsl')


def emit_compute(rd, name, version):
    sections = split_sections((rd / name).read_text(encoding='utf-8'))
    if 'compute' not in sections:
        raise ValueError(f'compute stage missing: {name}')
    defines = variants(sections)[version]
    stage = sections['compute'].replace('#VERSION_DEFINES', defines)
    if '#VERSION_DEFINES' in stage:
        raise ValueError(f'unexpanded version defines: {name} {version}')

    def expand(match):
        path = (rd / match.group(1)).resolve()
        if not path.is_relative_to(rd.resolve()) or not path.is_file():
            raise ValueError(f'invalid include: {match.group(1)}')
        return path.read_text(encoding='utf-8')

    stage = INCLUDE.sub(expand, stage)
    if INCLUDE.search(stage):
        raise ValueError(f'unsupported nested include: {name} {version}')
    return stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--glslang', default='glslangValidator')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    rd = shader_root(args.root)
    args.out.mkdir(parents=True, exist_ok=True)
    report = {}
    for name in FILES:
        for version in variants(split_sections((rd / name).read_text())).keys():
            key = f'{name}:{version}'
            stem = f'{name.removesuffix(".glsl")}__{version}'
            comp = args.out / f'{stem}.comp'
            spv = args.out / f'{stem}.spv'
            comp.write_text(emit_compute(rd, name, version), encoding='utf-8')
            command = [str(args.glslang), '-V', '--target-env', 'vulkan1.2', '-S', 'comp',
                       '--spirv-val', str(comp), '-o', str(spv)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            output = result.stdout + result.stderr
            passed = result.returncode == 0 and spv.exists() and 'Validation Errors' not in output
            report[key] = {'compiled_and_validated': passed,
                           'instruction_count': count_spirv(spv)['instruction_count'] if passed else None,
                           'diagnostics': output[-2000:] if not passed else ''}
            print(f'{key}: {"PASS" if passed else "FAIL"}', flush=True)
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if all(v['compiled_and_validated'] for v in report.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
