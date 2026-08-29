// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_PERSISTENT_H
#define TERRAIN3D_RD_PERSISTENT_H

#include <stdint.h>
#include <string.h>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string_name.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

inline static bool t3dRdPersistentMappingAvailable( RenderingDevice *const pRd ) {
	if ( !pRd ) {
		return false;
	}
	static const StringName s_mapMethod( "buffer_persistent_map_advance" );
	static const StringName s_flushMethod( "buffer_flush" );
	return pRd->has_method( s_mapMethod ) && pRd->has_method( s_flushMethod );
}

inline static BitField<RenderingDevice::BufferCreationBits> t3dRdPersistentCreationBits( void ) {
	return BitField<RenderingDevice::BufferCreationBits>( RenderingDevice::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT );
}

inline static uint8_t *t3dRdPersistentMapAdvance( RenderingDevice *const pRd, const RID &rBuffer ) {
	if ( !pRd || !rBuffer.is_valid() ) {
		return NULL;
	}
	const uint64_t nAddress = pRd->buffer_persistent_map_advance( rBuffer );
	return ( uint8_t * )( ( uintptr_t )nAddress );
}

inline static bool t3dRdPersistentWrite(
        RenderingDevice *const pRd, const RID &rBuffer, const void *const pSource, const uint32_t nBytes, const uint32_t nOffset = 0u ) {
	if ( !pSource || nBytes == 0u ) {
		return true;
	}
	uint8_t *const pMapped = t3dRdPersistentMapAdvance( pRd, rBuffer );
	if ( !pMapped ) {
		return false;
	}
	memcpy( pMapped + nOffset, pSource, nBytes );
	pRd->buffer_flush( rBuffer );
	return true;
}

#endif // TERRAIN3D_RD_PERSISTENT_H
