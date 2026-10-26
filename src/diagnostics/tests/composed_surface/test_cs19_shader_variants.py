#!/usr/bin/env python3
"""CS19: expand actual Forward+ CS variants and optionally compile them with glslang.

No generated SPIR-V is checked in. Use --require-compiler in CI to enforce the GPU
shader compilation gate. Without glslang, expansion and contract assertions still run.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[4]
SHADERS = PROJECT / 'project/addons/terrain_3d/rd'
SHADER = SHADERS / 'terrain_world_raster.glsl'
VARIANT = re.compile(r'^([a-zA-Z][a-zA-Z_0-9]*)\s*=\s*("(?:\\.|[^"\\])*");\s*$', re.M)
INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"\s*$', re.M)
RESOURCE = re.compile(r'layout\s*\(\s*set\s*=\s*(\d+)\s*,\s*binding\s*=\s*(\d+)')
EXPECTED_WORLD = {(1, 13), (1, 14), (1, 15), (1, 16)}


def split_stages(source):
    chunks = re.split(r'^#\[(versions|vertex|fragment|compute)\]\s*$', source, flags=re.M)
    assert chunks[0].strip().startswith('//'), 'unexpected shader prologue'
    return {chunks[i]: chunks[i + 1] for i in range(1, len(chunks), 2)}


def flatten(source, base, stack=()):
    def expand(match):
        child = (base / match.group(1)).resolve()
        if not child.is_relative_to(SHADERS.resolve()):
            raise AssertionError(f'include escaped shader root: {child}')
        if child in stack:
            raise AssertionError(f'include cycle: {child}')
        if not child.is_file():
            raise AssertionError(f'missing include: {child}')
        return flatten(child.read_text(), child.parent, stack + (child,))
    return INCLUDE.sub(expand, source)


def validate_static(variants, stages):
    composed = {k: v for k, v in variants.items() if k.startswith('cs_terrain_strip_')}
    assert len(composed) == 10, f'expected 10 composed shader versions, got {len(composed)}'
    for name, macros in composed.items():
        baseline = name[3:]
        assert baseline in variants
        assert macros == variants[baseline] + '\n#define T3D_CS_ACTIVE', name
        assert ('T3D_FORWARD_NORMAL_ROUGHNESS' in macros) == ('depth_normal_roughness' in name), name
        assert 'T3D_WORLD_RASTER_TERRAIN_STRIP' in macros
    for stage in ('vertex', 'fragment'):
        assert stage in stages
        expanded = flatten(stages[stage], SHADERS, (SHADER.resolve(),))
        assert '#include "' not in expanded, stage
    # Inspect descriptor declarations in the source, including the fragment
    # interface guarded by T3D_CS_ACTIVE. The four constants are explicitly
    # injected by the embedded world root and are *not* clipmap set-0 bindings.
    interface = (SHADERS / 'terrain_clipmap_fragment_interface.glslinc').read_text()
    root_frag = stages['fragment']
    expected = {'T3D_CS_DESCRIPTOR_SET': 1,
                'T3D_CS_PAGE_TABLE_BINDING': 13,
                'T3D_CS_ALBEDO_BINDING': 14,
                'T3D_CS_NORMAL_BINDING': 15,
                'T3D_CS_SURFACE_BINDING': 16}
    for key, val in expected.items():
        assert re.search(r'^#define\s+' + key + r'\s+' + str(val) + r'\s*$', root_frag, re.M), key
    for resource in ('terrain_cs_pages[]', 'terrain_cs_albedo', 'terrain_cs_normal', 'terrain_cs_surface'):
        assert resource in interface
    cpp = (PROJECT / 'src/rendering/rd/world/visibility/terrain_3d_world_raster.cpp').read_text()
    for token in ('single( 13,', 'samplerTexture( 14 + ( int ) i',
                  'pComposed->m_hPageTable', 'm_bExperimentalComposed'):
        assert token in cpp, token
    # The standalone clipmap namespace must stay separate from world set-1.
    assert '#define T3D_CS_PAGE_TABLE_BINDING 39' in interface
    return composed


def main():
    args = argparse.ArgumentParser()
    args.add_argument('--require-compiler', action='store_true')
    args.add_argument('--output-dir', type=Path, help='retain expanded GLSL and SPIR-V for inspection')
    opt = args.parse_args()
    source = SHADER.read_text()
    stages = split_stages(source)
    versions = {k: json.loads(v) for k, v in VARIANT.findall(stages['versions'])}
    composed = validate_static(versions, stages)
    glslang = shutil.which('glslangValidator')
    if opt.require_compiler and not glslang:
        sys.exit('CS19 BLOCKED: glslangValidator unavailable (required for --require-compiler)')
    with tempfile.TemporaryDirectory(prefix='t3d_cs19_') as td:
        out = opt.output_dir or Path(td)
        out.mkdir(parents=True, exist_ok=True)
        total = 0
        for version, macros in composed.items():
            for stage, ext in (('vertex', 'vert'), ('fragment', 'frag')):
                body = flatten(stages[stage], SHADERS, (SHADER.resolve(),))
                assert body.lstrip().startswith('#version 450'), (version, stage)
                body = re.sub(r'^\s*#version\s+450\s*', '', body, count=1)
                # RDShaderFile's version placeholder is removed by Godot. The
                # offline compiler must reproduce that expansion, not emit an
                # invalid '#VERSION_DEFINES' preprocessor directive.
                assert body.count('#VERSION_DEFINES') == 1, (version, stage)
                body = body.replace('#VERSION_DEFINES', '')
                text = '#version 450\n' + macros + '\n' + body
                path = out / (version + '.' + ext)
                path.write_text(text)
                if glslang:
                    cmd = [glslang, '-V', '--target-env', 'vulkan1.2', '-S', ext,
                           str(path), '-o', str(path) + '.spv']
                    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT)
                    if result.returncode:
                        print(f'CS19 FAIL {version} / {ext}:\n{result.stdout}', file=sys.stderr)
                        sys.exit(1)
                total += 1
        print(f'CS19 expanded {len(composed)} composed variants / {total} shader stages; '
              f'SPIR-V: {"PASS" if glslang else "NOT RUN (glslangValidator unavailable)"}')


if __name__ == '__main__':
    main()
