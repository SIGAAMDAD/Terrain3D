// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "instances/physics/terrain_3d_instance_physics.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "data/model/terrain_3d_data.h"
#include "data/model/terrain_3d_region.h"
#include "instances/contracts/terrain_3d_instance_capacity.h"
#include "instances/storage/terrain_3d_instance_store.h"
#include "instances/contracts/terrain_3d_instance_traversal.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "physics/runtime/terrain_3d_collision.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/world3d.hpp>

bool Terrain3DInstancePhysics::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone ) {
		return false;
	}
	m_cells.set_allocator( Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy ) );
	m_pRuntimeAllocatorZone = pZone;
	m_eRuntimeAllocatorOverflowPolicy = ePolicy;
	return true;
}



void Terrain3DInstancePhysics::configureBody( t3dInstancePhysicsCell_t &rRecord ) {
	if ( !m_pTerrain || !rRecord.m_hBody.is_valid() || !m_pTerrain->isInsideWorld() || !m_pTerrain->sceneNode().get_world_3d().is_valid() ) {
		return;
	}
	PS->body_set_mode( rRecord.m_hBody, PhysicsServer3D::BODY_MODE_STATIC );
	PS->body_set_space( rRecord.m_hBody, m_pTerrain->sceneNode().get_world_3d()->get_space() );
	PS->body_attach_object_instance_id( rRecord.m_hBody, m_pTerrain->sceneNode().get_instance_id() );
	PS->body_set_collision_layer( rRecord.m_hBody, m_pTerrain->getCollisionLayer() );
	PS->body_set_collision_mask( rRecord.m_hBody, m_pTerrain->getCollisionMask() );
	PS->body_set_collision_priority( rRecord.m_hBody, m_pTerrain->getCollisionPriority() );
}

bool Terrain3DInstancePhysics::rebuildCellCapacityCold( void ) {
	Terrain3DColdPhaseScope cold_phase_scope;
	if ( !m_pTerrain || !m_pTerrain->dataObject() || m_pTerrain->assetsResource().is_null() ) {
		return false;
	}

	size_t required_cells = 0u;
	Terrain3DRegion *const *const ppRegions = m_pTerrain->dataObject()->getRegionPtrMapInternal();
	t3dForEachResidentInstanceMesh( ppRegions, T3D_REGION_SLOTS, [&]( Terrain3DRegion &, const t3dInstanceMesh_t &rMesh ) {
		const Ref<Terrain3DMeshAsset> asset = m_pTerrain->assetsResource()->getMeshAsset( rMesh.m_nId );
		if ( asset.is_null() || !asset->requiresInstancePhysics() ) {
			return true;
		}
		if ( !t3dAccumulateInstanceCellCount( rMesh.m_cells.size(), required_cells ) ) {
			return false;
		}
		return true;
	} );

	const t3dInstanceCellCapacityPlan_t capacity_plan = t3dPlanInstanceCellCapacity( required_cells, m_cells.size() );
	const size_t target_capacity = capacity_plan.m_nTarget;
	if ( target_capacity > m_cells.capacity() && !t3dContainerReserveCold( m_cells, target_capacity ) ) {
		++m_nCapacityMisses;
		return false;
	}
	while ( m_cells.size() < target_capacity ) {
		t3dInstancePhysicsCell_t record;
		record.m_hBody = PS->body_create();
		if ( !record.m_hBody.is_valid() ) {
			++m_nCapacityMisses;
			return false;
		}
		if ( !t3dVectorPushBackWithinCapacity( m_cells, record ) ) {
			PS->free_rid( record.m_hBody );
			++m_nCapacityMisses;
			return false;
		}
		configureBody( m_cells.back() );
	}
	m_nCellCapacity = m_cells.size();
	++m_nColdCapacityRebuilds;
	return true;
}

int Terrain3DInstancePhysics::findCellRecord( const Vector2i &rRegion, const Vector2i &rCell, const int nMeshId ) const {
	for ( size_t i = 0u; i < m_cells.size(); ++i ) {
		const t3dInstancePhysicsCell_t &record = m_cells[i];
		if ( record.m_bActive && record.m_region == rRegion && record.m_cell == rCell && record.m_nMeshId == nMeshId ) {
			return ( int )( i );
		}
	}
	return -1;
}

