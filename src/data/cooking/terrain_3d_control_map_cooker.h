// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CONTROL_MAP_COOKER_H
#define TERRAIN3D_CONTROL_MAP_COOKER_H

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_binary_io.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <godot_cpp/variant/packed_byte_array.hpp>

typedef struct t3dControlCookSource_s
{
	PackedByteArray m_bytes;
	bool			m_bPresent = false;
	bool			m_bImplicit = true;
} t3dControlCookSource_t;

// Fixed 3x3 neighborhood in row-major Z/X order; the center is element four.
typedef struct t3dControlCookNeighborhood_s
{
	t3dControlCookSource_t m_sources[9];
} t3dControlCookNeighborhood_t;

Ref< Image > t3dCookGpuRenderControlImage( const t3dControlCookNeighborhood_t &rNeighborhood );

// Neighborhood order and packing have one authority. The callback retains its
// own synchronous/asynchronous source policy and returns false to defer cooking.
template< typename TResolve >
EA_FORCE_INLINE Ref< Image > t3dControlCookNeighborhoodImage(
	const Vector2i &rCenter,
	TResolve		resolve ) {
	t3dControlCookNeighborhood_t neighborhood;
	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i			location = rCenter + Vector2i( dx, dz );
			t3dControlCookSource_t &source = neighborhood.m_sources[( size_t ) ( ( dz + 1 ) * 3 + dx + 1 )];
			if ( !resolve( location, source ) ) {
				return Ref< Image >();
			}
		}
	}
	return t3dCookGpuRenderControlImage( neighborhood );
}

bool t3dControlCookSetExplicitSource( t3dControlCookSource_t &rSource, const Ref< Image > &rControl );
EA_FORCE_INLINE uint32_t t3dControlCookReadU32Unaligned(
	const uint8_t *pBytes,
	const size_t   nOffset ) {
	return pBytes ? t3dReadNativeUnaligned< uint32_t >( pBytes + nOffset ) : 0u;
}

EA_FORCE_INLINE uint8_t t3dControlCookColorToByte(
	const float fValue ) {
	return ( uint8_t ) ( CLAMP( Math::round( fValue * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
}

uint32_t t3dControlCookMaterialMaskFromControlImage( const Ref< Image > &rControl );

#endif // TERRAIN3D_CONTROL_MAP_COOKER_H
