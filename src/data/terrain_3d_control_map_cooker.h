// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CONTROL_MAP_COOKER_H
#define TERRAIN3D_CONTROL_MAP_COOKER_H

#include "Terrain3DConstants.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <godot_cpp/variant/packed_byte_array.hpp>

typedef struct t3dControlCookSource_s {
	PackedByteArray m_bytes;
	bool m_bPresent = false;
	bool m_bImplicit = true;
} t3dControlCookSource_t;

bool t3dControlCookSetExplicitSource( t3dControlCookSource_t &rSource, const Ref<Image> &rControl );
EA_FORCE_INLINE uint32_t t3dControlCookReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

EA_FORCE_INLINE uint8_t t3dControlCookColorToByte( const float fValue ) {
	return ( uint8_t )( CLAMP( Math::round( fValue * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
}

uint32_t t3dControlCookMaterialMaskFromControlImage( const Ref<Image> &rControl );
Ref<Image> t3dCookGpuRenderControlImage( const t3dControlCookSource_t ( &rSources )[9] );

#endif // TERRAIN3D_CONTROL_MAP_COOKER_H
