#include "data/persistence/terrain_3d_raster_bundle.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include "Terrain3DConstants.h"
#include "data/persistence/terrain_3d_region_codec.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <EASTL/hash_map.h>
#include <EASTL/bitset.h>
#include <string.h>

using namespace godot;

static uint64_t t3dRasterBundleHash(
	const uint8_t *const pData,
	const uint32_t		 nBytes ) {
	uint64_t hash = 1469598103934665603ull;
	for ( uint32_t i = 0; i < nBytes; ++i ) {
		hash = ( hash ^ pData[i] ) * 1099511628211ull;
	}
	return hash;
}

// Lossless pixel-run codec used only on explicitly cooked immutable bundles.
// All authored control words and R16 height codes are preserved byte-for-byte.
// A run stores little-endian uint16 count, followed by the exact source pixel.
static uint32_t t3dRasterBundleEncodeRLE(
	const uint8_t *const pRaw,
	const uint32_t		 nPixelBytes,
	uint8_t *const		 pEncoded ) {
	uint32_t source_pixel = 0u;
	uint32_t output = 0u;
	while ( source_pixel < 1024u ) {
		uint32_t count = 1u;
		while ( source_pixel + count < 1024u && memcmp( pRaw + source_pixel * nPixelBytes,
														pRaw + ( source_pixel + count ) * nPixelBytes,
														nPixelBytes ) == 0 ) {
			++count;
		}
		pEncoded[output++] = ( uint8_t ) ( count & 0xffu );
		pEncoded[output++] = ( uint8_t ) ( count >> 8u );
		memcpy( pEncoded + output, pRaw + source_pixel * nPixelBytes, nPixelBytes );
		output += nPixelBytes;
		source_pixel += count;
	}
	return output;
}

static bool t3dRasterBundleDecodeRLE(
	const uint8_t *const pEncoded,
	const uint32_t		 nEncodedBytes,
	const uint32_t		 nPixelBytes,
	uint8_t *const		 pRaw ) {
	uint32_t input = 0u;
	uint32_t output_pixels = 0u;
	while ( input < nEncodedBytes ) {
		if ( nEncodedBytes - input < nPixelBytes + 2u ) {
			return false;
		}
		const uint32_t count = ( uint32_t ) pEncoded[input] | ( ( uint32_t ) pEncoded[input + 1u] << 8u );
		input += 2u;
		if ( count == 0u || count > 1024u - output_pixels ) {
			return false;
		}
		for ( uint32_t repeat = 0u; repeat < count; ++repeat ) {
			memcpy( pRaw + ( output_pixels + repeat ) * nPixelBytes, pEncoded + input, nPixelBytes );
		}
		input += nPixelBytes;
		output_pixels += count;
	}
	return output_pixels == 1024u;
}

// Immutable bundle physical ordering groups every 4x4-region sector and
// its inner regions in Morton order, then channel and tile. IDs remain the
// established row-major logical addresses; this changes only disk locality.
static uint16_t t3dRasterSpatialPhysicalId(
	const uint32_t nOrdinal ) {
	const uint32_t tileMorton = nOrdinal % 16u;
	const uint32_t channel = ( nOrdinal / 16u ) % REGION_TYPE_MAX;
	const uint32_t regionMorton = nOrdinal / ( 16u * REGION_TYPE_MAX );
	const uint32_t sectorMorton = regionMorton / 16u;
	const uint32_t withinMorton = regionMorton % 16u;
	uint32_t	   sectorX = 0u, sectorY = 0u, regionX = 0u, regionY = 0u, tileX = 0u, tileY = 0u;
	for ( uint32_t bit = 0u; bit < 3u; ++bit ) {
		sectorX |= ( ( sectorMorton >> ( 2u * bit ) ) & 1u ) << bit;
		sectorY |= ( ( sectorMorton >> ( 2u * bit + 1u ) ) & 1u ) << bit;
		if ( bit < 2u ) {
			regionX |= ( ( withinMorton >> ( 2u * bit ) ) & 1u ) << bit;
			regionY |= ( ( withinMorton >> ( 2u * bit + 1u ) ) & 1u ) << bit;
			tileX |= ( ( tileMorton >> ( 2u * bit ) ) & 1u ) << bit;
			tileY |= ( ( tileMorton >> ( 2u * bit + 1u ) ) & 1u ) << bit;
		}
	}
	const uint32_t location = ( sectorY * 4u + regionY ) * 32u + sectorX * 4u + regionX;
	return ( uint16_t ) ( ( location * REGION_TYPE_MAX + channel ) * 16u + tileY * 4u + tileX );
}

