#!/usr/bin/env python3
"""Static CS05–CS06 ABI, dormant-variant, and pass-fallback checks (not GLSL compilation)."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[4]
rd = root / 'project/addons/terrain_3d/rd'
if not rd.exists():
    root = Path(__file__).resolve().parents[5]
    rd = root / 'project/addons/terrain_3d/rd'

interface = (rd / 'terrain_clipmap_fragment_interface.glslinc').read_text()
fragment = (rd / 'terrain_clipmap_fragment_inc.glslinc').read_text()
sampling = (rd / 'terrain_composed_surface_sampling.glslinc').read_text()
world = (rd / 'terrain_world_raster.glsl').read_text()
backend = (root / 'src/rendering/rd/vt/gpu/terrain_3d_composed_surface_gpu.cpp').read_text()
clipmap_host = (root / 'src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp').read_text()

assert '#ifdef T3D_COMP_SURF_ACTIVE' in interface and '#ifdef T3D_COMP_SURF_ACTIVE' in sampling
assert '#include "terrain_composed_surface_sampling.glslinc"' not in fragment
for shader_root in ('terrain_clipmap.glsl', 'terrain_clipmap.glsl',
                    'terrain_clipmap.glsl', 'terrain_world_raster.glsl'):
    stage = (rd / shader_root).read_text().split('#[fragment]', 1)[1]
    assert '#include "terrain_composed_surface_sampling.glslinc"' in stage
assert 'if (!composed_hit)' in fragment
assert 't3d_comp_surf_world_grad_x = dFdx(world_texel)' in fragment
assert 't3d_comp_surf_world_grad_y = dFdy(world_texel)' in fragment
assert fragment.count('t3d_comp_surf_sample(world_texel, composed_grad_x, composed_grad_y, composed)') == 1
assert fragment.count('t3d_comp_surf_sample(world_texel, t3d_comp_surf_world_grad_x, t3d_comp_surf_world_grad_y, composed)') == 1
assert 'dFdx(' not in sampling and 'dFdy(' not in sampling
assert 'result.ao_affect = orm.a;' in sampling
assert 'result.ao_affect = composed.ao_affect;' in fragment
assert 'T3D_COMP_SURF_LEGACY_COMPOSITE_OK && t3d_middle_use_composite' in fragment
assert 'composite_mode = false' in fragment
assert 't3d_comp_surf_decode' not in sampling  # canonical normal decode is reused
assert 'textureGrad(terrain_cs_albedo' in sampling
assert 'textureGrad(terrain_cs_normal' in sampling
assert 'textureGrad(terrain_cs_surface' in sampling
assert 'terrain_cs_pages.length()' in sampling
assert 'm_nRevisionHi' in (root / 'src/rendering/rd/vt/core/terrain_3d_composed_surface_gpu_table.h').read_text()
assert 't3d_comp_surf_gpu_epoch_completed( pending.m_nUploadEpoch, nCompletedEpoch )' in backend
assert 't3d_comp_surf_gpu_epoch_completed( nLastRasterUseEpoch, nCompletedEpoch )' in backend
assert 'serviceCompletedUploadsHot' in backend
assert 'rCache.readyForUploadHot' in backend
assert 'rCache.uploadingRecordHot' in backend
assert '!rCache.retirementPendingHot( retirement )' in backend
assert 'T3D_RD_CONTEXT_MEMORY_COMPOSED_SURFACE' in backend

# The standalone clipmap owns binding 38 already; composing must not alias it.
assert re.search(r'BindingBuilder::single\( 38\s*,', clipmap_host)
for i, key in enumerate(['PAGE_TABLE', 'ALBEDO', 'NORMAL', 'SURFACE']):
    assert f'#define T3D_CS_{key}_BINDING {39 + i}' in interface
    assert f'#define T3D_CS_{key}_BINDING {13 + i}' in world
for name in ['terrain_clipmap.glsl', 'terrain_clipmap.glsl',
             'terrain_clipmap.glsl', 'terrain_world_raster.glsl']:
    text = (rd / name).read_text()
    variants = text.split('#[vertex]')[0]
    for variant in variants.splitlines():
        if variant.startswith('cs_terrain_strip_'):
            continue
        assert 'T3D_COMP_SURF_ACTIVE' not in variant, 'existing variants must stay unchanged' 
print('CS06: lookup ABI, binding isolation, color/DNR fallback and opt-in topology: PASS')
