#include "data/storage/terrain_3d_raster_tile_store.h"
#include "data/persistence/terrain_3d_region_codec.h"

#include <string.h>
#include <godot_cpp/classes/image.hpp>
#include <EASTL/hash_set.h>

using namespace godot;

void Terrain3DRasterTileStore::defaultPixel(
	const t3dMapType_t eChannel,
	uint8_t *const	   pPixel ) {
	if ( eChannel == TYPE_HEIGHT ) {
		const uint16_t encoded = t3dRegionCodecEncodeHeightU16( 0.0f );
		memcpy( pPixel, &encoded, 2u );
	} else if ( eChannel == TYPE_CONTROL ) {
		const uint32_t automatic = T3D_REGION_DEFAULT_CONTROL;
		memcpy( pPixel, &automatic, 4u );
	} else {
		pPixel[0] = 255u;
		pPixel[1] = 255u;
		pPixel[2] = 255u;
		pPixel[3] = 128u;
	}
}

uint64_t Terrain3DRasterTileStore::hashBytes(
	const uint8_t *const pData,
	const uint32_t		 nCount ) {
	uint64_t hash = 1469598103934665603ull;
	for ( uint32_t i = 0u; i < nCount; ++i ) {
		hash = ( hash ^ pData[i] ) * 1099511628211ull;
	}
	return hash;
}

// The hash index is advisory. Dropping the removed representative is O(1)
// and avoids scanning all live tiles during high-frequency brush revisions.
// Future raw writes can repopulate a missing hash without affecting correctness.
void Terrain3DRasterTileStore::unlinkRawTile(
	const uint16_t nId,
	const t3dRasterTile_t &rTile ) {
	if ( rTile.m_eEncoding != T3D_RASTER_TILE_RAW ) {
		return;
	}
	const auto indexed = m_hashToTile.find( rTile.m_nHash );
	if ( indexed != m_hashToTile.end() && indexed->second == nId ) {
		m_hashToTile.erase( indexed );
	}
}

bool Terrain3DRasterTileStore::updateRegion(
	const Vector2i	   &rRegion,
	const t3dMapType_t	eChannel,
	const Ref< Image > &rImage,
	const Rect2i	   &rDirty ) {
	const uint32_t pixel_bytes = t3dRasterTilePixelBytes( eChannel );
	if ( pixel_bytes == 0u || t3dRasterTileId( rRegion, eChannel, 0u, 0u ) == UINT16_MAX ) {
		return false;
	}
	const bool present = rImage.is_valid();
	if ( present && ( rImage->get_width() != T3D_REGION_SIZE || rImage->get_height() != T3D_REGION_SIZE ||
					  rImage->get_format() != ( eChannel == TYPE_HEIGHT	   ? Image::FORMAT_R16
												: eChannel == TYPE_CONTROL ? Image::FORMAT_RF
																		   : Image::FORMAT_RGBA8 ) ) ) {
		return false;
	}
	const PackedByteArray source = present ? rImage->get_data() : PackedByteArray();
	if ( present && source.size() != ( int64_t ) ( T3D_REGION_SIZE * T3D_REGION_SIZE * pixel_bytes ) ) {
		return false;
	}
	const uint8_t *const pSource = present ? source.ptr() : NULL;
	if ( present && !pSource ) {
		return false;
	}
	const Rect2i bounded = rDirty.size.x > 0 && rDirty.size.y > 0
							   ? rDirty.intersection( Rect2i( 0, 0, T3D_REGION_SIZE, T3D_REGION_SIZE ) )
							   : Rect2i( 0, 0, T3D_REGION_SIZE, T3D_REGION_SIZE );
	if ( bounded.size.x <= 0 || bounded.size.y <= 0 ) {
		return true;
	}
	const int begin_x = bounded.position.x / 32;
	const int begin_y = bounded.position.y / 32;
	const int end_x = ( bounded.position.x + bounded.size.x - 1 ) / 32;
	const int end_y = ( bounded.position.y + bounded.size.y - 1 ) / 32;
	uint8_t	  default_pixel[4] = {};
	defaultPixel( eChannel, default_pixel );
	for ( int ty = begin_y; ty <= end_y; ++ty ) {
		for ( int tx = begin_x; tx <= end_x; ++tx ) {
			const uint16_t id = t3dRasterTileId( rRegion, eChannel, ( uint8_t ) tx, ( uint8_t ) ty );
			const auto	   old = m_tiles.find( id );
			if ( !present ) {
				if ( old != m_tiles.end() ) {
					unlinkRawTile( id, old->second );
					m_tiles.erase( old );
					m_dirty[id] = ++m_nRevision;
				}
				continue;
			}
			const uint32_t raw_size = t3dRasterTileRawBytes( eChannel );
			// Reused stack staging avoids a Godot allocation for every no-op or
			// constant-tile edit, even while a brush fires at high frequency.
			uint8_t		   staged[4096];
			uint8_t *const pTile = staged;
			for ( int row = 0; row < 32; ++row ) {
				const size_t input = ( ( size_t ) ( ty * 32 + row ) * 128u + ( size_t ) tx * 32u ) * pixel_bytes;
				memcpy( pTile + ( size_t ) row * 32u * pixel_bytes, pSource + input, 32u * pixel_bytes );
			}
			const uint8_t *const pFirst = pTile;
			bool				 constant = true;
			bool				 is_default = memcmp( pFirst, default_pixel, pixel_bytes ) == 0;
			for ( uint32_t i = pixel_bytes; i < raw_size; i += pixel_bytes ) {
				if ( memcmp( pTile + i, pFirst, pixel_bytes ) != 0 ) {
					constant = false;
					is_default = false;
					break;
				}
			}
			if ( is_default ) {
				if ( old != m_tiles.end() ) {
					unlinkRawTile( id, old->second );
					m_tiles.erase( old );
					m_dirty[id] = ++m_nRevision;
				}
				continue;
			}
			const uint64_t				  hash = hashBytes( pTile, raw_size );
			const t3dRasterTileEncoding_t encoding = constant ? T3D_RASTER_TILE_CONSTANT : T3D_RASTER_TILE_RAW;
			if ( old != m_tiles.end() && old->second.m_eEncoding == encoding && old->second.m_nHash == hash &&
				 ( constant ? memcmp( old->second.m_szConstant, pFirst, pixel_bytes ) == 0
							: old->second.m_bytes.size() == raw_size &&
								  memcmp( old->second.m_bytes.ptr(), pTile, raw_size ) == 0 ) ) {
				continue;
			}
			t3dRasterTile_t replacement;
			replacement.m_eEncoding = encoding;
			replacement.m_nHash = hash;
			replacement.m_nRevision = ++m_nRevision;
			if ( constant ) {
				memcpy( replacement.m_szConstant, pFirst, pixel_bytes );
			} else {
				const auto					 equalHash = m_hashToTile.find( hash );
				const t3dRasterTile_t *const pShared =
					equalHash != m_hashToTile.end() ? findTile( equalHash->second ) : NULL;
				if ( pShared && pShared->m_eEncoding == T3D_RASTER_TILE_RAW && pShared->m_bytes.size() == raw_size &&
					 memcmp( pShared->m_bytes.ptr(), staged, raw_size ) == 0 ) {
					replacement.m_bytes = pShared->m_bytes; // reference-counted immutable backing
				} else {
					replacement.m_bytes.resize( raw_size );
					memcpy( replacement.m_bytes.ptrw(), staged, raw_size );
				}
			}
			if ( old != m_tiles.end() ) {
				unlinkRawTile( id, old->second );
			}
			m_tiles[id] = replacement;
			if ( !constant ) {
				m_hashToTile[hash] = id;
			}
			m_dirty[id] = replacement.m_nRevision;
		}
	}
	return true;
}

