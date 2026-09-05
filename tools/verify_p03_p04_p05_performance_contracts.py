#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
failures = []

def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8', errors='replace')

def require(rel, needle, label=None):
    if needle not in read(rel):
        failures.append(label or f'{rel}: missing {needle!r}')

def forbid(rel, needles, label=None):
    text = read(rel)
    for needle in needles:
        if needle in text:
            failures.append(label or f'{rel}: forbidden {needle!r}')

def function_body(text, signature):
    start = text.find(signature)
    if start < 0:
        failures.append(f'missing function {signature}')
        return ''
    brace = text.find('{', start)
    if brace < 0:
        failures.append(f'malformed function {signature}')
        return ''
    depth = 0
    for i in range(brace, len(text)):
        c = text[i]
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[brace:i+1]
    failures.append(f'unterminated function {signature}')
    return ''

# P03: canonical fixed shader/address contract.
common = 'project/addons/terrain_3d/shaders/includes/terrain_common_uniforms.gdshaderinc'
for needle in (
    'const int T3D_REGION_SIZE = 128;',
    'const int T3D_REGION_SHIFT = 7;',
    'const int T3D_REGION_MASK = 127;',
    'const float T3D_REGION_SIZE_F = 128.0;',
    'const float T3D_REGION_INV_SIZE = 0.0078125;',
    'const int T3D_REGION_MAP_SIZE = 32;',
    'const int T3D_REGION_MAP_HALF = 16;',
):
    require(common, needle)

shader_roots = [ROOT/'project/addons/terrain_3d/shaders', ROOT/'project/addons/terrain_3d/extras']
for root in shader_roots:
    for p in root.rglob('*'):
        if not p.is_file() or p.suffix not in ('.gdshader', '.gdshaderinc', '.tres', '.gd'):
            continue
        text = p.read_text(encoding='utf-8', errors='replace')
        for token in ('_region_size', '_region_texel_size', '_region_map_size'):
            if token in text:
                failures.append(f'{p.relative_to(ROOT)}: legacy runtime shader geometry token {token}')

p03_files = [
    'src/instances/terrain_3d_instance_layout.h',
    'src/instances/terrain_3d_instancer.cpp',
    'src/instances/terrain_3d_instancer.h',
    'src/physics/terrain_3d_collision.cpp',
    'src/physics/terrain_3d_collision_surface_builder.cpp',
    'src/physics/terrain_3d_collision_surface_builder.h',
    'src/data/terrain_3d_runtime_bake_codec.cpp',
    'src/data/terrain_3d_runtime_bake_codec.h',
    'src/streaming/terrain_3d_cpu_cache_policy.h',
    'src/streaming/terrain_3d_gpu_cache_policy.h',
    'src/streaming/terrain_3d_cpu_region_cache.cpp',
]
for rel in p03_files:
    text = read(rel)
    for pattern in (r'\bnRegionSize\b', r'\bnSourceWidth\b', r'getRegionSize\(\)', r'getVertexSpacing\(\)'):
        if re.search(pattern, text):
            failures.append(f'{rel}: forbidden runtime geometry pattern {pattern}')
require('src/core/terrain_3d_util.h', 't3dRegionCoordinateFromGrid')
require('src/core/terrain_3d_util.h', 't3dRegionLocalFromGrid')
require('src/Terrain3DConstants.h', '#define s_nRegionShift ( 7 )')
require('src/Terrain3DConstants.h', '#define s_nMaxCollisionAxisSamples ( s_nMaxRegionBlendSamples + 1 )')
for needle in (
    's_szRuntimeMinMaxLevelSides[] = { 128u, 64u, 32u, 16u, 8u, 4u, 2u, 1u }',
    's_nRuntimeMinMaxLevelCount = 8u',
    's_nRuntimeMinMaxBlobBytes = 174836u',
    'side != expected_side',
):
    require('src/data/terrain_3d_runtime_bake_codec.cpp', needle)
for stale in ('For a 2048 region', 'source_side == 0u', 'level_count > 32u'):
    forbid('src/data/terrain_3d_runtime_bake_codec.cpp', (stale,))

# P04: explicit allocation domain and central-zone ownership.
require('src/core/terrain_3d_allocation_domain.h', 'TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED')
for rel in ('src/core/terrain_3d_fixed_buffer.h', 'src/core/terrain_3d_slab_array.h'):
    require(rel, 'requireRuntimeZone')
    require(rel, 'bindAllocatorZone')
    require(rel, 'TERRAIN3D_NATIVE_ALLOCATION_RUNTIME_ZONE_REQUIRED')
require('src/memory/terrain_3d_memory.h', 'TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME')
require('src/memory/terrain_3d_memory.h', 'TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER')
forbid('src/memory/terrain_3d_memory.h', ('m_szRawDeniedEvents', 'm_szRawDeniedBytes', 't3dRuntimeRawAllocationAllowed', 't3dRecordRuntimeRawAllocationDenied'), 'P04 allocator phase permission must remain telemetry-only')
forbid('src/memory/terrain_3d_memory_policy.cpp', ('raw_denied_events_by_phase', 'raw_denied_bytes_by_phase'), 'P04 denied-allocation telemetry must not be resurrected')

