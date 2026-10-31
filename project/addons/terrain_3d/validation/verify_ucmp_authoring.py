#!/usr/bin/env python3
"""UCMP-06/07/08 authoring and retirement gate source assertions (no device claims)."""
from pathlib import Path
import math
import re

root = Path(__file__).resolve().parents[4]
read = lambda p: (root / p).read_text(encoding='utf-8')
mat = read('src/rendering/material/terrain_3d_material.cpp')
hdr = read('src/rendering/material/terrain_3d_material.h')
builder = read('src/rendering/rd/material/terrain_3d_material_state_builder.cpp')
publication = read('src/rendering/rd/core/terrain_3d_render_publication_builder.cpp')
coordinator = read('src/rendering/rd/core/terrain_3d_render_coordinator.cpp')
journal = read('src/authoring/vt/integration/terrain_3d_composed_surface_coordinator_hooks.h')
cooker = read('src/authoring/vt/integration/terrain_3d_composed_surface_editor_source.cpp')
authoring = read('src/authoring/vt/integration/terrain_3d_render_authoring_companion.cpp')

names = ('texture_filtering','material_sampling_quality','terrain_normal_quality',
         'terrain_color_quality','surface_response_quality','texture_surface_mode',
         'lighting_model','detiling_quality','authored_color_contribution',
         'wetness_contribution','material_lod_bias')
for n in names:
    assert f'"cooked_{n}"' in mat, n
    assert f'"set_cooked_{n}"' in mat and f'"get_cooked_{n}"' in mat, n
assert mat.count('PROPERTY_USAGE_EDITOR ),') >= len(names), 'Alias must not serialize duplicate values'
assert 'rProperty.usage = PROPERTY_USAGE_NO_EDITOR;' in mat
assert all(n in mat for n in ('name.begins_with( "near_" )', 'name.begins_with( "middle_" )', 'name.begins_with( "far_" )'))
assert 'setNearMaterialLodBias' in hdr and 'setMiddleMaterialLodBias' in hdr
assert 't3dRdFillTierConfig( config.m_szTier[0]' in builder
assert 'config.m_szTier[1]' not in builder
assert 'T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED' in publication
assert 'getMiddleCompositeMapsRid()' not in publication
assert 'snapshot.m_nTerrainMaterialBackend != T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED' in publication
assert 't3d_comp_surf_editor_dirty_bounds(' in coordinator
assert '#if defined( T3D_EDITOR_NATIVE ) && defined( T3D_COMPOSED_SURFACE_EDITOR_EXPERIMENT )' not in coordinator
assert 'journal.dirtyRegionCold(' in journal and 'locationToRegionIndex( x, z, index )' in journal
assert 'for ( uint32_t i = 0u; i < T3D_COMP_SURF_EDITOR_REGION_COUNT; ++i )' not in journal.split('inline void t3d_comp_surf_editor_dirty_bounds',1)[1].split('inline void t3d_comp_surf_editor_invalidate_sources',1)[0]
assert 'm_bComposedCandidatePending' in authoring
assert 'result["composed_bake_parity_ready"] = false' in authoring
assert 't3d_comp_surf_may_retire_legacy_middle_atlas( t3dComposedSurfaceRetirementEvidence_t{} )' in authoring
assert 'getCookedTextureFiltering()' in cooker and 'getCookedMaterialLodBias()' in cooker
assert 'editor_dirty_tracking_enabled' in coordinator

# Negative-coordinate, region-edge, halo, and world-bound examples used to
# verify the intended bounded projection against a brute-force overlap oracle.
def projected(x0,z0,x1,z1,tpm=2):
    halo=6/tpm
    lo=-16*128; hi=16*128
    if x1+halo <= lo or z1+halo <= lo or x0-halo >= hi or z0-halo >= hi:
        return set()
    ax=math.floor(max(lo,x0-halo)/128); az=math.floor(max(lo,z0-halo)/128)
    bx=math.floor((min(hi,x1+halo)-0.0001)/128)
    bz=math.floor((min(hi,z1+halo)-0.0001)/128)
    return {(x,z) for z in range(az,bz+1) for x in range(ax,bx+1)}

def oracle(x0,z0,x1,z1,tpm=2):
    halo=6/tpm
    return {(x,z) for x in range(-16,16) for z in range(-16,16)
            if x0-halo < (x+1)*128 and x1+halo > x*128 and
               z0-halo < (z+1)*128 and z1+halo > z*128}
for box in ((0,0,1,1),(-1,-1,1,1),(-129,126,-126,129),
            (-2047,0,-2042,1),(-10000,0,-9999,1),(2047,2047,3000,3000),
            (110,110,270,270),(-300,-300,-298,-298)):
    assert projected(*box) == oracle(*box), box
print('UCMP-06/07 editor aliases, persistence, shared records, dirty projection and parity retirement gate PASS')