String Terrain3DRasterBundle::getPath(
	const String &rDirectory ) {
	return rDirectory.path_join( "terrain3d_raster.t3rb" );
}

Error Terrain3DRasterBundle::save(
	const String				   &rDirectory,
	const Terrain3DRasterTileStore &rStore ) {
	if ( rDirectory.is_empty() ) {
		return ERR_INVALID_PARAMETER;
	}
	const String			 path = getPath( rDirectory );
	Terrain3DFilePublication transaction( path, path + String( ".staged" ), path + String( ".previous" ) );
	Ref< FileAccess >		 file;
	Error					 error = transaction.openStaged( file );
	if ( error != OK ) {
		return error;
	}
	uint64_t bytes_written = 24u;
	file->store_32( T3D_RASTER_BUNDLE_MAGIC );
	file->store_32( T3D_RASTER_BUNDLE_VERSION );
	file->store_32( T3D_REGION_SIZE );
	file->store_32( T3D_REGION_MAP_SIZE );
	file->store_32( ( uint32_t ) rStore.getTiles().size() );
	file->store_32( 0u ); // reserved
	// The immutable V2 dictionary deduplicates exact raw source pixels across
	// regions, regardless of their source tile encoding. Collision checks compare
	// the expanded bytes, not the 64-bit hash alone. References always point
	// backward in this deterministic ID order (no cycles or extra index I/O).
	eastl::hash_map< uint64_t, uint16_t > sharedSourceIds;
	// V2 groups nearby regions and their channels into bounded 4x4 spatial
	// sectors while retaining unchanged stable tile IDs and index offsets.
	const uint32_t max_tiles = T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE * REGION_TYPE_MAX * 16;
	for ( uint32_t ordinal = 0; ordinal < max_tiles; ++ordinal ) {
		const uint32_t				 id = t3dRasterSpatialPhysicalId( ordinal );
		const t3dRasterTile_t *const pTile = rStore.findTile( ( uint16_t ) id );
		if ( !pTile ) {
			continue;
		}
		const t3dMapType_t channel = t3dRasterTileChannel( ( uint16_t ) id );
		const uint32_t	   raw_size = t3dRasterTileRawBytes( channel );
		uint32_t		   content_size =
			  pTile->m_eEncoding == T3D_RASTER_TILE_CONSTANT ? t3dRasterTilePixelBytes( channel ) : raw_size;
		const uint8_t *pContent =
			pTile->m_eEncoding == T3D_RASTER_TILE_CONSTANT ? pTile->m_szConstant : pTile->m_bytes.ptr();
		t3dRasterTileEncoding_t stored_encoding = pTile->m_eEncoding;
		if ( pTile->m_eEncoding != T3D_RASTER_TILE_CONSTANT && pTile->m_eEncoding != T3D_RASTER_TILE_RAW ) {
			transaction.rollback();
			return ERR_INVALID_DATA;
		}
		if ( pTile->m_eEncoding == T3D_RASTER_TILE_RAW && pTile->m_bytes.size() != raw_size ) {
			transaction.rollback();
			return ERR_INVALID_DATA;
		}
		uint8_t		   sharedReference[2];
		const uint64_t lookupHash = pTile->m_nHash ^ ( ( uint64_t ) channel << 61u );
		const auto	   seen = sharedSourceIds.find( lookupHash );
		bool		   bShared = false;
		if ( seen != sharedSourceIds.end() ) {
			PackedByteArray candidate, existing;
			if ( rStore.reconstructTile( ( uint16_t ) id, candidate ) &&
				 rStore.reconstructTile( seen->second, existing ) && candidate.size() == existing.size() &&
				 memcmp( candidate.ptr(), existing.ptr(), ( size_t ) candidate.size() ) == 0 ) {
				bShared = true;
				sharedReference[0] = ( uint8_t ) seen->second;
				sharedReference[1] = ( uint8_t ) ( seen->second >> 8u );
				pContent = sharedReference;
				content_size = 2u;
				stored_encoding = T3D_RASTER_TILE_SHARED;
			}
		} else {
			sharedSourceIds[lookupHash] = ( uint16_t ) id;
		}
		// Encode only in the cold save phase. Never RLE-compress during brush edits.
		uint8_t encoded[6144];
		if ( !bShared && pTile->m_eEncoding == T3D_RASTER_TILE_RAW ) {
			const uint32_t encoded_bytes =
				t3dRasterBundleEncodeRLE( pContent, t3dRasterTilePixelBytes( channel ), encoded );
			if ( encoded_bytes < raw_size ) {
				stored_encoding = T3D_RASTER_TILE_RLE;
				pContent = encoded;
				content_size = encoded_bytes;
			}
		}
		file->store_16( ( uint16_t ) id );
		file->store_8( ( uint8_t ) stored_encoding );
		file->store_8( ( uint8_t ) channel );
		file->store_32( pTile->m_nRevision );
		file->store_32( content_size );
		file->store_64( t3dRasterBundleHash( pContent, content_size ) );
		PackedByteArray payload;
		payload.resize( content_size );
		memcpy( payload.ptrw(), pContent, content_size );
		file->store_buffer( payload );
		bytes_written += 20u + content_size;
	}
	error = transaction.finishStaged( file, bytes_written );
	if ( error != OK ) {
		transaction.rollback();
		return error;
	}
	error = transaction.publish();
	if ( error == OK ) {
		error = transaction.commit();
	}
	if ( error != OK ) {
		transaction.rollback();
	}
	return error;
}

