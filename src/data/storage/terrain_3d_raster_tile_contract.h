// Sparse authored raster tile contract. The logical region/image ABI is unchanged.
#ifndef TERRAIN3D_RASTER_TILE_CONTRACT_H
#define TERRAIN3D_RASTER_TILE_CONTRACT_H

#include "Terrain3DConstants.h"
#include "Terrain3DEnums.h"
#include <stdint.h>
#include <godot_cpp/variant/vector2i.hpp>

#define T3D_RASTER_TILE_SIDE		( 32u )
#define T3D_RASTER_TILES_PER_AXIS	( T3D_REGION_SIZE / T3D_RASTER_TILE_SIDE )
#define T3D_RASTER_TILES_PER_REGION ( T3D_RASTER_TILES_PER_AXIS * T3D_RASTER_TILES_PER_AXIS )
#define T3D_RASTER_BUNDLE_MAGIC		( 0x42523354u ) // 'T3RB', little endian
#define T3D_RASTER_BUNDLE_VERSION	( 2u )

static_assert( T3D_REGION_SIZE == 128, "Sparse source tiles require the unchanged 128-sample region geometry" );
static_assert( T3D_REGION_MAP_SIZE == 32, "Sparse source tiles require the 32x32 logical world" );
static_assert( T3D_RASTER_TILES_PER_REGION == 16, "Sparse leaf must contain 16 tiles per region" );
static_assert( ( T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE * REGION_TYPE_MAX * T3D_RASTER_TILES_PER_REGION ) < 65536,
			   "Tile identity fits in a uint16_t with no sentinel" );

typedef enum t3dRasterTileEncoding_e : uint8_t
{
	T3D_RASTER_TILE_DEFAULT = 0u,
	T3D_RASTER_TILE_CONSTANT = 1u,
	T3D_RASTER_TILE_RAW = 2u,
	T3D_RASTER_TILE_RLE = 3u,	 // Cooked bundle only: uint16 run + 2/4-byte pixel.
	T3D_RASTER_TILE_SHARED = 4u, // Cooked bundle v2 only: uint16 earlier source tile ID.
} t3dRasterTileEncoding_t;

// A stable ID is independent of cache residency or physical backing allocation.
inline uint16_t t3dRasterTileId(
	const godot::Vector2i &rRegion,
	t3dMapType_t		   eChannel,
	uint8_t				   nTileX,
	uint8_t				   nTileY ) {
	if ( rRegion.x < -16 || rRegion.x >= 16 || rRegion.y < -16 || rRegion.y >= 16 || eChannel < TYPE_HEIGHT ||
		 eChannel >= REGION_TYPE_MAX || nTileX >= 4u || nTileY >= 4u ) {
		return UINT16_MAX;
	}
	const uint32_t region = ( uint32_t ) ( rRegion.y + 16 ) * 32u + ( uint32_t ) ( rRegion.x + 16 );
	return ( uint16_t ) ( ( region * REGION_TYPE_MAX + ( uint32_t ) eChannel ) * 16u + nTileY * 4u + nTileX );
}

inline bool t3dRasterTileIdValid(
	const uint32_t nId ) {
	return nId < ( uint32_t ) T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE * REGION_TYPE_MAX * T3D_RASTER_TILES_PER_REGION;
}

inline t3dMapType_t t3dRasterTileChannel(
	const uint16_t nId ) {
	return ( t3dMapType_t ) ( ( nId / 16u ) % REGION_TYPE_MAX );
}

inline uint32_t t3dRasterTilePixelBytes(
	const t3dMapType_t eType ) {
	return eType == TYPE_HEIGHT ? 2u : ( eType == TYPE_CONTROL || eType == TYPE_COLOR ? 4u : 0u );
}

inline uint32_t t3dRasterTileRawBytes(
	const t3dMapType_t eType ) {
	return T3D_RASTER_TILE_SIDE * T3D_RASTER_TILE_SIDE * t3dRasterTilePixelBytes( eType );
}

#endif
