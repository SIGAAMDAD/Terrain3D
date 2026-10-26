#include "streaming/io/terrain_3d_shared_stream_pack.h"
#include "diagnostics/core/terrain_3d_streaming_profiler.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include "data/runtime/terrain_3d_runtime_page_format.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <string.h>
#include <atomic>
#include <EASTL/sort.h>
#include <EASTL/hash_map.h>

namespace
{
static const uint32_t MAGIC = 0x50533354u; // T3SP
static const uint32_t VERSION = 1u;
static const uint32_t HEADER_BYTES = 40u;
static const uint32_t MAX_RECORDS = 4096u;
static const uint64_t MAX_INDEX_BYTES = 4u * 1024u * 1024u;
static uint64_t		  hashBytes(
		  const uint8_t *pData,
		  size_t		 nBytes ) {
	uint64_t n = 14695981039346656037ull;
	for ( size_t i = 0u; i < nBytes; ++i ) {
		n = ( n ^ pData[i] ) * 1099511628211ull;
	}
	return n;
}
static void appendU32(
	PackedByteArray &r,
	uint32_t		 n ) {
	for ( int i = 0; i < 4; ++i ) {
		r.append( ( uint8_t ) ( n >> ( i * 8 ) ) );
	}
}
static void appendU64(
	PackedByteArray &r,
	uint64_t		 n ) {
	for ( int i = 0; i < 8; ++i ) {
		r.append( ( uint8_t ) ( n >> ( i * 8 ) ) );
	}
}
static bool readU32(
	const uint8_t *p,
	size_t		   nSize,
	size_t		  &rCursor,
	uint32_t	  &r ) {
	if ( rCursor > nSize || nSize - rCursor < 4u ) {
		return false;
	}
	r = 0u;
	for ( int i = 0; i < 4; ++i ) {
		r |= ( uint32_t ) p[rCursor++] << ( i * 8 );
	}
	return true;
}
static bool readU64(
	const uint8_t *p,
	size_t		   nSize,
	size_t		  &rCursor,
	uint64_t	  &r ) {
	if ( rCursor > nSize || nSize - rCursor < 8u ) {
		return false;
	}
	r = 0u;
	for ( int i = 0; i < 8; ++i ) {
		r |= ( uint64_t ) p[rCursor++] << ( i * 8 );
	}
	return true;
}
static uint64_t align4k(
	uint64_t n ) {
	return ( n + 4095u ) & ~uint64_t( 4095u );
}
static bool matchingPayload(
	const t3dSharedStreamPackRecord_t &a,
	const t3dSharedStreamPackRecord_t &b ) {
	return a.m_key.m_nFormat == b.m_key.m_nFormat && a.m_key.m_nCompression == b.m_key.m_nCompression &&
		   a.m_key.m_nChannel == b.m_key.m_nChannel && a.m_key.m_nChecksum == b.m_key.m_nChecksum &&
		   a.m_key.m_nDecodedBytes == b.m_key.m_nDecodedBytes && a.m_bytes.size() == b.m_bytes.size() &&
		   memcmp( a.m_bytes.ptr(), b.m_bytes.ptr(), ( size_t ) a.m_bytes.size() ) == 0;
}
static bool verifyRecord(
	const t3dSharedStreamPackRecord_t &r ) {
	if ( r.m_key.m_path.is_empty() || r.m_key.m_nSourceSignature == 0u || r.m_bytes.is_empty() ||
		 r.m_bytes.size() != ( int64_t ) r.m_key.m_nStoredBytes || r.m_key.m_nDecodedBytes == 0u ||
		 r.m_key.m_nStoredBytes > T3D_RUNTIME_PAGE_MAX_BYTES || r.m_key.m_nDecodedBytes > T3D_RUNTIME_PAGE_MAX_BYTES ) {
		return false;
	}
	if ( r.m_key.m_nCompression == TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE ) {
		return r.m_key.m_nStoredBytes == r.m_key.m_nDecodedBytes &&
			   t3dRuntimePageChecksum( r.m_bytes.ptr(), r.m_bytes.size() ) == r.m_key.m_nChecksum;
	}
	if ( r.m_key.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE ) {
		return false;
	}
	uint32_t		written = 0u;
	PackedByteArray decoded;
	decoded.resize( r.m_key.m_nDecodedBytes );
	if ( decoded.size() != ( int64_t ) r.m_key.m_nDecodedBytes ) {
		return false;
	}
	const uint8_t *const p = r.m_bytes.ptr();
	uint8_t *const		 q = decoded.ptrw();
	for ( uint32_t pos = 0u; pos < r.m_key.m_nStoredBytes; ) {
		if ( r.m_key.m_nStoredBytes - pos < 2u ) {
			return false;
		}
		const uint32_t run = p[pos++];
		const uint8_t  value = p[pos++];
		if ( run == 0u || run > r.m_key.m_nDecodedBytes - written ) {
			return false;
		}
		memset( q + written, value, run );
		written += run;
	}
	return written == r.m_key.m_nDecodedBytes && t3dRuntimePageChecksum( q, written ) == r.m_key.m_nChecksum;
}
static bool writeZeros(
	const Ref< FileAccess > &rFile,
	uint64_t				 nTarget ) {
	static const uint8_t zeros[4096] = {};
	while ( rFile->get_position() < nTarget ) {
		const uint64_t remaining = nTarget - rFile->get_position();
		const uint32_t amount = remaining < sizeof( zeros ) ? ( uint32_t ) remaining : sizeof( zeros );
		rFile->store_buffer( zeros, amount );
	}
	return rFile->get_position() == nTarget;
}
// Two independently validated commit slots. A damaged/new selector can never
// hide the preceding committed immutable pair. Generation files are not
// modified after the selector is published.
#ifdef T3D_TESTS_ENABLED
static std::atomic< uint32_t > g_publicationFault{ 0u };
#endif
static bool publicationFault( uint32_t stage ) {
#ifdef T3D_TESTS_ENABLED
	return g_publicationFault.load( std::memory_order_relaxed ) == stage;
#else
	( void ) stage;
	return false;
#endif
}
static const uint32_t COMMIT_MAGIC = 0x43503354u; // T3PC
static const uint32_t COMMIT_VERSION = 1u;
static String generationPath( const String &base, uint64_t sequence ) {
	return base + String( ".gen" ) + String::num_int64( ( int64_t ) sequence );
}
static String commitPath( const String &base, uint32_t slot ) {
	return base + ( slot == 0u ? String( ".commit0" ) : String( ".commit1" ) );
}
static bool readCommitPath( const String &path, uint64_t &sequence, uint64_t &indexSignature ) {
	const Ref< FileAccess > file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() || file->get_length() != 32u ) {
		return false;
	}
	const PackedByteArray bytes = file->get_buffer( 32u );
	if ( bytes.size() != 32 ) {
		return false;
	}
	size_t pos = 0u;
	uint32_t magic = 0u, version = 0u;
	uint64_t checksum = 0u;
	if ( !readU32( bytes.ptr(), 32u, pos, magic ) || !readU32( bytes.ptr(), 32u, pos, version ) ||
		 !readU64( bytes.ptr(), 32u, pos, sequence ) || !readU64( bytes.ptr(), 32u, pos, indexSignature ) ||
		 !readU64( bytes.ptr(), 32u, pos, checksum ) || magic != COMMIT_MAGIC || version != COMMIT_VERSION ||
		 sequence == 0u || sequence > ( uint64_t ) INT64_MAX || checksum != hashBytes( bytes.ptr(), 24u ) ) {
		return false;
	}
	return true;
}
static bool removeUncommittedFile( const String &path ) {
	return !FileAccess::file_exists( path ) || DirAccess::remove_absolute( path ) == OK;
}
static bool prepareUncommittedGeneration( const String &base, uint64_t sequence ) {
	const String path = generationPath( base, sequence );
	// This sequence exceeds every active/backup selector: no published reader
	// can refer to these files. Retire abandoned cold staging from a prior crash.
	return removeUncommittedFile( path + String( ".staged" ) ) &&
		removeUncommittedFile( path + String( ".previous" ) ) &&
		removeUncommittedFile( path + String( ".t3pm.tmp" ) ) &&
		removeUncommittedFile( path + String( ".t3pm.bak" ) ) &&
		removeUncommittedFile( path + String( ".t3pm" ) ) &&
		removeUncommittedFile( path );
}
static bool readPackageSignature( const String &rPath, uint64_t &signature ) {
	const Ref< FileAccess > file = FileAccess::open( rPath, FileAccess::READ );
	if ( file.is_null() || file->get_length() < HEADER_BYTES ) {
		return false;
	}
	const PackedByteArray header = file->get_buffer( HEADER_BYTES );
	if ( header.size() != HEADER_BYTES ) {
		return false;
	}
	size_t pos = 32u;
	return readU64( header.ptr(), HEADER_BYTES, pos, signature );
}
static bool publishCommit( const String &base, uint64_t sequence, uint64_t signature ) {
	PackedByteArray commit;
	appendU32( commit, COMMIT_MAGIC );
	appendU32( commit, COMMIT_VERSION );
	appendU64( commit, sequence );
	appendU64( commit, signature );
	appendU64( commit, hashBytes( commit.ptr(), ( size_t ) commit.size() ) );
 	const String path = commitPath( base, ( uint32_t ) ( sequence & 1u ) );
	const String previous = path + String( ".previous" );
	if ( FileAccess::file_exists( previous ) && FileAccess::file_exists( path ) ) {
		uint64_t oldSequence = 0u, oldSignature = 0u;
		uint64_t publishedSequence = 0u, publishedSignature = 0u;
		if ( !readCommitPath( previous, oldSequence, oldSignature ) ||
			 !readCommitPath( path, publishedSequence, publishedSignature ) ||
			 DirAccess::remove_absolute( previous ) != OK ) {
			return false;
		}
	}
	Terrain3DFilePublication publication( path, path + String( ".staged" ), path + String( ".previous" ) );
	if ( publication.stagePacked( commit ) != OK || publication.publish() != OK ) {
		return false;
	}
	if ( publicationFault( 5u ) ) {
		( void ) publication.rollback();
		return false;
	}
	// The other slot still selects the old generation even while the selected
	// slot's two renames are in flight. Never clean up generation files here.
	return publication.commit() == OK;
}

}

