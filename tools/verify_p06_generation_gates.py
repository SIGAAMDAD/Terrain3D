#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
failures = []

def text(rel):
    return (ROOT / rel).read_text(encoding='utf-8')

def require(rel, needle, label):
    data = text(rel)
    if needle not in data:
        failures.append(f'{label}: missing {needle!r} in {rel}')

def forbid(rel, needle, label):
    data = text(rel)
    if needle in data:
        failures.append(f'{label}: forbidden {needle!r} remains in {rel}')

def block(rel, start, end):
    data = text(rel)
    a = data.find(start)
    if a < 0:
        failures.append(f'missing block start {start!r} in {rel}')
        return ''
    b = data.find(end, a + len(start))
    if b < 0:
        failures.append(f'missing block end {end!r} in {rel}')
        return data[a:]
    return data[a:b]

# P06: source-owned generations and coordinator generation gates.
require('src/assets/terrain_3d_assets.h', 'getTexturePublicationGeneration', 'P06 texture publication generation')
require('src/assets/terrain_3d_assets.h', 'getTextureLayerMappingGeneration', 'P06 layer-map generation')
require('src/assets/terrain_3d_assets.h', 'getTextureLayerPhysicalIndexInternal', 'P06 semantic layer-map accessor')
forbid('src/assets/terrain_3d_assets.h', 'getTextureLayerMapDataInternal', 'P06 raw layer-map accessor regression')
require('src/assets/terrain_3d_assets.h', 'getMeshSourceGeneration', 'P06 mesh source generation')
require('src/rendering/terrain_3d_material.h', 'getRdPublicationGeneration', 'P06 material generation')
require('src/lighting/terrain_3d_region_lighting.h', 'getGlobalPublicationGeneration', 'P06 regional lighting generation')
require('src/terrain_3d.h', 'getOceanPublicationGeneration', 'P06 ocean generation')
coordinator_publish = block('src/rendering/rd/terrain_3d_render_coordinator.cpp',
                            'void Terrain3DRenderCoordinator::publishSnapshot',
                            'void Terrain3DRenderCoordinator::resetSnapshotPublication')
if 'computeSourceFingerprint' in coordinator_publish:
    failures.append('P06 mesh fingerprint remains in normal publishSnapshot hot path')
if 'getTextureLayerMap()' in coordinator_publish:
    failures.append('P06 coordinator still requests an allocating PackedInt32Array layer map')
material_builder = block('src/rendering/rd/terrain_3d_render_coordinator.cpp',
                         'void Terrain3DRenderCoordinator::buildMaterialState',
                         'uint32_t Terrain3DRenderCoordinator::selectWritablePublicationBank')
if 'getTextureLayerPhysicalIndexInternal' not in material_builder:
    failures.append('P06 coordinator does not preserve runtime-cache/identity layer-map semantics')
for symbol in ('material_source_generation', 'region_lighting_source_generation', 'mesh_source_generation', 'ocean_source_generation'):
    if symbol not in coordinator_publish:
        failures.append(f'P06 missing coordinator generation gate: {symbol}')
for telemetry in ('m_nMaterialStateBuildCount', 'm_nRegionLightingScanCount', 'm_nMeshSourceBuildCount', 'm_nOceanStaticStateBuildCount'):
    if telemetry not in coordinator_publish:
        failures.append(f'P06 missing producer telemetry: {telemetry}')

# Regression coverage: material-state caching must invalidate when startup GPU
# residency/feature state changes, not only when authored resources change.
for symbol in ('areControlMapsImplicit', 'areColorMapsImplicit',
               'getDerivedMapsRid', 'getMiddleCompositeMapsRid', 'getMiddleHeightMapsRid',
               'getMiddleHeightRegionMapRid', 'getFarHeightMapRid', 'getFarColorMapRid',
               'getFarAlbedoArrayRid', 'getComputeDisplacementTextureRid'):
    if symbol not in coordinator_publish:
        failures.append(f'P06 material source gate misses runtime dependency: {symbol}')

tier_change = block('src/rendering/terrain_3d_material.cpp',
                    'void Terrain3DMaterial::tierQualityChanged',
                    'void Terrain3DMaterial::refreshGpuRegionChannelPolicy')
if 'm_nRdPublicationGeneration' not in tier_change:
    failures.append('P06 dynamic tier values do not invalidate cached RD material state')

mesh_update = block('src/assets/terrain_3d_assets.cpp',
                    'void Terrain3DAssets::updateMeshInternal',
                    'void Terrain3DAssets::updateMesh(')
if mesh_update.find('advanceMeshSourceGeneration();') < 0 or mesh_update.find('advanceMeshSourceGeneration();') > mesh_update.find('if ( bPersistentChange )'):
    failures.append('P06 preview/nonpersistent mesh changes do not advance mesh source generation')

lighting_replace = block('src/data/terrain_3d_region.cpp',
                         'void Terrain3DRegion::setLighting',
                         'void Terrain3DRegion::setLocation')
if 'markGlobalPublicationDirtyInternal' not in lighting_replace:
    failures.append('P06 replacing a region lighting resource does not invalidate lighting generation')
# P07 is intentionally layered on this package; continue rejecting P08-only markers.
for rel, needle, label in (
    ('src/rendering/rd/render_world/terrain_3d_render_world.h', 'prepare_camera_visibility_hot', 'P08 contamination'),
    ('src/rendering/rd/terrain_3d_render_coordinator.cpp', 'render_world_compute_prepares', 'P08 diagnostics contamination'),
):
    forbid(rel, needle, label)

if failures:
    print('P06 generation-gate verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)
print('P06 generation-gate verification PASS')
