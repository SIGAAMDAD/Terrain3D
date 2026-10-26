#include "data/persistence/terrain_3d_raster_hierarchy_bundle.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include <godot_cpp/classes/file_access.hpp>
#include <string.h>

using namespace godot;

static uint64_t t3dRasterHierarchyHash(
	const uint8_t *const pBytes,
	const uint32_t		 nCount ) {
	uint64_t hash = 1469598103934665603ull;
	for ( uint32_t i = 0u; i < nCount; ++i ) {
		hash = ( hash ^ pBytes[i] ) * 1099511628211ull;
	}
	return hash;
}

static void t3dRasterHierarchyEncode(
	const t3dRasterHierarchyPayload_t &rPayload,
	PackedByteArray					  &rEncoded ) {
	rEncoded.resize( 10240u );
	uint8_t *const p = rEncoded.ptrw();
	for ( uint32_t i = 0u; i < 1024u; ++i ) {
		const uint16_t h = rPayload.m_szHeight[i];
		p[i * 2u] = ( uint8_t ) h;
		p[i * 2u + 1u] = ( uint8_t ) ( h >> 8u );
		const uint32_t c = rPayload.m_szControl[i];
		for ( uint32_t k = 0u; k < 4u; ++k ) {
			p[2048u + i * 4u + k] = ( uint8_t ) ( c >> ( 8u * k ) );
		}
	}
	memcpy( p + 6144u, rPayload.m_szColor, 4096u );
}

static bool t3dRasterHierarchyDecode(
	const PackedByteArray		&rBytes,
	t3dRasterHierarchyPayload_t &rPayload ) {
	if ( rBytes.size() != 10240u ) {
		return false;
	}
	const uint8_t *const p = rBytes.ptr();
	for ( uint32_t i = 0u; i < 1024u; ++i ) {
		rPayload.m_szHeight[i] = ( uint16_t ) ( p[i * 2u] | ( ( uint16_t ) p[i * 2u + 1u] << 8u ) );
		rPayload.m_szControl[i] = ( uint32_t ) p[2048u + i * 4u] | ( ( uint32_t ) p[2048u + i * 4u + 1u] << 8u ) |
								  ( ( uint32_t ) p[2048u + i * 4u + 2u] << 16u ) |
								  ( ( uint32_t ) p[2048u + i * 4u + 3u] << 24u );
	}
	memcpy( rPayload.m_szColor, p + 6144u, 4096u );
	return true;
}

String Terrain3DRasterHierarchyBundle::getPath(
	const String &rDirectory ) {
	return rDirectory.path_join( "terrain3d_hierarchy_v1.t3rh" );
}

Error Terrain3DRasterHierarchyBundle::save(
	const String						  &rDirectory,
	const Terrain3DRasterHierarchyBuilder &rBuilder ) {
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
	const auto	  &payloads = rBuilder.getPayloads();
	const uint32_t count = ( uint32_t ) payloads.size();
	file->store_32( T3D_RASTER_HIERARCHY_MAGIC );
	file->store_32( T3D_RASTER_HIERARCHY_VERSION );
	file->store_32( T3D_REGION_MAP_SIZE );
	file->store_32( 32u );
	file->store_32( count );
	file->store_32( 0u );
	uint64_t		written = 24u;
	PackedByteArray bytes;
	for ( uint16_t id = 0u; id < 1365u; ++id ) {
		const auto it = payloads.find( id );
		if ( it == payloads.end() ) {
			continue;
		}
		t3dRasterHierarchyEncode( it->second, bytes );
		file->store_16( id );
		file->store_16( 0u );
		file->store_32( 1u ); // immutable derived generation for this publication
		file->store_64( t3dRasterHierarchyHash( bytes.ptr(), ( uint32_t ) bytes.size() ) );
		file->store_buffer( bytes );
		written += 16u + 10240u;
	}
	error = transaction.finishStaged( file, written );
	if ( error == OK ) {
		error = transaction.publish();
	}
	if ( error == OK ) {
		// Retain the v1 backup until the independently published v2 sidecar
		// succeeds. The primary production hierarchy must survive a v2 failure.
		error = saveV2( rDirectory, rBuilder );
	}
	if ( error == OK ) {
		error = transaction.commit();
	}
	if ( error != OK ) {
		( void ) transaction.rollback();
	}
	return error;
}

