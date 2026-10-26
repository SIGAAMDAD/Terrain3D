// Copyright (c) 2026 Terrain3D Contributors.
// Shared cold-cook chunk/index ABI for future FAR and runtime page formats.
// Existing T3PG, T3SP, and T3DFAR readers retain their on-disk contracts.
#ifndef TERRAIN3D_COLD_CHUNK_FORMAT_H
#define TERRAIN3D_COLD_CHUNK_FORMAT_H

#include "core/utility/terrain_3d_binary_io.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define T3D_COLD_CHUNK_MAGIC             ( 0x43433354u ) // T3CC
#define T3D_COLD_INDEX_MAGIC             ( 0x49433354u ) // T3CI
#define T3D_COLD_FORMAT_VERSION          ( 1u )
#define T3D_COLD_INDEX_HEADER_BYTES      ( 56u )
#define T3D_COLD_CHUNK_RECORD_BYTES      ( 80u )
#define T3D_COLD_CHUNK_ALIGNMENT         ( 4096u )
#define T3D_COLD_CHUNK_MAX_BYTES         ( 256u * 1024u * 1024u )
#define T3D_COLD_INDEX_MAX_RECORDS       ( 65536u )

typedef enum t3dColdChunkCodec_e : uint32_t
{
	T3D_COLD_CHUNK_CODEC_RAW = 0u,
	T3D_COLD_CHUNK_CODEC_RLE = 1u
} t3dColdChunkCodec_t;

// All fields are serialized individually, little-endian; never memcpy structs to disk.
typedef struct t3dColdChunkRecord_s
{
	int32_t  m_iRegionX = 0;
	int32_t  m_iRegionY = 0;
	uint32_t m_nKind = 0u; // Format owner assigns channel/surface kind.
	uint32_t m_nLevel = 0u;
	uint32_t m_nFormat = 0u;
	uint32_t m_nCodec = T3D_COLD_CHUNK_CODEC_RAW;
	uint64_t m_nRegionGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nOffset = 0u;
	uint32_t m_nEncodedBytes = 0u;
	uint32_t m_nDecodedBytes = 0u;
	uint32_t m_nEncodedChecksum = 0u;
	uint32_t m_nDecodedChecksum = 0u;
	uint64_t m_nDecodedHash = 0u;
} t3dColdChunkRecord_t;

typedef struct t3dColdIndexHeader_s
{
	uint32_t m_nRecordCount = 0u;
	uint64_t m_nIndexOffset = T3D_COLD_INDEX_HEADER_BYTES;
	uint64_t m_nIndexBytes = 0u;
	uint64_t m_nIndexHash = 0u;
	uint64_t m_nGeneration = 0u;
} t3dColdIndexHeader_t;

inline uint32_t t3dColdChunkChecksum( const uint8_t *pData, const size_t nBytes )
{
	return !pData && nBytes != 0u ? 0u : t3dHashFnv1a32( pData, nBytes, 2166136261u );
}

inline bool t3dColdChunkAlign( const uint64_t nValue, uint64_t &rAligned )
{
	return t3dCheckedAlignUp< uint64_t >( nValue, T3D_COLD_CHUNK_ALIGNMENT, rAligned );
}

// Reuse the existing page RLE byte-pair convention; no allocations or device sync.
inline bool t3dColdChunkDecodeRle( const uint8_t *pEncoded, const uint32_t nEncodedBytes,
	uint8_t *pDecoded, const uint32_t nDecodedBytes )
{
	if ( !pEncoded || !pDecoded || !nEncodedBytes || !nDecodedBytes || ( nEncodedBytes & 1u ) != 0u ) {
		return false;
	}
	uint32_t write = 0u;
	for ( uint32_t read = 0u; read < nEncodedBytes; read += 2u ) {
		const uint32_t run = pEncoded[read];
		if ( run == 0u || run > nDecodedBytes - write ) {
			return false;
		}
		memset( pDecoded + write, pEncoded[read + 1u], run );
		write += run;
	}
	return write == nDecodedBytes;
}

inline bool t3dColdChunkEncodeRle( const uint8_t *pSource, const uint32_t nBytes,
	uint8_t *pEncoded, const uint32_t nCapacity, uint32_t &rEncodedBytes )
{
	rEncodedBytes = 0u;
	if ( !pSource || !pEncoded || nBytes == 0u || nBytes > T3D_COLD_CHUNK_MAX_BYTES ) {
		return false;
	}
	uint32_t read = 0u;
	while ( read < nBytes ) {
		if ( nCapacity - rEncodedBytes < 2u ) {
			return false;
		}
		uint32_t run = 1u;
		while ( run < 255u && run < nBytes - read && pSource[read] == pSource[read + run] ) {
			++run;
		}
		pEncoded[rEncodedBytes++] = ( uint8_t ) run;
		pEncoded[rEncodedBytes++] = pSource[read];
		read += run;
	}
	return true;
}

