// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_generation.h"
#include "instances/physics/terrain_3d_instance_occlusion.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "data/model/terrain_3d_region.h"
#include "instances/contracts/terrain_3d_instance_capacity.h"
#include "instances/storage/terrain_3d_instance_store.h"
#include "instances/contracts/terrain_3d_instance_traversal.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/array_occluder3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>

bool Terrain3DInstanceOcclusion::bindRuntimeAllocator(
	Terrain3DMemoryZone *const		   pZone,
	const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone ) {
		return false;
	}
	m_cells.set_allocator( Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy ) );
	m_pRuntimeAllocatorZone = pZone;
	m_eRuntimeAllocatorOverflowPolicy = ePolicy;
	return true;
}

void Terrain3DInstanceOcclusion::advancePublicationGeneration(
	void ) {
	m_nPublicationGeneration = t3dNextGeneration( m_nPublicationGeneration );
}

bool Terrain3DInstanceOcclusion::rebuildCellCapacityCold(
	void ) {
	Terrain3DColdPhaseScope cold_phase_scope;
	if ( !m_pTerrain || !m_pTerrain->dataObject() || m_pTerrain->assetsResource().is_null() ) {
		return false;
	}

	Terrain3DRegion *const *const ppRegions = m_pTerrain->dataObject()->getRegionPtrMapInternal();
	const size_t				  required_cells =
		t3dCountResidentInstanceCells( ppRegions, T3D_REGION_SLOTS, [&]( const t3dInstanceMesh_t &rMesh ) {
			const Ref< Terrain3DMeshAsset > asset = m_pTerrain->assetsResource()->getMeshAsset( rMesh.m_nId );
			return asset.is_valid() && asset->getInstanceCategory() == INSTANCE_CATEGORY_OCCLUDER &&
				   asset->usesInstanceOcclusion();
		} );

	const t3dInstanceCellCapacityPlan_t capacity_plan = t3dPlanInstanceCellCapacity( required_cells, m_cells.size() );
	const size_t						target_capacity = capacity_plan.m_nTarget;
	if ( target_capacity <= m_cells.capacity() ) {
		m_nCellCapacity = m_cells.capacity();
		return true;
	}
	if ( !t3dContainerReserveCold( m_cells, target_capacity ) ) {
		++m_nCapacityMisses;
		return false;
	}
	m_nCellCapacity = m_cells.capacity();
	++m_nColdCapacityRebuilds;
	return true;
}

bool Terrain3DInstanceOcclusion::hasUsableExplicitProxy(
	const Ref< Terrain3DMeshAsset > &rAsset ) const {
	if ( rAsset.is_null() || rAsset->getInstanceOcclusionPolicy() != INSTANCE_OCCLUSION_EXPLICIT_MESH ||
		 !rAsset->hasInstanceOcclusionProxy() ) {
		return false;
	}
	const Ref< ArrayOccluder3D > occluder = rAsset->getInstanceOccluder();
	if ( occluder.is_null() ) {
		return false;
	}
	const PackedVector3Array vertices = occluder->get_vertices();
	const PackedInt32Array	 indices = occluder->get_indices();
	return vertices.size() >= 3 && indices.size() >= 3 && ( indices.size() % 3 ) == 0;
}

