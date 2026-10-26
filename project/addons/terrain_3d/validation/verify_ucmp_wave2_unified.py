#!/usr/bin/env python3
"""UCMP-04/05: source/ABI closure for the opt-in unified material path."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[4]
source = root / 'project/addons/terrain_3d/rd/terrain_clipmap.glsl'
near = source
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
legacy_versions = set(re.findall(r'^([a-z][a-z0-9_]*)\s*=', read(near), re.M))
assert versions == legacy_versions, 'Unified root lost an existing pass/version'
assert shader.count('#define T3D_UCMP_COOKED 1') == 2
assert shader.count('#define T3D_RUNTIME_TIER 1') == 2
assert '#include "terrain_composed_surface_sampling.glslinc"' in shader
assert '#include "terrain_clipmap_fragment_inc.glslinc"' in shader

color = ['color_native','color_specular','color_motion','color_specular_motion',
         'color_native_radiance_array','color_specular_radiance_array',
         'color_motion_radiance_array','color_specular_motion_radiance_array']
for c in color:
    for stem in ('', 'compat_', 'editor_'):
        for suffix in ('', '_one_sun', '_compact'):
            expected = (stem + c.replace('_radiance_array', '_one_sun_radiance_array') if suffix == '_one_sun' and '_radiance_array' in c else stem + c + suffix)
            assert expected in versions, (stem, c, suffix)
    for suffix in ('', '_one_sun', '_compact'):
        expected = ('feedback_' + c.replace('_radiance_array', '_one_sun_radiance_array') if suffix == '_one_sun' and '_radiance_array' in c else 'feedback_' + c + suffix)
        assert expected in versions, (c, suffix)
for stem in ('', 'compat_', 'editor_'):
    for suffix in ('depth','depth_normal_roughness','depth_normal_roughness_voxel_gi'):
        assert stem + suffix in versions
assert len(versions) >= 108, len(versions)

fragment = read(frag)
assert '#ifdef T3D_UCMP_COOKED' in fragment
assert 'TierMaterialConfig tier_config = material_config.tier[0];' in fragment
assert 'bool composed_eligible = true;' in fragment
assert 'composite_mode = false;' in fragment
assert 'need_far_surface = outside_region;' in fragment

renderer = read(inc)
assert 'm_bUnifiedCookedMaterialActive ? 1u : 3u' in renderer
assert 'm_bUnifiedCookedMaterialActive ? 1 : 3' in renderer
assert 't3dRdUnifiedTerrainId(' in renderer
assert 'm_bUnifiedCookedMaterialActive ? 0u :' in renderer
assert 'freeForwardPipelines();' in renderer and 'freeUniformSets();' in renderer
assert 'm_hUnifiedRasterShader.releaseTracked' in renderer
assert 'm_bUnifiedCookedMaterialActive' in renderer
assert 'm_bUnifiedCooked' in renderer
assert 't3dRdUnifiedTerrainId(' in read(header)
assert 'T3D_RD_SHADER_UCMP_ALIAS' in read(catalog)
assert 'terrain_clipmap.glsl' in read(catalog)
assert 'T3D_RD_SHADER_UCMP_ALIAS' in read(lib)
assert 'm_bUnifiedCookedPreview = true;' in read(lib)
assert 'isUnifiedCookedPreviewEnabledCold()' in read(builder)
assert 'T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1' in read(material)
print('UCMP Wave 2 shader-root/pass versions, opt-in, catalog, state and descriptor contracts PASS:', len(versions), 'shader versions')