inline bool t3dColdChunkValidateCodec( const t3dColdChunkRecord_t &r )
{
	if ( r.m_nRegionGeneration == 0u || r.m_nEncodedBytes == 0u || r.m_nDecodedBytes == 0u ||
		 r.m_nEncodedBytes > T3D_COLD_CHUNK_MAX_BYTES || r.m_nDecodedBytes > T3D_COLD_CHUNK_MAX_BYTES ) {
		return false;
	}
	if ( r.m_nCodec == T3D_COLD_CHUNK_CODEC_RAW ) {
		return r.m_nEncodedBytes == r.m_nDecodedBytes;
	}
	if ( r.m_nCodec == T3D_COLD_CHUNK_CODEC_RLE ) {
		return ( r.m_nEncodedBytes & 1u ) == 0u &&
			( uint64_t ) r.m_nDecodedBytes <= ( ( uint64_t ) r.m_nEncodedBytes / 2u ) * 255u;
	}
	return false;
}

inline bool t3dColdChunkDecode( const t3dColdChunkRecord_t &r,
	const uint8_t *pEncoded, const size_t nEncodedCapacity,
	uint8_t *pDecoded, const size_t nDecodedCapacity )
{
	if ( !t3dColdChunkValidateCodec( r ) || !pEncoded || !pDecoded ||
		r.m_nEncodedBytes > nEncodedCapacity || r.m_nDecodedBytes > nDecodedCapacity ||
		t3dColdChunkChecksum( pEncoded, r.m_nEncodedBytes ) != r.m_nEncodedChecksum ) {
		return false;
	}
	if ( r.m_nCodec == T3D_COLD_CHUNK_CODEC_RAW ) {
		memcpy( pDecoded, pEncoded, r.m_nDecodedBytes );
	} else if ( !t3dColdChunkDecodeRle( pEncoded, r.m_nEncodedBytes, pDecoded, r.m_nDecodedBytes ) ) {
		return false;
	}
	return t3dColdChunkChecksum( pDecoded, r.m_nDecodedBytes ) == r.m_nDecodedChecksum &&
		t3dHashStableBytes( pDecoded, r.m_nDecodedBytes ) == r.m_nDecodedHash;
}

inline bool t3dColdChunkSetHashes( t3dColdChunkRecord_t &r,
	const uint8_t *pEncoded, const uint8_t *pDecoded )
{
	if ( !t3dColdChunkValidateCodec( r ) || !pEncoded || !pDecoded ) {
		return false;
	}
	r.m_nEncodedChecksum = t3dColdChunkChecksum( pEncoded, r.m_nEncodedBytes );
	r.m_nDecodedChecksum = t3dColdChunkChecksum( pDecoded, r.m_nDecodedBytes );
	r.m_nDecodedHash = t3dHashStableBytes( pDecoded, r.m_nDecodedBytes );
	return true;
}

inline int32_t t3dColdChunkCompareKey( const t3dColdChunkRecord_t &a, const t3dColdChunkRecord_t &b )
{
	if ( a.m_iRegionX != b.m_iRegionX ) {
		return a.m_iRegionX < b.m_iRegionX ? -1 : 1;
	}
	if ( a.m_iRegionY != b.m_iRegionY ) {
		return a.m_iRegionY < b.m_iRegionY ? -1 : 1;
	}
	if ( a.m_nKind != b.m_nKind ) {
		return a.m_nKind < b.m_nKind ? -1 : 1;
	}
	if ( a.m_nLevel != b.m_nLevel ) {
		return a.m_nLevel < b.m_nLevel ? -1 : 1;
	}
	return 0;
}