Error Terrain3DRasterBundle::load(
	const String			 &rDirectory,
	Terrain3DRasterTileStore &rStore ) {
	const String path = getPath( rDirectory );
	if ( !FileAccess::file_exists( path ) ) {
		return ERR_FILE_NOT_FOUND;
	}
	const Ref< FileAccess > file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() || file->get_length() < 24u ) {
		return ERR_FILE_CORRUPT;
	}
	const uint32_t magic = file->get_32();
	const uint32_t version = file->get_32();
	const uint32_t region_size = file->get_32();
	const uint32_t map_size = file->get_32();
	const uint32_t count = file->get_32();
	const uint32_t reserved = file->get_32();
	const uint32_t max_tiles = T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE * REGION_TYPE_MAX * 16;
	if ( magic != T3D_RASTER_BUNDLE_MAGIC || ( version != 1u && version != T3D_RASTER_BUNDLE_VERSION ) ||
		 region_size != T3D_REGION_SIZE || map_size != T3D_REGION_MAP_SIZE || count > max_tiles || reserved != 0u ) {
		return ERR_FILE_CORRUPT;
	}
	Terrain3DRasterTileStore														  candidate;
	eastl::bitset< T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE * REGION_TYPE_MAX * 16 > seenIds;
	int32_t																			  last_id = -1;
	for ( uint32_t i = 0; i < count; ++i ) {
		if ( file->get_position() > file->get_length() || file->get_length() - file->get_position() < 20u ) {
			return ERR_FILE_CORRUPT;
		}
		const uint16_t id = file->get_16();
		const uint8_t  encoding = file->get_8();
		const uint8_t  channel = file->get_8();
		const uint32_t revision = file->get_32();
		const uint32_t content_size = file->get_32();
		const uint64_t hash = file->get_64();
		if ( !t3dRasterTileIdValid( id ) || ( version == 1u && id <= last_id ) ||
			 ( t3dRasterTileIdValid( id ) && seenIds.test( id ) ) ||
			 channel != ( uint8_t ) t3dRasterTileChannel( id ) ||
			 ( encoding != T3D_RASTER_TILE_CONSTANT && encoding != T3D_RASTER_TILE_RAW &&
			   encoding != T3D_RASTER_TILE_RLE && ( version < 2u || encoding != T3D_RASTER_TILE_SHARED ) ) ||
			 revision == 0u ) {
			return ERR_FILE_CORRUPT;
		}
		const uint32_t required = encoding == T3D_RASTER_TILE_CONSTANT
									  ? t3dRasterTilePixelBytes( ( t3dMapType_t ) channel )
									  : t3dRasterTileRawBytes( ( t3dMapType_t ) channel );
		const uint32_t run_width = t3dRasterTilePixelBytes( ( t3dMapType_t ) channel ) + 2u;
		const bool	   valid_size = encoding == T3D_RASTER_TILE_SHARED ? content_size == 2u
									: encoding == T3D_RASTER_TILE_RLE
										? content_size > 0u && content_size < required && content_size % run_width == 0u
										: content_size == required;
		if ( !valid_size || file->get_position() > file->get_length() ||
			 file->get_length() - file->get_position() < content_size ) {
			return ERR_FILE_CORRUPT;
		}
		const PackedByteArray content = file->get_buffer( content_size );
		if ( content.size() != content_size || t3dRasterBundleHash( content.ptr(), content_size ) != hash ) {
			return ERR_FILE_CORRUPT;
		}
		t3dRasterTile_t tile;
		tile.m_eEncoding = ( t3dRasterTileEncoding_t ) encoding;
		tile.m_nRevision = revision;
		if ( encoding == T3D_RASTER_TILE_SHARED ) {
			const uint16_t				 sourceId = ( uint16_t ) content[0] | ( ( uint16_t ) content[1] << 8u );
			const t3dRasterTile_t *const pSource = candidate.findTile( sourceId );
			if ( !t3dRasterTileIdValid( sourceId ) || t3dRasterTileChannel( sourceId ) != ( t3dMapType_t ) channel ||
				 !pSource ) {
				return ERR_FILE_CORRUPT;
			}
			tile.m_eEncoding = T3D_RASTER_TILE_RAW;
			if ( !candidate.reconstructTile( sourceId, tile.m_bytes ) ) {
				return ERR_FILE_CORRUPT;
			}
		} else if ( encoding == T3D_RASTER_TILE_CONSTANT ) {
			memcpy( tile.m_szConstant, content.ptr(), content_size );
		} else if ( encoding == T3D_RASTER_TILE_RLE ) {
			const uint32_t raw_bytes = t3dRasterTileRawBytes( ( t3dMapType_t ) channel );
			tile.m_eEncoding = T3D_RASTER_TILE_RAW;
			tile.m_bytes.resize( raw_bytes );
			if ( !t3dRasterBundleDecodeRLE( content.ptr(),
											content_size,
											t3dRasterTilePixelBytes( ( t3dMapType_t ) channel ),
											tile.m_bytes.ptrw() ) ) {
				return ERR_FILE_CORRUPT;
			}
		} else {
			tile.m_bytes = content;
		}
		PackedByteArray expanded;
		// Verifying the expanded tile hash also covers the constant representation.
		candidate.importTile( id, tile );
		if ( !candidate.reconstructTile( id, expanded ) ) {
			return ERR_FILE_CORRUPT;
		}
		tile.m_nHash = t3dRasterBundleHash( expanded.ptr(), ( uint32_t ) expanded.size() );
		candidate.importTile( id, tile );
		seenIds.set( id );
		last_id = id;
	}
	if ( file->get_position() != file->get_length() ) {
		return ERR_FILE_CORRUPT;
	}
	rStore = eastl::move( candidate );
	return OK;
}