int Terrain3DInstancePhysics::findFreeCellRecord( void ) const {
	for ( size_t i = 0u; i < m_cells.size(); ++i ) {
		if ( !m_cells[i].m_bActive ) {
			return ( int )( i );
		}
	}
	return -1;
}

void Terrain3DInstancePhysics::deactivateCellRecord( t3dInstancePhysicsCell_t &rRecord ) {
	if ( rRecord.m_hBody.is_valid() ) {
		PS->body_clear_shapes( rRecord.m_hBody );
	}
	rRecord.m_region = Vector2i( INT32_MAX, INT32_MAX );
	rRecord.m_cell = Vector2i( INT32_MAX, INT32_MAX );
	rRecord.m_nMeshId = -1;
	rRecord.m_nSourceRevision = 0u;
	rRecord.m_nCollisionGeneration = 0u;
	rRecord.m_nShapeCount = 0u;
	rRecord.m_nMirroredShapeCount = 0u;
	rRecord.m_bActive = false;
	rRecord.m_bDesired = false;
}

bool Terrain3DInstancePhysics::rebuildCellRecord( t3dInstancePhysicsCell_t &rRecord,
        Terrain3DRegion *const pRegion,
        const t3dInstanceCell_s &rCell,
        const Ref<Terrain3DMeshAsset> &rAsset ) {
	if ( !m_pTerrain || !pRegion || pRegion->isDeleted() || rAsset.is_null() || !rAsset->requiresInstancePhysics()
	        || !rAsset->hasInstanceCollisionProxy() || rCell.m_transforms.empty() ) {
		return false;
	}
	const Ref<ConcavePolygonShape3D> shape = rAsset->getInstanceCollisionShape();
	if ( shape.is_null() || !shape->get_rid().is_valid() || !rRecord.m_hBody.is_valid() ) {
		return false;
	}

	configureBody( rRecord );
	PS->body_clear_shapes( rRecord.m_hBody );
	const Transform3D terrain_transform = m_pTerrain->sceneNode().get_global_transform();
	const Vector3 region_offset( ( real_t )( pRegion->getLocation().x * T3D_REGION_WORLD_SIZE_METERS ),
	        0.0f,
	        ( real_t )( pRegion->getLocation().y * T3D_REGION_WORLD_SIZE_METERS ) );
	uint32_t shape_count = 0u;
	uint32_t mirrored_shape_count = 0u;
	for ( size_t i = 0u; i < rCell.m_transforms.size(); ++i ) {
		Transform3D transform = rCell.getTransform( i );
		transform.origin += region_offset;
		transform = terrain_transform * transform;
		if ( transform.basis.determinant() < 0.0f ) {
			++mirrored_shape_count;
		}
		PS->body_add_shape( rRecord.m_hBody, shape->get_rid(), transform, false );
		++shape_count;
	}

	rRecord.m_region = pRegion->getLocation();
	rRecord.m_cell = rCell.m_location;
	rRecord.m_nMeshId = rAsset->getId();
	rRecord.m_nSourceRevision = rCell.m_nRevision;
	rRecord.m_nCollisionGeneration = rAsset->getInstanceCollisionGeneration();
	rRecord.m_nShapeCount = shape_count;
	rRecord.m_nMirroredShapeCount = mirrored_shape_count;
	rRecord.m_bActive = shape_count > 0u;
	rRecord.m_bDesired = rRecord.m_bActive;
	++m_nCellRebuilds;
	return rRecord.m_bActive;
}

