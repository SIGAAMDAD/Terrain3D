// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_INSTANCE_LAYOUT_H
#define TERRAIN3D_INSTANCE_LAYOUT_H

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"

#include <cstdint>


typedef struct t3dinstanceLayout_s {
	uint16_t nCellSize = TERRAIN3D_INSTANCE_DEFAULT_CELL_SIZE;
	uint16_t cellsPerAxis = 4;

	bool isValid( void ) const {
		return nCellSize >= TERRAIN3D_INSTANCE_MIN_CELL_SIZE &&
			nCellSize <= TERRAIN3D_INSTANCE_MAX_CELL_SIZE &&
			( nCellSize & ( nCellSize - 1u ) ) == 0u &&
			cellsPerAxis > 0u && cellsPerAxis <= TERRAIN3D_INSTANCE_MAX_CELLS_PER_AXIS &&
			( cellsPerAxis & ( cellsPerAxis - 1u ) ) == 0u &&
			( uint32_t ) ( nCellSize ) * ( uint32_t ) ( cellsPerAxis ) >= TERRAIN3D_INSTANCE_MIN_REGION_SIZE &&
			( uint32_t ) ( nCellSize ) * ( uint32_t ) ( cellsPerAxis ) <= TERRAIN3D_INSTANCE_MAX_REGION_SIZE;
	}

	EA_FORCE_INLINE bool contains( const int iX, const int iY ) const { return iX >= 0 && iY >= 0 && iX < ( int ) ( cellsPerAxis ) && iY < ( int ) ( cellsPerAxis ); }

	EA_FORCE_INLINE uint32_t maximumCellCount( void ) const { return ( uint32_t ) ( cellsPerAxis ) * ( uint32_t ) ( cellsPerAxis ); }
} t3dinstanceLayout_t;

inline bool terrain3dInstanceLayoutForRegion(
	const int nRegionSize, const int nCellSize,
	t3dinstanceLayout_t &rLayout
) {
	if ( nRegionSize <= 0 || nCellSize < TERRAIN3D_INSTANCE_MIN_CELL_SIZE ||
		nCellSize > TERRAIN3D_INSTANCE_MAX_CELL_SIZE ||
		( nCellSize & ( nCellSize - 1 ) ) != 0 || nRegionSize % nCellSize != 0 ) {
		return false;
	}
	const int cells_per_axis = nRegionSize / nCellSize;
	if ( cells_per_axis <= 0 || cells_per_axis > TERRAIN3D_INSTANCE_MAX_CELLS_PER_AXIS ) {
		return false;
	}
	rLayout.nCellSize = ( uint16_t ) ( nCellSize );
	rLayout.cellsPerAxis = ( uint16_t ) ( cells_per_axis );
	return true;
}

#endif // TERRAIN3D_INSTANCE_LAYOUT_H
