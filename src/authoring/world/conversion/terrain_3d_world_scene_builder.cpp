// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/world/conversion/terrain_3d_world_scene_builder.h"

#include <math.h>
#include <string.h>

static uint32_t t3d_world_cell_hash(
	int32_t pX,
	int32_t pZ ) {
	uint32_t x = ( uint32_t ) pX;
	uint32_t z = ( uint32_t ) pZ;
	x *= 0x8da6b343u;
	z *= 0xd8163841u;
	return x ^ z ^ ( x >> 16u ) ^ ( z >> 13u );
}

static float t3d_world_cell_row_length(
	const float pRow[3] ) {
	return sqrtf( pRow[0] * pRow[0] + pRow[1] * pRow[1] + pRow[2] * pRow[2] );
}

static void t3d_world_cell_transform_sphere(
	const t3dWorldTransform_t		&rTransform,
	const t3dWorldPrototypeBounds_t &pLocal,
	float							 rCenter[3],
	float							&rRadius ) {
	const float *const pRow0 = rTransform.m_fRow0;
	const float *const pRow1 = rTransform.m_fRow1;
	const float *const pRow2 = rTransform.m_fRow2;
	rCenter[0] = pRow0[0] * pLocal.vCenter[0] + pRow0[1] * pLocal.vCenter[1] + pRow0[2] * pLocal.vCenter[2] + pRow0[3];
	rCenter[1] = pRow1[0] * pLocal.vCenter[0] + pRow1[1] * pLocal.vCenter[1] + pRow1[2] * pLocal.vCenter[2] + pRow1[3];
	rCenter[2] = pRow2[0] * pLocal.vCenter[0] + pRow2[1] * pLocal.vCenter[1] + pRow2[2] * pLocal.vCenter[2] + pRow2[3];
	const float aRow0[3] = { pRow0[0], pRow0[1], pRow0[2] };
	const float aRow1[3] = { pRow1[0], pRow1[1], pRow1[2] };
	const float aRow2[3] = { pRow2[0], pRow2[1], pRow2[2] };
	float		fScale = t3d_world_cell_row_length( aRow0 );
	const float fScale1 = t3d_world_cell_row_length( aRow1 );
	const float fScale2 = t3d_world_cell_row_length( aRow2 );
	if ( fScale1 > fScale ) {
		fScale = fScale1;
	}
	if ( fScale2 > fScale ) {
		fScale = fScale2;
	}
	rRadius = pLocal.fRadius * fScale;
}

