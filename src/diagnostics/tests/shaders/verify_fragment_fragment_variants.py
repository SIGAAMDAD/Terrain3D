#!/usr/bin/env python3
"""Wave 13 fragment shader preprocessor gate, optional glslang/SPIR-V gate.

The C preprocessor checks version-specific GLSL macros and include expansion;
it is NOT a GLSL compiler. With --glslang, every selected variant is compiled
and validated as Vulkan 1.2 SPIR-V using the project's original toolchain.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from compile_fragment_wave import SHADERS, emit_fragment, shader_root, split_sections, variants

ROOT = Path(__file__).resolve().parents[4]
REMOVE = re.compile(r'^\s*#(?:version|extension)\b[^\n]*\n?', re.MULTILINE)
COMMENTS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', re.DOTALL)


def balanced_shader_source(source):
    stack = []
    opposites = {')': '(', ']': '[', '}': '{'}
    for token in COMMENTS.sub(lambda m: ' ' * len(m.group()), source):
        if token in '([{':
            stack.append(token)
        elif token in ')]}':
            if not stack or stack.pop() != opposites[token]:
                return False
    return not stack


def test_cpp(cpp, frag, defines):
    args = [cpp, '-P', '-undef', '-nostdinc', '-x', 'c']
    args.extend('-D' + name + '=1' for name in defines)
    args.append('-')
    result = subprocess.run(args, input=REMOVE.sub('', frag), text=True,
                            capture_output=True, timeout=25)
    if result.returncode:
        raise RuntimeError(result.stderr[-2000:])
    if not balanced_shader_source(result.stdout):
        raise RuntimeError('unbalanced delimiters after preprocessor expansion')
    return result.stdout


def test_glslang(glslang, source, source_name):
    with tempfile.TemporaryDirectory(prefix='t3d_wave13_glsl_') as dirname:
        frag = Path(dirname) / (source_name + '.frag')
        spirv = Path(dirname) / (source_name + '.spv')
        frag.write_text(source)
        result = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.2',
                                 '-S', 'frag', '--spirv-val', str(frag), '-o', str(spirv)],
                                capture_output=True, text=True, timeout=95)
        log = result.stdout + result.stderr
        if result.returncode or not spirv.exists() or 'SPIRV-Tools Validation Errors' in log:
            raise RuntimeError(log[-3500:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--cpp', default=shutil.which('cpp'))
    parser.add_argument('--glslang', default=None)
    parser.add_argument('--fast', action='store_true', help='representative source variants only')
    args = parser.parse_args()
    if not args.cpp:
        parser.error('cpp is required for preprocessor contract checks')
    rd = shader_root(args.root)
    default_count = opt_count = 0
    for shader in SHADERS:
        choices = list(variants(split_sections((rd / shader).read_text())))
        if args.fast:
            choices = choices[:4]
        for variant in choices:
            frag = emit_fragment(rd, shader, variant)
            try:
                test_cpp(args.cpp, frag, ())
                default_count += 1
                if args.glslang:
                    test_glslang(args.glslang, frag, shader.removesuffix('.glsl')+'_'+variant)
                # In each shader family, compile opt-in permutations on a
                # bounded representative set to catch hidden #if interactions.
                if (variant in ('color_native', 'editor_color_native', 'feedback_color_native',
                               'depth_normal_roughness', 'terrain_strip_color_native',
                               'terrain_strip_depth_normal_roughness')):
                    define_lines = ('#define T3D_COLOR_TWO_KEY_SPECIALIZATION 1\n'
                                    '#define T3D_DNR_SOURCE_REUSE 1\n')
                    opt_frag = frag.replace('#version 450\n', '#version 450\n' + define_lines, 1)
                    test_cpp(args.cpp, opt_frag, ())
                    opt_count += 1
                    if args.glslang:
                        test_glslang(args.glslang, opt_frag,
                                     shader.removesuffix('.glsl')+'_'+variant+'_opt')
            except Exception as e:
                raise RuntimeError(f'{shader}:{variant}: {e}') from e
    print(f'PASS Wave13: {default_count} shader stages expanded and CPP checked; {opt_count} opt-in stages CPP checked')
    if args.glslang:
        print(f'PASS Wave13: {default_count + opt_count} glslang + Vulkan 1.2 SPIR-V validations')
    else:
        print('SKIPPED glslang/SPIR-V: pass --glslang /path/to/glslangValidator to run the compiler gate')


if __name__ == '__main__':
    main()
