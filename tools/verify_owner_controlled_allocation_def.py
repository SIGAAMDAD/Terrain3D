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
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[brace + 1:i]
    return ''

# D/E global acceptance gates: no permission-era or process-global router in production source.
production = ''
for p in SRC.rglob('*'):
    if p.is_file() and p.suffix in {'.h', '.hpp', '.cpp', '.c', '.inl'} and 'tests' not in p.parts:
        production += '\n' + p.read_text(encoding='utf-8', errors='replace')
for forbidden in (
    'Terrain3DZoneAllocatorRouter',
    'g_t3dRuntimeAllocationPermission',
    't3dValidateRuntimeAllocationRequest',
    'Terrain3DColdAllocationScope',
):
    require(forbidden not in production, f'production source still contains {forbidden}')

# E: allocator carries explicit owner zone and default/bootstrap is explicit.
eastl = read('src/eastl_config.h')
require('Terrain3DMemoryZone *m_pZone' in eastl, 'Terrain3DEastlAllocator does not carry explicit zone pointer')
require('zoneAllocator( Terrain3DMemoryZone *const pZone' in eastl, 'zoneAllocator helper missing')
require('bootstrapAllocator( void )' in eastl, 'bootstrapAllocator helper missing')
require('return Terrain3DEastlAllocator();' in eastl, 'bootstrap allocator is not explicit default construction')
require('return m_pZone == NULL;' in eastl, 'bootstrap ownership test missing')
require('header.m_pZone = pZone;' in eastl, 'allocation header does not retain originating zone')

# E: retained runtime owners bind to the current Terrain3D zone.
terrain = read('src/terrain_3d.cpp')
render_integration = read('src/rendering/terrain_3d_render_integration.cpp')
require('m_pInstancer->bindRuntimeAllocator( &m_runtimeMemoryZone, getRuntimeAllocatorOverflowPolicyInternal() )' in terrain,
        'Instancer is not explicitly bound to Terrain3D runtime zone')
require('m_pHlodStreamer->bindRuntimeAllocator( &m_runtimeMemoryZone, getRuntimeAllocatorOverflowPolicyInternal() )' in terrain,
        'HLOD streamer is not explicitly bound to Terrain3D runtime zone')
require('m_pRenderCoordinator->bindRuntimeAllocator( getRuntimeMemoryZoneInternal(), getRuntimeAllocatorOverflowPolicyInternal() )' in render_integration,
        'RenderCoordinator is not explicitly bound to Terrain3D runtime zone')

# E: Resource-owned state remains bootstrap because it may outlive a Terrain3D node.
region = read('src/data/terrain_3d_region.cpp')
require('Terrain3DRegion::Terrain3DRegion( void ) : m_streamedNodeRuntime( Terrain3DEastlAllocator::bootstrapAllocator() )' in region,
        'Terrain3DRegion streamed-node Resource storage is not explicitly bootstrap-owned')

# D1: hot instance cache selection never materializes backing.
instance_cache = read('src/streaming/terrain_3d_instance_region_cache.cpp')
ensure_body = function_body(instance_cache, 'bool Terrain3DInstanceRegionCache::ensureResident(')
materialize_body = function_body(instance_cache, 'bool Terrain3DInstanceRegionCache::materializeRegionCold(')
service_body = function_body(instance_cache, 'void Terrain3DInstanceRegionCache::serviceColdMaterialization(')
require(bool(ensure_body), 'could not find InstanceRegionCache::ensureResident')
require('ensureInstanceStoreResident' not in ensure_body and 'materializeRegionCold' not in ensure_body,
        'hot InstanceRegionCache::ensureResident still materializes instance backing')
require('m_bMaterializationPending = true' in ensure_body, 'instance cache does not queue materialization on non-residency')
require('m_bMaterializationServiceActive' in materialize_body, 'cold instance materialization lacks owner service-state guard')
require('ensureInstanceStoreResident' in materialize_body or 'takeInstance' in materialize_body,
        'cold instance materializer does not own instance-store realization')
require('m_bMaterializationServiceActive = true' in service_body and 'm_bMaterializationServiceActive = false' in service_body,
        'instance cold service does not bracket owner service state')

# D2/D3: request only marks pending; service owns rebuild.
node = read('src/streaming/terrain_3d_node_streamer.cpp')
node_req = function_body(node, 'void Terrain3DNodeStreamer::requestActivationStorageRebuild( void )')
node_service = function_body(node, 'void Terrain3DNodeStreamer::serviceColdRebuild( void )')
require('m_bActivationStorageRebuildPending = true' in node_req, 'NodeStreamer rebuild request does not set pending state')
require('rebuildActivationStorageCold' not in node_req and 'serviceColdRebuild' not in node_req,
        'NodeStreamer request performs rebuild immediately')
require('rebuildActivationStorageCold' in node_service and 'm_bColdRebuildServiceActive = true' in node_service,
        'NodeStreamer cold service does not own activation-storage rebuild')