void Terrain3DRasterHierarchyIndex::clear(
	void ) {
	m_path = String();
	m_entries.clear();
}

Error Terrain3DRasterHierarchyIndex::open(
	const String &rDirectory ) {
	clear();
	const String			path = Terrain3DRasterHierarchyBundle::getPath( rDirectory );
	const Ref< FileAccess > file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() || file->get_length() < 24u ) {
		return ERR_FILE_NOT_FOUND;
	}
	if ( file->get_32() != T3D_RASTER_HIERARCHY_MAGIC || file->get_32() != T3D_RASTER_HIERARCHY_VERSION ||
		 file->get_32() != T3D_REGION_MAP_SIZE || file->get_32() != 32u ) {
		return ERR_FILE_CORRUPT;
	}
	const uint32_t count = file->get_32();
	if ( file->get_32() != 0u || count > 1365u ) {
		return ERR_FILE_CORRUPT;
	}
	eastl::hash_map< uint16_t, t3dRasterHierarchyIndexEntry_t > candidate;
	int															previous = -1;
	for ( uint32_t i = 0u; i < count; ++i ) {
		const uint64_t offset = file->get_position();
		if ( offset > file->get_length() || file->get_length() - offset < 16u + 10240u ) {
			return ERR_FILE_CORRUPT;
		}
		const uint16_t id = file->get_16();
		const uint16_t reserved = file->get_16();
		const uint32_t revision = file->get_32();
		const uint64_t hash = file->get_64();
		if ( id >= 1365u || ( int ) id <= previous || reserved != 0u || revision == 0u ) {
			return ERR_FILE_CORRUPT;
		}
		t3dRasterHierarchyIndexEntry_t entry;
		entry.m_nOffset = file->get_position();
		entry.m_nHash = hash;
		entry.m_nRevision = revision;
		candidate[id] = entry;
		file->seek( entry.m_nOffset + 10240u );
		previous = ( int ) id;
	}
	if ( file->get_position() != file->get_length() ) {
		return ERR_FILE_CORRUPT;
	}
	m_path = path;
	m_entries = eastl::move( candidate );
	return OK;
}

bool Terrain3DRasterHierarchyIndex::read(
	const uint8_t				 nLevel,
	const uint8_t				 nX,
	const uint8_t				 nY,
	t3dRasterHierarchyPayload_t &rPayload ) const {
	const uint16_t id = Terrain3DRasterHierarchyBuilder::nodeId( nLevel, nX, nY );
	const auto	   it = m_entries.find( id );
	if ( id == UINT16_MAX || it == m_entries.end() ) {
		return false;
	}
	const Ref< FileAccess > file = FileAccess::open( m_path, FileAccess::READ );
	if ( file.is_null() || it->second.m_nOffset > file->get_length() ||
		 file->get_length() - it->second.m_nOffset < 10240u ) {
		return false;
	}
	file->seek( it->second.m_nOffset );
	const PackedByteArray bytes = file->get_buffer( 10240u );
	if ( bytes.size() != 10240u || t3dRasterHierarchyHash( bytes.ptr(), 10240u ) != it->second.m_nHash ) {
		return false;
	}
	return t3dRasterHierarchyDecode( bytes, rPayload );
}