void Terrain3DRasterBundleIndex::clear(
	void ) {
	// Cold transition: index callers must have finished their tile reads.
	{
		Terrain3DScopedLock lock( m_sourceMutex );
		++m_nSourceEpoch;
		for ( SourceEntry &entry : m_sourceEntries ) {
			entry = SourceEntry();
		}
		m_nSourceClock = 1u;
		m_nSourceResidentBytes = 0u;
		m_nSourceHits = m_nSourceJoins = m_nSourceReads = m_nSourceBytes = m_nSourceEvictions = 0u;
		m_sourceReady.notifyAll();
	}
	m_path = String();
	m_nSourceModified = 0u;
	m_nSourceFileBytes = 0u;
	m_entries.clear();
}

Error Terrain3DRasterBundleIndex::open(
	const String &rDirectory ) {
	clear();
	const String			path = Terrain3DRasterBundle::getPath( rDirectory );
	const Ref< FileAccess > file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() || file->get_length() < 24u ) {
		return ERR_FILE_NOT_FOUND;
	}
	const uint32_t magic = file->get_32();
	const uint32_t version = file->get_32();
	if ( magic != T3D_RASTER_BUNDLE_MAGIC || ( version != 1u && version != T3D_RASTER_BUNDLE_VERSION ) ||
		 file->get_32() != T3D_REGION_SIZE || file->get_32() != T3D_REGION_MAP_SIZE ) {
		return ERR_FILE_CORRUPT;
	}
	const uint32_t count = file->get_32();
	const uint32_t reserved = file->get_32();
	if ( reserved != 0u || count > T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE * REGION_TYPE_MAX * 16 ) {
		return ERR_FILE_CORRUPT;
	}
	eastl::hash_map< uint16_t, t3dRasterBundleIndexEntry_t >						  pending;
	eastl::bitset< T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE * REGION_TYPE_MAX * 16 > seenIds;
	int32_t																			  last_id = -1;
	for ( uint32_t i = 0u; i < count; ++i ) {
		if ( file->get_position() > file->get_length() || file->get_length() - file->get_position() < 20u ) {
			return ERR_FILE_CORRUPT;
		}
		const uint16_t id = file->get_16();
		const uint8_t  encoding = file->get_8();
		const uint8_t  channel = file->get_8();
		const uint32_t revision = file->get_32();
		const uint32_t bytes = file->get_32();
		const uint64_t hash = file->get_64();
		if ( !t3dRasterTileIdValid( id ) || ( version == 1u && ( int32_t ) id <= last_id ) ||
			 ( t3dRasterTileIdValid( id ) && seenIds.test( id ) ) ||
			 channel != ( uint8_t ) t3dRasterTileChannel( id ) ||
			 ( encoding != T3D_RASTER_TILE_CONSTANT && encoding != T3D_RASTER_TILE_RAW &&
			   encoding != T3D_RASTER_TILE_RLE && ( version < 2u || encoding != T3D_RASTER_TILE_SHARED ) ) ||
			 revision == 0u ) {
			return ERR_FILE_CORRUPT;
		}
		const uint32_t pixel_bytes = t3dRasterTilePixelBytes( ( t3dMapType_t ) channel );
		const uint32_t expected =
			encoding == T3D_RASTER_TILE_CONSTANT ? pixel_bytes : t3dRasterTileRawBytes( ( t3dMapType_t ) channel );
		if ( encoding == T3D_RASTER_TILE_SHARED ? bytes != 2u
			 : encoding == T3D_RASTER_TILE_RLE
				 ? ( bytes == 0u || bytes >= expected || bytes % ( pixel_bytes + 2u ) != 0u )
				 : bytes != expected ) {
			return ERR_FILE_CORRUPT;
		}
		const uint64_t offset = file->get_position();
		if ( offset > file->get_length() || file->get_length() - offset < bytes ) {
			return ERR_FILE_CORRUPT;
		}
		t3dRasterBundleIndexEntry_t entry;
		if ( encoding == T3D_RASTER_TILE_SHARED ) {
			const PackedByteArray reference = file->get_buffer( 2u );
			if ( reference.size() != 2u || t3dRasterBundleHash( reference.ptr(), 2u ) != hash ) {
				return ERR_FILE_CORRUPT;
			}
			const uint16_t sourceId = ( uint16_t ) reference[0] | ( ( uint16_t ) reference[1] << 8u );
			const auto	   source = pending.find( sourceId );
			if ( !t3dRasterTileIdValid( sourceId ) || t3dRasterTileChannel( sourceId ) != ( t3dMapType_t ) channel ||
				 source == pending.end() ) {
				return ERR_FILE_CORRUPT;
			}
			entry = source->second; // Resolved to actual payload on index load, never at shader time.
		} else {
			entry.m_nOffset = offset;
			entry.m_nHash = hash;
			entry.m_nBytes = bytes;
			entry.m_nEncoding = encoding;
		}
		entry.m_nRevision = revision;
		pending[id] = entry;
		file->seek( offset + bytes );
		seenIds.set( id );
		last_id = id;
	}
	if ( file->get_position() != file->get_length() ) {
		return ERR_FILE_CORRUPT;
	}
	m_entries = eastl::move( pending );
	m_path = path;
	m_nSourceFileBytes = ( uint64_t ) file->get_length();
	m_nSourceModified = FileAccess::get_modified_time( path );
	return OK;
}

