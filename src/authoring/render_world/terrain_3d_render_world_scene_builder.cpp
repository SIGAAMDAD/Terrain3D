// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/render_world/terrain_3d_render_world_scene_builder.h"

#include <math.h>
#include <string.h>

static uint32_t t3d_rw_cell_hash( int32_t pX, int32_t pZ ) {
	uint32_t x = ( uint32_t )pX;
	uint32_t z = ( uint32_t )pZ;
	x *= 0x8da6b343u;
	z *= 0xd8163841u;
	return x ^ z ^ ( x >> 16u ) ^ ( z >> 13u );
}

static float t3d_rw_cell_row_length( const float pRow[3] ) {
	return sqrtf( pRow[0] * pRow[0] + pRow[1] * pRow[1] + pRow[2] * pRow[2] );
}

static void t3d_rw_cell_transform_sphere(
        const T3DInstanceRecord &pInstance, const T3DRenderWorldPrototypeBounds &pLocal, float rCenter[3], float &rRadius ) {
	rCenter[0] = pInstance.mTransform3x4[0] * pLocal.vCenter[0] + pInstance.mTransform3x4[1] * pLocal.vCenter[1]
	        + pInstance.mTransform3x4[2] * pLocal.vCenter[2] + pInstance.mTransform3x4[3];
	rCenter[1] = pInstance.mTransform3x4[4] * pLocal.vCenter[0] + pInstance.mTransform3x4[5] * pLocal.vCenter[1]
	        + pInstance.mTransform3x4[6] * pLocal.vCenter[2] + pInstance.mTransform3x4[7];
	rCenter[2] = pInstance.mTransform3x4[8] * pLocal.vCenter[0] + pInstance.mTransform3x4[9] * pLocal.vCenter[1]
	        + pInstance.mTransform3x4[10] * pLocal.vCenter[2] + pInstance.mTransform3x4[11];
	const float aRow0[3] = { pInstance.mTransform3x4[0], pInstance.mTransform3x4[1], pInstance.mTransform3x4[2] };
	const float aRow1[3] = { pInstance.mTransform3x4[4], pInstance.mTransform3x4[5], pInstance.mTransform3x4[6] };
	const float aRow2[3] = { pInstance.mTransform3x4[8], pInstance.mTransform3x4[9], pInstance.mTransform3x4[10] };
	float fScale = t3d_rw_cell_row_length( aRow0 );
	const float fScale1 = t3d_rw_cell_row_length( aRow1 );
	const float fScale2 = t3d_rw_cell_row_length( aRow2 );
	if ( fScale1 > fScale ) {
		fScale = fScale1;
	}
	if ( fScale2 > fScale ) {
		fScale = fScale2;
	}
	rRadius = pLocal.fRadius * fScale;
}

static T3DRenderWorldCellHashEntry *t3d_rw_cell_find_or_insert(
        T3DRenderWorldCellBuildScratch &rScratch, int32_t pCellX, int32_t pCellZ, uint32_t &rCellCount, uint32_t pCellCapacity ) {
	if ( rScratch.nCellHashCapacity == 0u || ( rScratch.nCellHashCapacity & ( rScratch.nCellHashCapacity - 1u ) ) != 0u ) {
		return NULL;
	}
	const uint32_t nMask = rScratch.nCellHashCapacity - 1u;
	uint32_t nSlot = t3d_rw_cell_hash( pCellX, pCellZ ) & nMask;
	for ( uint32_t i = 0u; i < rScratch.nCellHashCapacity; ++i ) {
		T3DRenderWorldCellHashEntry &Entry = rScratch.pCellHash[nSlot];
		if ( !Entry.bOccupied ) {
			if ( rCellCount >= pCellCapacity || rCellCount >= rScratch.nCellScratchCapacity ) {
				return NULL;
			}
			memset( &Entry, 0, sizeof( Entry ) );
			Entry.bOccupied = true;
			Entry.iCellX = pCellX;
			Entry.iCellZ = pCellZ;
			Entry.nCellIndex = rCellCount++;
			return &Entry;
		}
		if ( Entry.iCellX == pCellX && Entry.iCellZ == pCellZ ) {
			return &Entry;
		}
		nSlot = ( nSlot + 1u ) & nMask;
	}
	return NULL;
}