bool Terrain3DInstanceOcclusion::buildCellRecord(
	Terrain3DRegion *const			 pRegion,
	const t3dInstanceCell_s			&rCell,
	const Ref< Terrain3DMeshAsset > &rAsset,
	t3dInstanceOcclusionCell_t		&rRecord ) const {
	if ( !m_pTerrain || !pRegion || pRegion->isDeleted() || rAsset.is_null() || !rAsset->usesInstanceOcclusion() ||
		 rCell.m_transforms.empty() ) {
		return false;
	}
	const t3dInstanceCategory_t		   category = rAsset->getInstanceCategory();
	const t3dInstanceOcclusionPolicy_t policy = rAsset->getInstanceOcclusionPolicy();
	if ( category != INSTANCE_CATEGORY_OCCLUDER || policy != INSTANCE_OCCLUSION_EXPLICIT_MESH ||
		 !hasUsableExplicitProxy( rAsset ) ) {
		return false;
	}
	const int cell_size =
		m_pTerrain->instancerObject() ? m_pTerrain->instancerObject()->getCellSize() : T3D_INSTANCE_DEFAULT_CELL_SIZE;
	if ( !t3dInstanceCellWithinRange( pRegion->getLocation(),
									  rCell.m_location,
									  cell_size,
									  rAsset->getInstanceOcclusionRange(),
									  m_lastTargetPosition ) ) {
		return false;
	}

	const Ref< ArrayOccluder3D > occluder = rAsset->getInstanceOccluder();
	const PackedVector3Array	 proxy_vertices = occluder->get_vertices();
	AABB						 local_bounds( proxy_vertices[0], Vector3() );
	for ( int iVertex = 1; iVertex < proxy_vertices.size(); ++iVertex ) {
		local_bounds.expand_to( proxy_vertices[iVertex] );
	}
	if ( local_bounds.size.x <= 0.0f || local_bounds.size.y <= 0.0f || local_bounds.size.z <= 0.0f ) {
		return false;
	}

	rRecord = t3dInstanceOcclusionCell_t();
	rRecord.m_region = pRegion->getLocation();
	rRecord.m_cell = rCell.m_location;
	rRecord.m_nMeshId = rAsset->getId();
	rRecord.m_nSourceRevision = rCell.m_nRevision;
	rRecord.m_nProxyGeneration = rAsset->getInstanceOcclusionProxyGeneration();
	rRecord.m_nInstanceCount =
		( uint32_t ) ( rCell.m_transforms.size() > ( size_t ) ( UINT32_MAX ) ? UINT32_MAX : rCell.m_transforms.size() );
	rRecord.m_nProxyPolicy = ( uint8_t ) ( policy );
	rRecord.m_localProxyBounds = local_bounds;

	const Transform3D terrain_transform = m_pTerrain->sceneNode().get_global_transform();
	const Vector3	  region_offset( ( real_t ) ( rRecord.m_region.x * T3D_REGION_WORLD_SIZE_METERS ),
								 0.0f,
								 ( real_t ) ( rRecord.m_region.y * T3D_REGION_WORLD_SIZE_METERS ) );
	bool			  bounds_initialized = false;
	uint32_t		  mirrored_count = 0u;
	for ( size_t i = 0u; i < rCell.m_transforms.size(); ++i ) {
		Transform3D transform = rCell.getTransform( i );
		transform.origin += region_offset;
		transform = terrain_transform * transform;
		if ( transform.basis.determinant() < 0.0f ) {
			++mirrored_count;
		}
		const AABB transformed = transform.xform( local_bounds );
		if ( !bounds_initialized ) {
			rRecord.m_worldBounds = transformed;
			bounds_initialized = true;
		} else {
			rRecord.m_worldBounds.merge_with( transformed );
		}
	}
	if ( !bounds_initialized ) {
		return false;
	}
	rRecord.m_nMirroredInstanceCount =
		( uint16_t ) ( mirrored_count > ( uint32_t ) ( UINT16_MAX ) ? UINT16_MAX : mirrored_count );
	if ( mirrored_count > 0u ) {
		rRecord.m_nFlags |= T3D_INSTANCE_OCCLUSION_RECORD_MIRRORED;
	}
	return true;
}

bool Terrain3DInstanceOcclusion::getCellRecord(
	const Vector2i			   &rRegion,
	const Vector2i			   &rCell,
	const int					nMeshId,
	t3dInstanceOcclusionCell_t &rRecord ) const {
	const int index = findCellRecord( rRegion, rCell, nMeshId );
	if ( index < 0 ) {
		return false;
	}
	rRecord = m_cells[( size_t ) ( index )];
	return true;
}

int Terrain3DInstanceOcclusion::findCellRecord(
	const Vector2i &rRegion,
	const Vector2i &rCell,
	const int		nMeshId ) const {
	for ( size_t i = 0u; i < m_cells.size(); ++i ) {
		const t3dInstanceOcclusionCell_t &record = m_cells[i];
		if ( record.m_region == rRegion && record.m_cell == rCell && record.m_nMeshId == nMeshId ) {
			return ( int ) ( i );
		}
	}
	return -1;
}