bool Terrain3DRasterBundleIndex::hasRegionChannel(
	const Vector2i	  &rRegion,
	const t3dMapType_t eChannel ) const {
	for ( uint8_t ty = 0u; ty < 4u; ++ty ) {
		for ( uint8_t tx = 0u; tx < 4u; ++tx ) {
			const uint16_t id = t3dRasterTileId( rRegion, eChannel, tx, ty );
			if ( id != UINT16_MAX && m_entries.find( id ) != m_entries.end() ) {
				return true;
			}
		}
	}
	return false;
}

Ref< Image > Terrain3DRasterBundleIndex::reconstructRegion(
	const Vector2i	  &rRegion,
	const t3dMapType_t eChannel ) const {
	const uint32_t pixelBytes = t3dRasterTilePixelBytes( eChannel );
	if ( m_path.is_empty() || pixelBytes == 0u ||
		 ( eChannel != TYPE_HEIGHT && !hasRegionChannel( rRegion, eChannel ) ) ) {
		return Ref< Image >();
	}
	PackedByteArray output;
	output.resize( 128u * 128u * pixelBytes );
	uint8_t *const pOutput = output.ptrw();
	uint8_t		   defaultPixel[4] = {};
	if ( eChannel == TYPE_HEIGHT ) {
		const uint16_t encoded = t3dRegionCodecEncodeHeightU16( 0.0f );
		memcpy( defaultPixel, &encoded, 2u );
	} else if ( eChannel == TYPE_CONTROL ) {
		const uint32_t encoded = T3D_REGION_DEFAULT_CONTROL;
		memcpy( defaultPixel, &encoded, 4u );
	} else {
		defaultPixel[0] = defaultPixel[1] = defaultPixel[2] = 255u;
		defaultPixel[3] = 128u;
	}
	for ( size_t i = 0u; i < 128u * 128u; ++i ) {
		memcpy( pOutput + i * pixelBytes, defaultPixel, pixelBytes );
	}
	// Use the same verified source and decoding path as tile residency.
	// Open a single reader only on the first uncached tile if one is needed.
	Ref< FileAccess > reader;
	for ( uint8_t ty = 0u; ty < 4u; ++ty ) {
		for ( uint8_t tx = 0u; tx < 4u; ++tx ) {
			const uint16_t id = t3dRasterTileId( rRegion, eChannel, tx, ty );
			if ( m_entries.find( id ) == m_entries.end() ) {
				continue;
			}
			PackedByteArray tile;
			if ( !readTile( id, tile, reader ) || tile.size() != t3dRasterTileRawBytes( eChannel ) ) {
				return Ref< Image >();
			}
			for ( uint32_t y = 0u; y < 32u; ++y ) {
				memcpy( pOutput + ( ( size_t ) ( ty * 32u + y ) * 128u + tx * 32u ) * pixelBytes,
						tile.ptr() + y * 32u * pixelBytes,
						32u * pixelBytes );
			}
		}
	}
	const Image::Format format = eChannel == TYPE_HEIGHT	? Image::FORMAT_R16
								 : eChannel == TYPE_CONTROL ? Image::FORMAT_RF
															: Image::FORMAT_RGBA8;
	return Image::create_from_data( 128, 128, false, format, output );
}

