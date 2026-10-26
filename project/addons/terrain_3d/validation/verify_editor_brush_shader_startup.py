#!/usr/bin/env python3
"""Regression checks for editor brush startup and unified VHM shader ordering.

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
        check(includes.index('#include "terrain_clipmap_height_vhm.glslinc"') <
              includes.index('#include "terrain_clipmap_vertex_inc.glslinc"'),
              f'{shader}: VHM declarations must precede terrain vertex sampling')
        for name, macros in variants(split_sections(text)).items():
            out = expanded_vertex(shader, name, macros, cpp)
            if '#define T3D_WORLD_RASTER_MESH' in macros:
                continue
            sample = re.search(r'bool\s+t3d_st_sample_height_meters\s*\([^)]*\)\s*\{', out)
            vhm = re.search(r'bool\s+t3d_vhm_sample\s*\([^)]*\)\s*\{', out)
            check(sample is not None, f'{shader}/{name}: missing common VHM height entrypoint')
            check(vhm is not None and vhm.start() < sample.start(),
                  f'{shader}/{name}: VHM sampler must be defined before the terrain height entrypoint')
            check('t3d_st_region_ready' in out,
                  f'{shader}/{name}: missing region generation readiness gate')
            total += 1
    print(f'PASS: editor brush metadata/native contract and {total} VHM vertex variants')


if __name__ == '__main__':
    main()