inline bool t3dColdChunkWriteRecord( const t3dColdChunkRecord_t &r, uint8_t *pDest, const size_t nBytes )
{
	if ( !pDest || nBytes < T3D_COLD_CHUNK_RECORD_BYTES || !t3dColdChunkValidateCodec( r ) ) {
		return false;
	}
	t3dByteWriter_t w( pDest, T3D_COLD_CHUNK_RECORD_BYTES );
	return w.writeU32( T3D_COLD_CHUNK_MAGIC ) && w.writeU16( T3D_COLD_FORMAT_VERSION ) &&
		w.writeU16( T3D_COLD_CHUNK_RECORD_BYTES ) && w.writeU32( r.m_nCodec ) &&
		w.writeU32( r.m_nKind ) && w.writeI32( r.m_iRegionX ) && w.writeI32( r.m_iRegionY ) &&
		w.writeU32( r.m_nLevel ) && w.writeU32( r.m_nFormat ) &&
		w.writeU64( r.m_nRegionGeneration ) && w.writeU64( r.m_nMaterialGeneration ) &&
		w.writeU64( r.m_nOffset ) && w.writeU32( r.m_nEncodedBytes ) &&
		w.writeU32( r.m_nDecodedBytes ) && w.writeU32( r.m_nEncodedChecksum ) &&
		w.writeU32( r.m_nDecodedChecksum ) && w.writeU64( r.m_nDecodedHash ) && w.complete();
}

inline bool t3dColdChunkReadRecord( const uint8_t *pSource, const size_t nBytes, t3dColdChunkRecord_t &r )
{
	if ( !pSource || nBytes < T3D_COLD_CHUNK_RECORD_BYTES ) {
		return false;
	}
	t3dByteReader_t rd( pSource, T3D_COLD_CHUNK_RECORD_BYTES );
	const uint32_t magic = rd.readU32();
	const uint16_t version = rd.readU16();
	const uint16_t headerBytes = rd.readU16();
	t3dColdChunkRecord_t entry;
	entry.m_nCodec = rd.readU32();
	entry.m_nKind = rd.readU32();
	entry.m_iRegionX = rd.readI32();
	entry.m_iRegionY = rd.readI32();
	entry.m_nLevel = rd.readU32();
	entry.m_nFormat = rd.readU32();
	entry.m_nRegionGeneration = rd.readU64();
	entry.m_nMaterialGeneration = rd.readU64();
	entry.m_nOffset = rd.readU64();
	entry.m_nEncodedBytes = rd.readU32();
	entry.m_nDecodedBytes = rd.readU32();
	entry.m_nEncodedChecksum = rd.readU32();
	entry.m_nDecodedChecksum = rd.readU32();
	entry.m_nDecodedHash = rd.readU64();
	if ( !rd.complete() || magic != T3D_COLD_CHUNK_MAGIC || version != T3D_COLD_FORMAT_VERSION ||
		headerBytes != T3D_COLD_CHUNK_RECORD_BYTES || !t3dColdChunkValidateCodec( entry ) ) {
		return false;
	}
	r = entry;
	return true;
}

inline bool t3dColdIndexWriteHeader( const t3dColdIndexHeader_t &r, uint8_t *pDest, const size_t nBytes )
{
	uint64_t end = 0u;
	if ( !pDest || nBytes < T3D_COLD_INDEX_HEADER_BYTES || r.m_nRecordCount > T3D_COLD_INDEX_MAX_RECORDS ||
		r.m_nIndexBytes != ( uint64_t ) r.m_nRecordCount * T3D_COLD_CHUNK_RECORD_BYTES ||
		r.m_nIndexOffset < T3D_COLD_INDEX_HEADER_BYTES ||
		!t3dCheckedAdd< uint64_t >( r.m_nIndexOffset, r.m_nIndexBytes, end ) ||
		r.m_nGeneration == 0u ) {
		return false;
	}
	t3dByteWriter_t w( pDest, T3D_COLD_INDEX_HEADER_BYTES );
	return w.writeU32( T3D_COLD_INDEX_MAGIC ) && w.writeU32( T3D_COLD_FORMAT_VERSION ) &&
		w.writeU32( T3D_COLD_INDEX_HEADER_BYTES ) && w.writeU32( T3D_COLD_CHUNK_RECORD_BYTES ) &&
		w.writeU32( r.m_nRecordCount ) && w.writeU32( T3D_COLD_CHUNK_ALIGNMENT ) &&
		w.writeU64( r.m_nIndexOffset ) && w.writeU64( r.m_nIndexBytes ) &&
		w.writeU64( r.m_nIndexHash ) && w.writeU64( r.m_nGeneration ) && w.complete();
}

