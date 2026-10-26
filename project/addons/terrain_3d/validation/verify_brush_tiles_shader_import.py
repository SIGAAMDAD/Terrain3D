#!/usr/bin/env python3
"""Preflight the editor brush RDShaderFile source before startup loading.

Use --require-glslang in a developer/toolchain environment to fail when the
compute stage cannot be compiled. The default static gate remains usable by
systems without glslangValidator and does not claim SPIR-V compilation.
"""

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SHADER = ROOT / 'project/addons/terrain_3d/compute/terrain_brush_tiles.glsl'
CATALOG = ROOT / 'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp'


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--require-glslang', action='store_true')
    args = parser.parse_args()

    text = SHADER.read_text(encoding='utf-8')
    catalog = CATALOG.read_text(encoding='utf-8')
    check(text.startswith('#[compute]\n#version 450\n'), 'brush RD shader stage header changed')
    check(text.count('#[compute]') == 1, 'expected exactly one compute stage')
    check('terrain_brush_tiles.glsl' in catalog, 'brush shader missing from RD catalog')
    check('T3D_RD_SHADER_EDITOR_BRUSH_TILES' in catalog, 'editor brush shader ID missing')
    code = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/', '', text)
    check(re.search(r'\buint\s+height_brush\s*\(\s*uint\s+encoded_height\b', code),
          'height brush must use an ordinary GLSL identifier')
    check(not re.search(r'\bhalf\b', code), 'reserved GLSL token half reintroduced')
    check(code.count('{') == code.count('}'), 'unbalanced compute shader braces')
    check('layout(push_constant, std430) uniform Push' in text, 'BRP push constant ABI missing')
    check('layout(set = 0, binding = 1, std430) readonly buffer TileTable' in text,
          'BRP set-0 tile-table descriptor contract missing')

    compiler = shutil.which('glslangValidator')
    if compiler:
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / 'terrain_brush_tiles.comp'
            dst = Path(temp) / 'brush.spv'
            src.write_text(text.replace('#[compute]\n', '', 1), encoding='utf-8')
            result = subprocess.run([compiler, '-V', '-S', 'comp', str(src), '-o', str(dst)],
                                    text=True, capture_output=True, timeout=60)
            check(result.returncode == 0,
                  'terrain_brush_tiles.glsl SPIR-V compile failed:\n' + result.stdout + result.stderr)
            print('PASS: BRP brush shader glslangValidator -V -S comp')
    else:
        check(not args.require_glslang, 'glslangValidator is required but unavailable')
        print('SKIP: SPIR-V compilation (glslangValidator unavailable)')
    print('PASS: BRP brush source, catalog routing, and reserved-token contract')


if __name__ == '__main__':
    main()
