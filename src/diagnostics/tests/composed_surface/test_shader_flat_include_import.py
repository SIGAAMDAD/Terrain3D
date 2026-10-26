#!/usr/bin/env python3
"""Godot RDShaderFile imports direct stage includes only; reject nested GLSL includes.

This deliberately simulates a *single* expansion pass rather than the recursive
expansion used by offline CS19 tooling (which previously hid the regression).
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]
RD = ROOT / 'project/addons/terrain_3d/rd'
INCLUDE = re.compile(r'^\s*#include\s+"([^"\n]+)"\s*$', re.M)
ROOTS = sorted(RD.glob('*.glsl'))
assert ROOTS, 'no shader stage roots found'

checked_stages = 0
for shader in ROOTS:
    source = shader.read_text(encoding='utf-8')
    # For every stage, replace includes once, exactly as the importer does.
    stages = re.split(r'^#\[(?:vertex|fragment|compute)\]\s*$', source, flags=re.M)
    for stage in stages[1:]:
        for name in INCLUDE.findall(stage):
            target = (shader.parent / name).resolve()
            assert target.is_relative_to(RD.resolve()), (shader.name, name)
            assert target.is_file(), (shader.name, name)
        expanded = INCLUDE.sub(
            lambda match: (shader.parent / match.group(1)).read_text(encoding='utf-8'),
            stage)
        assert not INCLUDE.search(expanded), f'nested #include leaked from {shader.name}'
        checked_stages += 1

for name in ('terrain_clipmap.glsl', 'terrain_clipmap.glsl',
             'terrain_clipmap.glsl', 'terrain_world_raster.glsl'):
    frag = (RD / name).read_text().split('#[fragment]', 1)[1]
    math = frag.index('#include "terrain_composed_material_math.glslinc"')
    sampling = frag.index('#include "terrain_composed_surface_sampling.glslinc"')
    fragment = frag.index('#include "terrain_clipmap_fragment_inc.glslinc"')
    assert math < sampling < fragment, name
    assert frag.index('vec3 decode_surface_normal(vec2 encoded_xy);') < sampling, name

print(f'Godot-style single-pass includes: {len(ROOTS)} shader roots / {checked_stages} stages: PASS')