collision = read('src/physics/terrain_3d_collision.cpp')
collision_req = function_body(collision, 'void Terrain3DCollision::requestColdRebuild( void )')
collision_service = function_body(collision, 'void Terrain3DCollision::serviceColdRebuild( void )')
require('m_bColdRebuildPending = true' in collision_req, 'Collision rebuild request does not set pending state')
require('build(' not in collision_req and 'serviceColdRebuild' not in collision_req,
        'Collision request performs rebuild immediately')
require('build();' in collision_service and 'm_bColdRebuildServiceActive = true' in collision_service,
        'Collision cold service does not own rebuild')

# D4: hot coordinator publication defers capacity misses; only cold service grows banks.
coord = read('src/rendering/rd/terrain_3d_render_coordinator.cpp')
publish = function_body(coord, 'void Terrain3DRenderCoordinator::publishSnapshot( void )')
cold_capacity = function_body(coord, 'void Terrain3DRenderCoordinator::serviceColdCapacity( void )')
require('ensureInstancePublicationStorage' not in publish and 'ensureInstanceMeshBankStorage' not in publish,
        'publishSnapshot still grows retained publication backing')
require('recordCapacityMiss' in publish, 'publishSnapshot does not defer owner capacity miss')
require('ensureInstancePublicationStorage' in cold_capacity and 'ensureInstanceMeshBankStorage' in cold_capacity,
        'RenderCoordinator cold capacity service does not own retained-bank growth')
require('Terrain3DColdPhaseScope' in cold_capacity, 'RenderCoordinator cold capacity transition is not phase-labelled')

# F: owner diagnostics are present on the critical plateau owners.
for key in ('owner_capacity_misses', 'owner_growth_denied', 'owner_cold_capacity_rebuilds',
            'owner_publication_capacity_pending', 'owner_mesh_bank_capacity_pending'):
    require(key in coord, f'RenderCoordinator diagnostic missing: {key}')
for key in ('materialization_pending', 'materialization_service_active', 'cold_materializations', 'materialization_misses'):
    require(key in instance_cache, f'Instance cache diagnostic missing: {key}')
for key in ('cold_rebuild_count', 'cold_rebuild_failures', 'capacity_misses'):
    require(key in node, f'NodeStreamer diagnostic missing: {key}')
for key in ('cold_rebuild_count', 'cold_rebuild_failures'):
    require(key in collision, f'Collision diagnostic missing: {key}')

instancer = read('src/instances/terrain_3d_instancer.cpp')
for key in ('owner_capacity_misses', 'owner_growth_denied', 'owner_cold_rebuilds'):
    require(key in instancer, f'Instancer diagnostic missing: {key}')
hlod = read('src/hlod/terrain_3d_hlod_streamer.cpp')
for key in ('owner_capacity_misses', 'owner_cold_rebuilds'):
    require(key in hlod, f'HLOD diagnostic missing: {key}')

rd_instance = read('src/rendering/rd/terrain_3d_rd_instance_renderer.cpp')
for key in ('owner_buffer_capacity_misses', 'owner_buffer_rebuilds', 'owner_buffer_rebuild_failures',
            'owner_cpu_capacity_rebuilds', 'owner_cpu_capacity_rebuild_failures',
            'mesh_owner_arena_capacity_misses', 'mesh_owner_arena_growth_requests', 'mesh_owner_arena_rebuilds'):
    require(key in rd_instance, f'RD instance renderer diagnostic missing: {key}')

# E lifetime: zone-bound runtime owners must die before runtime zone shutdown.
predelete = function_body(terrain, 'case NOTIFICATION_PREDELETE:')
# case extraction via function parser cannot parse case labels; use textual ordered slice.
start = terrain.find('case NOTIFICATION_PREDELETE:')
end = terrain.find('\n\t\tdefault:', start)
predelete = terrain[start:end] if start >= 0 and end > start else ''
require(bool(predelete), 'could not locate NOTIFICATION_PREDELETE block')
shutdown_pos = predelete.find('shutdownRuntimeMemoryImage()')
for destroy_call in ('destroyRenderCoordinator( true )', 'destroyHlodStreamer()', 'destroyInstancer()', 'destroyCollision( true )'):
    pos = predelete.find(destroy_call)
    require(pos >= 0 and shutdown_pos >= 0 and pos < shutdown_pos,
            f'{destroy_call} does not occur before runtime-zone shutdown')
require('releaseRuntimeAllocatorStorageInternal()' in predelete and predelete.find('releaseRuntimeAllocatorStorageInternal()') < shutdown_pos,
        'Terrain3DData runtime allocator storage is not released before zone shutdown')

if errors:
    for e in errors:
        print('FAIL:', e)
    sys.exit(1)
print('PASS: owner-controlled allocation patches D/E/F ownership and diagnostics gates.')
