#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
failures = []

def text(rel):
    return (ROOT / rel).read_text(encoding='utf-8')

def require(rel, needle, label):
    if needle not in text(rel):
        failures.append(f'{label}: missing {needle!r} in {rel}')

def forbid(rel, needle, label):
    if needle in text(rel):
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

# Exact 128m finite mip-chain accounting.
policy = text('src/streaming/terrain_3d_gpu_cache_policy.h')
if 'for ( ;; )' not in policy or 'bytes += texels * 4u' not in policy:
    failures.append('P07 RGBA8 mip accounting is not an exact finite mip-chain sum')
if 't3dGpuCacheShadowBytes' not in policy or 'bytes += texels;' not in policy:
    failures.append('P07 R8 shadow mip accounting is not an exact finite mip-chain sum')

def mip_bytes(side, bytes_per_texel):
    total = 0
    while True:
        total += side * side * bytes_per_texel
        if side == 1:
            return total
        side = max(1, side >> 1)

full = 128 * 128
color = mip_bytes(128, 4)
shadow = mip_bytes(128, 1)
total = full * 2 + full * 2 + full * 4 + color + color + shadow
if (color, shadow, total) != (87380, 21845, 327677):
    failures.append(f'P07 reference byte arithmetic wrong: color={color} shadow={shadow} total={total}')

cache = text('src/streaming/terrain_3d_gpu_region_cache.cpp')
for needle in (
    'm_nGpuRegionMiddleCompositeBytesLastTick',
    'm_nGpuRegionTerrainShadowBytesLastTick',
    'refillGpuUploadTokens( update_call_budget )',
    'm_nGpuRegionUploadByteTokens',
    'm_nGpuRegionUploadCallTokens',
):
    if needle not in cache:
        failures.append(f'P07 missing accounting/throttle symbol: {needle}')
if 'const uint64_t elapsed = now - m_nGpuRegionUploadTokenRefillUsec;' not in cache:
    failures.append('P07 upload admission is not elapsed-time token based')

full_upload = block(
    'src/streaming/terrain_3d_gpu_region_cache.cpp',
    'bool Terrain3DGpuRegionCache::uploadGpuRegionLayer',
    'bool Terrain3DGpuRegionCache::serviceRuntimePages',
)
# Shipping/runtime keeps cooked pages authoritative, but editor Authoring must
# retain P06's live-source authority. A stale/missing cook must not leave the
# base terrain/instance presentation dormant until a forced edit.
if 'if ( !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() )' not in full_upload:
    failures.append('P07 runtime cooked-page authority no longer distinguishes editor Authoring')
if 'return uploadGpuRegionLayer( nLayer, vLocation, eMapType, false )' not in full_upload:
    failures.append('P07 editor Authoring live fallback is missing for incompatible prepared pages')
if 'The live retry remains bounded by P07' not in full_upload:
    failures.append('P07 editor live fallback contract marker is missing')

# Preserve the repaired P06 exact resident-layer grant rather than regressing to
# the older cumulative P07/P08 archive's base-only capacity arithmetic.
require('src/streaming/terrain_3d_gpu_cache_policy.h', 't3dGpuCacheEstimateResidentLayerBytes', 'P06 exact physical layer grant')
require('src/streaming/terrain_3d_gpu_region_cache.cpp', 'calculateGpuRegionPhysicalLayers( capacity )', 'P06 physical layer capacity')

# P08 must remain absent from this isolated P01->P07 package.
forbid('src/rendering/rd/render_world/terrain_3d_render_world.h', 'prepare_camera_visibility_hot', 'P08 contamination')
forbid('src/rendering/rd/terrain_3d_render_coordinator.cpp', 'render_world_compute_prepares', 'P08 diagnostics contamination')

if failures:
    print('P07 upload-accounting verification FAIL')
    for failure in failures:
        print(' -', failure)
    sys.exit(1)
print('P07 upload-accounting verification PASS')
print(f'P07 fixed complete-page reference: {total} bytes (RGBA8 mip plane {color}, L8 shadow mip plane {shadow})')