bool Terrain3DSharedStreamPack::buildRecords(
	const String									   &rPath,
	const eastl::vector< t3dSharedStreamPackRecord_t > &rRecords ) {
	if ( rPath.is_empty() || rRecords.empty() || rRecords.size() > MAX_RECORDS ) {
		return false;
	}
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_H05_PACK_DEDUP );
	Terrain3DStreamingProfileWorkScope dedupWork( T3D_SP_H05_PACK_DEDUP );
	// Stable path/channel order makes repeated cooks byte-identical.
	eastl::vector< t3dSharedStreamPackRecord_t > sorted = rRecords;
	eastl::sort( sorted.begin(),
				 sorted.end(),
				 []( const t3dSharedStreamPackRecord_t &a, const t3dSharedStreamPackRecord_t &b ) {
					 if ( a.m_key.m_path != b.m_key.m_path ) {
						 return a.m_key.m_path < b.m_key.m_path;
					 }
					 if ( a.m_key.m_nSourceSignature != b.m_key.m_nSourceSignature ) {
						 return a.m_key.m_nSourceSignature < b.m_key.m_nSourceSignature;
					 }
					 if ( a.m_key.m_nOffset != b.m_key.m_nOffset ) {
						 return a.m_key.m_nOffset < b.m_key.m_nOffset;
					 }
					 return a.m_key.m_nChannel < b.m_key.m_nChannel;
				 } );
	// The single-file writer publishes to its given path; baked pages use an unpublished generation path.
	Terrain3DFilePublication publish( rPath, rPath + String( ".staged" ), rPath + String( ".previous" ) );
	Ref< FileAccess >		 file;
	if ( publish.openStaged( file ) != OK || file.is_null() ) {
		return false;
	}
	if ( !writeZeros( file, HEADER_BYTES ) ) {
		( void ) publish.rollback();
		return false;
	}
	eastl::vector< uint64_t > offsets;
	offsets.resize( sorted.size(), 0u );
	eastl::vector< uint64_t > hashes;
	hashes.resize( sorted.size(), 0u );
	// Collision buckets are cold-cook only. Compare actual bytes and format
	// within each matching hash bucket; no change to deduplication identity.
	eastl::hash_map< uint64_t, eastl::vector< size_t > > payloadByHash;
	for ( size_t i = 0u; i < sorted.size(); ++i ) {
		const t3dSharedStreamPackRecord_t &r = sorted[i];
		if ( !verifyRecord( r ) ) {
			file.unref();
			( void ) publish.rollback();
			return false;
		}
		hashes[i] = hashBytes( r.m_bytes.ptr(), ( size_t ) r.m_bytes.size() );
		const auto found = payloadByHash.find( hashes[i] );
		if ( found != payloadByHash.end() ) {
			for ( size_t j : found->second ) {
				dedupWork.add();
				if ( matchingPayload( sorted[i], sorted[j] ) ) {
					offsets[i] = offsets[j];
					break;
				}
			}
		}
		if ( offsets[i] != 0u ) {
			continue;
		}
		const uint64_t offset = align4k( file->get_position() );
		if ( offset > UINT32_MAX || !writeZeros( file, offset ) ) {
			file.unref();
			( void ) publish.rollback();
			return false;
		}
		offsets[i] = offset;
		file->store_buffer( r.m_bytes );
		if ( file->get_error() != OK ) {
			file.unref();
			( void ) publish.rollback();
			return false;
		}
		payloadByHash[hashes[i]].push_back( i );
	}
	PackedByteArray index;
	for ( size_t i = 0u; i < sorted.size(); ++i ) {
		const t3dSharedStreamPackRecord_t &r = sorted[i];
		const CharString				   path = r.m_key.m_path.utf8();
		if ( path.length() < 1 || path.length() > 1024 ) {
			file.unref();
			( void ) publish.rollback();
			return false;
		}
		appendU32( index, ( uint32_t ) path.length() );
		appendU64( index, r.m_key.m_nSourceSignature );
		appendU64( index, offsets[i] );
		appendU64( index, hashes[i] );
		appendU32( index, r.m_key.m_nOffset );
		appendU32( index, r.m_key.m_nStoredBytes );
		appendU32( index, r.m_key.m_nDecodedBytes );
		appendU32( index, r.m_key.m_nChecksum );
		appendU32( index, r.m_key.m_nCompression );
		appendU32( index, r.m_key.m_nFormat );
		appendU32( index, r.m_key.m_nChannel );
		for ( int n = 0; n < path.length(); ++n ) {
			index.append( ( uint8_t ) path.get_data()[n] );
		}
		if ( index.size() > ( int64_t ) MAX_INDEX_BYTES ) {
			file.unref();
			( void ) publish.rollback();
			return false;
		}
	}
	const uint64_t indexOffset = file->get_position();
	file->store_buffer( index );
	if ( file->get_error() != OK ) {
		file.unref();
		( void ) publish.rollback();
		return false;
	}
	PackedByteArray header;
	appendU32( header, MAGIC );
	appendU32( header, VERSION );
	appendU32( header, ( uint32_t ) sorted.size() );
	appendU32( header, 0u );
	appendU64( header, indexOffset );
	appendU64( header, ( uint64_t ) index.size() );
	appendU64( header, hashBytes( index.ptr(), ( size_t ) index.size() ) );
	if ( header.size() != HEADER_BYTES ) {
		file.unref();
		( void ) publish.rollback();
		return false;
	}
	file->seek( 0 );
	file->store_buffer( header );
	file->flush();
	const bool valid = file->get_error() == OK;
	file.unref();
	if ( !valid ) {
		( void ) publish.rollback();
		return false;
	}
	if ( publish.publish() != OK ) {
		return false;
	}
	// Only the subsequent verified two-file commit marker makes this
	// generation visible to logical-path readers.
	return publish.commit() == OK;
}

