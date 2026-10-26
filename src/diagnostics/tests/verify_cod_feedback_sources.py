#!/usr/bin/env python3
"""Source-level invariant check for active CoD feedback shader versions.

This is intentionally not a substitute for compiling Godot's shader variants
or inspecting SPIR-V reflection on a supported RenderingDevice.
"""
import argparse
from pathlib import Path
import re


def verify(rd):
    versions = {}
    for name in ('terrain_clipmap.glsl', 'terrain_clipmap.glsl',
                 'terrain_clipmap.glsl', 'terrain_instances.glsl',
                 'terrain_world_raster.glsl'):
        txt = (rd / name).read_text(encoding='utf-8')
        versions[name] = dict(re.findall(r'^([\w]+) = "([^"\n]*)";', txt, re.MULTILINE))
    expected = [
        'color_native', 'color_specular', 'color_motion', 'color_specular_motion',
        'color_native_radiance_array', 'color_specular_radiance_array',
        'color_motion_radiance_array', 'color_specular_motion_radiance_array',
        'color_native_one_sun', 'color_specular_one_sun',
        'color_motion_one_sun', 'color_specular_motion_one_sun',
        'color_native_one_sun_radiance_array', 'color_specular_one_sun_radiance_array',
        'color_motion_one_sun_radiance_array', 'color_specular_motion_one_sun_radiance_array'
    ]
    for name in ('terrain_clipmap.glsl', 'terrain_clipmap.glsl', 'terrain_clipmap.glsl'):
        for base in expected:
            variant = 'feedback_' + base
            assert variant in versions[name], (name,variant)
            defs = versions[name][variant]
            assert 'T3D_VT_COARSE_FEEDBACK' in defs and 'T3D_VT_FRAGMENT_FEEDBACK' in defs
        assert len([v for v in versions[name] if v.startswith('feedback_')]) == 16
    for name in ('terrain_instances.glsl', 'terrain_world_raster.glsl'):
        for variant, defines in versions[name].items():
            if 'T3D_VT_FRAGMENT_FEEDBACK' in defines:
                assert 'T3D_VT_COARSE_FEEDBACK' in defines, (name,variant)
    feedback = (rd/'vt/terrain_vt_feedback_inc.glslinc').read_text(encoding='utf-8')
    assert not re.search(r'\batomic(?:CompSwap|Min|Max|Add|Or|Exchange)\s*\(', feedback)
    assert 'vt_request_hash.entries' not in feedback
    assert '#error "Fragment feedback producer must use the versioned coarse-grid ABI"' in feedback
    print('PASS: 48 terrain feedback versions and all world/instance feedback versions use coarse-grid ABI')
    print('PASS: no legacy fragment atomic hash producer in shared feedback include')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rd', type=Path, required=True)
    verify(parser.parse_args().rd)
