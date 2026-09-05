#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src'
errors = []


def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8', errors='replace')


def require(cond, msg):
    if not cond:
        errors.append(msg)


def function_body(text, signature):
    pos = text.find(signature)
    if pos < 0:
        return ''
    brace = text.find('{', pos)
    if brace < 0:
        return ''
    depth = 0
    for i in range(brace, len(text)):
        ch = text[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return text[brace + 1:i]
    return ''


# G1: permission-era policy may not return to production source.
production_files = [p for p in SRC.rglob('*') if p.is_file() and p.suffix in {'.h', '.hpp', '.cpp', '.c', '.inl'} and 'tests' not in p.parts]
production = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in production_files)
for forbidden in (
    'g_t3dRuntimeAllocationPermission',
    't3dValidateRuntimeAllocationRequest',
    't3dRuntimeAllocationPhaseAllowsAllocation',
    'Terrain3DColdAllocationScope',
    'Terrain3DZoneAllocatorRouter',
    'current_allocation_permission',
    'restricted_cold_override',
    'allocation_denied_checks',
    't3dRuntimeRawAllocationAllowed',
    't3dRecordRuntimeRawAllocationDenied',
    'm_szRawDeniedEvents',
    'm_szRawDeniedBytes',
):
    require(forbidden not in production, f'permission-era production symbol/text remains: {forbidden}')

# G2: allocator layers may record telemetry but may not use phase state as policy.
allocator_layers = {
    'src/eastl_config.h': read('src/eastl_config.h'),
    'src/memory/terrain_3d_memory_zone.cpp': read('src/memory/terrain_3d_memory_zone.cpp'),
    'src/memory/terrain_3d_linear_arena.h': read('src/memory/terrain_3d_linear_arena.h'),
    'src/memory/terrain_3d_zone_allocator.h': read('src/memory/terrain_3d_zone_allocator.h'),
}
for rel, text in allocator_layers.items():
    for forbidden in (
        'g_t3dRuntimeAllocationPhase',
        't3dGetRuntimeAllocationPhase(',
        'TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME',
        'TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER',
        'Terrain3DAllocationPhaseScope',
        'Terrain3DColdPhaseScope',
    ):
        require(forbidden not in text, f'allocator layer {rel} depends on runtime phase policy: {forbidden}')

memory = read('src/memory/terrain_3d_memory.h')
require('Phase labels never grant or deny allocation.' in memory, 'memory phase contract does not state telemetry-only phase semantics')
require('t3dRecordRuntimeEastlAllocation' in memory and 't3dRecordRuntimeZoneAllocation' in memory and 't3dRecordRuntimeRawAllocation' in memory,
        'allocation phase telemetry is incomplete')

# G3: linear arena cursor allocation must never change physical backing.
linear = allocator_layers['src/memory/terrain_3d_linear_arena.h']
linear_allocate = function_body(linear, 'void *allocate( const size_t nBytes, const size_t nAlignment )')
require(bool(linear_allocate), 'could not locate Terrain3DLinearArena::allocate')
for forbidden in ('configure(', 'ensureCapacity(', 't3dMemoryAllocate(', 't3dMemoryRelease(', 'releaseBacking(', 'm_bCapacitySealed'):
    require(forbidden not in linear_allocate, f'linear arena cursor allocation contains backing-growth logic: {forbidden}')

# G4: named steady-state owners must stay within established capacity.
hot_checks = [
    ('src/streaming/terrain_3d_instance_region_cache.cpp', 'void Terrain3DInstanceRegionCache::update(',
     ('ensureInstanceStoreResident', 'materializeRegionCold', 'takeInstance(', 't3dContainerReserveCold', '.reserve(')),
    ('src/streaming/terrain_3d_node_streamer.cpp', 'void Terrain3DNodeStreamer::updateResidency(',
     ('rebuildActivationStorageCold', 'serviceColdRebuild(', 't3dContainerReserveCold', '.reserve(')),
    ('src/physics/terrain_3d_collision.cpp', 'void Terrain3DCollision::update( const Vector2i &rRegionLoc, const bool bRebuild )',
     ('build();', 'serviceColdRebuild(', 't3dContainerReserveCold', '.reserve(', '.resize(')),
    ('src/rendering/rd/terrain_3d_render_coordinator.cpp', 'void Terrain3DRenderCoordinator::publishSnapshot( void )',
     ('ensureInstancePublicationStorage', 'ensureInstanceMeshBankStorage', 't3dContainerReserveCold', '.reserve(', '.resize(')),
    ('src/instances/terrain_3d_instancer.cpp', 'void Terrain3DInstancer::processUpdates( void )',
     ('rebuildRuntimeStorageCapacities', 'serviceColdCapacity(', 't3dContainerReserveCold', '.reserve(')),
    ('src/instances/terrain_3d_instancer.cpp', 'void Terrain3DInstancer::updateStreaming( const Vector3 &rTargetPosition )',
     ('rebuildRuntimeStorageCapacities', 'serviceColdCapacity(', 't3dContainerReserveCold', '.reserve(')),
]
for rel, signature, forbidden_tokens in hot_checks:
    body = function_body(read(rel), signature)
    require(bool(body), f'could not locate hot owner function: {signature}')
    for forbidden in forbidden_tokens:
        require(forbidden not in body, f'hot owner {signature} contains backing-growth path: {forbidden}')

