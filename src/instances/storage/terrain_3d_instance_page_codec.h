// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_INSTANCE_PAGE_CODEC_H
#define TERRAIN3D_INSTANCE_PAGE_CODEC_H

#include "Terrain3DConstants.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "instances/storage/terrain_3d_instance_store.h"

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/variant/packed_byte_array.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class Terrain3DInstancePageCodec
{
	CLASS_NAME_STATIC( "Terrain3DInstancePageCodec" );

public:
	static bool encode( const Terrain3DInstanceStore &rStore,
						PackedByteArray				 &rPage,
						uint32_t					 *pMeshCounts,
						uint32_t					 &nInstanceCount,
						uint16_t					 &nMeshCount );
	static bool decode( const PackedByteArray  &rPage,
						Terrain3DInstanceStore &rStore,
						uint32_t			   *pMeshCounts,
						uint32_t			   &nInstanceCount,
						uint16_t			   &nMeshCount );
	static bool decode( const PackedByteArray  &rPage,
						size_t					nPageBytes,
						Terrain3DInstanceStore &rStore,
						uint32_t			   *pMeshCounts,
						uint32_t			   &nInstanceCount,
						uint16_t			   &nMeshCount );
	static bool
	inspect( const PackedByteArray &rPage, uint32_t *pMeshCounts, uint32_t &nInstanceCount, uint16_t &nMeshCount );
#if defined( TERRAIN3D_INSTANCE_PAGE_CODEC_TEST_ACCESS )
	static void testSetDecodeFailureCountdown( int nCountdown );
	static void testClearDecodeFailure( void );
#endif
};

#endif // TERRAIN3D_INSTANCE_PAGE_CODEC_H