bool Terrain3DSharedStreamPack::open(
	const String &rPath ) {
	close();
	uint64_t sequence[4] = {}, signatures[4] = {};
	bool valid[4] = {};
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		const String selector = commitPath( rPath, i & 1u ) + ( i >= 2u ? String( ".previous" ) : String() );
		valid[i] = readCommitPath( selector, sequence[i], signatures[i] );
	}
	for ( uint32_t n = 0u; n < 4u; ++n ) {
		int best = -1;
		for ( uint32_t i = 0u; i < 4u; ++i ) {
			if ( valid[i] && ( best < 0 || sequence[i] > sequence[best] ) ) {
				best = ( int ) i;
			}
		}
		if ( best < 0 ) {
			break;
		}
		valid[best] = false;
		const String path = generationPath( rPath, sequence[best] );
		uint64_t signature = 0u;
		if ( readPackageSignature( path, signature ) && signature == signatures[best] && openDirect( path ) ) {
			return true;
		}
	}
	// Existing fixed-path packages remain readable without a selector.
	return openDirect( rPath );
}

bool Terrain3DSharedStreamPack::openDirect(
	const String &rPath ) {
	close();
	Ref< FileAccess > file = FileAccess::open( rPath, FileAccess::READ );
	if ( file.is_null() || file->get_length() < HEADER_BYTES ) {
		return false;
	}
	const uint64_t		  length = file->get_length();
	const PackedByteArray header = file->get_buffer( HEADER_BYTES );
	if ( header.size() != HEADER_BYTES ) {
		return false;
	}
	size_t				 pos = 0u;
	uint32_t			 magic = 0u, version = 0u, count = 0u, reserved = 0u;
	uint64_t			 indexOffset = 0u, indexBytes = 0u, signature = 0u;
	const uint8_t *const h = header.ptr();
	if ( !readU32( h, HEADER_BYTES, pos, magic ) || !readU32( h, HEADER_BYTES, pos, version ) ||
		 !readU32( h, HEADER_BYTES, pos, count ) || !readU32( h, HEADER_BYTES, pos, reserved ) ||
		 !readU64( h, HEADER_BYTES, pos, indexOffset ) || !readU64( h, HEADER_BYTES, pos, indexBytes ) ||
		 !readU64( h, HEADER_BYTES, pos, signature ) || magic != MAGIC || version != VERSION || reserved != 0u ||
		 count == 0u || count > MAX_RECORDS || indexBytes > MAX_INDEX_BYTES || indexOffset < HEADER_BYTES ||
		 indexOffset > length || indexBytes > length - indexOffset ) {
		return false;
	}
	file->seek( indexOffset );
	PackedByteArray bytes = file->get_buffer( indexBytes );
	if ( bytes.size() != ( int64_t ) indexBytes || hashBytes( bytes.ptr(), ( size_t ) indexBytes ) != signature ) {
		return false;
	}
	const uint8_t *const p = bytes.ptr();
	pos = 0u;
	eastl::vector< Item > items;
	items.reserve( count );
	for ( uint32_t i = 0u; i < count; ++i ) {
		uint32_t pathLength = 0u;
		Item	 item;
		if ( !readU32( p, indexBytes, pos, pathLength ) || pathLength == 0u || pathLength > 1024u ||
			 !readU64( p, indexBytes, pos, item.m_nSourceSignature ) ||
			 !readU64( p, indexBytes, pos, item.m_nDataOffset ) || !readU64( p, indexBytes, pos, item.m_nHash ) ||
			 !readU32( p, indexBytes, pos, item.m_nSourceOffset ) ||
			 !readU32( p, indexBytes, pos, item.m_nStoredBytes ) ||
			 !readU32( p, indexBytes, pos, item.m_nDecodedBytes ) || !readU32( p, indexBytes, pos, item.m_nChecksum ) ||
			 !readU32( p, indexBytes, pos, item.m_nCompression ) || !readU32( p, indexBytes, pos, item.m_nFormat ) ||
			 !readU32( p, indexBytes, pos, item.m_nChannel ) || pos > indexBytes || pathLength > indexBytes - pos ||
			 item.m_nStoredBytes == 0u || item.m_nDataOffset < HEADER_BYTES || item.m_nDataOffset > indexOffset ||
			 item.m_nStoredBytes > indexOffset - item.m_nDataOffset ) {
			return false;
		}
		item.m_path = String::utf8( ( const char * ) ( p + pos ), pathLength );
		pos += pathLength;
		items.push_back( item );
	}
	if ( pos != indexBytes ) {
		return false;
	}
	// T3PM v1 accompanies one-way migrations. An optional sidecar makes the
	// package authoritative even after the original cooked pages are retired.
	eastl::vector< PageMeta > pages;
	const String			  metadataPath = rPath + String( ".t3pm" );
	if ( !FileAccess::file_exists( metadataPath ) ) {
		return false;
	}
	{
		Ref< FileAccess > meta = FileAccess::open( metadataPath, FileAccess::READ );
		if ( meta.is_null() || meta->get_length() < 40u ) {
			return false;
		}
		PackedByteArray metaHeader = meta->get_buffer( 40u );
		if ( metaHeader.size() != 40u ) {
			return false;
		}
		const uint8_t *const mh = metaHeader.ptr();
		size_t				 cursor = 0u;
		uint32_t			 metaMagic = 0u, metaVersion = 0u, pageCount = 0u, metaHeaderBytes = 0u;
		uint64_t			 packageSignature = 0u, payloadBytes = 0u, metadataSignature = 0u;
		if ( !readU32( mh, 40u, cursor, metaMagic ) || !readU32( mh, 40u, cursor, metaVersion ) ||
			 !readU32( mh, 40u, cursor, pageCount ) || !readU32( mh, 40u, cursor, metaHeaderBytes ) ||
			 !readU64( mh, 40u, cursor, packageSignature ) || !readU64( mh, 40u, cursor, payloadBytes ) ||
			 !readU64( mh, 40u, cursor, metadataSignature ) || metaMagic != 0x4d503354u || metaVersion != 1u ||
			 metaHeaderBytes != 40u || packageSignature != signature || pageCount > MAX_RECORDS ||
			 payloadBytes > ( uint64_t ) MAX_RECORDS * ( T3D_RUNTIME_PAGE_HEADER_BYTES + 1032u ) ||
			 payloadBytes != meta->get_length() - 40u ) {
			return false;
		}
		PackedByteArray payload = meta->get_buffer( payloadBytes );
		if ( payload.size() != ( int64_t ) payloadBytes ||
			 hashBytes( payload.ptr(), payloadBytes ) != metadataSignature ) {
			return false;
		}
		const uint8_t *const mp = payload.ptr();
		size_t				 metaCursor = 0u;
		pages.reserve( pageCount );
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_H05_PACK_OPEN_DUPLICATE );
		for ( uint32_t i = 0u; i < pageCount; ++i ) {
			uint32_t pathBytes = 0u, logicalBytes = 0u;
			if ( !readU32( mp, payloadBytes, metaCursor, pathBytes ) ||
				 !readU32( mp, payloadBytes, metaCursor, logicalBytes ) || pathBytes == 0u || pathBytes > 1024u ||
				 metaCursor > payloadBytes || payloadBytes - metaCursor < T3D_RUNTIME_PAGE_HEADER_BYTES + pathBytes ) {
				return false;
			}
			PageMeta page;
			page.m_nLogicalLength = logicalBytes;
			page.m_header.resize( T3D_RUNTIME_PAGE_HEADER_BYTES );
			memcpy( page.m_header.ptrw(), mp + metaCursor, T3D_RUNTIME_PAGE_HEADER_BYTES );
			metaCursor += T3D_RUNTIME_PAGE_HEADER_BYTES;
			page.m_path = String::utf8( ( const char * ) ( mp + metaCursor ), pathBytes );
			metaCursor += pathBytes;
			if ( page.m_nLogicalLength < T3D_RUNTIME_PAGE_HEADER_BYTES ||
				 page.m_nLogicalLength > T3D_RUNTIME_PAGE_MAX_BYTES ||
				 memcmp( page.m_header.ptr(), "T3PG", 4u ) != 0 ) {
				return false;
			}
			pages.push_back( page );
		}
		if ( metaCursor != payloadBytes || pages.empty() ) {
			return false;
		}
		// Sorted metadata is also the index for lookupPage(). Reject duplicates.
		eastl::sort( pages.begin(), pages.end(), []( const PageMeta &a, const PageMeta &b ) {
			return a.m_path < b.m_path;
		} );
		Terrain3DStreamingProfileWorkScope duplicateChecks( T3D_SP_H05_PACK_OPEN_DUPLICATE );
		for ( size_t i = 1u; i < pages.size(); ++i ) {
			duplicateChecks.add();
			if ( pages[i - 1u].m_path == pages[i].m_path ) {
				return false;
			}
		}
	}
	// Index by path, source signature, offset and channel, matching the
	// original writer key order. Retain index as final tiebreaker so duplicate
	// keys resolve to the same earliest serialized entry as the linear scan.
	eastl::vector< uint16_t > sortedIndices;
	sortedIndices.reserve( items.size() );
	for ( size_t i = 0u; i < items.size(); ++i ) {
		sortedIndices.push_back( ( uint16_t ) i );
	}
	eastl::sort( sortedIndices.begin(), sortedIndices.end(), [&items]( uint16_t left, uint16_t right ) {
		const Item &a = items[left];
		const Item &b = items[right];
		if ( a.m_path != b.m_path ) {
			return a.m_path < b.m_path;
		}
		if ( a.m_nSourceSignature != b.m_nSourceSignature ) {
			return a.m_nSourceSignature < b.m_nSourceSignature;
		}
		if ( a.m_nSourceOffset != b.m_nSourceOffset ) {
			return a.m_nSourceOffset < b.m_nSourceOffset;
		}
		if ( a.m_nChannel != b.m_nChannel ) {
			return a.m_nChannel < b.m_nChannel;
		}
		return left < right;
	} );
	m_path = rPath;
	m_nModified = FileAccess::get_modified_time( rPath );
	m_nLength = length;
	m_items.swap( items );
	m_sortedItemIndices.swap( sortedIndices );
	m_pages.swap( pages );
	return true;
}

