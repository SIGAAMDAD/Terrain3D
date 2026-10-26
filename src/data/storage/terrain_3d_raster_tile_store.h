// Sparse, lossless derived representation of the authoritative editor images.
#ifndef TERRAIN3D_RASTER_TILE_STORE_H
#define TERRAIN3D_RASTER_TILE_STORE_H

#include "Terrain3DTypes.h"
#include "data/storage/terrain_3d_raster_tile_contract.h"
#include "eastl_config.h"

#include <EASTL/hash_map.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/dictionary.hpp>

// Constant tiles hold one 2/4-byte pixel; raw tiles hold 2/4 KiB.
// No entry at all denotes an implicit default tile.
typedef struct t3dRasterTile_s
{
	uint64_t				m_nHash = 0u;
	uint32_t				m_nRevision = 1u;
	t3dRasterTileEncoding_t m_eEncoding = T3D_RASTER_TILE_DEFAULT;
	uint8_t					m_szConstant[4] = {};
	godot::PackedByteArray	m_bytes;
} t3dRasterTile_t;

class Terrain3DRasterTileStore
{
public:
	bool					   updateRegion( const godot::Vector2i			  &rRegion,
											 t3dMapType_t					   eChannel,
											 const godot::Ref< godot::Image > &rImage,
											 const godot::Rect2i			  &rDirty );
	godot::Ref< godot::Image > reconstructRegion( const godot::Vector2i &rRegion, t3dMapType_t eChannel ) const;
	bool					   reconstructTile( uint16_t nId, godot::PackedByteArray &rBytes ) const;
	const t3dRasterTile_t	  *findTile( uint16_t nId ) const;
	bool					   hasRegionChannel( const godot::Vector2i &rRegion, t3dMapType_t eChannel ) const;
	void					   removeRegion( const godot::Vector2i &rRegion );
	void					   clear( void );
	void					   acknowledgeSaved( void );
	godot::Dictionary		   getStats( void ) const;
	uint32_t				   getDirtyTileCount( void ) const;
	const eastl::hash_map< uint16_t, t3dRasterTile_t > &getTiles(
		void ) const {
		return m_tiles;
	}
	void importTile( uint16_t nId, const t3dRasterTile_t &rTile ); // only from a validated bundle

private:
	eastl::hash_map< uint16_t, t3dRasterTile_t > m_tiles;
	eastl::hash_map< uint16_t, uint32_t >		 m_dirty;
	// Hash index is advisory; equality is always verified before sharing backing.
	eastl::hash_map< uint64_t, uint16_t > m_hashToTile;
	uint32_t							  m_nRevision = 1u;
	void								  unlinkRawTile( uint16_t nId, const t3dRasterTile_t &rTile );
	static void							  defaultPixel( t3dMapType_t eChannel, uint8_t *pPixel );
	static uint64_t						  hashBytes( const uint8_t *pData, uint32_t nCount );
};

#endif
