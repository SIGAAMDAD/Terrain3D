// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_LAYOUT_H
#define TERRAIN3D_INSTANCE_LAYOUT_H

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"

#include <stdint.h>

EA_FORCE_INLINE uint8_t t3dInstanceCellShift(
	const int nCellSize ) {
	switch ( nCellSize ) {
	case 8:
		return 3u;
	case 16:
		return 4u;
	case 32:
		return 5u;
	case 64:
		return 6u;
	case 128:
		return 7u;
	default:
		return UINT8_MAX;
	}
}

typedef struct t3dInstanceLayout_s
{
	uint16_t m_nCellSize = T3D_INSTANCE_DEFAULT_CELL_SIZE;
	uint16_t m_nCellsPerAxis = T3D_REGION_SIZE >> 5;
	uint8_t	 m_nCellShift = 5u;

	bool isValid(
		void ) const {
		return m_nCellSize >= T3D_INSTANCE_MIN_CELL_SIZE && m_nCellSize <= T3D_INSTANCE_MAX_CELL_SIZE &&
			   m_nCellShift != UINT8_MAX && m_nCellSize == ( uint16_t ) ( 1u << m_nCellShift ) &&
			   m_nCellsPerAxis == ( uint16_t ) ( T3D_REGION_SIZE >> m_nCellShift ) && m_nCellsPerAxis > 0u &&
			   m_nCellsPerAxis <= T3D_INSTANCE_MAX_CELLS_PER_AXIS;
	}

	EA_FORCE_INLINE bool contains(
		const int iX,
		const int iY ) const {
		return iX >= 0 && iY >= 0 && iX < ( int ) ( m_nCellsPerAxis ) && iY < ( int ) ( m_nCellsPerAxis );
	}

	EA_FORCE_INLINE uint32_t maximumCellCount(
		void ) const {
		return ( uint32_t ) ( m_nCellsPerAxis ) * ( uint32_t ) ( m_nCellsPerAxis );
	}
} t3dInstanceLayout_t;

EA_FORCE_INLINE bool t3dInstanceLayoutForCellSize(
	const int			 nCellSize,
	t3dInstanceLayout_t &rLayout ) {
	const uint8_t cell_shift = t3dInstanceCellShift( nCellSize );
	if ( cell_shift == UINT8_MAX || nCellSize < T3D_INSTANCE_MIN_CELL_SIZE || nCellSize > T3D_INSTANCE_MAX_CELL_SIZE ) {
		return false;
	}
	rLayout.m_nCellSize = ( uint16_t ) ( nCellSize );
	rLayout.m_nCellShift = cell_shift;
	rLayout.m_nCellsPerAxis = ( uint16_t ) ( T3D_REGION_SIZE >> cell_shift );
	return rLayout.isValid();
}

#endif // TERRAIN3D_INSTANCE_LAYOUT_H