bool Terrain3DInstanceOcclusion::appendCellRecord(
	const t3dInstanceOcclusionCell_t &rRecord ) {
	if ( m_cells.size() >= m_nCellCapacity || !t3dVectorPushBackWithinCapacity( m_cells, rRecord ) ) {
		++m_nCapacityMisses;
		m_bCapacityRebuildPending = true;
		return false;
	}
	return true;
}

void Terrain3DInstanceOcclusion::eraseCellRecordAt(
	const size_t iIndex ) {
	if ( iIndex >= m_cells.size() ) {
		return;
	}
	m_cells[iIndex] = m_cells.back();
	m_cells.pop_back();
}

void Terrain3DInstanceOcclusion::eraseRegionRecords(
	const Vector2i &rRegion ) {
	bool changed = false;
	for ( size_t i = m_cells.size(); i > 0u; --i ) {
		if ( m_cells[i - 1u].m_region == rRegion ) {
			eraseCellRecordAt( i - 1u );
			++m_nRegionUnloadReleases;
			changed = true;
		}
	}
	if ( changed ) {
		++m_nProxyRebuilds;
		advancePublicationGeneration();
		refreshDiagnostics();
	}
}

void Terrain3DInstanceOcclusion::eraseMeshRecords(
	const int nMeshId ) {
	bool changed = false;
	for ( size_t i = m_cells.size(); i > 0u; --i ) {
		if ( m_cells[i - 1u].m_nMeshId == nMeshId ) {
			eraseCellRecordAt( i - 1u );
			changed = true;
		}
	}
	if ( changed ) {
		++m_nProxyRebuilds;
		advancePublicationGeneration();
	}
}

bool Terrain3DInstanceOcclusion::refreshCellRecord(
	const Vector2i &rRegion,
	const Vector2i &rCell,
	const int		nMeshId,
	const uint32_t	nExpectedRevision ) {
	const int existing = findCellRecord( rRegion, rCell, nMeshId );
	if ( !m_pTerrain || !m_pTerrain->dataObject() || m_pTerrain->assetsResource().is_null() || nMeshId < 0 ||
		 nMeshId >= T3D_MAX_MESH_ASSETS ) {
		if ( existing >= 0 ) {
			eraseCellRecordAt( ( size_t ) ( existing ) );
			advancePublicationGeneration();
		}
		return true;
	}
	Terrain3DRegion *const				pRegion = m_pTerrain->dataObject()->getRegionPtr( rRegion );
	const Terrain3DInstanceStore *const pStore =
		pRegion && !pRegion->isDeleted() ? pRegion->getResidentInstanceStoreInternal() : NULL;
	const t3dInstanceCell_s *const	pCell = pStore ? pStore->findCell( nMeshId, rCell ) : NULL;
	const Ref< Terrain3DMeshAsset > asset = m_pTerrain->assetsResource()->getMeshAsset( nMeshId );
	if ( !pCell || asset.is_null() ) {
		if ( existing >= 0 ) {
			eraseCellRecordAt( ( size_t ) ( existing ) );
			++m_nProxyRebuilds;
			advancePublicationGeneration();
			refreshDiagnostics();
		}
		return true;
	}
	if ( nExpectedRevision != 0u && pCell->m_nRevision < nExpectedRevision ) {
		++m_nStaleRevisionErrors;
		return false;
	}

	t3dInstanceOcclusionCell_t record;
	if ( !buildCellRecord( pRegion, *pCell, asset, record ) ) {
		if ( existing >= 0 ) {
			eraseCellRecordAt( ( size_t ) ( existing ) );
			++m_nProxyRebuilds;
			advancePublicationGeneration();
			refreshDiagnostics();
		}
		return true;
	}
	if ( existing >= 0 ) {
		m_cells[( size_t ) ( existing )] = record;
		++m_nProxyRebuilds;
		advancePublicationGeneration();
		refreshDiagnostics();
		return true;
	}
	if ( !appendCellRecord( record ) ) {
		return false;
	}
	++m_nProxyRebuilds;
	advancePublicationGeneration();
	refreshDiagnostics();
	return true;
}

