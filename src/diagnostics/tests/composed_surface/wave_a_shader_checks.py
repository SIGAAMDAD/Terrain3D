#!/usr/bin/env python3
"""CS00/CS02 static shader topology checks; requires only Python 3."""
import ast
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SHADERS = ROOT / 'project/addons/terrain_3d/rd'
if not SHADERS.exists():
    # Repo layout keeps 'src' and 'project' as siblings.
    ROOT = Path(__file__).resolve().parents[5]
    SHADERS = ROOT / 'project/addons/terrain_3d/rd'


def read(name):
    return (SHADERS / name).read_text(encoding='utf-8')


def manifest():
    variants = []
    for filename in ['terrain_clipmap.glsl', 'terrain_clipmap.glsl',
                     'terrain_clipmap.glsl', 'terrain_world_raster.glsl']:
        text = read(filename)
        match = re.search(r'(?ms)^#\[versions\]\s*\n(.*?)(?=^#\[(?:vertex|fragment|compute)\])', text)
        if match:
            for key, literal in re.findall(r'^([A-Za-z0-9_]+)\s*=\s*("(?:\\.|[^"\\])*")\s*;',
                                           match.group(1), re.M):
                defines = ast.literal_eval(literal)
                variants.append({'shader': filename, 'variant': key,
                                 'defines': defines.splitlines(),
                                 'depth': 'T3D_FORWARD_DEPTH' in defines,
                                 'dnr': 'T3D_FORWARD_NORMAL_ROUGHNESS' in defines,
                                 'feedback': 'T3D_VT_FRAGMENT_FEEDBACK' in defines,
                                 'one_sun': 'T3D_FORWARD_ONE_SUN' in defines,
                                 'editor': key.startswith('editor_'),
                                 'compat': key.startswith('compat_')})
        else:
            variants.append({'shader': filename, 'variant': 'default', 'defines': [],
                             'depth': False, 'dnr': False, 'feedback': False,
                             'one_sun': False, 'editor': False, 'compat': False})
    return variants


def verify():
    raster = read('terrain_clipmap_fragment_inc.glslinc')
    pure = read('terrain_composed_material_math.glslinc')
    fixture = read('terrain_composed_surface_fixture.glsl')
    assert '#include "terrain_composed_material_math.glslinc"' not in raster
    for name in ('terrain_clipmap.glsl', 'terrain_clipmap.glsl',
                 'terrain_clipmap.glsl', 'terrain_world_raster.glsl'):
        stage = read(name).split('#[fragment]', 1)[1]
        assert stage.count('#include "terrain_composed_material_math.glslinc"') == 1
        assert stage.count('#include "terrain_composed_surface_sampling.glslinc"') == 1
        assert stage.index('#include "terrain_composed_material_math.glslinc"') < stage.index('#include "terrain_clipmap_fragment_inc.glslinc"')
    assert fixture.count('#include "terrain_composed_material_math.glslinc"') == 1
    assert raster.count('t3d_cs_height_blend(') == 2, 'both color and DNR must reuse shared blend'
    assert pure.count('float t3d_cs_height_blend(') == 1
    assert 'struct SampledMaterial {' not in raster
    assert 'struct SampledMaterial {' in pure
    assert 'ControlBlendSet4 make_control_blend_set4' in pure
    for token in ['dFdx(', 'dFdy(', 'discard', 'gl_FragCoord', 'frame_data', 'material_config']:
        assert token not in pure, f'camera-dependent token in shared math: {token}'
    for token in ['dFdx(', 'dFdy(', 'gl_FragCoord', 'texture(', 'textureLod(']:
        assert token not in fixture, f'implicit sample or fragment builtin in compositor fixture: {token}'
    assert 'textureGrad(' in fixture
    assert 'ivec2(136)' in fixture
    assert 'ivec2(4)' in fixture
    assert 'T3D_DIV_255' in fixture
    assert 'T3D_DIV_255' in raster
    for p in SHADERS.glob('*.glsl'):
        contents = p.read_text(encoding='utf-8')
        for inc in re.findall(r'^\s*#include\s+"([^"]+)"', contents, re.M):
            assert (p.parent / inc).is_file(), f'missing shader include {p.name}: {inc}'
    variants = manifest()
    assert any(v['depth'] and v['dnr'] for v in variants), 'missing DNR variant'
    assert any(v['editor'] for v in variants), 'missing editor variants'
    assert any(v['one_sun'] for v in variants), 'missing one-sun variant'
    return variants


def main():
    variants = verify()
    output = Path(__file__).with_name('shader_variants.json')
    output.write_text(json.dumps({'stage': 'CS00', 'type': 'source inventory',
                                  'gpu_measured': False, 'variants': variants}, indent=2) + '\n',
                      encoding='utf-8')
    print(f'CS00/02: shared math, explicit gradients, include topology and {len(variants)} shader variants: PASS')


if __name__ == '__main__':
    main()