const t3dRasterTile_t *Terrain3DRasterTileStore::findTile(
	const uint16_t nId ) const {
	const auto it = m_tiles.find( nId );
	return it == m_tiles.end() ? NULL : &it->second;
}

bool Terrain3DRasterTileStore::hasRegionChannel(
	const Vector2i	  &rRegion,
	const t3dMapType_t eChannel ) const {
	for ( uint8_t y = 0; y < 4u; ++y ) {
		for ( uint8_t x = 0; x < 4u; ++x ) {
			const uint16_t id = t3dRasterTileId( rRegion, eChannel, x, y );
			if ( id != UINT16_MAX && findTile( id ) ) {
				return true;
			}
		}
	}
	return false;
}

bool Terrain3DRasterTileStore::reconstructTile(
	const uint16_t	 nId,
	PackedByteArray &rBytes ) const {
	if ( !t3dRasterTileIdValid( nId ) ) {
		return false;
	}
	const t3dMapType_t			 channel = t3dRasterTileChannel( nId );
	const uint32_t				 pixel_bytes = t3dRasterTilePixelBytes( channel );
	const uint32_t				 byte_count = t3dRasterTileRawBytes( channel );
	const t3dRasterTile_t *const pTile = findTile( nId );
	if ( pTile && pTile->m_eEncoding == T3D_RASTER_TILE_RAW ) {
		if ( pTile->m_bytes.size() != byte_count ) {
			return false;
		}
		rBytes = pTile->m_bytes; // immutable, reference-counted backing
		return true;
	}
	rBytes.resize( byte_count );
	uint8_t *const pDst = rBytes.ptrw();
	uint8_t		   constant[4] = {};
	defaultPixel( channel, constant );
	if ( pTile && pTile->m_eEncoding == T3D_RASTER_TILE_CONSTANT ) {
		memcpy( constant, pTile->m_szConstant, pixel_bytes );
	}
	for ( uint32_t offset = 0; offset < byte_count; offset += pixel_bytes ) {
		memcpy( pDst + offset, constant, pixel_bytes );
	}
	return true;
}