inline bool t3dColdIndexReadHeader( const uint8_t *pSource, const size_t nBytes,
	const uint64_t nFileBytes, t3dColdIndexHeader_t &r )
{
	if ( !pSource || nBytes < T3D_COLD_INDEX_HEADER_BYTES ) {
		return false;
	}
	t3dByteReader_t rd( pSource, T3D_COLD_INDEX_HEADER_BYTES );
	const uint32_t magic = rd.readU32();
	const uint32_t version = rd.readU32();
	const uint32_t headerBytes = rd.readU32();
	const uint32_t recordBytes = rd.readU32();
	t3dColdIndexHeader_t h;
	h.m_nRecordCount = rd.readU32();
	const uint32_t alignment = rd.readU32();
	h.m_nIndexOffset = rd.readU64();
	h.m_nIndexBytes = rd.readU64();
	h.m_nIndexHash = rd.readU64();
	h.m_nGeneration = rd.readU64();
	if ( !rd.complete() || magic != T3D_COLD_INDEX_MAGIC || version != T3D_COLD_FORMAT_VERSION ||
		headerBytes != T3D_COLD_INDEX_HEADER_BYTES || recordBytes != T3D_COLD_CHUNK_RECORD_BYTES ||
		alignment != T3D_COLD_CHUNK_ALIGNMENT || h.m_nRecordCount > T3D_COLD_INDEX_MAX_RECORDS ||
		h.m_nIndexBytes != ( uint64_t ) h.m_nRecordCount * T3D_COLD_CHUNK_RECORD_BYTES ||
		h.m_nIndexOffset < T3D_COLD_INDEX_HEADER_BYTES || h.m_nIndexOffset > nFileBytes ||
		h.m_nIndexBytes != nFileBytes - h.m_nIndexOffset || h.m_nGeneration == 0u ) {
		return false;
	}
	r = h;
	return true;
}

// The index is sorted in key order AND its payloads are physically monotone.
// This makes duplicate/overlap checks linear and keeps cold lookup logarithmic.
inline bool t3dColdIndexReadRecords( const t3dColdIndexHeader_t &h,
	const uint8_t *pIndexBytes, const size_t nAvailable,
	t3dColdChunkRecord_t *pRecords, const size_t nCapacity )
{
	if ( h.m_nRecordCount > T3D_COLD_INDEX_MAX_RECORDS || h.m_nRecordCount > nCapacity ||
		h.m_nIndexBytes != ( uint64_t ) h.m_nRecordCount * T3D_COLD_CHUNK_RECORD_BYTES ||
		h.m_nIndexBytes > nAvailable ||
		( h.m_nRecordCount && ( !pRecords || !pIndexBytes ) ) ||
		t3dHashStableBytes( pIndexBytes, h.m_nIndexBytes ) != h.m_nIndexHash ) {
		return false;
	}
	uint64_t previousEnd = T3D_COLD_INDEX_HEADER_BYTES;
	for ( uint32_t i = 0u; i < h.m_nRecordCount; ++i ) {
		t3dColdChunkRecord_t record;
		if ( !t3dColdChunkReadRecord( pIndexBytes + ( size_t ) i * T3D_COLD_CHUNK_RECORD_BYTES,
			T3D_COLD_CHUNK_RECORD_BYTES, record ) ||
			( record.m_nOffset & ( T3D_COLD_CHUNK_ALIGNMENT - 1u ) ) != 0u ||
			record.m_nOffset < previousEnd || record.m_nOffset > h.m_nIndexOffset ||
			record.m_nEncodedBytes > h.m_nIndexOffset - record.m_nOffset ||
			( i > 0u && t3dColdChunkCompareKey( pRecords[i - 1u], record ) >= 0 ) ) {
			return false;
		}
		pRecords[i] = record;
		previousEnd = record.m_nOffset + record.m_nEncodedBytes;
	}
	return true;
}

inline const t3dColdChunkRecord_t *t3dColdIndexFind( const t3dColdChunkRecord_t *pRecords,
	const uint32_t nCount, const int32_t iRegionX, const int32_t iRegionY,
	const uint32_t nKind, const uint32_t nLevel )
{
	if ( !pRecords ) {
		return NULL;
	}
	t3dColdChunkRecord_t key;
	key.m_iRegionX = iRegionX;
	key.m_iRegionY = iRegionY;
	key.m_nKind = nKind;
	key.m_nLevel = nLevel;
	uint32_t first = 0u, last = nCount;
	while ( first < last ) {
		const uint32_t mid = first + ( last - first ) / 2u;
		if ( t3dColdChunkCompareKey( pRecords[mid], key ) < 0 ) {
			first = mid + 1u;
		} else {
			last = mid;
		}
	}
	return first < nCount && t3dColdChunkCompareKey( pRecords[first], key ) == 0 ? pRecords + first : NULL;
}

inline bool t3dColdChunkGenerationMatches( const t3dColdChunkRecord_t &r,
	const uint64_t nRegionGeneration, const uint64_t nMaterialGeneration )
{
	return r.m_nRegionGeneration == nRegionGeneration &&
		r.m_nMaterialGeneration == nMaterialGeneration;
}

#endif // TERRAIN3D_COLD_CHUNK_FORMAT_H