bool Terrain3DInstanceOcclusion::refreshRegionRecords(
	const Vector2i &rRegion,
	const int		nMeshId ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return false;
	}
	Terrain3DRegion *const				pRegion = m_pTerrain->dataObject()->getRegionPtr( rRegion );
	const Terrain3DInstanceStore *const pStore =
		pRegion && !pRegion->isDeleted() ? pRegion->getResidentInstanceStoreInternal() : NULL;
	if ( !pStore ) {
		return true;
	}
	for ( size_t iMesh = 0u; iMesh < pStore->meshes().size(); ++iMesh ) {
		const t3dInstanceMesh_t &mesh = pStore->meshes()[iMesh];
		if ( nMeshId >= 0 && mesh.m_nId != nMeshId ) {
			continue;
		}
		for ( size_t iCell = 0u; iCell < mesh.m_cells.size(); ++iCell ) {
			if ( !refreshCellRecord( rRegion,
									 mesh.m_cells[iCell].m_location,
									 mesh.m_nId,
									 mesh.m_cells[iCell].m_nRevision ) ) {
				return false;
			}
		}
	}
	return true;
}

bool Terrain3DInstanceOcclusion::rebuildResidency(
	const Vector3 &rTarget ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() || m_pTerrain->assetsResource().is_null() ) {
		return true;
	}
	m_cells.clear();
	m_lastTargetPosition = rTarget;
	m_bTargetValid = true;
	Terrain3DRegion *const *const ppRegions = m_pTerrain->dataObject()->getRegionPtrMapInternal();
	const bool					  completed = t3dForEachResidentInstanceMesh(
		   ppRegions,
		   T3D_REGION_SLOTS,
		   [&]( Terrain3DRegion &rRegion, const t3dInstanceMesh_t &rMesh ) {
			   const Ref< Terrain3DMeshAsset > asset = m_pTerrain->assetsResource()->getMeshAsset( rMesh.m_nId );
			   if ( asset.is_null() || asset->getInstanceCategory() != INSTANCE_CATEGORY_OCCLUDER ||
					!asset->usesInstanceOcclusion() ) {
				   return true;
			   }
			   for ( size_t iCell = 0u; iCell < rMesh.m_cells.size(); ++iCell ) {
				   t3dInstanceOcclusionCell_t record;
				   if ( !buildCellRecord( &rRegion, rMesh.m_cells[iCell], asset, record ) ) {
					   continue;
				   }
				   if ( !appendCellRecord( record ) ) {
					   return false;
				   }
			   }
			   return true;
		   } );
	if ( !completed ) {
		refreshDiagnostics();
		return false;
	}
	++m_nProxyRebuilds;
	advancePublicationGeneration();
	refreshDiagnostics();
	return true;
}