// Cold, indexed source access for the bounded decoded-tile cache. Never called
// from the render thread, and never used to recook baked material VT packages.
bool Terrain3DRasterBundleIndex::hasTile(
	const uint16_t nId ) const {
	return m_entries.find( nId ) != m_entries.end();
}

Ref< FileAccess > Terrain3DRasterBundleIndex::openColdReader(
	void ) const {
	return m_path.is_empty() ? Ref< FileAccess >() : FileAccess::open( m_path, FileAccess::READ );
}

// Index identity includes the immutable package generation established by open().
// The stored-byte cache is independent of the bounded decoded-tile cache and
// uses the same exact hash and byte count verified by the legacy reader.
bool Terrain3DRasterBundleIndex::readStored(
	const t3dRasterBundleIndexEntry_t &rEntry,
	PackedByteArray					  &rStored,
	Ref< FileAccess >				  &rReader ) const {
	rStored = PackedByteArray();
	if ( m_path.is_empty() || m_nSourceFileBytes == 0u || rEntry.m_nOffset > m_nSourceFileBytes ||
		 rEntry.m_nBytes > m_nSourceFileBytes - rEntry.m_nOffset ||
		 ( m_nSourceModified && FileAccess::get_modified_time( m_path ) != m_nSourceModified ) ) {
		return false;
	}
	const bool cacheable = m_nSourceModified != 0u;
	int		   slot = -1;
	uint64_t   epoch = 0u;
	{
		Terrain3DUniqueLock lock( m_sourceMutex );
		epoch = m_nSourceEpoch;
		if ( cacheable ) {
			for ( ;; ) {
				int waiting = -1;
				for ( int i = 0; i < ( int ) m_sourceEntries.size(); ++i ) {
					SourceEntry &entry = m_sourceEntries[( size_t ) i];
					if ( !( entry.m_bReady || entry.m_bReading || entry.m_bFailed ) ||
						 entry.m_nOffset != rEntry.m_nOffset || entry.m_nBytes != rEntry.m_nBytes ||
						 entry.m_nHash != rEntry.m_nHash ) {
						continue;
					}
					if ( entry.m_bFailed ) {
						if ( t3dSteadyClockUsec() < entry.m_nRetryAfter ) {
							return false;
						}
						entry = SourceEntry();
						continue;
					}
					if ( entry.m_bReading ) {
						waiting = i;
						break;
					}
					entry.m_nUse = ++m_nSourceClock;
					rStored = entry.m_stored;
					++m_nSourceHits;
					return true;
				}
				if ( waiting < 0 ) {
					break;
				}
				++m_nSourceJoins;
				m_sourceReady.wait( lock );
				if ( epoch != m_nSourceEpoch ) {
					return false;
				}
			}
			for ( int i = 0; i < ( int ) m_sourceEntries.size(); ++i ) {
				if ( !m_sourceEntries[( size_t ) i].m_bReading && !m_sourceEntries[( size_t ) i].m_bReady &&
					 !m_sourceEntries[( size_t ) i].m_bFailed ) {
					slot = i;
					break;
				}
			}
			if ( slot < 0 ) {
				for ( int i = 0; i < ( int ) m_sourceEntries.size(); ++i ) {
					const SourceEntry &entry = m_sourceEntries[( size_t ) i];
					if ( !entry.m_bReading && ( slot < 0 || entry.m_nUse < m_sourceEntries[( size_t ) slot].m_nUse ) ) {
						slot = i;
					}
				}
			}
			if ( slot >= 0 ) {
				SourceEntry &entry = m_sourceEntries[( size_t ) slot];
				if ( entry.m_bReady ) {
					m_nSourceResidentBytes -= ( uint64_t ) entry.m_stored.size();
					++m_nSourceEvictions;
				}
				entry = SourceEntry();
				entry.m_nOffset = rEntry.m_nOffset;
				entry.m_nBytes = rEntry.m_nBytes;
				entry.m_nHash = rEntry.m_nHash;
				entry.m_bReading = true;
			}
		}
	}
	if ( rReader.is_null() ) {
		rReader = openColdReader();
	}
	const Ref< FileAccess > &file = rReader;
	bool					 valid = file.is_valid() && ( uint64_t ) file->get_length() == m_nSourceFileBytes;
	PackedByteArray			 stored;
	if ( valid ) {
		file->seek( rEntry.m_nOffset );
		stored = file->get_buffer( rEntry.m_nBytes );
		valid = stored.size() == ( int64_t ) rEntry.m_nBytes &&
				t3dRasterBundleHash( stored.ptr(), rEntry.m_nBytes ) == rEntry.m_nHash &&
				( !m_nSourceModified || FileAccess::get_modified_time( m_path ) == m_nSourceModified );
	}
	{
		Terrain3DScopedLock lock( m_sourceMutex );
		valid = valid && epoch == m_nSourceEpoch;
		++m_nSourceReads;
		m_nSourceBytes += ( uint64_t ) stored.size();
		if ( slot >= 0 && epoch == m_nSourceEpoch ) {
			SourceEntry &entry = m_sourceEntries[( size_t ) slot];
			if ( entry.m_bReading && entry.m_nOffset == rEntry.m_nOffset && entry.m_nBytes == rEntry.m_nBytes &&
				 entry.m_nHash == rEntry.m_nHash ) {
				if ( valid ) {
					// 128 * 4096 maximum stored tile bytes = 512 KiB.
					entry.m_stored = stored;
					entry.m_bReady = true;
					entry.m_nUse = ++m_nSourceClock;
					m_nSourceResidentBytes += ( uint64_t ) stored.size();
				} else {
					entry.m_stored = PackedByteArray();
					entry.m_bFailed = true;
					entry.m_nRetryAfter = t3dSteadyClockUsec() + 500000u;
				}
				entry.m_bReading = false;
				m_sourceReady.notifyAll();
			}
		}
	}
	if ( valid ) {
		rStored = stored;
	}
	return valid;
}