static t3dWorldCellHashEntry_t *t3d_world_cell_find_or_insert(
	t3dWorldCellBuildScratch_t &rScratch,
	int32_t						pCellX,
	int32_t						pCellZ,
	uint32_t				   &rCellCount,
	uint32_t					pCellCapacity ) {
	if ( rScratch.nCellHashCapacity == 0u ||
		 ( rScratch.nCellHashCapacity & ( rScratch.nCellHashCapacity - 1u ) ) != 0u ) {
		return NULL;
	}
	const uint32_t nMask = rScratch.nCellHashCapacity - 1u;
	uint32_t	   nSlot = t3d_world_cell_hash( pCellX, pCellZ ) & nMask;
	for ( uint32_t i = 0u; i < rScratch.nCellHashCapacity; ++i ) {
		t3dWorldCellHashEntry_t &Entry = rScratch.pCellHash[nSlot];
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

bool t3d_world_build_scene_cells_cold(
	t3dWorldInstance_t				*pInstances,
	t3dWorldTransform_t				*pTransforms,
	uint64_t						*pInstanceSourceIds,
	uint32_t						 pInstanceCount,
	const t3dWorldPrototypeBounds_t *pPrototypeBounds,
	uint32_t						 pPrototypeBoundsCount,
	float							 pCellSize,
	t3dWorldCell_t					*pCells,
	uint32_t						 pCellCapacity,
	t3dWorldCellBuildScratch_t		&rScratch,
	t3dWorldOwnershipRegistry_t		&rOwnership,
	uint32_t						&rCellCount ) {
	rCellCount = 0u;
	if ( pInstances == NULL || pTransforms == NULL || pInstanceSourceIds == NULL || pPrototypeBounds == NULL ||
		 pCells == NULL || rScratch.pReorderedInstances == NULL || rScratch.pReorderedTransforms == NULL ||
		 rScratch.pReorderedSourceIds == NULL || rScratch.pInstanceCellIds == NULL || rScratch.pCellCounts == NULL ||
		 rScratch.pCellWriteCursor == NULL || rScratch.pCellHash == NULL || pCellSize <= 0.0f ||
		 pInstanceCount > rScratch.nInstanceCapacity ) {
		return false;
	}
	if ( pInstanceCount == 0u ) {
		return true;
	}

	for ( uint32_t i = 0u; i < rScratch.nCellHashCapacity; ++i ) {
		memset( &rScratch.pCellHash[i], 0, sizeof( t3dWorldCellHashEntry_t ) );
	}
	for ( uint32_t i = 0u; i < rScratch.nCellScratchCapacity; ++i ) {
		rScratch.pCellCounts[i] = 0u;
		rScratch.pCellWriteCursor[i] = 0u;
	}

	for ( uint32_t i = 0u; i < pInstanceCount; ++i ) {
		const t3dWorldInstance_t &Instance = pInstances[i];
		if ( Instance.m_nSourceId >= pPrototypeBoundsCount || Instance.m_nTransformIndex >= pInstanceCount ) {
			return false;
		}
		float vCenter[3];
		float fRadius = 0.0f;
		t3d_world_cell_transform_sphere( pTransforms[Instance.m_nTransformIndex],
										 pPrototypeBounds[Instance.m_nSourceId],
										 vCenter,
										 fRadius );
		const int32_t			 iCellX = ( int32_t ) floorf( vCenter[0] / pCellSize );
		const int32_t			 iCellZ = ( int32_t ) floorf( vCenter[2] / pCellSize );
		t3dWorldCellHashEntry_t *pEntry =
			t3d_world_cell_find_or_insert( rScratch, iCellX, iCellZ, rCellCount, pCellCapacity );
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
				if ( fMin < pEntry->vMin[axis] )
					pEntry->vMin[axis] = fMin;
				if ( fMax > pEntry->vMax[axis] )
					pEntry->vMax[axis] = fMax;
			}
		}
		++pEntry->nInstanceCount;
	}

	uint32_t nPrefix = 0u;
	for ( uint32_t c = 0u; c < rCellCount; ++c ) {
		pCells[c].m_nFirstInstance = nPrefix;
		pCells[c].m_nInstanceCount = rScratch.pCellCounts[c];
		pCells[c].m_nVisibilityLayers = UINT32_MAX;
		pCells[c].m_nFlags = 0u;
		rScratch.pCellWriteCursor[c] = nPrefix;
		nPrefix += rScratch.pCellCounts[c];
	}
	if ( nPrefix != pInstanceCount ) {
		return false;
	}

	for ( uint32_t h = 0u; h < rScratch.nCellHashCapacity; ++h ) {
		const t3dWorldCellHashEntry_t &Entry = rScratch.pCellHash[h];
		if ( !Entry.bOccupied ) {
			continue;
		}
		t3dWorldCell_t &Cell = pCells[Entry.nCellIndex];
		float			fRadiusSq = 0.0f;
		for ( uint32_t axis = 0u; axis < 3u; ++axis ) {
			Cell.m_fCenter[axis] = ( Entry.vMin[axis] + Entry.vMax[axis] ) * 0.5f;
			const float fHalf = ( Entry.vMax[axis] - Entry.vMin[axis] ) * 0.5f;
			fRadiusSq += fHalf * fHalf;
		}
		Cell.m_fRadius = sqrtf( fRadiusSq );
	}

	for ( uint32_t i = 0u; i < pInstanceCount; ++i ) {
		const uint32_t nCell = rScratch.pInstanceCellIds[i];
		const uint32_t nWrite = rScratch.pCellWriteCursor[nCell]++;
		rScratch.pReorderedInstances[nWrite] = pInstances[i];
		rScratch.pReorderedTransforms[nWrite] = pTransforms[pInstances[i].m_nTransformIndex];
		rScratch.pReorderedInstances[nWrite].m_nTransformIndex = nWrite;
		rScratch.pReorderedInstances[nWrite].m_nPreviousTransformIndex = nWrite;
		rScratch.pReorderedInstances[nWrite].m_nCellId = nCell;
		rScratch.pReorderedSourceIds[nWrite] = pInstanceSourceIds[i];
	}
	memcpy( pInstances, rScratch.pReorderedInstances, ( size_t ) pInstanceCount * sizeof( t3dWorldInstance_t ) );
	memcpy( pTransforms, rScratch.pReorderedTransforms, ( size_t ) pInstanceCount * sizeof( t3dWorldTransform_t ) );
	memcpy( pInstanceSourceIds, rScratch.pReorderedSourceIds, ( size_t ) pInstanceCount * sizeof( uint64_t ) );

	for ( uint32_t i = 0u; i < pInstanceCount; ++i ) {
		const t3dWorldOwnershipEntry_t *pOld =
			t3d_world_ownership_registry_find_reference_debug( rOwnership, pInstanceSourceIds[i] );
		if ( pOld == NULL || pOld->eOwnership != T3D_WORLD_OWNERSHIP_WORLD ) {
			continue;
		}
		t3dWorldOwnershipEntry_t Updated = *pOld;
		Updated.nInstanceId = i;
		if ( !t3d_world_ownership_registry_publish_cold( rOwnership, Updated ) ) {
			return false;
		}
	}
	return true;
}