bool t3d_render_world_build_scene_cells_cold( T3DInstanceRecord *pInstances,
        uint64_t *pInstanceSourceIds,
        uint32_t pInstanceCount,
        const T3DRenderWorldPrototypeBounds *pPrototypeBounds,
        uint32_t pPrototypeBoundsCount,
        float pCellSize,
        T3DSceneCell *pCells,
        uint32_t pCellCapacity,
        T3DRenderWorldCellBuildScratch &rScratch,
        T3DRenderWorldOwnershipRegistry &rOwnership,
        uint32_t &rCellCount ) {
	rCellCount = 0u;
	if ( pInstances == NULL || pInstanceSourceIds == NULL || pPrototypeBounds == NULL || pCells == NULL
	        || rScratch.pReorderedInstances == NULL || rScratch.pReorderedSourceIds == NULL || rScratch.pInstanceCellIds == NULL
	        || rScratch.pCellCounts == NULL || rScratch.pCellWriteCursor == NULL || rScratch.pCellHash == NULL || pCellSize <= 0.0f
	        || pInstanceCount > rScratch.nInstanceCapacity ) {
		return false;
	}
	if ( pInstanceCount == 0u ) {
		return true;
	}

	for ( uint32_t i = 0u; i < rScratch.nCellHashCapacity; ++i ) {
		memset( &rScratch.pCellHash[i], 0, sizeof( T3DRenderWorldCellHashEntry ) );
	}
	for ( uint32_t i = 0u; i < rScratch.nCellScratchCapacity; ++i ) {
		rScratch.pCellCounts[i] = 0u;
		rScratch.pCellWriteCursor[i] = 0u;
	}

	for ( uint32_t i = 0u; i < pInstanceCount; ++i ) {
		const T3DInstanceRecord &Instance = pInstances[i];
		if ( Instance.nMeshId >= pPrototypeBoundsCount ) {
			return false;
		}
		float vCenter[3];
		float fRadius = 0.0f;
		t3d_rw_cell_transform_sphere( Instance, pPrototypeBounds[Instance.nMeshId], vCenter, fRadius );
		const int32_t iCellX = ( int32_t )floorf( vCenter[0] / pCellSize );
		const int32_t iCellZ = ( int32_t )floorf( vCenter[2] / pCellSize );
		T3DRenderWorldCellHashEntry *pEntry = t3d_rw_cell_find_or_insert( rScratch, iCellX, iCellZ, rCellCount, pCellCapacity );
		if ( pEntry == NULL ) {
			return false;
		}
		const uint32_t nCell = pEntry->nCellIndex;
		rScratch.pInstanceCellIds[i] = nCell;
		++rScratch.pCellCounts[nCell];
		if ( pEntry->nInstanceCount == 0u ) {
			for ( uint32_t axis = 0u; axis < 3u; ++axis ) {
				pEntry->vMin[axis] = vCenter[axis] - fRadius;
				pEntry->vMax[axis] = vCenter[axis] + fRadius;
			}
		} else {
			for ( uint32_t axis = 0u; axis < 3u; ++axis ) {
				const float fMin = vCenter[axis] - fRadius;
				const float fMax = vCenter[axis] + fRadius;
				if ( fMin < pEntry->vMin[axis] ) {
					pEntry->vMin[axis] = fMin;
				}
				if ( fMax > pEntry->vMax[axis] ) {
					pEntry->vMax[axis] = fMax;
				}
			}
		}
		++pEntry->nInstanceCount;
	}

	uint32_t nPrefix = 0u;
	for ( uint32_t c = 0u; c < rCellCount; ++c ) {
		pCells[c].nInstanceOffset = nPrefix;
		pCells[c].nInstanceCount = rScratch.pCellCounts[c];
		pCells[c].nFlags = 0u;
		pCells[c].nGeneration = rOwnership.nGeneration;
		rScratch.pCellWriteCursor[c] = nPrefix;
		nPrefix += rScratch.pCellCounts[c];
	}
	if ( nPrefix != pInstanceCount ) {
		return false;
	}

	for ( uint32_t h = 0u; h < rScratch.nCellHashCapacity; ++h ) {
		const T3DRenderWorldCellHashEntry &Entry = rScratch.pCellHash[h];
		if ( !Entry.bOccupied ) {
			continue;
		}
		T3DSceneCell &Cell = pCells[Entry.nCellIndex];
		float fRadiusSq = 0.0f;
		for ( uint32_t axis = 0u; axis < 3u; ++axis ) {
			Cell.vCenter[axis] = ( Entry.vMin[axis] + Entry.vMax[axis] ) * 0.5f;
			const float fHalf = ( Entry.vMax[axis] - Entry.vMin[axis] ) * 0.5f;
			fRadiusSq += fHalf * fHalf;
		}
		Cell.fRadius = sqrtf( fRadiusSq );
	}

	for ( uint32_t i = 0u; i < pInstanceCount; ++i ) {
		const uint32_t nCell = rScratch.pInstanceCellIds[i];
		const uint32_t nWrite = rScratch.pCellWriteCursor[nCell]++;
		rScratch.pReorderedInstances[nWrite] = pInstances[i];
		rScratch.pReorderedInstances[nWrite].nCellId = nCell;
		rScratch.pReorderedSourceIds[nWrite] = pInstanceSourceIds[i];
	}
	memcpy( pInstances, rScratch.pReorderedInstances, ( size_t )pInstanceCount * sizeof( T3DInstanceRecord ) );
	memcpy( pInstanceSourceIds, rScratch.pReorderedSourceIds, ( size_t )pInstanceCount * sizeof( uint64_t ) );

	for ( uint32_t i = 0u; i < pInstanceCount; ++i ) {
		const T3DRenderWorldOwnershipEntry *pOld =
		        t3d_render_world_ownership_registry_find_reference_debug( rOwnership, pInstanceSourceIds[i] );
		if ( pOld == NULL || pOld->eOwnership != T3D_RW_OWNERSHIP_RENDERWORLD ) {
			continue;
		}
		T3DRenderWorldOwnershipEntry Updated = *pOld;
		Updated.nInstanceId = i;
		if ( !t3d_render_world_ownership_registry_publish_cold( rOwnership, Updated ) ) {
			return false;
		}
	}
	return true;
}