bool Terrain3DSharedStreamPack::lookupPage(
	const String	&rPath,
	PackedByteArray &rHeader,
	uint64_t		&rLogicalLength ) const {
	if ( m_path.is_empty() || !FileAccess::file_exists( m_path ) ||
		 m_nModified != FileAccess::get_modified_time( m_path ) ) {
		return false;
	}
	size_t first = 0u, last = m_pages.size();
	while ( first < last ) {
		const size_t mid = first + ( last - first ) / 2u;
		if ( m_pages[mid].m_path < rPath ) {
			first = mid + 1u;
		} else {
			last = mid;
		}
	}
	if ( first < m_pages.size() && m_pages[first].m_path == rPath ) {
		rHeader = m_pages[first].m_header;
		rLogicalLength = m_pages[first].m_nLogicalLength;
		return true;
	}
	return false;
}

bool Terrain3DSharedStreamPack::resolve(
	const t3dStreamRangeKey_t &rKey,
	t3dStreamRangeKey_t		  &rPhysicalKey ) const {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_H01_PACK_RESOLVE );
	Terrain3DStreamingProfileWorkScope work( T3D_SP_H01_PACK_RESOLVE );
	Terrain3DStreamingProfileEventScope events( T3D_SP_H01_PACK_RESOLVE );
	if ( m_path.is_empty() || m_nModified == 0u ) {
		return false;
	}
	events.add();
	if ( FileAccess::get_modified_time( m_path ) != m_nModified ) {
		return false;
	}
	events.add();
	if ( !FileAccess::file_exists( m_path ) ) {
		return false;
	}
	// Binary search the immutable, cold-built index. Compare all key fields
	// for the small same-path/signature/offset/channel range only.
	size_t first = 0u, last = m_sortedItemIndices.size();
	while ( first < last ) {
		const size_t mid = first + ( last - first ) / 2u;
		const Item &entry = m_items[m_sortedItemIndices[mid]];
		work.add();
		const bool before = entry.m_path < rKey.m_path ||
			( entry.m_path == rKey.m_path && ( entry.m_nSourceSignature < rKey.m_nSourceSignature ||
			( entry.m_nSourceSignature == rKey.m_nSourceSignature && ( entry.m_nSourceOffset < rKey.m_nOffset ||
			( entry.m_nSourceOffset == rKey.m_nOffset && entry.m_nChannel < rKey.m_nChannel ) ) ) ) );
		if ( before ) {
			first = mid + 1u;
		} else {
			last = mid;
		}
	}
	for ( size_t i = first; i < m_sortedItemIndices.size(); ++i ) {
		const Item &entry = m_items[m_sortedItemIndices[i]];
		work.add();
		if ( entry.m_path != rKey.m_path || entry.m_nSourceSignature != rKey.m_nSourceSignature ||
			 entry.m_nSourceOffset != rKey.m_nOffset || entry.m_nChannel != rKey.m_nChannel ) {
			break;
		}
		if ( entry.m_nStoredBytes != rKey.m_nStoredBytes || entry.m_nDecodedBytes != rKey.m_nDecodedBytes ||
			 entry.m_nChecksum != rKey.m_nChecksum || entry.m_nCompression != rKey.m_nCompression ||
			 entry.m_nFormat != rKey.m_nFormat || entry.m_nDataOffset > UINT32_MAX ) {
			continue;
		}
		rPhysicalKey = rKey;
		rPhysicalKey.m_nSourceSignature = entry.m_nHash;
		rPhysicalKey.m_path = m_path;
		rPhysicalKey.m_nOffset = ( uint32_t ) entry.m_nDataOffset;
		rPhysicalKey.m_nModifiedTime = m_nModified;
		rPhysicalKey.m_nFileLength = m_nLength;
		return true;
	}
	return false;
}