bool Terrain3DRasterHierarchyIndex::readNearestAncestor(
	const uint8_t				 nLevel,
	const uint8_t				 nX,
	const uint8_t				 nY,
	t3dRasterHierarchyPayload_t &rPayload,
	uint8_t						&rFoundLevel ) const {
	if ( nLevel > 5u || nX >= ( 1u << nLevel ) || nY >= ( 1u << nLevel ) ) {
		return false;
	}
	for ( int level = nLevel; level >= 0; --level ) {
		const uint8_t shift = ( uint8_t ) ( nLevel - level );
		if ( read( ( uint8_t ) level, ( uint8_t ) ( nX >> shift ), ( uint8_t ) ( nY >> shift ), rPayload ) ) {
			rFoundLevel = ( uint8_t ) level;
			return true;
		}
	}
	return false;
}

// F01 T3RH v2: explicitly encoded little-endian directory. The 34x34 height
// channel includes a one-texel same-LOD halo; categorical controls remain 32x32.
// No v1 offsets or layouts are changed.
#include <math.h>
#include <limits.h>

static void t3dHierarchyWrite32( uint8_t *p, uint32_t value ) {
	for ( uint32_t k = 0u; k < 4u; ++k ) p[k] = ( uint8_t ) ( value >> ( k * 8u ) );
}
static void t3dHierarchyWrite64( uint8_t *p, uint64_t value ) {
	for ( uint32_t k = 0u; k < 8u; ++k ) p[k] = ( uint8_t ) ( value >> ( k * 8u ) );
}
static uint32_t t3dHierarchyRead32( const uint8_t *p ) {
	return ( uint32_t ) p[0] | ( ( uint32_t ) p[1] << 8u ) |
		( ( uint32_t ) p[2] << 16u ) | ( ( uint32_t ) p[3] << 24u );
}
static uint64_t t3dHierarchyRead64( const uint8_t *p ) {
	uint64_t result = 0u;
	for ( uint32_t k = 0u; k < 8u; ++k ) result |= ( uint64_t ) p[k] << ( k * 8u );
	return result;
}
static void t3dHierarchyWriteFloat( uint8_t *p, float f ) {
	uint32_t bits;
	memcpy( &bits, &f, 4u );
	t3dHierarchyWrite32( p, bits );
}
static float t3dHierarchyReadFloat( const uint8_t *p ) {
	const uint32_t bits = t3dHierarchyRead32( p );
	float value;
	memcpy( &value, &bits, 4u );
	return value;
}
// Authenticating the fixed header prevents silent generation and range-header
// changes; only the trailing 64-bit hash field itself is excluded.
static uint64_t t3dHierarchyMetadataHash( const uint8_t *pHead, const uint8_t *pDirectory, uint32_t nDirectoryBytes ) {
	uint64_t hash = 1469598103934665603ull;
	for ( uint32_t i = 0u; i < 56u; ++i ) {
		hash = ( hash ^ pHead[i] ) * 1099511628211ull;
	}
	for ( uint32_t i = 0u; i < nDirectoryBytes; ++i ) {
		hash = ( hash ^ pDirectory[i] ) * 1099511628211ull;
	}
	return hash;
}
static bool t3dHierarchyDecodeAddress( const uint16_t nId, uint8_t &rLevel, uint8_t &rX, uint8_t &rY ) {
	uint32_t start = 0u;
	for ( uint32_t level = 0u; level <= 5u; ++level ) {
		const uint32_t width = 1u << level;
		if ( nId < start + width * width ) {
			const uint32_t ordinal = nId - start;
			rLevel = ( uint8_t ) level;
			rX = ( uint8_t ) ( ordinal % width );
			rY = ( uint8_t ) ( ordinal / width );
			return true;
		}
		start += width * width;
	}
	return false;
}
static void t3dHierarchyEncodeV2Channels(
	const Terrain3DRasterHierarchyBuilder &builder, uint16_t nId,
	const t3dRasterHierarchyPayload_t &payload,
	PackedByteArray &height, PackedByteArray &control, PackedByteArray &color ) {
	height.resize( T3D_RASTER_HIERARCHY_V2_HEIGHT_BYTES );
	control.resize( T3D_RASTER_HIERARCHY_V2_CONTROL_BYTES );
	color.resize( T3D_RASTER_HIERARCHY_V2_COLOR_BYTES );
	uint8_t *pHeight = height.ptrw();
	uint8_t *pControl = control.ptrw();
	uint8_t level = 0u, tileX = 0u, tileY = 0u;
	( void ) t3dHierarchyDecodeAddress( nId, level, tileX, tileY );
	const int maxTile = 1 << level;
	for ( int py = 0; py < 34; ++py ) {
		for ( int px = 0; px < 34; ++px ) {
			const int localX = px - 1, localY = py - 1;
			const int neighborX = ( int ) tileX + ( localX < 0 ? -1 : localX >= 32 ? 1 : 0 );
			const int neighborY = ( int ) tileY + ( localY < 0 ? -1 : localY >= 32 ? 1 : 0 );
			const bool inside = neighborX >= 0 && neighborX < maxTile && neighborY >= 0 && neighborY < maxTile;
			const t3dRasterHierarchyPayload_t *neighbor = inside ?
				builder.getPayload( level, ( uint8_t ) neighborX, ( uint8_t ) neighborY ) : nullptr;
			if ( !neighbor ) neighbor = &payload;
			const int sampleX = localX < 0 ? ( neighbor == &payload ? 0 : 31 ) :
				localX >= 32 ? ( neighbor == &payload ? 31 : 0 ) : localX;
			const int sampleY = localY < 0 ? ( neighbor == &payload ? 0 : 31 ) :
				localY >= 32 ? ( neighbor == &payload ? 31 : 0 ) : localY;
			const uint16_t encoded = neighbor->m_szHeight[sampleY * 32 + sampleX];
			pHeight[( py * 34 + px ) * 2] = ( uint8_t ) encoded;
			pHeight[( py * 34 + px ) * 2 + 1] = ( uint8_t ) ( encoded >> 8u );
		}
	}
	for ( uint32_t i = 0u; i < 1024u; ++i ) {
		t3dHierarchyWrite32( pControl + i * 4u, payload.m_szControl[i] );
	}
	memcpy( color.ptrw(), payload.m_szColor, 4096u );
}