godot::Dictionary Terrain3DRasterBundleIndex::getSourceIoStats(
	void ) const {
	Terrain3DScopedLock lock( m_sourceMutex );
	godot::Dictionary	result;
	result["raster_stored_cache_bytes"] = ( int64_t ) m_nSourceResidentBytes;
	result["raster_stored_cache_hits"] = ( int64_t ) m_nSourceHits;
	result["raster_stored_cache_joins"] = ( int64_t ) m_nSourceJoins;
	result["raster_stored_file_reads"] = ( int64_t ) m_nSourceReads;
	result["raster_stored_file_bytes"] = ( int64_t ) m_nSourceBytes;
	result["raster_stored_evictions"] = ( int64_t ) m_nSourceEvictions;
	return result;
}

bool Terrain3DRasterBundleIndex::readTile(
	const uint16_t	   nId,
	PackedByteArray	  &rRaw,
	Ref< FileAccess > &rReader ) const {
	const auto it = m_entries.find( nId );
	if ( !t3dRasterTileIdValid( nId ) || it == m_entries.end() || m_path.is_empty() ) {
		return false;
	}
	const t3dRasterBundleIndexEntry_t &entry = it->second;
	const t3dMapType_t				   channel = t3dRasterTileChannel( nId );
	const uint32_t					   pixelBytes = t3dRasterTilePixelBytes( channel );
	const uint32_t					   rawBytes = t3dRasterTileRawBytes( channel );
	PackedByteArray					   content;
	if ( !readStored( entry, content, rReader ) ) {
		return false;
	}
	PackedByteArray result;
	result.resize( rawBytes );
	uint8_t *const pDst = result.ptrw();
	if ( entry.m_nEncoding == T3D_RASTER_TILE_CONSTANT ) {
		for ( uint32_t offset = 0u; offset < rawBytes; offset += pixelBytes ) {
			memcpy( pDst + offset, content.ptr(), pixelBytes );
		}
	} else if ( entry.m_nEncoding == T3D_RASTER_TILE_RAW ) {
		memcpy( pDst, content.ptr(), rawBytes );
	} else if ( entry.m_nEncoding == T3D_RASTER_TILE_RLE ) {
		if ( !t3dRasterBundleDecodeRLE( content.ptr(), entry.m_nBytes, pixelBytes, pDst ) ) {
			return false;
		}
	} else {
		return false;
	}
	rRaw = result;
	return true;
}