void Terrain3DInstanceOcclusion::refreshDiagnostics(
	void ) {
	m_nProxyCells = ( uint64_t ) ( m_cells.size() );
	m_nProxyInstances = 0u;
	m_nProxyVertices = 0u;
	m_nProxyTriangles = 0u;
	m_nCategoryPolicyViolations = 0u;
	m_nMirroredInstances = 0u;
	m_nProxyBytes = 0u;
	if ( m_pTerrain && m_pTerrain->assetsResource().is_valid() ) {
		for ( int mesh_id = 0; mesh_id < m_pTerrain->assetsResource()->getMeshCount(); ++mesh_id ) {
			const Ref< Terrain3DMeshAsset > asset = m_pTerrain->assetsResource()->getMeshAsset( mesh_id );
			if ( asset.is_null() ) {
				continue;
			}
			if ( asset->getInstanceCategory() != INSTANCE_CATEGORY_OCCLUDER &&
				 asset->getInstanceOcclusionPolicy() != INSTANCE_OCCLUSION_NONE ) {
				++m_nCategoryPolicyViolations;
			}
			if ( asset->getInstanceCategory() == INSTANCE_CATEGORY_OCCLUDER && asset->usesInstanceOcclusion() &&
				 asset->hasInstanceOcclusionProxy() ) {
				m_nProxyVertices += asset->getInstanceOcclusionProxyVertexCount();
				m_nProxyTriangles += asset->getInstanceOcclusionProxyTriangleCount();
			}
		}
	}
	m_nProxyBytes = m_nProxyVertices * ( uint64_t ) ( sizeof( float ) * 3u ) +
					m_nProxyTriangles * ( uint64_t ) ( sizeof( uint32_t ) * 3u );
	for ( size_t i = 0u; i < m_cells.size(); ++i ) {
		const t3dInstanceOcclusionCell_t &record = m_cells[i];
		const Ref< Terrain3DMeshAsset >	  asset = m_pTerrain && m_pTerrain->assetsResource().is_valid()
													  ? m_pTerrain->assetsResource()->getMeshAsset( record.m_nMeshId )
													  : Ref< Terrain3DMeshAsset >();
		if ( asset.is_null() || asset->getInstanceCategory() != INSTANCE_CATEGORY_OCCLUDER ||
			 !asset->usesInstanceOcclusion() ||
			 record.m_nProxyGeneration != asset->getInstanceOcclusionProxyGeneration() ) {
			++m_nCategoryPolicyViolations;
		} else {
			m_nProxyInstances += record.m_nInstanceCount;
		}
		m_nMirroredInstances += record.m_nMirroredInstanceCount;
	}
}

void Terrain3DInstanceOcclusion::initialize(
	Terrain3DOwner *const pTerrain ) {
	m_pTerrain = pTerrain;
	if ( !m_pTerrain || !m_pRuntimeAllocatorZone ) {
		return;
	}
	if ( !rebuildCellCapacityCold() ) {
		m_bCapacityRebuildPending = true;
		return;
	}
	rebuildResidency( m_pTerrain->getClipmapTargetPosition() );
}

void Terrain3DInstanceOcclusion::destroy(
	void ) {
	m_cells.clear();
	m_pTerrain = NULL;
	m_bTargetValid = false;
	m_bCapacityRebuildPending = false;
	m_nCellCapacity = 0u;
}

void Terrain3DInstanceOcclusion::serviceColdCapacity(
	void ) {
	if ( !m_bCapacityRebuildPending ) {
		return;
	}
	if ( rebuildCellCapacityCold() ) {
		m_bCapacityRebuildPending = false;
		rebuildResidency( m_bTargetValid ? m_lastTargetPosition : m_pTerrain->getClipmapTargetPosition() );
	}
}

void Terrain3DInstanceOcclusion::updateResidency(
	const Vector3 &rTargetPosition,
	const bool	   bForce ) {
	if ( !m_pTerrain ) {
		return;
	}
	const int cell_size =
		m_pTerrain->instancerObject() ? m_pTerrain->instancerObject()->getCellSize() : T3D_INSTANCE_DEFAULT_CELL_SIZE;
	const real_t step = ( real_t ) ( MAX( cell_size, 1 ) );
	if ( !bForce && m_bTargetValid && m_lastTargetPosition.distance_squared_to( rTargetPosition ) < step * step ) {
		return;
	}
	rebuildResidency( rTargetPosition );
}

void Terrain3DInstanceOcclusion::onMutation(
	const t3dInstanceMutationEvent_t &rEvent ) {
	++m_nMutationEvents;
	if ( ( rEvent.m_nFlags & T3D_INSTANCE_MUTATION_REGION_UNLOAD ) != 0u ) {
		eraseRegionRecords( rEvent.m_region );
		return;
	}
	if ( ( rEvent.m_nFlags & T3D_INSTANCE_MUTATION_POLICY ) != 0u ) {
		onAssetPolicyChanged( rEvent.m_nMeshId );
		return;
	}
	if ( rEvent.m_region.x != INT32_MAX && rEvent.m_cell.x == INT32_MAX ) {
		bool changed = false;
		for ( size_t i = m_cells.size(); i > 0u; --i ) {
			const t3dInstanceOcclusionCell_t &record = m_cells[i - 1u];
			if ( record.m_region == rEvent.m_region &&
				 ( rEvent.m_nMeshId < 0 || record.m_nMeshId == rEvent.m_nMeshId ) ) {
				eraseCellRecordAt( i - 1u );
				changed = true;
			}
		}
		if ( changed ) {
			++m_nProxyRebuilds;
			advancePublicationGeneration();
		}
		refreshRegionRecords( rEvent.m_region, rEvent.m_nMeshId );
		refreshDiagnostics();
		return;
	}
	if ( rEvent.m_nMeshId < 0 || rEvent.m_region.x == INT32_MAX || rEvent.m_cell.x == INT32_MAX ) {
		rebuildResidency( m_bTargetValid ? m_lastTargetPosition
										 : ( m_pTerrain ? m_pTerrain->getClipmapTargetPosition() : Vector3() ) );
		return;
	}
	refreshCellRecord( rEvent.m_region, rEvent.m_cell, rEvent.m_nMeshId, rEvent.m_nSourceRevision );
}

