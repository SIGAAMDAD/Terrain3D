#!/usr/bin/env python3
"""BR09 GLSL->SPIR-V compiler and descriptor reflection gate.

Run from any working directory:
  python verify_vt_hint_shaders.py --root . --glslang /path/glslangValidator
"""
import argparse
from collections import defaultdict
from pathlib import Path
import shutil
import struct
import tempfile
from compile_fragment_variants import emit_fragment, shader_root
from verify_fragment_material_variants import test_cpp
import subprocess

VARIANTS = (
    ('terrain_clipmap.glsl', 'color_native', (0, 43)),
    ('terrain_clipmap.glsl', 'color_native', (0, 43)),
    ('terrain_clipmap.glsl', 'color_native', (0, 43)),
    ('terrain_clipmap.glsl', 'feedback_color_native', (0, 43)),
    ('terrain_clipmap.glsl', 'depth_normal_roughness', (0, 43)),
    ('terrain_world_raster.glsl', 'mesh_color_native', (1, 17)),
    ('terrain_world_raster.glsl', 'terrain_strip_color_native', (1, 17)),
    ('terrain_world_raster.glsl', 'feedback_mesh_color_native', (1, 17)),
    ('terrain_instances.glsl', 'color_native', (1, 11)),
    ('terrain_instances.glsl', 'depth_normal_roughness', (1, 11)),
    ('terrain_instances.glsl', 'feedback_color_native', (1, 11)),
)


def check(root, shader, variant, flags, cpp, glslang, validator, work):
    """Compile a shader version with the explicit ancestor-hint descriptor gate."""
    source = emit_fragment(shader_root(root), shader, variant)
    for flag in flags:
        declaration = f'#define {flag} 1'
        if declaration not in source:
            source = source.replace('#version 450\n',
                                    '#version 450\n' + declaration + '\n', 1)
    test_cpp(cpp, source, ())
    fragment = work / 'frag.glsl'
    spirv = work / 'frag.spv'
    fragment.write_text(source)
    result = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.2',
                             '--spirv-val', '-S', 'frag', str(fragment), '-o', str(spirv)],
                            capture_output=True, text=True, timeout=95)
    if result.returncode or not spirv.exists() or 'SPIRV-Tools Validation Errors' in result.stdout + result.stderr:
        raise RuntimeError(f'{shader}:{variant}: {result.stdout}{result.stderr}')


def reflected_bindings(path):
    data = path.read_bytes()
    if len(data) % 4 or len(data) < 20:
        raise ValueError('invalid SPIR-V binary')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    decorators = defaultdict(dict)
    i = 5
    while i < len(words):
        w = words[i]
        length = w >> 16
        opcode = w & 0xffff
        if length == 0 or i + length > len(words):
            raise ValueError('invalid SPIR-V instruction')
        if opcode == 71 and length >= 4:  # OpDecorate
            target, deco, operand = words[i + 1:i + 4]
            if deco == 33:  # Binding
                decorators[target]['binding'] = operand
            elif deco == 34:  # DescriptorSet
                decorators[target]['set'] = operand
        i += length
    bindings = [(v['set'], v['binding']) for v in decorators.values() if 'set' in v and 'binding' in v]
    if len(bindings) != len(set(bindings)):
        raise AssertionError('duplicate set/binding decorations')
    return set(bindings)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    p.add_argument('--glslang', required=True)
    args = p.parse_args()
    cpp = shutil.which('cpp')
    assert cpp, 'GNU cpp required'
    with tempfile.TemporaryDirectory(prefix='br20_spirv_') as temp:
        for shader, version, expected in VARIANTS:
            check(args.root, shader, version, ('T3D_VT_RESIDENT_ANCESTOR_HINTS',), cpp,
                  args.glslang, None, Path(temp))
            bindings = reflected_bindings(Path(temp) / 'frag.spv')
            if expected not in bindings:
                raise AssertionError(f'{shader}:{version} missing ancestor buffer {expected}: {bindings}')
    print(f'PASS BR09: {len(VARIANTS)} SPIR-V variants, hint descriptor bindings and validation')


if __name__ == '__main__':
    main()