bool Terrain3DInstancePhysics::reconcileResidency( const Vector3 &rTarget ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() || m_pTerrain->assetsResource().is_null() ) {
		return true;
	}
	for ( size_t i = 0u; i < m_cells.size(); ++i ) {
		m_cells[i].m_bDesired = false;
	}
	m_nMissingCollisionSkips = 0u;

	const bool collision_enabled = m_pTerrain->getCollisionMode() != COLLISION_DISABLED;
	if ( collision_enabled ) {
		const int cell_size = m_pTerrain->instancerObject() ? m_pTerrain->instancerObject()->getCellSize() : T3D_INSTANCE_DEFAULT_CELL_SIZE;
		const real_t range = ( real_t )( MAX( ( int )( m_pTerrain->getCollisionRadius() ), cell_size ) );
		Terrain3DRegion *const *const ppRegions = m_pTerrain->dataObject()->getRegionPtrMapInternal();
		t3dForEachResidentInstanceMesh( ppRegions, T3D_REGION_SLOTS, [&]( Terrain3DRegion &rRegion, const t3dInstanceMesh_t &rMesh ) {
			const Ref<Terrain3DMeshAsset> asset = m_pTerrain->assetsResource()->getMeshAsset( rMesh.m_nId );
			if ( asset.is_null() || !asset->requiresInstancePhysics() ) {
				return true;
			}
			if ( !asset->hasInstanceCollisionProxy() ) {
				++m_nMissingCollisionSkips;
				return true;
			}
			for ( size_t iCell = 0u; iCell < rMesh.m_cells.size(); ++iCell ) {
				const t3dInstanceCell_s &cell = rMesh.m_cells[iCell];
				if ( !t3dInstanceCellWithinRange( rRegion.getLocation(), cell.m_location, cell_size, range, rTarget ) ) {
					continue;
				}
				int record_index = findCellRecord( rRegion.getLocation(), cell.m_location, rMesh.m_nId );
				if ( record_index >= 0 ) {
					t3dInstancePhysicsCell_t &record = m_cells[( size_t )( record_index )];
					record.m_bDesired = true;
					if ( record.m_nSourceRevision == cell.m_nRevision
					        && record.m_nCollisionGeneration == asset->getInstanceCollisionGeneration() ) {
						configureBody( record );
						++m_nCellsReused;
						continue;
					}
				} else {
					record_index = findFreeCellRecord();
				}
				if ( record_index < 0 ) {
					++m_nCapacityMisses;
					m_bCapacityRebuildPending = true;
					continue;
				}
				if ( !rebuildCellRecord( m_cells[( size_t )( record_index )], &rRegion, cell, asset ) ) {
					deactivateCellRecord( m_cells[( size_t )( record_index )] );
				}
			}
			return true;
		} );
	}

	for ( size_t i = 0u; i < m_cells.size(); ++i ) {
		if ( m_cells[i].m_bActive && !m_cells[i].m_bDesired ) {
			deactivateCellRecord( m_cells[i] );
		}
	}
	m_lastTargetPosition = rTarget;
	m_bTargetValid = true;
	m_bResidencyDirty = false;
	m_lastTerrainTransform = m_pTerrain->sceneNode().get_global_transform();
	m_nLastCollisionLayer = m_pTerrain->getCollisionLayer();
	m_nLastCollisionMask = m_pTerrain->getCollisionMask();
	m_nLastCollisionRadius = m_pTerrain->getCollisionRadius();
	m_nLastCollisionMode = ( int32_t )( m_pTerrain->getCollisionMode() );
	m_fLastCollisionPriority = m_pTerrain->getCollisionPriority();
	refreshDiagnostics();
	return !m_bCapacityRebuildPending;
}

void Terrain3DInstancePhysics::refreshDiagnostics( void ) {
	m_nActiveCells = 0u;
	m_nActiveShapes = 0u;
	m_nMirroredShapes = 0u;
	for ( size_t i = 0u; i < m_cells.size(); ++i ) {
		if ( !m_cells[i].m_bActive ) {
			continue;
		}
		++m_nActiveCells;
		m_nActiveShapes += m_cells[i].m_nShapeCount;
		m_nMirroredShapes += m_cells[i].m_nMirroredShapeCount;
	}
}

void Terrain3DInstancePhysics::initialize( Terrain3DOwner *const pTerrain ) {
	m_pTerrain = pTerrain;
	m_bResidencyDirty = true;
	if ( !m_pTerrain || !m_pRuntimeAllocatorZone ) {
		return;
	}
	if ( !rebuildCellCapacityCold() ) {
		m_bCapacityRebuildPending = true;
		return;
	}
	if ( m_pTerrain->isInsideWorld() && m_pTerrain->sceneNode().get_world_3d().is_valid() ) {
		reconcileResidency( m_pTerrain->getClipmapTargetPosition() );
	}
}

