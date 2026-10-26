#ifndef TERRAIN3D_INSTANCE_MUTATION_HELPERS_H
#define TERRAIN3D_INSTANCE_MUTATION_HELPERS_H
// Shared instance-store primitives; no authoring jobs or transaction state.
#include "instances/runtime/terrain_3d_instancer.h"
#include <EASTL/numeric_limits.h>
#include <string.h>
static bool t3dBeginInstancePageAccess(
	Terrain3DInstancer *pInstancer,
	Terrain3DData	   *pData,
	const Vector2i	   &rLocation,
	bool			   &bTemporary ) {
	bTemporary = false;
	if ( !pData ) {
		return false;
	}
	const bool bCacheEnabled = pData->isInstanceRegionCacheEnabled();
	const bool bWasResident = bCacheEnabled && pData->isInstanceRegionResident( rLocation );
	if ( !pData->ensureInstanceRegionResident( rLocation ) ) {
		return false;
	}
	bTemporary = bCacheEnabled && !bWasResident;
	Terrain3DRegion *pRegion = pData->getRegionPtr( rLocation );
	if ( pRegion && pInstancer && !pInstancer->ensureRegionLayout( pRegion ) ) {
		if ( bTemporary ) {
			pData->evictInstanceRegion( rLocation );
			bTemporary = false;
		}
		return false;
	}
	return true;
}

static void t3dEndInstancePageAccess(
	Terrain3DData  *pData,
	const Vector2i &rLocation,
	const bool		bTemporary ) {
	if ( bTemporary && pData ) {
		pData->evictInstanceRegion( rLocation );
	}
}

static bool t3dRetainMutatedInstancePage(
	Terrain3DData	*pData,
	Terrain3DRegion *pRegion,
	bool			&bTemporary ) {
	if ( !bTemporary || !pData || !pRegion || !pRegion->hasInstances() ) {
		return !bTemporary;
	}
	// Empty regions intentionally consume no instance-cache slot. A first
	// placement therefore begins as a temporary page access. Once mutation has
	// populated the store, promote that same resident backing into the cache
	// before the temporary-access cleanup can evict it. The cache's loaded
	// notification schedules the normal render-cell update path.
	if ( !pData->ensureInstanceRegionResident( pRegion->getLocation() ) ) {
		return false;
	}
	bTemporary = false;
	return true;
}

static bool t3dInstancerAppendPendingCellPayload(
	t3dInstanceCell_s				  &rCell,
	const t3dPendingPlacement_t *const pPlacements,
	const size_t					   nCount,
	const bool						   bColorData,
	const bool						   bOwnerData ) {
	if ( !pPlacements || nCount == 0u ) {
		return true;
	}
	const size_t old_transforms = rCell.m_transforms.size();
	const size_t old_colors = rCell.m_colors.size();
	const size_t old_owners = rCell.m_owners.size();
	if ( !rCell.prepareAppend( nCount, bColorData, bOwnerData ) ) {
		return false;
	}
	for ( size_t i = 0u; i < nCount; ++i ) {
		const t3dPendingPlacement_t &placement = pPlacements[i];
		if ( !rCell.m_transforms.pushBack( placement.m_transform, placement.m_nBehaviorFlags ) ||
			 ( !rCell.m_colors.empty() && !rCell.pushColor( placement.m_color ) ) ||
			 ( !rCell.m_owners.empty() && !rCell.m_owners.pushBack( placement.m_nOwner ) ) ) {
			// prepareAppend() preflights all three planes, so this is an invariant
			// failure. Restore logical counts before exposing the cell again.
			rCell.m_transforms.resize( old_transforms );
			rCell.m_colors.resize( old_colors );
			rCell.m_owners.resize( old_owners );
			return false;
		}
	}
	return true;
}