for rel in (
    'src/memory/terrain_3d_resident_pool.cpp',
    'src/streaming/terrain_3d_node_streamer.cpp',
    'src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp',
):
    forbid(rel, ('t3dMemoryAllocate(', 't3dMemoryRelease('), f'{rel}: runtime owner still uses raw Terrain3D allocation')
require('src/memory/terrain_3d_resident_pool.cpp', 'm_pRuntimeAllocatorZone')
require('src/streaming/terrain_3d_node_streamer.cpp', 'm_pRuntimeAllocatorZone')
require('src/rendering/rd/render_world/terrain_3d_render_world_runtime.cpp', 'm_pRuntimeAllocatorZone')
require('src/instances/terrain_3d_instance_store.h', 'TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED')
require('src/instances/terrain_3d_instance_store.cpp', 'runtimeAllocatorReady')
require('src/streaming/terrain_3d_runtime_page_store.cpp', 'TERRAIN3D_INSTANCE_STORE_RUNTIME_PREPARED')
require('src/streaming/terrain_3d_runtime_page_store.h', 'setPreparedByteBudget')
for needle in ('m_nPreparedByteBudget', 'm_nPreparedReservedBytes', 'm_nPayloadBytesPerSlot', 'm_nWorkerScratchReservedBytes'):
    require('src/streaming/terrain_3d_runtime_page_store.h', needle)
require('src/memory/terrain_3d_memory_policy.cpp', 'setRuntimePagePreparedByteBudget')
require('src/core/terrain_3d_slab_array.h', 'canBindAllocatorZone')
require('src/instances/terrain_3d_instance_store.cpp', 'm_transformSlab.canBindAllocatorZone( pZone )')
forbid('src/memory/terrain_3d_memory.h', ('Terrain3D raw allocation attempted from HOT_FRAME/HOT_RENDER',), 'allocator must not reject HOT_FRAME/HOT_RENDER allocations')

# P05: worker-side cooked material preparation and owner-side bounded consume.
assets = read('src/assets/terrain_3d_assets.cpp')
worker = function_body(assets, 'void Terrain3DAssets::runtimeMaterialWorkerMain')
owner = function_body(assets, 'void Terrain3DAssets::updateRuntimeMaterialResidencyInternal')
for needle in ('FileAccess::open', 'seek(', 'get_buffer(', 'Image::create_from_data'):
    if needle not in worker:
        failures.append(f'assets worker: missing worker-side operation {needle}')
for needle in ('FileAccess::open', 'get_buffer(', 'Image::create_from_data', 't3dRuntimeMaterialChecksum'):
    if needle in owner:
        failures.append(f'assets owner hot path: synchronous preparation survived: {needle}')
for needle in (
    's_nRuntimeMaterialDiskBytesPerSecond',
    's_nRuntimeMaterialPreparedByteLimit',
    's_nRuntimeMaterialGpuBytesPerSecond',
    's_nRuntimeMaterialUpdateCallsPerSecond',
    'consumeRuntimeMaterialBucket',
    'm_runtimeMaterialChecksumVerified',
    'm_runtimeMaterialRequestRing',
    'm_runtimeMaterialCompletionRing',
):
    if needle not in assets and needle not in read('src/assets/terrain_3d_assets.h'):
        failures.append(f'assets async material contract missing {needle}')
if worker.count('FileAccess::open') != 1:
    failures.append('assets worker: persistent cooked reader must open exactly once per worker lifetime')
if owner.find('refillRuntimeMaterialBucket( m_runtimeMaterialGpuBucket') > owner.find('m_nRuntimeMaterialResidentCount >= mapped_capacity'):
    failures.append('assets owner: GPU/upload token admission occurs after residency eviction')
stop_worker = function_body(assets, 'void Terrain3DAssets::stopRuntimeMaterialWorker')
reset_cache = function_body(assets, 'void Terrain3DAssets::resetRuntimeMaterialCacheState')
if 'memset( m_runtimeMaterialChecksumVerified' in stop_worker:
    failures.append('assets worker stop: immutable cooked checksum cache is discarded on worker restart')
if 'memset( m_runtimeMaterialChecksumVerified' not in reset_cache:
    failures.append('assets cache reset: checksum verification cache is not invalidated with cooked state')
if 'cooked_identity_changed' not in assets:
    failures.append('assets cooked index: checksum cache is not invalidated when cooked identity changes')
if 'm_nRuntimeMaterialPreparedBytesInFlight -= request.m_nPreparedBytes' not in worker:
    failures.append('assets worker: completion-ring overflow can leak prepared-byte admission')

if failures:
    print('P03/P04/P05 verification FAILED')
    for f in failures:
        print(' -', f)
    sys.exit(1)
print('P03/P04/P05 verification PASS')
