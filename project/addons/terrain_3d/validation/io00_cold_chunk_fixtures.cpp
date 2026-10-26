// Independent host fixtures for the cold indexed chunk format. Build/run with
// clang++ -std=c++17 -I src io00_cold_chunk_fixtures.cpp -o io00 && ./io00.
#include "streaming/io/terrain_3d_cold_chunk_format.h"
#include <stdio.h>

static uint32_t g_cases = 0u;
static uint32_t g_failures = 0u;
#define CHECK( expression ) do { ++g_cases; if ( !( expression ) ) { ++g_failures; \
	fprintf( stderr, "IO-00 FAILED %s:%d: %s\n", __FILE__, __LINE__, #expression ); } } while ( 0 )

int main( void )
{
	// RAW: exact identities, deterministic decoded hash, size and checksum guards.
	const uint8_t raw[8] = { 1u, 2u, 1u, 2u, 3u, 4u, 5u, 6u };
	t3dColdChunkRecord_t one;
	one.m_iRegionX = -3;
	one.m_iRegionY = 17;
	one.m_nKind = 2u;
	one.m_nLevel = 0u;
	one.m_nFormat = 7u;
	one.m_nRegionGeneration = 56u;
	one.m_nMaterialGeneration = 93u;
	one.m_nEncodedBytes = sizeof( raw );
	one.m_nDecodedBytes = sizeof( raw );
	one.m_nOffset = 4096u;
	CHECK( t3dColdChunkSetHashes( one, raw, raw ) );
	{
		t3dColdChunkRecord_t missingGeneration = one;
		missingGeneration.m_nRegionGeneration = 0u;
		CHECK( !t3dColdChunkValidateCodec( missingGeneration ) );
	}
	CHECK( one.m_nDecodedHash == t3dHashStableBytes( raw, sizeof( raw ) ) );
	CHECK( one.m_nDecodedChecksum == t3dColdChunkChecksum( raw, sizeof( raw ) ) );
	uint8_t result[2048] = {};
	CHECK( t3dColdChunkDecode( one, raw, sizeof( raw ), result, sizeof( result ) ) );
	CHECK( memcmp( result, raw, sizeof( raw ) ) == 0 );
	CHECK( !t3dColdChunkDecode( one, raw, sizeof( raw ) - 1u, result, sizeof( result ) ) );
	CHECK( !t3dColdChunkDecode( one, raw, sizeof( raw ), result, sizeof( raw ) - 1u ) );
	CHECK( !t3dColdChunkGenerationMatches( one, 55u, 93u ) );
	CHECK( !t3dColdChunkGenerationMatches( one, 56u, 94u ) );
	CHECK( t3dColdChunkGenerationMatches( one, 56u, 93u ) );

	uint8_t recordBytes[T3D_COLD_CHUNK_RECORD_BYTES] = {};
	CHECK( t3dColdChunkWriteRecord( one, recordBytes, sizeof( recordBytes ) ) );
	CHECK( recordBytes[0] == 'T' && recordBytes[1] == '3' && recordBytes[2] == 'C' && recordBytes[3] == 'C' );
	CHECK( recordBytes[16] == 0xfdu && recordBytes[17] == 0xffu && recordBytes[18] == 0xffu && recordBytes[19] == 0xffu );
	t3dColdChunkRecord_t roundtrip;
	CHECK( t3dColdChunkReadRecord( recordBytes, sizeof( recordBytes ), roundtrip ) );
	CHECK( roundtrip.m_iRegionX == -3 && roundtrip.m_iRegionY == 17 &&
		roundtrip.m_nDecodedHash == one.m_nDecodedHash && roundtrip.m_nOffset == 4096u );
	CHECK( !t3dColdChunkReadRecord( recordBytes, sizeof( recordBytes ) - 1u, roundtrip ) );
	uint8_t damaged[T3D_COLD_CHUNK_RECORD_BYTES];
	memcpy( damaged, recordBytes, sizeof( damaged ) );
	damaged[4] = 2u; // Unsupported version.
	CHECK( !t3dColdChunkReadRecord( damaged, sizeof( damaged ), roundtrip ) );
	memcpy( damaged, recordBytes, sizeof( damaged ) );
	damaged[8] = 0x7fu; // Unknown codec.
	CHECK( !t3dColdChunkReadRecord( damaged, sizeof( damaged ), roundtrip ) );
	memcpy( damaged, recordBytes, sizeof( damaged ) );
	damaged[0] = 'P'; // Legacy T3PG/T3SP/T3DFAR are not this schema.
	CHECK( !t3dColdChunkReadRecord( damaged, sizeof( damaged ), roundtrip ) );

	// RLE: deliberately include a run above 255 and alternating bytes.
	uint8_t repeated[600];
	memset( repeated, 5u, sizeof( repeated ) );
	repeated[300] = 19u;
	uint8_t encoded[2048] = {};
	uint32_t used = 0u;
	CHECK( t3dColdChunkEncodeRle( repeated, sizeof( repeated ), encoded, sizeof( encoded ), used ) );
	CHECK( used < sizeof( repeated ) );
	t3dColdChunkRecord_t two = one;
	two.m_nKind = 3u;
	two.m_nCodec = T3D_COLD_CHUNK_CODEC_RLE;
	two.m_nOffset = 8192u;
	two.m_nDecodedBytes = sizeof( repeated );
	two.m_nEncodedBytes = used;
	CHECK( t3dColdChunkSetHashes( two, encoded, repeated ) );
	CHECK( t3dColdChunkDecode( two, encoded, used, result, sizeof( result ) ) );
	CHECK( memcmp( result, repeated, sizeof( repeated ) ) == 0 );
	CHECK( !t3dColdChunkDecode( two, encoded, used - 1u, result, sizeof( result ) ) );
	t3dColdChunkRecord_t corrupt = two;
	++corrupt.m_nDecodedHash;
	CHECK( !t3dColdChunkDecode( corrupt, encoded, used, result, sizeof( result ) ) );
	corrupt = two;
	++corrupt.m_nEncodedChecksum;
	CHECK( !t3dColdChunkDecode( corrupt, encoded, used, result, sizeof( result ) ) );
	corrupt = two;
	corrupt.m_nEncodedBytes = 3u; // Odd length.
	CHECK( !t3dColdChunkValidateCodec( corrupt ) );
	uint8_t zeroRun[2] = { 0, 8 };
	CHECK( !t3dColdChunkDecodeRle( zeroRun, 2u, result, 1u ) );
	uint8_t longRun[2] = { 7, 8 };
	CHECK( !t3dColdChunkDecodeRle( longRun, 2u, result, 6u ) );
	CHECK( !t3dColdChunkEncodeRle( repeated, sizeof( repeated ), encoded, 2u, used ) );
	CHECK( !t3dColdChunkAlign( UINT64_MAX, two.m_nOffset ) );

	// Index: two sorted, 4K-aligned independently readable chunks.
	one.m_nOffset = 4096u;
	two.m_nOffset = 8192u;
	uint8_t index[2 * T3D_COLD_CHUNK_RECORD_BYTES] = {};
	CHECK( t3dColdChunkWriteRecord( one, index, T3D_COLD_CHUNK_RECORD_BYTES ) );
	CHECK( t3dColdChunkWriteRecord( two, index + T3D_COLD_CHUNK_RECORD_BYTES, T3D_COLD_CHUNK_RECORD_BYTES ) );
	t3dColdIndexHeader_t h;
	h.m_nRecordCount = 2u;
	h.m_nIndexOffset = two.m_nOffset + two.m_nEncodedBytes;
	h.m_nIndexBytes = sizeof( index );
	h.m_nIndexHash = t3dHashStableBytes( index, sizeof( index ) );
	h.m_nGeneration = 99u;
	uint8_t header[T3D_COLD_INDEX_HEADER_BYTES] = {};
	CHECK( t3dColdIndexWriteHeader( h, header, sizeof( header ) ) );
	CHECK( header[0] == 'T' && header[1] == '3' && header[2] == 'C' && header[3] == 'I' );
	t3dColdIndexHeader_t decodedHeader;
	CHECK( t3dColdIndexReadHeader( header, sizeof( header ),
		h.m_nIndexOffset + h.m_nIndexBytes, decodedHeader ) );
	CHECK( decodedHeader.m_nGeneration == 99u && decodedHeader.m_nRecordCount == 2u );
	t3dColdChunkRecord_t records[2];
	CHECK( t3dColdIndexReadRecords( decodedHeader, index, sizeof( index ), records, 2u ) );
	CHECK( t3dColdIndexFind( records, 2u, -3, 17, 2u, 0u ) == &records[0] );
	CHECK( t3dColdIndexFind( records, 2u, -3, 17, 3u, 0u ) == &records[1] );
	CHECK( t3dColdIndexFind( records, 2u, -3, 17, 9u, 0u ) == NULL );
	CHECK( !t3dColdIndexReadRecords( decodedHeader, index, sizeof( index ) - 1u, records, 2u ) );
	CHECK( !t3dColdIndexReadRecords( decodedHeader, index, sizeof( index ), records, 1u ) );
	{
		t3dColdIndexHeader_t impossible = decodedHeader;
		impossible.m_nIndexBytes = 0u;
		CHECK( !t3dColdIndexReadRecords( impossible, index, sizeof( index ), records, 2u ) );
		impossible = decodedHeader;
		impossible.m_nIndexOffset = UINT64_MAX;
		uint8_t unusedHeader[T3D_COLD_INDEX_HEADER_BYTES] = {};
		CHECK( !t3dColdIndexWriteHeader( impossible, unusedHeader, sizeof( unusedHeader ) ) );
	}

	uint8_t corruptIndex[sizeof( index )];
	memcpy( corruptIndex, index, sizeof( index ) );
	corruptIndex[70] ^= 1u;
	CHECK( !t3dColdIndexReadRecords( decodedHeader, corruptIndex, sizeof( corruptIndex ), records, 2u ) );
	memcpy( corruptIndex, index, sizeof( index ) );
	// In-range but overlapping data extents, signed by a matching index hash.
	t3dWriteLittleUnsigned< uint64_t >( corruptIndex + T3D_COLD_CHUNK_RECORD_BYTES + 48u, 4096u );
	decodedHeader.m_nIndexHash = t3dHashStableBytes( corruptIndex, sizeof( corruptIndex ) );
	CHECK( !t3dColdIndexReadRecords( decodedHeader, corruptIndex, sizeof( corruptIndex ), records, 2u ) );
	memcpy( corruptIndex, index, sizeof( index ) );
	// Duplicate key, physically valid offset.
	t3dWriteLittleUnsigned< uint32_t >( corruptIndex + T3D_COLD_CHUNK_RECORD_BYTES + 12u, one.m_nKind );
	decodedHeader.m_nIndexHash = t3dHashStableBytes( corruptIndex, sizeof( corruptIndex ) );
	CHECK( !t3dColdIndexReadRecords( decodedHeader, corruptIndex, sizeof( corruptIndex ), records, 2u ) );
	memcpy( corruptIndex, index, sizeof( index ) );
	t3dWriteLittleUnsigned< uint64_t >( corruptIndex + T3D_COLD_CHUNK_RECORD_BYTES + 48u, 4097u );
	decodedHeader.m_nIndexHash = t3dHashStableBytes( corruptIndex, sizeof( corruptIndex ) );
	CHECK( !t3dColdIndexReadRecords( decodedHeader, corruptIndex, sizeof( corruptIndex ), records, 2u ) );
	decodedHeader.m_nIndexHash = h.m_nIndexHash;
	CHECK( !t3dColdIndexReadHeader( header, sizeof( header ), h.m_nIndexOffset + h.m_nIndexBytes - 1u, decodedHeader ) );
	uint8_t reversed[T3D_COLD_INDEX_HEADER_BYTES];
	memcpy( reversed, header, sizeof( reversed ) );
	reversed[4] = 2u;
	CHECK( !t3dColdIndexReadHeader( reversed, sizeof( reversed ), h.m_nIndexOffset + h.m_nIndexBytes, decodedHeader ) );
	reversed[4] = header[4];
	reversed[0] = 'P';
	CHECK( !t3dColdIndexReadHeader( reversed, sizeof( reversed ), h.m_nIndexOffset + h.m_nIndexBytes, decodedHeader ) );
	CHECK( !t3dColdIndexReadHeader( header, sizeof( header ) - 1u,
		h.m_nIndexOffset + h.m_nIndexBytes, decodedHeader ) );

	printf( "IO-00 %u contract checks, %u failures\n", g_cases, g_failures );
	return g_failures ? 1 : 0;
}
