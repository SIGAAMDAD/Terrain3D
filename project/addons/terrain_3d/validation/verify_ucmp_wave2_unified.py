#!/usr/bin/env python3
"""UCMP-04/05: source/ABI closure for mandatory unified cooked material."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[4]
source = root / 'project/addons/terrain_3d/rd/terrain_clipmap.glsl'
frag = root / 'project/addons/terrain_3d/rd/terrain_clipmap_fragment_inc.glslinc'
inc = root / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp'
header = root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.h'
catalog = root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_catalog.cpp'
lib = root / 'src/rendering/rd/shaders/terrain_3d_rd_shader_library.cpp'
builder = root / 'src/rendering/rd/material/terrain_3d_material_state_builder.cpp'
material = root / 'src/rendering/material/terrain_3d_material.cpp'
read = lambda p: p.read_text(encoding='utf-8')
shader = read(source)
versions = set(re.findall(r'^([a-z][a-z0-9_]*)\s*=', shader, re.M))
assert shader.count('#define T3D_UCMP_COOKED 1') == 2
assert shader.count('#define T3D_RUNTIME_TIER 1') == 2
assert '#include "terrain_composed_surface_sampling.glslinc"' in shader
assert '#include "terrain_clipmap_fragment_inc.glslinc"' in shader

color = ['color_native','color_specular','color_motion','color_specular_motion',
         'color_native_radiance_array','color_specular_radiance_array',
         'color_motion_radiance_array','color_specular_motion_radiance_array']
for c in color:
    for stem in ('', 'editor_'):
        for suffix in ('', '_one_sun', '_compact'):
            expected = (stem + c.replace('_radiance_array', '_one_sun_radiance_array') if suffix == '_one_sun' and '_radiance_array' in c else stem + c + suffix)
            assert expected in versions, (stem, c, suffix)
    for suffix in ('', '_one_sun', '_compact'):
        expected = ('feedback_' + c.replace('_radiance_array', '_one_sun_radiance_array') if suffix == '_one_sun' and '_radiance_array' in c else 'feedback_' + c + suffix)
        assert expected in versions, (c, suffix)
for stem in ('', 'editor_'):
    for suffix in ('depth','depth_normal_roughness','depth_normal_roughness_voxel_gi'):
        assert stem + suffix in versions
assert len(versions) == 81, len(versions)
assert not any(v.startswith('compat_') for v in versions)

fragment = read(frag)
assert '#ifdef T3D_UCMP_COOKED' in fragment
assert 'TierMaterialConfig tier_config = material_config.tier[0];' in fragment
assert 'bool composed_eligible = true;' in fragment
assert 'composite_mode = false;' in fragment
assert 'need_far_surface = outside_region;' in fragment

renderer = read(inc)
assert 'm_bUnifiedCookedMaterialActive' not in renderer
assert 't3dRdUnifiedTerrainId(' in renderer
assert 'm_hUnifiedRasterShader.releaseTracked' in renderer
assert 'm_hszRasterUniformSets[1]' in read(root / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h')
assert 't3dRdUnifiedTerrainId(' in read(header)
assert 'T3D_RD_SHADER_UCMP_ALIAS' in read(catalog)
assert 'terrain_clipmap.glsl' in read(catalog)
assert 't3dRdShaderCatalogIsActive( id )' in read(lib)
assert 'm_bUnifiedCookedPreview = true;' in read(lib)
assert 'T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1' in read(builder)
assert 'T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1' in read(material)
print('UCMP Wave 2 mandatory UCMP root, pass variants, catalog, state and descriptors PASS:', len(versions), 'shader versions')