void Terrain3DInstancePhysics::destroy( void ) {
	for ( size_t i = 0u; i < m_cells.size(); ++i ) {
		if ( m_cells[i].m_hBody.is_valid() ) {
			PS->body_clear_shapes( m_cells[i].m_hBody );
			PS->free_rid( m_cells[i].m_hBody );
			m_cells[i].m_hBody = RID();
		}
	}
	m_cells.clear();
	m_pTerrain = NULL;
	m_bTargetValid = false;
	m_bResidencyDirty = true;
	m_bCapacityRebuildPending = false;
	m_nLastCollisionLayer = 0u;
	m_nLastCollisionMask = 0u;
	m_nLastCollisionRadius = 0u;
	m_nLastCollisionMode = -1;
	m_fLastCollisionPriority = -1.0f;
	m_nCellCapacity = 0u;
	refreshDiagnostics();
}

void Terrain3DInstancePhysics::serviceColdCapacity( void ) {
	if ( !m_bCapacityRebuildPending ) {
		return;
	}
	if ( rebuildCellCapacityCold() ) {
		m_bCapacityRebuildPending = false;
		m_bResidencyDirty = true;
	}
}

void Terrain3DInstancePhysics::updateResidency( const Vector3 &rTargetPosition, const bool bForce ) {
	if ( !m_pTerrain || !m_pTerrain->isInsideWorld() || m_pTerrain->sceneNode().get_world_3d().is_null() ) {
		m_bResidencyDirty = true;
		return;
	}
	const int cell_size = m_pTerrain->instancerObject() ? m_pTerrain->instancerObject()->getCellSize() : T3D_INSTANCE_DEFAULT_CELL_SIZE;
	const bool target_moved = !m_bTargetValid || Math::abs( rTargetPosition.x - m_lastTargetPosition.x ) >= ( real_t )( cell_size )
	        || Math::abs( rTargetPosition.z - m_lastTargetPosition.z ) >= ( real_t )( cell_size );
	const bool terrain_transform_changed = !m_bTargetValid || m_pTerrain->sceneNode().get_global_transform() != m_lastTerrainTransform;
	const bool collision_configuration_changed = !m_bTargetValid || m_nLastCollisionLayer != m_pTerrain->getCollisionLayer()
	        || m_nLastCollisionMask != m_pTerrain->getCollisionMask() || m_nLastCollisionRadius != m_pTerrain->getCollisionRadius()
	        || m_nLastCollisionMode != ( int32_t )( m_pTerrain->getCollisionMode() )
	        || !Math::is_equal_approx( m_fLastCollisionPriority, m_pTerrain->getCollisionPriority() );
	if ( terrain_transform_changed || collision_configuration_changed ) {
		m_bResidencyDirty = true;
		for ( size_t i = 0u; i < m_cells.size(); ++i ) {
			m_cells[i].m_nSourceRevision = 0u;
		}
	}
	if ( !bForce && !target_moved && !m_bResidencyDirty ) {
		return;
	}
	reconcileResidency( rTargetPosition );
}

void Terrain3DInstancePhysics::onMutation( const t3dInstanceMutationEvent_t &rEvent ) {
	++m_nMutationEvents;
	m_bResidencyDirty = true;
	if ( ( rEvent.m_nFlags & T3D_INSTANCE_MUTATION_BULK ) != 0u || ( rEvent.m_nFlags & T3D_INSTANCE_MUTATION_POLICY ) != 0u ) {
		m_bCapacityRebuildPending = true;
	}
}

Dictionary Terrain3DInstancePhysics::getStats( void ) const {
	Dictionary stats;
	stats["instance_physics_active_cells"] = ( int64_t )( m_nActiveCells );
	stats["instance_physics_active_shapes"] = ( int64_t )( m_nActiveShapes );
	stats["instance_physics_cell_capacity"] = ( int64_t )( m_nCellCapacity );
	stats["instance_physics_cell_rebuilds"] = ( int64_t )( m_nCellRebuilds );
	stats["instance_physics_cells_reused"] = ( int64_t )( m_nCellsReused );
	stats["instance_physics_capacity_misses"] = ( int64_t )( m_nCapacityMisses );
	stats["instance_physics_cold_capacity_rebuilds"] = ( int64_t )( m_nColdCapacityRebuilds );
	stats["instance_physics_mutation_events"] = ( int64_t )( m_nMutationEvents );
	stats["instance_physics_missing_collision_skips"] = ( int64_t )( m_nMissingCollisionSkips );
	stats["instance_physics_mirrored_shapes"] = ( int64_t )( m_nMirroredShapes );
	stats["instance_physics_capacity_rebuild_pending"] = m_bCapacityRebuildPending;
	return stats;
}