typedef struct t3dPreparedRegionMutation_s
{
	Terrain3DRegion		   *m_pRegion = NULL;
	Terrain3DInstanceStore *m_pCommitStore = NULL;
	Vector2i				m_location = s_v2iMax;
	bool					m_bTemporaryPage = false;
	Terrain3DInstanceStore	m_store;

	t3dPreparedRegionMutation_s( void ) = default;
	t3dPreparedRegionMutation_s( const t3dPreparedRegionMutation_s & ) = delete;
	t3dPreparedRegionMutation_s &operator=( const t3dPreparedRegionMutation_s & ) = delete;
	t3dPreparedRegionMutation_s(
		t3dPreparedRegionMutation_s &&rOther ) noexcept :
		m_pRegion( rOther.m_pRegion ),
		m_pCommitStore( rOther.m_pCommitStore ),
		m_location( rOther.m_location ),
		m_bTemporaryPage( rOther.m_bTemporaryPage ),
		m_store( eastl::move( rOther.m_store ) ) {
		rOther.m_pRegion = NULL;
		rOther.m_pCommitStore = NULL;
		rOther.m_bTemporaryPage = false;
	}
	t3dPreparedRegionMutation_s &operator=(
		t3dPreparedRegionMutation_s &&rOther ) noexcept {
		if ( this != &rOther ) {
			m_pRegion = rOther.m_pRegion;
			m_pCommitStore = rOther.m_pCommitStore;
			m_location = rOther.m_location;
			m_bTemporaryPage = rOther.m_bTemporaryPage;
			m_store = eastl::move( rOther.m_store );
			rOther.m_pRegion = NULL;
			rOther.m_pCommitStore = NULL;
			rOther.m_bTemporaryPage = false;
		}
		return *this;
	}
} t3dPreparedRegionMutation_t;

static bool t3dInstancerStoreCloneMatches(
	const Terrain3DInstanceStore &rSource,
	const Terrain3DInstanceStore &rClone ) {
	if ( !rSource.validateDeep() || !rClone.validateDeep() || rSource.meshCount() != rClone.meshCount() ||
		 rSource.instanceCount() != rClone.instanceCount() ||
		 rSource.getLayout().m_nCellSize != rClone.getLayout().m_nCellSize ) {
		return false;
	}
	for ( size_t iMesh = 0u; iMesh < rSource.meshes().size(); ++iMesh ) {
		const t3dInstanceMesh_t		  &source_mesh = rSource.meshes()[iMesh];
		const t3dInstanceMesh_t *const pCloneMesh = rClone.findMesh( source_mesh.m_nId );
		if ( !pCloneMesh || pCloneMesh->m_cells.size() != source_mesh.m_cells.size() ) {
			return false;
		}
		for ( size_t iCell = 0u; iCell < source_mesh.m_cells.size(); ++iCell ) {
			const t3dInstanceCell_s &source_cell = source_mesh.m_cells[iCell];
			const t3dInstanceCell_s &clone_cell = pCloneMesh->m_cells[iCell];
			if ( source_cell.m_location != clone_cell.m_location || source_cell.m_nRevision != clone_cell.m_nRevision ||
				 source_cell.m_transforms.size() != clone_cell.m_transforms.size() ||
				 source_cell.m_colors.size() != clone_cell.m_colors.size() ||
				 source_cell.m_owners.size() != clone_cell.m_owners.size() ) {
				return false;
			}
			if ( !source_cell.m_transforms.empty() &&
				 memcmp( source_cell.m_transforms.data(),
						 clone_cell.m_transforms.data(),
						 source_cell.m_transforms.size() * sizeof( t3dPackedTransform_s ) ) != 0 ) {
				return false;
			}
			if ( !source_cell.m_colors.empty() && memcmp( source_cell.m_colors.data(),
														  clone_cell.m_colors.data(),
														  source_cell.m_colors.size() * sizeof( uint32_t ) ) != 0 ) {
				return false;
			}
			if ( !source_cell.m_owners.empty() && memcmp( source_cell.m_owners.data(),
														  clone_cell.m_owners.data(),
														  source_cell.m_owners.size() * sizeof( uint32_t ) ) != 0 ) {
				return false;
			}
		}
	}
	return true;
}

