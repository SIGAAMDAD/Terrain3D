#!/usr/bin/env python3
"""Regression checks for editor brush startup and shared V1 vertex shader ordering.

Preprocesses every authored clipmap/WORLD vertex version; this does not replace
Godot's glslang/SPIR-V compilation or a live editor DLL smoke test.
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
sys.path.insert(0, str(ROOT / 'src/diagnostics/tests/shaders'))
from compile_fragment_variants import INCLUDE, split_sections, variants


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def expanded_vertex(shader, version, macros, cpp):
    src = RD / shader
    sections = split_sections(src.read_text())
    stage = sections['vertex']
    check(stage.count('#VERSION_DEFINES') == 1, f'{shader}: invalid macro entrypoint')

    def expand(match):
        inc = RD / match.group(1)
        check(inc.is_file(), f'{shader}: missing include {match.group(1)}')
        return inc.read_text()

    stage = INCLUDE.sub(expand, stage).replace('#VERSION_DEFINES', macros)
    check(not INCLUDE.search(stage), f'{shader}: nested include')
    stage = re.sub(r'(?m)^#(?:version|extension)\b[^\n]*$', '', stage)
    result = subprocess.run([cpp, '-E', '-P', '-x', 'c', '-undef', '-nostdinc', '-'],
                            input=stage, text=True, capture_output=True, timeout=20)
    check(result.returncode == 0, f'{shader}/{version}: {result.stderr[-1200:]}')
    return result.stdout


def main():
    tools = ROOT / 'project/addons/terrain_3d/src/tool_settings.gd'
    plugin = ROOT / 'project/addons/terrain_3d/src/editor_plugin.gd'
    editor = ROOT / 'src/authoring/editor/terrain_3d_editor.cpp'
    tool_text = tools.read_text()
    plugin_text = plugin.read_text()
    editor_text = editor.read_text()
    check('p_btn.has_meta(&"terrain3d_cached_brush_texture")' in tool_text,
          'brush texture metadata must be checked before get_meta')
    check('editor.has_method(&"poll_editor_brush_commit")' in plugin_text,
          'editor DLL method contract must be checked during initialization')
    check('D_METHOD( "poll_editor_brush_commit" )' in editor_text,
          'native editor DLL must register the brush polling method')
    check('#if defined( T3D_EDITOR_NATIVE )\n\tClassDB::bind_method( D_METHOD( "poll_editor_brush_commit" )' in editor_text,
          'native poll method must remain editor-only')

    cpp = shutil.which('clang') or shutil.which('cpp')
    check(cpp is not None, 'need clang or cpp for shader preprocessor checks')
    total = 0
    for shader in ('terrain_clipmap.glsl', 'terrain_world_raster.glsl'):
        text = (RD / shader).read_text()
        includes = split_sections(text)['vertex']
        check(includes.index('#include "terrain_clipmap_vertex_inc.glslinc"') <
              includes.index('#include "terrain_far_height_vertex_v1.glslinc"'),
              f'{shader}: far V1 function placed before region definitions')
        for name, macros in variants(split_sections(text)).items():
            out = expanded_vertex(shader, name, macros, cpp)
            if '#define T3D_WORLD_RASTER_MESH' in macros:
                continue
            definitions = ('const int T3D_REGION_MAP_SIZE', 'const int T3D_REGION_MAP_HALF',
                           'const float T3D_REGION_INV_SIZE_F')
            fn = re.search(r'bool\s+t3d_far_height_v1\s*\([^)]*\)\s*\{', out)
            check(fn is not None, f'{shader}/{name}: missing far V1 implementation')
            for definition in definitions:
                check(definition in out and out.index(definition) < fn.start(),
                      f'{shader}/{name}: {definition} declared after far V1 function')
            check('bool t3d_far_height_v1(vec2 world_texel, out float encoded);' in out,
                  f'{shader}/{name}: missing forward declaration')
            total += 1
    print(f'PASS: editor brush metadata/native contract and {total} V1 vertex variants')


if __name__ == '__main__':
    main()
