// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_INSTANCE_LAYOUT_H
#define TERRAIN3D_INSTANCE_LAYOUT_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <cstdint>

typedef struct t3dInstanceLayout_s {
	uint16_t m_nCellSize = s_nInstanceDefaultCellSize;
	uint16_t m_nCellsPerAxis = 4;

	bool isValid( void ) const {
		return m_nCellSize >= s_nInstanceMinCellSize && m_nCellSize <= s_nInstanceMaxCellSize &&
				( m_nCellSize & ( m_nCellSize - 1u ) ) == 0u && m_nCellsPerAxis > 0u &&
				m_nCellsPerAxis <= s_nInstanceMaxCellsPerAxis && ( m_nCellsPerAxis & ( m_nCellsPerAxis - 1u ) ) == 0u &&
				( uint32_t )( m_nCellSize ) * ( uint32_t )( m_nCellsPerAxis ) >= s_nInstanceMinRegionSize &&
				( uint32_t )( m_nCellSize ) * ( uint32_t )( m_nCellsPerAxis ) <= s_nInstanceMaxRegionSize;
	}

	EA_FORCE_INLINE bool contains( const int iX, const int iY ) const {
		return iX >= 0 && iY >= 0 && iX < ( int )( m_nCellsPerAxis ) && iY < ( int )( m_nCellsPerAxis );
	}

	EA_FORCE_INLINE uint32_t maximumCellCount( void ) const {
		return ( uint32_t )( m_nCellsPerAxis ) * ( uint32_t )( m_nCellsPerAxis );
	}
} t3dInstanceLayout_t;

inline bool t3dInstanceLayoutForRegion( const int nRegionSize, const int nCellSize, t3dInstanceLayout_t &rLayout ) {
	if ( nRegionSize <= 0 || nCellSize < s_nInstanceMinCellSize || nCellSize > s_nInstanceMaxCellSize ||
			( nCellSize & ( nCellSize - 1 ) ) != 0 || nRegionSize % nCellSize != 0 ) {
		return false;
	}
	const int cells_per_axis = nRegionSize / nCellSize;
	if ( cells_per_axis <= 0 || cells_per_axis > s_nInstanceMaxCellsPerAxis ) {
		return false;
	}
	rLayout.m_nCellSize = ( uint16_t )( nCellSize );
	rLayout.m_nCellsPerAxis = ( uint16_t )( cells_per_axis );
	return true;
}

#endif // TERRAIN3D_INSTANCE_LAYOUT_H
