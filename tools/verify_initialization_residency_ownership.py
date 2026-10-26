#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
terrain = (ROOT / 'src/terrain_3d.cpp').read_text()
cache = (ROOT / 'src/streaming/cache/terrain_3d_instance_region_cache.cpp').read_text()
region = (ROOT / 'src/data/model/terrain_3d_region.h').read_text()
instancer = (ROOT / 'src/instances/runtime/terrain_3d_instancer.cpp').read_text()
data = (ROOT / 'src/data/model/terrain_3d_data.cpp').read_text()

errors = []

def require(cond, msg):
    if not cond:
        errors.append(msg)

physics_start = terrain.find('void Terrain3D::physicsProcess')
physics_end = terrain.find('\nvoid Terrain3D::', physics_start + 1)
physics = terrain[physics_start: physics_end if physics_end >= 0 else len(terrain)]
cold_start = terrain.find('void Terrain3D::serviceColdRuntimeOwners')
cold_end = terrain.find('\nvoid Terrain3D::', cold_start + 1)
cold_owners = terrain[cold_start: cold_end if cold_end >= 0 else len(terrain)]
hot = physics.find('TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME')
require(hot >= 0, 'HOT_FRAME phase missing from physicsProcess')
require('serviceColdRuntimeOwners( clipmap_target_position );' in physics,
        'physicsProcess does not invoke extracted cold-owner service')
require(physics.find('serviceColdRuntimeOwners( clipmap_target_position );') < hot,
        'cold-owner service occurs after HOT_FRAME')
for needle in (
    'serviceColdInstanceResidency( rTargetPosition, false )',
    'm_pNodeStreamer->serviceColdRebuild();',
    'm_pCollision->serviceColdRebuild();',
    'm_pInstancer->serviceColdCapacity();',
    'm_pRenderCoordinator->serviceColdCapacity();',
):
    require(needle in cold_owners, f'missing cold service: {needle}')
require(physics.find('updateRegionStreamingPolicy( clipmap_target_position, fDelta, false )') <
        physics.find('serviceColdRuntimeOwners( clipmap_target_position );'),
        'streaming policy must be resolved before cold instance materialization')

update_start = cache.find('void Terrain3DInstanceRegionCache::update(')
update_end = cache.find('\nt3dInstanceRegionCacheStats_t', update_start)
update = cache[update_start:update_end]
require('ensureInstanceStoreResident()' not in update, 'hot instance-cache update still decodes instance backing')
require('takeInstance(' not in update, 'hot instance-cache update still takes/decodes prepared instance pages')
require('attachResidentStore(' in update, 'hot instance-cache update does not use resident-only attachment')

cold_start = cache.find('void Terrain3DInstanceRegionCache::serviceColdMaterialization(')
cold_end = cache.find('\ntypedef struct t3dInstanceCollectDesiredContext_s', cold_start)
cold = cache[cold_start:cold_end]
require(cold_start >= 0, 'cold instance materialization service missing')
require('materializeRegionCold' in cold, 'cold materialization service does not own materialization')

data_cold_start = data.find('void Terrain3DData::serviceColdInstanceResidencyInternal(')
data_cold_end = data.find('\nvoid Terrain3DData::', data_cold_start + 1)
data_cold = data[data_cold_start:data_cold_end if data_cold_end >= 0 else len(data)]
require(data_cold.count('serviceGpuRuntimePagePrepare') == 2, 'cold data residency service must poll runtime pages before and after materialization')
require('m_residency.instanceRegionCache().serviceColdMaterialization' in data_cold, 'cold data residency service does not invoke cache materialization')
require('getResidentInstanceStoreInternal' in region, 'resident-only const instance accessor missing')
require('getResidentInstanceStoreInternalMut' in region, 'resident-only mutable instance accessor missing')

require('serviceColdInstanceResidencyInternal' in data, 'Terrain3DData cold instance residency service missing')
for needle in (
    'getResidentInstanceStoreInternal()',
    'getResidentInstanceStoreInternalMut()',
):
    require(needle in instancer, f'instancer does not use resident-only accessor: {needle}')

if errors:
    for error in errors:
        print('FAIL:', error)
    raise SystemExit(1)
print('PASS: initialization/residency ownership is split into cold materialization and hot resident-only selection.')