bool Terrain3DSharedStreamPack::buildBakedPages(
	const String								&rPath,
	const eastl::vector< t3dBakedRegionPage_t > &rPages ) {
	if ( rPath.is_empty() || rPages.empty() || rPages.size() > MAX_RECORDS ) {
		return false;
	}
	eastl::vector< t3dSharedStreamPackRecord_t > records;
	PackedByteArray								 metadata;
	for ( const t3dBakedRegionPage_t &page : rPages ) {
		const PackedByteArray &bytes = page.m_encodedPage;
		const uint64_t		   totalLength = ( uint64_t ) bytes.size();
		if ( page.m_virtualPath.is_empty() || totalLength < T3D_RUNTIME_PAGE_HEADER_BYTES ||
			 totalLength > T3D_RUNTIME_PAGE_MAX_BYTES ) {
			return false;
		}
		const CharString path = page.m_virtualPath.utf8();
		if ( path.length() <= 0 || path.length() > 1024 ) {
			return false;
		}
		const uint8_t *const p = bytes.ptr();
		size_t				 pos = 0u;
		uint32_t			 magic = 0u, version = 0u, headerBytes = 0u, ignored = 0u;
		if ( !readU32( p, totalLength, pos, magic ) || !readU32( p, totalLength, pos, version ) ||
			 !readU32( p, totalLength, pos, headerBytes ) || !readU32( p, totalLength, pos, ignored ) ||
			 magic != T3D_RUNTIME_PAGE_MAGIC || version != T3D_RUNTIME_PAGE_VERSION ||
			 headerBytes != T3D_RUNTIME_PAGE_HEADER_BYTES ) {
			return false;
		}
		pos = 28u;
		uint32_t expectedTotal = 0u, count = 0u, tableOffset = 0u;
		if ( !readU32( p, totalLength, pos, expectedTotal ) || !readU32( p, totalLength, pos, count ) ||
			 !readU32( p, totalLength, pos, tableOffset ) || count == 0u || count > T3D_RUNTIME_PAGE_CHANNEL_COUNT ||
			 tableOffset != 40u || expectedTotal != totalLength ) {
			return false;
		}
		pos = 40u + ( size_t ) T3D_RUNTIME_PAGE_CHANNEL_COUNT * 48u;
		uint32_t sigLo = 0u, sigHi = 0u;
		if ( !readU32( p, totalLength, pos, sigLo ) || !readU32( p, totalLength, pos, sigHi ) ) {
			return false;
		}
		const uint64_t sourceSignature = ( uint64_t ) sigLo | ( ( uint64_t ) sigHi << 32u );
		if ( sourceSignature == 0u ) {
			return false;
		}
		for ( uint32_t i = 0u; i < count; ++i ) {
			pos = 40u + ( size_t ) i * 48u;
			uint32_t fields[12] = {};
			for ( uint32_t &field : fields ) {
				if ( !readU32( p, totalLength, pos, field ) ) {
					return false;
				}
			}
			const uint32_t offset = fields[3], stored = fields[4], decoded = fields[5];
			if ( !stored || !decoded || offset < T3D_RUNTIME_PAGE_HEADER_BYTES || offset > totalLength ||
				 stored > totalLength - offset ) {
				return false;
			}
			t3dSharedStreamPackRecord_t record;
			record.m_key.m_path = page.m_virtualPath;
			record.m_key.m_nSourceSignature = sourceSignature;
			record.m_key.m_nOffset = offset;
			record.m_key.m_nStoredBytes = stored;
			record.m_key.m_nDecodedBytes = decoded;
			record.m_key.m_nFormat = fields[1];
			record.m_key.m_nCompression = fields[2];
			record.m_key.m_nChecksum = fields[10];
			record.m_key.m_nChannel = fields[0];
			record.m_bytes = bytes.slice( offset, ( int64_t ) offset + stored );
			if ( !verifyRecord( record ) ) {
				return false;
			}
			records.push_back( record );
			if ( records.size() > MAX_RECORDS ) {
				return false;
			}
		}
		// The entire T3PG header is *internal package metadata*, not a legacy
		// file. It preserves geometry/channel/version checks for the runtime.
		appendU32( metadata, ( uint32_t ) path.length() );
		appendU32( metadata, ( uint32_t ) totalLength );
		for ( uint32_t i = 0u; i < T3D_RUNTIME_PAGE_HEADER_BYTES; ++i ) {
			metadata.append( p[i] );
		}
		for ( int i = 0; i < path.length(); ++i ) {
			metadata.append( ( uint8_t ) path.get_data()[i] );
		}
	}
	uint64_t nextSequence = 1u;
 	for ( uint32_t marker = 0u; marker < 4u; ++marker ) {
		const String selector = commitPath( rPath, marker & 1u ) +
			( marker >= 2u ? String( ".previous" ) : String() );
		uint64_t sequence = 0u, signature = 0u;
		if ( readCommitPath( selector, sequence, signature ) && sequence >= nextSequence ) {
			if ( sequence == ( uint64_t ) INT64_MAX ) {
				return false;
			}
			nextSequence = sequence + 1u;
		}
	}
	const String bakedPath = generationPath( rPath, nextSequence );
	if ( !prepareUncommittedGeneration( rPath, nextSequence ) || !buildRecords( bakedPath, records ) ) {
		return false;
	}
	if ( publicationFault( 1u ) ) {
		return false;
	}
	// Sidecar carries authoritative per-region metadata and is signed to the
	// exact T3SP index. Old/mismatched pairs cannot be read together.
	Ref< FileAccess > pack = FileAccess::open( bakedPath, FileAccess::READ );
	if ( pack.is_null() || pack->get_length() < HEADER_BYTES ) {
		return false;
	}
	const PackedByteArray header = pack->get_buffer( HEADER_BYTES );
	if ( header.size() != HEADER_BYTES ) {
		return false;
	}
	size_t	 indexCursor = 32u;
	uint64_t indexSignature = 0u;
	if ( !readU64( header.ptr(), HEADER_BYTES, indexCursor, indexSignature ) ) {
		return false;
	}
	PackedByteArray sidecar;
	appendU32( sidecar, 0x4d503354u ); // T3PM
	appendU32( sidecar, 1u );
	appendU32( sidecar, ( uint32_t ) rPages.size() );
	appendU32( sidecar, 40u );
	appendU64( sidecar, indexSignature );
	appendU64( sidecar, ( uint64_t ) metadata.size() );
	appendU64( sidecar, hashBytes( metadata.ptr(), ( size_t ) metadata.size() ) );
	sidecar.append_array( metadata );
	const String			 sidecarPath = bakedPath + String( ".t3pm" );
	Terrain3DFilePublication publish( sidecarPath, sidecarPath + String( ".tmp" ), sidecarPath + String( ".bak" ) );
	if ( publish.stagePacked( sidecar ) != OK ) {
		return false;
	}
	if ( publicationFault( 2u ) ) {
		( void ) publish.rollback();
		return false;
	}
	if ( publish.publish() != OK ) {
		return false;
	}
	Terrain3DSharedStreamPack verification;
	if ( !verification.openDirect( bakedPath ) || verification.itemCount() != records.size() ) {
		( void ) publish.rollback();
		return false;
	}
	for ( const t3dBakedRegionPage_t &page : rPages ) {
		PackedByteArray storedHeader;
		uint64_t		storedLength = 0u;
		if ( !verification.lookupPage( page.m_virtualPath, storedHeader, storedLength ) ||
			 storedLength != ( uint64_t ) page.m_encodedPage.size() ||
			 memcmp( storedHeader.ptr(), page.m_encodedPage.ptr(), T3D_RUNTIME_PAGE_HEADER_BYTES ) != 0 ) {
			( void ) publish.rollback();
			return false;
		}
	}
	if ( publicationFault( 3u ) ) {
		( void ) publish.rollback();
		return false;
	}
	if ( publish.commit() != OK ) {
		return false;
	}
	if ( publicationFault( 4u ) ) {
		return false;
	}
	return publishCommit( rPath, nextSequence, indexSignature );
}

#ifdef T3D_TESTS_ENABLED
void Terrain3DSharedStreamPack::setPublicationFaultForTest( uint32_t stage ) {
	g_publicationFault.store( stage, std::memory_order_relaxed );
}
#endif
