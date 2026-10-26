// Copyright (c) 2026 Terrain3D Contributors.
// Cold-file adapter; Terrain3DFilePublication remains the sole publication owner.
#ifndef TERRAIN3D_COLD_CHUNK_FILE_H
#define TERRAIN3D_COLD_CHUNK_FILE_H

#include "streaming/io/terrain_3d_cold_chunk_format.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <EASTL/vector.h>

// Decode only the bounded index during cold mounting, never the entire cache.
inline bool t3dColdIndexOpenFile( const godot::Ref< godot::FileAccess > &rFile,
	t3dColdIndexHeader_t &rHeader, eastl::vector< t3dColdChunkRecord_t > &rRecords )
{
	rRecords.clear();
	if ( rFile.is_null() || rFile->get_length() < T3D_COLD_INDEX_HEADER_BYTES ) {
		return false;
	}
	rFile->seek( 0u );
	const godot::PackedByteArray headerBytes = rFile->get_buffer( T3D_COLD_INDEX_HEADER_BYTES );
	t3dColdIndexHeader_t header;
	if ( headerBytes.size() != T3D_COLD_INDEX_HEADER_BYTES ||
		!t3dColdIndexReadHeader( headerBytes.ptr(), headerBytes.size(), rFile->get_length(), header ) ) {
		return false;
	}
	rFile->seek( header.m_nIndexOffset );
	const godot::PackedByteArray index = rFile->get_buffer( header.m_nIndexBytes );
	if ( index.size() != ( int64_t ) header.m_nIndexBytes ) {
		return false;
	}
	rRecords.resize( header.m_nRecordCount );
	if ( !t3dColdIndexReadRecords( header, index.ptr(), ( size_t ) index.size(),
		rRecords.empty() ? NULL : rRecords.data(), rRecords.size() ) ) {
		rRecords.clear();
		return false;
	}
	rHeader = header;
	return true;
}

// Caller owns reusable worker scratch. No allocation, whole-file read, or GPU sync.
inline bool t3dColdChunkReadFileInto( const godot::Ref< godot::FileAccess > &rFile,
	const t3dColdChunkRecord_t &rRecord, const uint64_t nRegionGeneration,
	const uint64_t nMaterialGeneration, uint8_t *pEncodedScratch, const size_t nEncodedCapacity,
	uint8_t *pDecoded, const size_t nDecodedCapacity )
{
	if ( rFile.is_null() || !t3dColdChunkGenerationMatches( rRecord, nRegionGeneration, nMaterialGeneration ) ||
		!t3dColdChunkValidateCodec( rRecord ) || !pEncodedScratch || !pDecoded ||
		rRecord.m_nEncodedBytes > nEncodedCapacity || rRecord.m_nDecodedBytes > nDecodedCapacity ||
		rRecord.m_nOffset < T3D_COLD_INDEX_HEADER_BYTES ||
		( rRecord.m_nOffset & ( T3D_COLD_CHUNK_ALIGNMENT - 1u ) ) != 0u ||
		!t3dByteRangeFits< uint64_t >( rRecord.m_nOffset, rRecord.m_nEncodedBytes, rFile->get_length() ) ) {
		return false;
	}
	rFile->seek( rRecord.m_nOffset );
	if ( rFile->get_buffer( pEncodedScratch, rRecord.m_nEncodedBytes ) != rRecord.m_nEncodedBytes ) {
		return false;
	}
	return t3dColdChunkDecode( rRecord, pEncodedScratch, nEncodedCapacity, pDecoded, nDecodedCapacity );
}

// Authoring-only views: decoded bytes are supplied by the current format cooker.
// The writer validates decoded identities before publishing compressed payloads.
typedef struct t3dColdChunkSource_s
{
	t3dColdChunkRecord_t m_record;
	const uint8_t *m_pEncoded = NULL;
	const uint8_t *m_pDecoded = NULL;
} t3dColdChunkSource_t;

inline bool t3dColdChunkSourceValid( const t3dColdChunkSource_t &rSource )
{
	const t3dColdChunkRecord_t &r = rSource.m_record;
	if ( !t3dColdChunkValidateCodec( r ) || !rSource.m_pEncoded || !rSource.m_pDecoded ) {
		return false;
	}
	if ( r.m_nCodec == T3D_COLD_CHUNK_CODEC_RAW ) {
		return memcmp( rSource.m_pEncoded, rSource.m_pDecoded, r.m_nDecodedBytes ) == 0;
	}
	uint32_t read = 0u, written = 0u;
	while ( read < r.m_nEncodedBytes ) {
		const uint32_t run = rSource.m_pEncoded[read++];
		const uint8_t value = rSource.m_pEncoded[read++];
		if ( !run || run > r.m_nDecodedBytes - written ) {
			return false;
		}
		for ( uint32_t i = 0u; i < run; ++i ) {
			if ( rSource.m_pDecoded[written + i] != value ) {
				return false;
			}
		}
		written += run;
	}
	return written == r.m_nDecodedBytes;
}

