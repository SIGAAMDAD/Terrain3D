#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def text(rel):
    return (ROOT / rel).read_text(encoding='utf-8')

def require(rel, needle, message):
    if needle not in text(rel):
        raise SystemExit(f'FAIL: {message}')

cache = text('src/streaming/cache/terrain_3d_gpu_region_cache.cpp')
stream = text('src/streaming/integration/terrain_3d_streaming_integration.cpp')
terrain = text('src/terrain_3d.cpp')

minimum_start = cache.index('size_t Terrain3DGpuRegionCache::getMinimumBytes')
minimum_end = cache.index('size_t Terrain3DGpuRegionCache::getHardCapBytes', minimum_start)
minimum = cache[minimum_start:minimum_end]
if 'calculateRequiredCorePagesForLods( 0 )' not in minimum:
    raise SystemExit('FAIL: GPU page minimum is not based on the configured Near/detailed clipmap footprint')
if 's_nGpuCacheMinSpareLayers' not in minimum:
    raise SystemExit('FAIL: GPU page minimum no longer preserves legal spare layers')
if 'getActiveRegionCount()' not in minimum:
    raise SystemExit('FAIL: GPU page minimum does not bound the functional footprint by active topology')

normalize_start = stream.index('void Terrain3D::normalizeRuntimeTierLayout')
normalize_end = stream.index('void Terrain3D::applyRuntimeTierLayout', normalize_start)
normalize = stream[normalize_start:normalize_end]
if 'Clamped detailed terrain range to ' in normalize:
    raise SystemExit('FAIL: capacity shortfall still uses destructive tier-clamp path')
if 'm_nGpuRegionDetailLods = supported_lods' in normalize or 'm_nRuntimeNearShaderLods = supported_lods' in normalize:
    raise SystemExit('FAIL: capacity shortfall still mutates render/detail tier counts')
if 'required_pages > capacity' not in normalize or 'markMemoryBudgetDirty();' not in normalize:
    raise SystemExit('FAIL: capacity shortfall does not request owner budget reevaluation')
if 'Middle/Far fallback' not in normalize:
    raise SystemExit('FAIL: non-destructive residency fallback contract is not explicit')

init_start = terrain.index('void Terrain3D::initializeInternal')
init_end = terrain.index('void Terrain3D::deactivateRuntime', init_start)
init = terrain[init_start:init_end]
service_pos = init.index('serviceMemoryBudgetPolicy( true );')
normalize_pos = init.index('normalizeRuntimeTierLayout( true );')
if service_pos > normalize_pos:
    raise SystemExit('FAIL: startup tier normalization still runs before aggregate memory grant finalization')

print('PASS: GPU page functional minimum and non-destructive tier-layout contract')
