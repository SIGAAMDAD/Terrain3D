#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
terrain = (ROOT / 'src/terrain_3d.cpp').read_text()
coord_h = (ROOT / 'src/rendering/rd/terrain_3d_render_coordinator.h').read_text()
coord = (ROOT / 'src/rendering/rd/terrain_3d_render_coordinator.cpp').read_text()
renderer = (ROOT / 'src/rendering/rd/terrain_3d_rd_clipmap_renderer.cpp').read_text()

errors = []

def require(cond, msg):
    if not cond:
        errors.append(msg)

asset_init = terrain.find('m_assets->initialize( this );')
first_residency = terrain.find('m_pData->updateRegionResidency( initial_target, true );')
require(asset_init >= 0 and first_residency >= 0 and asset_init < first_residency,
        'assets must own Terrain3D before the first cooked-page residency pass')
require(terrain.count('m_assets->initialize( this );') == 1,
        'startup must initialize assets exactly once')

require('m_bRegionMapHeightTextureReady' in coord_h,
        'coordinator must track whether packed Near mappings have a valid height-array owner')
require('height_texture_ready != m_bRegionMapHeightTextureReady' in coord,
        'region map must republish when Near height-array readiness changes')
require('height_texture_ready && source_index >= 0 ? pPhysicalRegionMap[source_index] : 0u' in coord,
        'physical Near layers must publish as missing while the height array is unavailable')
require('resources.m_hHeightTextureRd = height_texture_rd;' in coord,
        'snapshot must use the same height RID that gated physical page publication')

require(renderer.count('height = m_hFallbackArrayTexture;') >= 2,
        'compositor and Forward+ paths must bind a valid neutral height descriptor during cache convergence')
require('Terrain3D direct RenderingDevice height texture is unavailable' not in renderer,
        'temporary missing Near height RID must not be renderer-fatal')

require('m_nFailureLatchTerrainTextureGeneration' in coord_h and 'm_nFailureLatchMaterialTextureGeneration' in coord_h,
        'latched renderer failure must remember resource generations')
require('Resource generation advanced after a latched failure; retrying renderer initialization' in coord,
        'latched renderer must recover when texture publication advances')
require('published_height_texture_valid' in coord and 'renderer_recoveries' in coord,
        'startup recovery state must be observable through runtime diagnostics')

if errors:
    print('render-startup recovery verification: FAIL')
    for error in errors:
        print(' -', error)
    raise SystemExit(1)

print('render-startup recovery verification: PASS')