inline bool t3dColdChunkWritePadding( const godot::Ref< godot::FileAccess > &rFile,
	const uint64_t nTarget )
{
	static const uint8_t zeros[T3D_COLD_CHUNK_ALIGNMENT] = {};
	while ( rFile->get_position() < nTarget ) {
		const uint64_t remaining = nTarget - rFile->get_position();
		const uint32_t length = remaining < sizeof( zeros ) ? ( uint32_t ) remaining : sizeof( zeros );
		rFile->store_buffer( zeros, length );
		if ( rFile->get_error() != godot::OK ) {
			return false;
		}
	}
	return rFile->get_position() == nTarget;
}

// Stage -> flush -> validate index -> publish -> commit. Uses the preexisting
// recoverable previous-file protocol; never modifies an already published file.
inline bool t3dColdIndexPublishFile( const godot::String &rPath,
	const t3dColdChunkSource_t *pChunks, const uint32_t nCount, const uint64_t nGeneration )
{
	if ( rPath.is_empty() || nCount > T3D_COLD_INDEX_MAX_RECORDS || ( nCount && !pChunks ) || !nGeneration ) {
		return false;
	}
	for ( uint32_t i = 0u; i < nCount; ++i ) {
		if ( !t3dColdChunkSourceValid( pChunks[i] ) ||
			( i > 0u && t3dColdChunkCompareKey( pChunks[i - 1u].m_record, pChunks[i].m_record ) >= 0 ) ) {
			return false;
		}
	}
	const godot::String staged = rPath + godot::String( ".staged" );
	Terrain3DFilePublication publication( rPath, staged, rPath + godot::String( ".previous" ) );
	godot::Ref< godot::FileAccess > file;
	if ( publication.openStaged( file ) != godot::OK ) {
		return false;
	}
	const uint8_t blank[T3D_COLD_INDEX_HEADER_BYTES] = {};
	file->store_buffer( blank, sizeof( blank ) );
	const uint64_t indexBytes = ( uint64_t ) nCount * T3D_COLD_CHUNK_RECORD_BYTES;
	godot::PackedByteArray index;
	index.resize( ( int64_t ) indexBytes );
	bool valid = file->get_error() == godot::OK && index.size() == ( int64_t ) indexBytes;
	for ( uint32_t i = 0u; valid && i < nCount; ++i ) {
		uint64_t offset = 0u;
		valid = t3dColdChunkAlign( file->get_position(), offset ) &&
			t3dColdChunkWritePadding( file, offset );
		if ( !valid ) {
			break;
		}
		t3dColdChunkRecord_t record = pChunks[i].m_record;
		record.m_nOffset = offset;
		valid = t3dColdChunkSetHashes( record, pChunks[i].m_pEncoded, pChunks[i].m_pDecoded ) &&
			t3dColdChunkWriteRecord( record, index.ptrw() + ( size_t ) i * T3D_COLD_CHUNK_RECORD_BYTES,
			T3D_COLD_CHUNK_RECORD_BYTES );
		if ( valid ) {
			file->store_buffer( pChunks[i].m_pEncoded, record.m_nEncodedBytes );
			valid = file->get_error() == godot::OK;
		}
	}
	t3dColdIndexHeader_t header;
	if ( valid ) {
		header.m_nRecordCount = nCount;
		header.m_nIndexOffset = file->get_position();
		header.m_nIndexBytes = indexBytes;
		header.m_nIndexHash = t3dHashStableBytes( index.ptr(), indexBytes );
		header.m_nGeneration = nGeneration;
		uint8_t headerBytes[T3D_COLD_INDEX_HEADER_BYTES];
		valid = t3dColdIndexWriteHeader( header, headerBytes, sizeof( headerBytes ) );
		if ( valid ) {
			file->store_buffer( index );
			valid = file->get_error() == godot::OK;
		}
		if ( valid ) {
			file->seek( 0u );
			file->store_buffer( headerBytes, sizeof( headerBytes ) );
			valid = file->get_error() == godot::OK;
		}
	}
	uint64_t finalBytes = 0u;
	valid = valid && t3dCheckedAdd< uint64_t >( header.m_nIndexOffset, header.m_nIndexBytes, finalBytes );
	if ( valid ) {
		valid = publication.finishStaged( file, finalBytes ) == godot::OK;
	} else {
		file.unref();
	}
	if ( valid ) {
		godot::Ref< godot::FileAccess > stagedFile =
			godot::FileAccess::open( staged, godot::FileAccess::READ );
		eastl::vector< t3dColdChunkRecord_t > check;
		t3dColdIndexHeader_t checked;
		valid = t3dColdIndexOpenFile( stagedFile, checked, check ) &&
			checked.m_nGeneration == nGeneration && check.size() == nCount;
		stagedFile.unref();
	}
	if ( valid ) {
		valid = publication.publish() == godot::OK;
	}
	if ( valid ) {
		return publication.commit() == godot::OK;
	}
	( void ) publication.rollback();
	return false;
}

#endif // TERRAIN3D_COLD_CHUNK_FILE_H