String Terrain3DRasterHierarchyBundle::getPathV2( const String &directory ) {
	return directory.path_join( "terrain3d_hierarchy_v2.t3rh" );
}

Error Terrain3DRasterHierarchyBundle::saveV2(
	const String &directory, const Terrain3DRasterHierarchyBuilder &builder ) {
	if ( directory.is_empty() ) return ERR_INVALID_PARAMETER;
	const auto &payloads = builder.getPayloads();
	if ( payloads.size() > 1365u ) return ERR_INVALID_PARAMETER;
	const uint64_t dataStart = T3D_RASTER_HIERARCHY_V2_HEADER_BYTES +
		( uint64_t ) T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES * 1365u;
	PackedByteArray directoryBytes;
	directoryBytes.resize( T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES * 1365u );
	uint8_t *const pDirectory = directoryBytes.ptrw();
	memset( pDirectory, 0, ( size_t ) directoryBytes.size() );
	uint64_t cursor = dataStart;
	uint32_t present = 0u;
	PackedByteArray height, control, color;
	for ( uint16_t id = 0u; id < 1365u; ++id ) {
		const auto it = payloads.find( id );
		if ( it == payloads.end() ) continue;
		const t3dRasterHierarchyNode_t *node = builder.getNodeById( id );
		if ( !node || !isfinite( node->m_fMinHeight ) || !isfinite( node->m_fMaxHeight ) ||
			!isfinite( node->m_fMaxHeightErrorM ) || node->m_fMinHeight > node->m_fMaxHeight ||
			node->m_fMaxHeightErrorM < 0.0f || node->m_nLeafCount == 0u ) return ERR_INVALID_DATA;
		uint8_t level = 0u, x = 0u, y = 0u;
		if ( !t3dHierarchyDecodeAddress( id, level, x, y ) ) return ERR_INVALID_DATA;
		t3dHierarchyEncodeV2Channels( builder, id, it->second, height, control, color );
		uint8_t *const e = pDirectory + ( size_t ) id * T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES;
		t3dHierarchyWrite32( e, 1u );
		e[4] = level; e[5] = node->m_nChildMask; e[6] = node->m_nCoverageMask; e[7] = 1u;
		t3dHierarchyWrite32( e + 8, node->m_nRevision );
		t3dHierarchyWrite32( e + 12, builder.getGeneration() );
		t3dHierarchyWrite32( e + 16, node->m_nLeafCount );
		t3dHierarchyWriteFloat( e + 20, node->m_fMinHeight );
		t3dHierarchyWriteFloat( e + 24, node->m_fMaxHeight );
		t3dHierarchyWriteFloat( e + 28, node->m_fMaxHeightErrorM );
		const PackedByteArray *channels[3] = { &height, &control, &color };
		const uint32_t offsetPositions[3] = { 32u, 52u, 72u };
		for ( uint32_t c = 0u; c < 3u; ++c ) {
			t3dHierarchyWrite64( e + offsetPositions[c], cursor );
			t3dHierarchyWrite32( e + offsetPositions[c] + 8u, ( uint32_t ) channels[c]->size() );
			t3dHierarchyWrite64( e + offsetPositions[c] + 12u,
				t3dRasterHierarchyHash( channels[c]->ptr(), ( uint32_t ) channels[c]->size() ) );
			cursor += ( uint64_t ) channels[c]->size();
		}
		++present;
	}
	PackedByteArray header;
	header.resize( T3D_RASTER_HIERARCHY_V2_HEADER_BYTES );
	uint8_t *const h = header.ptrw();
	memset( h, 0, ( size_t ) header.size() );
	t3dHierarchyWrite32( h, T3D_RASTER_HIERARCHY_MAGIC );
	t3dHierarchyWrite32( h + 4, T3D_RASTER_HIERARCHY_VERSION_V2 );
	t3dHierarchyWrite32( h + 8, T3D_REGION_MAP_SIZE );
	t3dHierarchyWrite32( h + 12, 32u );
	t3dHierarchyWrite32( h + 16, 1365u );
	t3dHierarchyWrite32( h + 20, T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES );
	t3dHierarchyWrite32( h + 24, present );
	t3dHierarchyWrite32( h + 28, builder.getGeneration() );
	t3dHierarchyWrite64( h + 32, T3D_RASTER_HIERARCHY_V2_HEADER_BYTES );
	t3dHierarchyWrite64( h + 40, dataStart );
	t3dHierarchyWrite64( h + 48, cursor );
	t3dHierarchyWrite64( h + 56,
		t3dHierarchyMetadataHash( h, directoryBytes.ptr(), ( uint32_t ) directoryBytes.size() ) );
	const String path = getPathV2( directory );
	Terrain3DFilePublication transaction( path, path + String( ".staged" ), path + String( ".previous" ) );
	Ref< FileAccess > file;
	Error err = transaction.openStaged( file );
	if ( err != OK ) return err;
	file->store_buffer( header );
	file->store_buffer( directoryBytes );
	for ( uint16_t id = 0u; id < 1365u; ++id ) {
		const auto it = payloads.find( id );
		if ( it == payloads.end() ) continue;
		t3dHierarchyEncodeV2Channels( builder, id, it->second, height, control, color );
		file->store_buffer( height );
		file->store_buffer( control );
		file->store_buffer( color );
	}
	err = transaction.finishStaged( file, cursor );
	if ( err == OK ) err = transaction.publish();
	if ( err == OK ) err = transaction.commit();
	if ( err != OK ) transaction.rollback();
	return err;
}

