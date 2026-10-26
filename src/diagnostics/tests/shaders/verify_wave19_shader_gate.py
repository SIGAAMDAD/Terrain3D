#!/usr/bin/env python3
"""BR00/BR06: preprocessor matrix, optional native Vulkan SPIR-V and ISA comparison.

Examples:
  python verify_wave19_shader_gate.py --root . --baseline /path/wave18 --out report.json
  python verify_wave19_shader_gate.py --root . --baseline /path/wave18 \\
      --glslang /path/glslangValidator --spirv-val /path/spirv-val --out report.json
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from compile_fragment_wave import emit_fragment, shader_root, SHADERS, split_sections, variants
from verify_wave13_fragment_variants import test_cpp

ROOT = Path(__file__).resolve().parents[4]
FLAGS = ('T3D_MATERIAL_ACTIVE_KEY_LOOP', 'T3D_VT_FIXED_FILTER_TAPS', 'T3D_FORWARD_SPECIALIZED_BRDF')
OPCODES = {246: 'OpLoopMerge', 247: 'OpSelectionMerge', 249: 'OpBranch',
           250: 'OpBranchConditional', 251: 'OpSwitch', 86: 'OpSampledImage',
           87: 'OpImageSampleImplicitLod', 88: 'OpImageSampleExplicitLod',
           95: 'OpImageFetch', 96: 'OpImageGather', 97: 'OpImageDrefGather'}


def inject(source, flags):
    return source.replace('#version 450\n', '#version 450\n' +
                          ''.join(f'#define {x} 1\n' for x in flags), 1)


def spirv_counts(path):
    b = path.read_bytes()
    if len(b) < 20 or len(b) % 4:
        raise RuntimeError('invalid SPIR-V size')
    words = struct.unpack('<' + 'I' * (len(b) // 4), b)
    if words[0] != 0x07230203:
        raise RuntimeError('invalid SPIR-V header')
    counts = Counter()
    i = 5
    while i < len(words):
        n = words[i] >> 16
        if n == 0 or i + n > len(words):
            raise RuntimeError('invalid instruction stream')
        counts[OPCODES.get(words[i] & 0xffff, 'Other')] += 1
        i += n
    return {'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest(),
            'instructions': sum(counts.values()), 'opcodes': dict(counts)}


def check(repo, shader, variant, flags, cpp, glslang, validator, tmpdir):
    source = inject(emit_fragment(shader_root(repo), shader, variant), flags)
    expanded = test_cpp(cpp, source, ())
    item = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
            'cpp_sha256': hashlib.sha256(expanded.encode()).hexdigest(),
            'cpp_if_sites': len(__import__('re').findall(r'\bif\s*\(', expanded)),
            'cpp_loop_sites': len(__import__('re').findall(r'\b(?:while|for)\s*\(', expanded)),
            'native_compiled': False, 'spirv_validated': False}
    if not glslang:
        return item
    src = tmpdir / 'frag.glsl'
    spv = tmpdir / 'frag.spv'
    src.write_text(source)
    cmd = [glslang, '-V', '--target-env', 'vulkan1.2', '--spirv-val', '-S', 'frag', str(src), '-o', str(spv)]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    if result.returncode != 0 or 'SPIRV-Tools Validation Errors' in result.stdout + result.stderr:
        raise RuntimeError(f'{shader}:{variant}:{flags}: {result.stdout} {result.stderr}')
    if validator:
        result = subprocess.run([validator, '--target-env', 'vulkan1.2', str(spv)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'spirv-val: {result.stdout} {result.stderr}')
    item['native_compiled'] = item['spirv_validated'] = True
    item.update(spirv_counts(spv))
    return item


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=ROOT)
    p.add_argument('--baseline', type=Path)
    p.add_argument('--out', type=Path, default=Path('wave19_shader_control_flow.json'))
    p.add_argument('--glslang', default=None)
    p.add_argument('--spirv-val', default=None)
    p.add_argument('--cpp', default=shutil.which('cpp'))
    p.add_argument('--fast', action='store_true')
    args = p.parse_args()
    if not args.cpp:
        p.error('GNU cpp required for offline checks')
    if args.glslang and not Path(args.glslang).is_file() and not shutil.which(args.glslang):
        p.error('requested glslangValidator not found')
    if args.spirv_val and not Path(args.spirv_val).is_file() and not shutil.which(args.spirv_val):
        p.error('requested spirv-val not found')
    rd = shader_root(args.root)
    checks = {}
    with tempfile.TemporaryDirectory(prefix='wave19_shader_') as temp:
        tempdir = Path(temp)
        count = 0
        for shader in SHADERS:
            choices = list(variants(split_sections((rd / shader).read_text())))
            if args.fast:
                choices = choices[:min(5, len(choices))]
            for version in choices:
                key = f'{shader}:{version}'
                current = check(args.root, shader, version, (), args.cpp, args.glslang, args.spirv_val, tempdir)
                result = {'current': current}
                if args.baseline:
                    before = check(args.baseline, shader, version, (), args.cpp, args.glslang, args.spirv_val, tempdir)
                    result['baseline'] = before
                    if current['native_compiled']:
                        result['native_deltas'] = {field: current['opcodes'].get(field, 0) - before['opcodes'].get(field, 0)
                                                   for field in sorted(OPCODES.values())}
                checks[key] = result
                count += 1
        opt_variants = [('terrain_clipmap_near.glsl', 'color_native'),
                        ('terrain_clipmap_near.glsl', 'editor_color_native'),
                        ('terrain_clipmap_near.glsl', 'depth_normal_roughness'),
                        ('terrain_clipmap_far.glsl', 'color_native'),
                        ('terrain_instances.glsl', 'color_native'),
                        ('terrain_world_raster.glsl', 'mesh_color_native')]
        for shader, version in opt_variants:
            # Individually switchable and combined experiments, without
            # touching previous Wave18 compile or physical sampler flags.
            for flags in [(x,) for x in FLAGS] + [FLAGS]:
                key = f'opt:{shader}:{version}:{"+".join(flags)}'
                expanded = check(args.root, shader, version, flags, args.cpp, args.glslang, args.spirv_val, tempdir)
                checks[key] = {'current': expanded}
                count += 1
    data = {'schema': 1, 'baseline': str(args.baseline) if args.baseline else None,
            'glslang': args.glslang, 'spirv_val': args.spirv_val,
            'compile_status': 'native-validated' if args.glslang else 'preprocessor-only',
            'warning': 'Static opcodes are not executed divergence, registers, texture traffic or GPU timings.',
            'capture_fields_required': ['gpu_pass_ms_p50', 'gpu_pass_ms_p95', 'gpu_pass_ms_p99',
                 'gpu_frame_ms_p50', 'gpu_frame_ms_p95', 'gpu_frame_ms_p99',
                 'registers', 'scratch_bytes', 'vt_residency_histogram',
                 'cluster_candidate_counts', 'gpu_memory_bytes', 'upload_bytes',
                 'image_diff', 'alpha_depth_diff', 'feedback_trace_diff'],
            'cases': checks}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(data, indent=2) + '\n')
    print(f'PASS Wave19: {count} variants checked ({data["compile_status"]}); {args.out}')
    if not args.glslang:
        print('NOT RUN: GLSL->SPIR-V compilation and native validation (provide --glslang)')


if __name__ == '__main__':
    main()