# G5: explicit zone binding is limited to Terrain3D-instance-owned runtime storage.
zone_calls = []
for p in production_files:
    text = p.read_text(encoding='utf-8', errors='replace')
    if 'zoneAllocator(' in text:
        zone_calls.append(p.relative_to(ROOT).as_posix())
allowed_zone_files = {
    'src/eastl_config.h',
    'src/core/terrain_3d_fixed_buffer.h',
    'src/core/terrain_3d_slab_array.h',
    'src/instances/terrain_3d_instancer.cpp',
    'src/instances/terrain_3d_instance_store.cpp',
    'src/hlod/terrain_3d_hlod_streamer.cpp',
    'src/rendering/rd/terrain_3d_rd_mesh_cache.cpp',
    'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp',
    'src/rendering/rd/terrain_3d_render_coordinator.cpp',
}
for rel in zone_calls:
    require(rel in allowed_zone_files, f'zoneAllocator appears in unreviewed lifetime owner: {rel}')
region = read('src/data/terrain_3d_region.cpp')
manifest = read('src/data/terrain_3d_runtime_world_manifest.cpp')
require('m_streamedNodeRuntime( Terrain3DEastlAllocator::bootstrapAllocator() )' in region,
        'Terrain3DRegion Resource-owned runtime metadata is not explicitly bootstrap-owned')
require('Terrain3DEastlAllocator::bootstrapAllocator()' in manifest,
        'runtime-world manifest persistent/resource-like storage lost explicit bootstrap ownership')

# G6: critical owners expose owner-specific capacity diagnostics rather than allocator denial.
instancer_h = read('src/instances/terrain_3d_instancer.h')
instancer_cpp = read('src/instances/terrain_3d_instancer.cpp')
coord_h = read('src/rendering/rd/terrain_3d_render_coordinator.h')
coord_cpp = read('src/rendering/rd/terrain_3d_render_coordinator.cpp')
require('m_nOwnerGrowthDeniedCount' in instancer_h and 'owner_growth_denied' in instancer_cpp,
        'Instancer owner-growth diagnostic is not explicitly owner-scoped')
require('m_nOwnerGrowthDeniedCount' in coord_h and 'owner_growth_denied' in coord_cpp,
        'RenderCoordinator owner-growth diagnostic is not explicitly owner-scoped')
for rel in (
    'src/streaming/terrain_3d_instance_region_cache.cpp',
    'src/streaming/terrain_3d_node_streamer.cpp',
    'src/physics/terrain_3d_collision.cpp',
    'src/hlod/terrain_3d_hlod_streamer.cpp',
    'src/rendering/rd/terrain_3d_rd_instance_renderer.cpp',
):
    text = read(rel)
    require('capacity_miss' in text or 'capacity_misses' in text or 'materialization_misses' in text or 'rebuild_failures' in text,
            f'owner diagnostic coverage missing from {rel}')

# G7: terminology and developer contract.
require(not list(SRC.rglob('*ColdAllocationScope*')), 'ColdAllocationScope-named source artifact remains')
for p in production_files:
    text = p.read_text(encoding='utf-8', errors='replace')
    require('Terrain3DColdPhaseScope allocation_scope' not in text,
            f'cold phase scope still uses allocator-permission variable naming: {p.relative_to(ROOT)}')
readme = read('src/README.md')
require('## Owner-controlled allocation' in readme, 'developer ownership contract missing from src/README.md')
require('runtime phase labels are diagnostic telemetry only' in readme, 'developer contract does not state telemetry-only phase semantics')

sconstruct = read('SConstruct')
require(not (SRC / 'memory' / 'tests').exists(),
        'standalone allocator tests still live under src/ and can enter production source discovery')
require((ROOT / 'tests' / 'memory' / 'terrain_3d_eastl_ownership_standalone.cpp').exists(),
        'EASTL ownership standalone test is missing from tests/memory')
require((ROOT / 'tests' / 'memory' / 'terrain_3d_owner_allocation_standalone.cpp').exists(),
        'owner-allocation standalone test is missing from tests/memory')
require('glob("src/**/*.cpp", recursive=True)' in sconstruct,
        'production source discovery no longer has a single src-only root')
require('Standalone test source entered Terrain3D production build' in sconstruct,
        'SConstruct lacks a fail-closed guard against test translation units entering the DLL')

if errors:
    for error in errors:
        print('FAIL:', error)
    sys.exit(1)
print('PASS: owner-controlled allocation patch G cleanup and ownership regression gates.')