void Terrain3DRasterHierarchyIndexV2::clear( void ) {
	m_path = String();
	m_entries.clear();
	m_nGeneration = 0u;
}

const t3dRasterHierarchyV2Node_t *Terrain3DRasterHierarchyIndexV2::getNode( const uint16_t id ) const {
	const auto it = m_entries.find( id );
	return it == m_entries.end() ? nullptr : &it->second;
}

Error Terrain3DRasterHierarchyIndexV2::open( const String &directory ) {
	clear();
	const String path = Terrain3DRasterHierarchyBundle::getPathV2( directory );
	const Ref< FileAccess > file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() || file->get_length() < T3D_RASTER_HIERARCHY_V2_HEADER_BYTES ) return ERR_FILE_NOT_FOUND;
	const PackedByteArray head = file->get_buffer( T3D_RASTER_HIERARCHY_V2_HEADER_BYTES );
	if ( head.size() != T3D_RASTER_HIERARCHY_V2_HEADER_BYTES ) return ERR_FILE_CORRUPT;
	const uint8_t *const h = head.ptr();
	const uint64_t dataStart = T3D_RASTER_HIERARCHY_V2_HEADER_BYTES +
		( uint64_t ) T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES * 1365u;
	if ( t3dHierarchyRead32( h ) != T3D_RASTER_HIERARCHY_MAGIC ||
		t3dHierarchyRead32( h + 4 ) != T3D_RASTER_HIERARCHY_VERSION_V2 ||
		t3dHierarchyRead32( h + 8 ) != T3D_REGION_MAP_SIZE || t3dHierarchyRead32( h + 12 ) != 32u ||
		t3dHierarchyRead32( h + 16 ) != 1365u ||
		t3dHierarchyRead32( h + 20 ) != T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES ||
		t3dHierarchyRead32( h + 24 ) > 1365u || t3dHierarchyRead32( h + 28 ) == 0u ||
		t3dHierarchyRead64( h + 32 ) != T3D_RASTER_HIERARCHY_V2_HEADER_BYTES ||
		t3dHierarchyRead64( h + 40 ) != dataStart || t3dHierarchyRead64( h + 48 ) != file->get_length() ) {
		return ERR_FILE_CORRUPT;
	}
	const PackedByteArray directoryBytes = file->get_buffer( T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES * 1365u );
	if ( directoryBytes.size() != T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES * 1365u ||
		t3dHierarchyMetadataHash( h, directoryBytes.ptr(), ( uint32_t ) directoryBytes.size() ) !=
			t3dHierarchyRead64( h + 56 ) ) return ERR_FILE_CORRUPT;
	const uint8_t *const p = directoryBytes.ptr();
	eastl::hash_map< uint16_t, t3dRasterHierarchyV2Node_t > candidate;
	uint64_t nextPayload = dataStart;
	uint32_t populated = 0u;
	for ( uint16_t id = 0u; id < 1365u; ++id ) {
		const uint8_t *const e = p + ( size_t ) id * T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES;
		if ( t3dHierarchyRead32( e ) == 0u ) {
			for ( uint32_t k = 4u; k < T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES; ++k ) {
				if ( e[k] != 0u ) return ERR_FILE_CORRUPT;
			}
			continue;
		}
		uint8_t level = 0u, x = 0u, y = 0u;
		if ( t3dHierarchyRead32( e ) != 1u || !t3dHierarchyDecodeAddress( id, level, x, y ) ||
			e[4] != level || e[6] != 7u || e[7] != 1u || ( e[5] & ~15u ) != 0u ||
			( level == 5u && e[5] != 0u ) ) return ERR_FILE_CORRUPT;
		for ( uint32_t k = 92u; k < T3D_RASTER_HIERARCHY_V2_ENTRY_BYTES; ++k ) {
			if ( e[k] != 0u ) return ERR_FILE_CORRUPT;
		}
		t3dRasterHierarchyV2Node_t node;
		node.m_nLevel = level;
		node.m_nChildMask = e[5];
		node.m_nCoverageMask = e[6];
		node.m_nRevision = t3dHierarchyRead32( e + 8 );
		node.m_nGeneration = t3dHierarchyRead32( e + 12 );
		node.m_nLeafCount = t3dHierarchyRead32( e + 16 );
		node.m_fMinHeight = t3dHierarchyReadFloat( e + 20 );
		node.m_fMaxHeight = t3dHierarchyReadFloat( e + 24 );
		node.m_fMaxHeightErrorM = t3dHierarchyReadFloat( e + 28 );
		if ( node.m_nRevision == 0u || node.m_nGeneration != t3dHierarchyRead32( h + 28 ) ||
			node.m_nLeafCount == 0u || node.m_nLeafCount > 1024u ||
			!isfinite( node.m_fMinHeight ) || !isfinite( node.m_fMaxHeight ) ||
			!isfinite( node.m_fMaxHeightErrorM ) || node.m_fMinHeight > node.m_fMaxHeight ||
			node.m_fMaxHeightErrorM < 0.0f ) return ERR_FILE_CORRUPT;
		uint64_t *offsets[3] = { &node.m_nHeightOffset, &node.m_nControlOffset, &node.m_nColorOffset };
		uint64_t *hashes[3] = { &node.m_nHeightHash, &node.m_nControlHash, &node.m_nColorHash };
		const uint32_t fields[3] = { 32u, 52u, 72u };
		const uint32_t lengths[3] = { T3D_RASTER_HIERARCHY_V2_HEIGHT_BYTES,
			T3D_RASTER_HIERARCHY_V2_CONTROL_BYTES, T3D_RASTER_HIERARCHY_V2_COLOR_BYTES };
		for ( uint32_t c = 0u; c < 3u; ++c ) {
			*offsets[c] = t3dHierarchyRead64( e + fields[c] );
			*hashes[c] = t3dHierarchyRead64( e + fields[c] + 12u );
			if ( *offsets[c] != nextPayload || t3dHierarchyRead32( e + fields[c] + 8u ) != lengths[c] ||
				*hashes[c] == 0u || nextPayload > file->get_length() ||
				( uint64_t ) lengths[c] > file->get_length() - nextPayload ) return ERR_FILE_CORRUPT;
			nextPayload += lengths[c];
		}
		candidate[id] = node;
		++populated;
	}
	if ( nextPayload != file->get_length() || populated != t3dHierarchyRead32( h + 24 ) ) return ERR_FILE_CORRUPT;
	// Validate sparse topology and conservative bounds using directory alone.
	for ( const auto &it : candidate ) {
		const uint16_t id = it.first;
		const t3dRasterHierarchyV2Node_t &node = it.second;
		uint8_t level = 0u, x = 0u, y = 0u;
		( void ) t3dHierarchyDecodeAddress( id, level, x, y );
		uint8_t mask = 0u;
		uint32_t leaves = 0u;
		for ( uint8_t dy = 0u; level < 5u && dy < 2u; ++dy ) {
			for ( uint8_t dx = 0u; dx < 2u; ++dx ) {
				const uint16_t child = Terrain3DRasterHierarchyBuilder::nodeId(
					( uint8_t ) ( level + 1 ), ( uint8_t ) ( x * 2u + dx ), ( uint8_t ) ( y * 2u + dy ) );
				const auto cit = candidate.find( child );
				if ( cit == candidate.end() ) continue;
				mask |= ( uint8_t ) ( 1u << ( dy * 2u + dx ) );
				leaves += cit->second.m_nLeafCount;
				if ( node.m_fMinHeight > cit->second.m_fMinHeight ||
					node.m_fMaxHeight < cit->second.m_fMaxHeight ||
					node.m_fMaxHeightErrorM < cit->second.m_fMaxHeightErrorM ) return ERR_FILE_CORRUPT;
			}
		}
		if ( mask != node.m_nChildMask || ( level < 5u && leaves != node.m_nLeafCount ) ||
			( level == 5u && node.m_nLeafCount != 1u ) ) return ERR_FILE_CORRUPT;
	}
	m_entries = eastl::move( candidate );
	m_nGeneration = t3dHierarchyRead32( h + 28 );
	m_path = path;
	return OK;
}