Ref< Image > Terrain3DRasterTileStore::reconstructRegion(
	const Vector2i	  &rRegion,
	const t3dMapType_t eChannel ) const {
	const uint32_t pixel_bytes = t3dRasterTilePixelBytes( eChannel );
	if ( pixel_bytes == 0u || ( eChannel != TYPE_HEIGHT && !hasRegionChannel( rRegion, eChannel ) ) ) {
		return Ref< Image >();
	}
	PackedByteArray result;
	result.resize( 128u * 128u * pixel_bytes );
	uint8_t *const pDst = result.ptrw();
	for ( uint8_t ty = 0; ty < 4u; ++ty ) {
		for ( uint8_t tx = 0; tx < 4u; ++tx ) {
			PackedByteArray tile;
			if ( !reconstructTile( t3dRasterTileId( rRegion, eChannel, tx, ty ), tile ) ) {
				return Ref< Image >();
			}
			for ( uint32_t row = 0u; row < 32u; ++row ) {
				const size_t output = ( ( size_t ) ( ty * 32u + row ) * 128u + ( size_t ) tx * 32u ) * pixel_bytes;
				memcpy( pDst + output, tile.ptr() + ( size_t ) row * 32u * pixel_bytes, 32u * pixel_bytes );
			}
		}
	}
	const Image::Format format = eChannel == TYPE_HEIGHT	? Image::FORMAT_R16
								 : eChannel == TYPE_CONTROL ? Image::FORMAT_RF
															: Image::FORMAT_RGBA8;
	return Image::create_from_data( 128, 128, false, format, result );
}

void Terrain3DRasterTileStore::removeRegion(
	const Vector2i &rRegion ) {
	for ( uint32_t type = TYPE_HEIGHT; type < REGION_TYPE_MAX; ++type ) {
		for ( uint8_t y = 0u; y < 4u; ++y ) {
			for ( uint8_t x = 0u; x < 4u; ++x ) {
				const uint16_t id = t3dRasterTileId( rRegion, ( t3dMapType_t ) type, x, y );
				const auto existing = m_tiles.find( id );
				if ( existing != m_tiles.end() ) {
					unlinkRawTile( id, existing->second );
					m_tiles.erase( existing );
					m_dirty[id] = ++m_nRevision;
				}
			}
		}
	}
}

void Terrain3DRasterTileStore::clear(
	void ) {
	m_tiles.clear();
	m_dirty.clear();
	m_hashToTile.clear();
	m_nRevision = 1u;
}

void Terrain3DRasterTileStore::acknowledgeSaved(
	void ) {
	m_dirty.clear();
}

void Terrain3DRasterTileStore::importTile(
	const uint16_t		   nId,
	const t3dRasterTile_t &rTile ) {
	if ( t3dRasterTileIdValid( nId ) && rTile.m_eEncoding != T3D_RASTER_TILE_DEFAULT ) {
		const auto existing = m_tiles.find( nId );
		if ( existing != m_tiles.end() ) {
			unlinkRawTile( nId, existing->second );
		}
		m_tiles[nId] = rTile;
		if ( rTile.m_eEncoding == T3D_RASTER_TILE_RAW ) {
			m_hashToTile[rTile.m_nHash] = nId;
		}
		if ( rTile.m_nRevision >= m_nRevision ) {
			m_nRevision = rTile.m_nRevision + 1u;
		}
	}
}

uint32_t Terrain3DRasterTileStore::getDirtyTileCount(
	void ) const {
	return ( uint32_t ) m_dirty.size();
}

Dictionary Terrain3DRasterTileStore::getStats(
	void ) const {
	Dictionary						   stats;
	uint64_t						   bytes = 0u;
	uint64_t						   unique_bytes = 0u;
	eastl::hash_set< const uint8_t * > unique_backings;
	uint32_t						   constant = 0u;
	uint32_t						   raw = 0u;
	for ( auto it = m_tiles.begin(); it != m_tiles.end(); ++it ) {
		const t3dRasterTile_t &tile = it->second;
		if ( tile.m_eEncoding == T3D_RASTER_TILE_CONSTANT ) {
			++constant;
		} else if ( tile.m_eEncoding == T3D_RASTER_TILE_RAW ) {
			++raw;
			bytes += ( uint64_t ) tile.m_bytes.size();
			if ( unique_backings.insert( tile.m_bytes.ptr() ).second ) {
				unique_bytes += ( uint64_t ) tile.m_bytes.size();
			}
		}
	}
	stats["raster_tile_count"] = ( int64_t ) m_tiles.size();
	stats["raster_raw_tiles"] = raw;
	stats["raster_hash_index_entries"] = ( int64_t ) m_hashToTile.size();
	stats["raster_constant_tiles"] = constant;
	stats["raster_implicit_tiles"] = ( int64_t ) ( 32 * 32 * REGION_TYPE_MAX * 16 ) - ( int64_t ) m_tiles.size();
	stats["raster_raw_bytes"] = ( int64_t ) bytes;
	stats["raster_unique_raw_bytes"] = ( int64_t ) unique_bytes;
	stats["raster_shared_saved_bytes"] = ( int64_t ) ( bytes - unique_bytes );
	stats["raster_dirty_tiles"] = ( int64_t ) m_dirty.size();
	stats["raster_revision"] = ( int64_t ) m_nRevision;
	return stats;
}