static bool t3dInstancerAppendPlacementsToPreparedStore(
	Terrain3DInstanceStore			  &rPrepared,
	const int						   nMeshId,
	const t3dPendingPlacement_t *const pPlacements,
	const size_t					   nPlacementCount ) {
	if ( nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS || !pPlacements || nPlacementCount == 0u ) {
		return false;
	}
	size_t begin = 0u;
	while ( begin < nPlacementCount ) {
		const Vector2i cell_location = pPlacements[begin].m_cell;
		size_t		   end = begin + 1u;
		bool		   bColorData = pPlacements[begin].m_color != s_colorWhite;
		bool		   bOwnerData = pPlacements[begin].m_nOwner != 0u;
		while ( end < nPlacementCount && pPlacements[end].m_cell == cell_location ) {
			bColorData = bColorData || pPlacements[end].m_color != s_colorWhite;
			bOwnerData = bOwnerData || pPlacements[end].m_nOwner != 0u;
			++end;
		}
		t3dInstanceCell_s *const pExisting = rPrepared.findCell( nMeshId, cell_location );
		const size_t			 existing_count = pExisting ? pExisting->m_transforms.size() : 0u;
		if ( end - begin > eastl::numeric_limits< size_t >::max() - existing_count ) {
			return false;
		}
		t3dInstanceCell_s *const pCell = rPrepared.tryGetOrCreateCell( nMeshId,
																	   cell_location,
																	   existing_count + ( end - begin ),
																	   bColorData,
																	   bOwnerData );
		if ( !pCell || !t3dInstancerAppendPendingCellPayload( *pCell,
															  pPlacements + begin,
															  end - begin,
															  bColorData,
															  bOwnerData ) ) {
			return false;
		}
		pCell->touch();
		begin = end;
	}
	return rPrepared.validateDeep();
}

static bool t3dInstancerPrepareStoreAppend(
	const Terrain3DInstanceStore	  &rSource,
	const int						   nMeshId,
	const t3dPendingPlacement_t *const pPlacements,
	const size_t					   nPlacementCount,
	Terrain3DInstanceStore			  &rReplacement ) {
	if ( nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS || !pPlacements || nPlacementCount == 0u ) {
		return false;
	}
	Terrain3DInstanceStore replacement( rSource );
	if ( !t3dInstancerStoreCloneMatches( rSource, replacement ) ||
		 !t3dInstancerAppendPlacementsToPreparedStore( replacement, nMeshId, pPlacements, nPlacementCount ) ) {
		return false;
	}
	rReplacement = eastl::move( replacement );
	return true;
}

static bool t3dInstancerPrepareStoreEraseMesh(
	const Terrain3DInstanceStore &rSource,
	const int					  nMeshId,
	Terrain3DInstanceStore		 &rReplacement ) {
	if ( nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS || !rSource.hasMesh( nMeshId ) ) {
		return false;
	}
	Terrain3DInstanceStore replacement( rSource );
	if ( !t3dInstancerStoreCloneMatches( rSource, replacement ) || !replacement.eraseMesh( nMeshId ) ||
		 !replacement.validateDeep() ) {
		return false;
	}
	rReplacement = eastl::move( replacement );
	return true;
}

static void t3dInstancerRefreshCommittedResidentBytes(
	Terrain3DData *const pData,
	const Vector2i		&rLocation ) {
	if ( !pData || !pData->isInstanceRegionCacheEnabled() || !pData->isInstanceRegionResident( rLocation ) ) {
		return;
	}
	Terrain3DRegion *const pRegion = pData->getRegionPtr( rLocation );
	if ( pRegion && !pRegion->hasInstances() ) {
		// Empty pages have no resident payload worth retaining. Explicit eviction
		// also removes their tracked byte contribution without a world scan.
		pData->evictInstanceRegion( rLocation );
		return;
	}
	// For an already resident slot ensureResident() takes WG-C's O(1)
	// attachResidentStore()->refreshResidentBytes(location) path.
	( void ) pData->ensureInstanceRegionResident( rLocation );
}

static void t3dInstancerClosePreparedPages(
	Terrain3DData *const												   pData,
	eastl::vector< t3dPreparedRegionMutation_t, Terrain3DEastlAllocator > &rPrepared ) {
	for ( size_t i = 0u; i < rPrepared.size(); ++i ) {
		t3dEndInstancePageAccess( pData, rPrepared[i].m_location, rPrepared[i].m_bTemporaryPage );
		rPrepared[i].m_bTemporaryPage = false;
	}
}


#endif