bool Terrain3DRasterHierarchyIndexV2::read( const uint16_t id, t3dRasterHierarchyPayload_t &out ) const {
	const t3dRasterHierarchyV2Node_t *node = getNode( id );
	if (!node) return false;
	const Ref< FileAccess > file = FileAccess::open( m_path, FileAccess::READ );
	if ( file.is_null() ) return false;
	const uint64_t offsets[3] = { node->m_nHeightOffset, node->m_nControlOffset, node->m_nColorOffset };
	const uint32_t lengths[3] = { T3D_RASTER_HIERARCHY_V2_HEIGHT_BYTES,
		T3D_RASTER_HIERARCHY_V2_CONTROL_BYTES, T3D_RASTER_HIERARCHY_V2_COLOR_BYTES };
	const uint64_t hashes[3] = { node->m_nHeightHash, node->m_nControlHash, node->m_nColorHash };
	PackedByteArray bytes[3];
	for ( uint32_t c = 0u; c < 3u; ++c ) {
		if ( offsets[c] > file->get_length() || lengths[c] > file->get_length() - offsets[c] ) return false;
		file->seek( offsets[c] );
		bytes[c] = file->get_buffer( lengths[c] );
		if ( bytes[c].size() != lengths[c] || t3dRasterHierarchyHash( bytes[c].ptr(), lengths[c] ) != hashes[c] )
			return false;
	}
	const uint8_t *height = bytes[0].ptr();
	const uint8_t *control = bytes[1].ptr();
	for ( uint32_t py = 0u; py < 32u; ++py ) {
		for ( uint32_t px = 0u; px < 32u; ++px ) {
			const uint32_t at = py * 32u + px;
			const uint32_t src = ( ( py + 1u ) * 34u + px + 1u ) * 2u;
			out.m_szHeight[at] = ( uint16_t ) ( height[src] | ( ( uint16_t ) height[src + 1u] << 8u ) );
			out.m_szControl[at] = t3dHierarchyRead32( control + at * 4u );
		}
	}
	memcpy( out.m_szColor, bytes[2].ptr(), 4096u );
	return true;
}

