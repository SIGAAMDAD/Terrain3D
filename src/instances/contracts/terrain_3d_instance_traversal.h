// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_TRAVERSAL_H
#define TERRAIN3D_INSTANCE_TRAVERSAL_H
#include "core/utility/terrain_3d_generation.h"

#include "Terrain3DConstants.h"
#include "data/model/terrain_3d_region.h"
#include "instances/storage/terrain_3d_instance_store.h"

EA_FORCE_INLINE bool t3dInstanceCellWithinRange(
	const Vector2i &rRegion,
	const Vector2i &rCell,
	const int		nCellSize,
	const real_t	fRange,
	const Vector3  &rTarget ) {
	if ( fRange <= 0.0f ) {
		return false;
	}
	const real_t region_x = ( real_t ) ( rRegion.x * T3D_REGION_WORLD_SIZE_METERS );
	const real_t region_z = ( real_t ) ( rRegion.y * T3D_REGION_WORLD_SIZE_METERS );
	const real_t min_x = region_x + ( real_t ) ( rCell.x * nCellSize );
	const real_t min_z = region_z + ( real_t ) ( rCell.y * nCellSize );
	const real_t max_x = min_x + ( real_t ) ( nCellSize );
	const real_t max_z = min_z + ( real_t ) ( nCellSize );
	const real_t dx = rTarget.x < min_x ? min_x - rTarget.x : ( rTarget.x > max_x ? rTarget.x - max_x : 0.0f );
	const real_t dz = rTarget.z < min_z ? min_z - rTarget.z : ( rTarget.z > max_z ? rTarget.z - max_z : 0.0f );
	return dx * dx + dz * dz <= fRange * fRange;
}

template< typename t3dRegionVisitor_t >
EA_FORCE_INLINE bool t3dForEachResidentInstanceRegion(
	Terrain3DRegion *const *ppRegions,
	const int				nRegionCount,
	t3dRegionVisitor_t	  &&rVisitor ) {
	if ( !ppRegions || nRegionCount <= 0 ) {
		return true;
	}
	for ( int iRegion = 0; iRegion < nRegionCount; ++iRegion ) {
		Terrain3DRegion *const pRegion = ppRegions[iRegion];
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const Terrain3DInstanceStore *const pStore = pRegion->getResidentInstanceStoreInternal();
		if ( !pStore ) {
			continue;
		}
		if ( !rVisitor( *pRegion, *pStore ) ) {
			return false;
		}
	}
	return true;
}

template< typename t3dMeshVisitor_t >
EA_FORCE_INLINE bool t3dForEachResidentInstanceMesh(
	Terrain3DRegion *const *ppRegions,
	const int				nRegionCount,
	t3dMeshVisitor_t	  &&rVisitor ) {
	return t3dForEachResidentInstanceRegion( ppRegions,
											 nRegionCount,
											 [&]( Terrain3DRegion &rRegion, const Terrain3DInstanceStore &rStore ) {
												 for ( size_t iMesh = 0u; iMesh < rStore.meshes().size(); ++iMesh ) {
													 if ( !rVisitor( rRegion, rStore.meshes()[iMesh] ) ) {
														 return false;
													 }
												 }
												 return true;
											 } );
}

EA_FORCE_INLINE bool t3dInstanceCellBoundsCurrent(
	const t3dInstanceCell_s &rCell,
	const AABB				&rSourceMeshAabb ) {
	return rCell.m_bCachedAabbValid && rCell.m_nCachedAabbRevision == rCell.m_nRevision &&
		   rCell.m_cachedSourceMeshAabb.is_equal_approx( rSourceMeshAabb );
}

EA_FORCE_INLINE bool t3dRefreshInstanceCellBounds(
	t3dInstanceCell_s &rCell,
	const AABB		  &rSourceMeshAabb ) {
	bool initialized = false;
	AABB bounds;
	if ( rSourceMeshAabb.has_surface() ) {
		for ( size_t i = 0u; i < rCell.m_transforms.size(); ++i ) {
			const AABB transformed = rCell.getTransform( i ).xform( rSourceMeshAabb );
			if ( !initialized ) {
				bounds = transformed;
				initialized = true;
			} else {
				bounds.merge_with( transformed );
			}
		}
	}
	rCell.m_cachedAabb = initialized ? bounds : AABB();
	rCell.m_cachedSourceMeshAabb = rSourceMeshAabb;
	rCell.m_nCachedAabbRevision = rCell.m_nRevision;
	rCell.m_nCachedAabbGeneration = t3dNextGeneration( rCell.m_nCachedAabbGeneration );
	rCell.m_bCachedAabbValid = true;
	return initialized;
}

#endif // TERRAIN3D_INSTANCE_TRAVERSAL_H