void Terrain3DInstanceOcclusion::onAssetPolicyChanged(
	const int nMeshId ) {
	if ( nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS ) {
		rebuildResidency( m_bTargetValid ? m_lastTargetPosition : Vector3() );
		return;
	}
	eraseMeshRecords( nMeshId );
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return;
	}
	Terrain3DRegion *const *const ppRegions = m_pTerrain->dataObject()->getRegionPtrMapInternal();
	t3dForEachResidentInstanceRegion( ppRegions,
									  T3D_REGION_SLOTS,
									  [&]( Terrain3DRegion &rRegion, const Terrain3DInstanceStore &rStore ) {
										  const t3dInstanceMesh_t *const pMesh = rStore.findMesh( nMeshId );
										  if ( !pMesh ) {
											  return true;
										  }
										  for ( size_t iCell = 0u; iCell < pMesh->m_cells.size(); ++iCell ) {
											  refreshCellRecord( rRegion.getLocation(),
																 pMesh->m_cells[iCell].m_location,
																 nMeshId,
																 pMesh->m_cells[iCell].m_nRevision );
										  }
										  return true;
									  } );
	refreshDiagnostics();
}

Dictionary Terrain3DInstanceOcclusion::getStats(
	void ) const {
	Dictionary stats;
	stats["occlusion_proxy_cells"] = ( int64_t ) ( m_nProxyCells );
	stats["occlusion_proxy_instances"] = ( int64_t ) ( m_nProxyInstances );
	stats["occlusion_proxy_vertices"] = ( int64_t ) ( m_nProxyVertices );
	stats["occlusion_proxy_triangles"] = ( int64_t ) ( m_nProxyTriangles );
	stats["occlusion_category_policy_violations"] = ( int64_t ) ( m_nCategoryPolicyViolations );
	stats["occlusion_proxy_rebuilds"] = ( int64_t ) ( m_nProxyRebuilds );
	stats["occlusion_proxy_bytes"] = ( int64_t ) ( m_nProxyBytes );
	stats["occlusion_record_bytes"] = ( int64_t ) ( m_cells.size() * sizeof( t3dInstanceOcclusionCell_t ) );
	stats["occlusion_capacity_misses"] = ( int64_t ) ( m_nCapacityMisses );
	stats["occlusion_stale_revision_errors"] = ( int64_t ) ( m_nStaleRevisionErrors );
	stats["occlusion_region_unload_releases"] = ( int64_t ) ( m_nRegionUnloadReleases );
	stats["occlusion_mutation_events"] = ( int64_t ) ( m_nMutationEvents );
	stats["occlusion_mirrored_instances"] = ( int64_t ) ( m_nMirroredInstances );
	stats["occlusion_cold_capacity_rebuilds"] = ( int64_t ) ( m_nColdCapacityRebuilds );
	stats["occlusion_capacity_rebuild_pending"] = m_bCapacityRebuildPending;
	stats["occlusion_resident_cells"] = ( int64_t ) ( m_cells.size() );
	stats["occlusion_cell_capacity"] = ( int64_t ) ( m_nCellCapacity );
	stats["occlusion_publication_generation"] = ( int64_t ) ( m_nPublicationGeneration );
	stats["occlusion_owned_rids"] = 0;
	return stats;
}