// F02: isolated worker read; no hierarchy dictionary or owner-thread state is
// accessed. Exact v2 offsets and hashes were frozen by the index-only open.
bool t3dRasterHierarchyReadV2Tile( const godot::String &path,
                                   const t3dRasterHierarchyV2Node_t &node,
                                   uint8_t *target, const uint32_t capacity ) {
    if ( path.is_empty() || !target || capacity < T3D_RASTER_HIERARCHY_V2_TILE_BYTES ||
         node.m_nRevision == 0u || node.m_nGeneration == 0u || node.m_nCoverageMask != 7u ) return false;
    const godot::Ref< godot::FileAccess > file = godot::FileAccess::open( path, godot::FileAccess::READ );
    if ( file.is_null() ) return false;
    const uint64_t offsets[3] = { node.m_nHeightOffset, node.m_nControlOffset, node.m_nColorOffset };
    const uint64_t hashes[3] = { node.m_nHeightHash, node.m_nControlHash, node.m_nColorHash };
    const uint32_t sizes[3] = { T3D_RASTER_HIERARCHY_V2_HEIGHT_BYTES,
        T3D_RASTER_HIERARCHY_V2_CONTROL_BYTES, T3D_RASTER_HIERARCHY_V2_COLOR_BYTES };
    uint32_t cursor = 0u;
    for ( uint32_t c = 0u; c < 3u; ++c ) {
        if ( offsets[c] > file->get_length() || sizes[c] > file->get_length() - offsets[c] ||
             ( c != 0u && offsets[c] != offsets[c - 1u] + sizes[c - 1u] ) ) return false;
        file->seek( offsets[c] );
        const godot::PackedByteArray data = file->get_buffer( sizes[c] );
        if ( ( uint32_t ) data.size() != sizes[c] ||
             t3dRasterHierarchyHash( data.ptr(), sizes[c] ) != hashes[c] ) return false;
        memcpy( target + cursor, data.ptr(), sizes[c] );
        cursor += sizes[c];
    }
    return true;
}
