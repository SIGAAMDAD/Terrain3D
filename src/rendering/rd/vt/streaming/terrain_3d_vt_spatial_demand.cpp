// Copyright (c) 2023-2026 Terrain3D contributors.
#include "rendering/rd/vt/streaming/terrain_3d_vt_spatial_demand.h"
#include <limits.h>
#include <string.h>

uint64_t t3d_vt_demand_hash(
	const uint8_t *const pBytes,
	const uint64_t		 nBytes ) {
	uint64_t h = 14695981039346656037ull;
	for ( uint64_t i = 0u; i < nBytes; ++i ) {
		h ^= pBytes[i];
		h *= 1099511628211ull;
	}
	return h;
}

void Terrain3DVTSpatialDemand::resetCold() {
	m_bytes.clear();
	m_boundAllocations.clear();
	m_nHeaderBytes = m_nRecordStride = m_nMappedRecords = 0u;
	m_header = t3dVTDemandHeader_t{};
	for ( uint32_t i = 0u; i < T3D_VT_DEMAND_MRU_SLOTS; ++i ) {
		m_cache[i].nCount = 0u;
		m_cache[i].nLastAccess = 0u;
	}
	m_accessClock = 0u;
}

bool Terrain3DVTSpatialDemand::loadCold(
	const uint8_t *const pBytes,
	const uint64_t		 nBytes ) {
	resetCold();
	if ( pBytes == nullptr || nBytes < sizeof( t3dVTDemandHeader_t ) || nBytes > 4u * 1024u * 1024u ) {
		return false;
	}
	t3dVTDemandHeader_t header{};
	memcpy( &header, pBytes, sizeof( header ) );
	if ( header.nMagic != T3D_VT_DEMAND_MAGIC ||
		 ( header.nVersion != T3D_VT_DEMAND_VERSION && header.nVersion != T3D_VT_DEMAND_VERSION_V2 ) ||
		 header.nReserved != 0u || header.nCellMeters != 128u || header.nPackageSignature == 0u ||
		 header.nCellCount > T3D_VT_DEMAND_MAX_CELLS || header.nRecordCount > T3D_VT_DEMAND_MAX_RECORDS ) {
		return false;
	}
	const bool	   v2 = header.nVersion == T3D_VT_DEMAND_VERSION_V2;
	const uint32_t headerBytes = v2 ? ( uint32_t ) sizeof( t3dVTDemandHeaderV2_t ) : ( uint32_t ) sizeof( header );
	const uint32_t stride =
		v2 ? ( uint32_t ) sizeof( t3dVTDemandRecordV2_t ) : ( uint32_t ) sizeof( t3dVTDemandRecord_t );
	if ( v2 ) {
		if ( nBytes < headerBytes ) {
			return false;
		}
		t3dVTDemandHeaderV2_t ext{};
		memcpy( &ext, pBytes, sizeof( ext ) );
		if ( ext.nAssetIdentityHash == 0u || ext.nRecordStride != stride || ext.nFlags != 0u ) {
			return false;
		}
	}
	const uint64_t cellBytes = ( uint64_t ) header.nCellCount * sizeof( t3dVTDemandCell_t );
	const uint64_t recordBytes = ( uint64_t ) header.nRecordCount * stride;
	if ( nBytes != headerBytes + cellBytes + recordBytes ||
		 t3d_vt_demand_hash( pBytes + headerBytes, cellBytes + recordBytes ) != header.nPayloadHash ) {
		return false;
	}
	const uint8_t *const cells = pBytes + headerBytes;
	const uint8_t *const records = cells + cellBytes;
	int32_t				 lastX = INT32_MIN;
	int32_t				 lastZ = INT32_MIN;
	uint32_t			 expectedFirst = 0u;
	for ( uint32_t i = 0u; i < header.nCellCount; ++i ) {
		t3dVTDemandCell_t cell{};
		memcpy( &cell, cells + ( size_t ) i * sizeof( cell ), sizeof( cell ) );
		if ( cell.nReserved != 0u || cell.nCount == 0u || cell.nCount > T3D_VT_DEMAND_MAX_RECORDS_PER_CELL ||
			 cell.nFirstRecord != expectedFirst || cell.nCount > header.nRecordCount - expectedFirst ||
			 ( i > 0u && ( cell.nX < lastX || ( cell.nX == lastX && cell.nZ <= lastZ ) ) ) ) {
			return false;
		}
		expectedFirst += cell.nCount;
		lastX = cell.nX;
		lastZ = cell.nZ;
	}
	if ( expectedFirst != header.nRecordCount ) {
		return false;
	}
	for ( uint32_t i = 0u; i < header.nRecordCount; ++i ) {
		t3dVTDemandRecord_t record{};
		uint64_t			stableKey = 1u;
		uint32_t			flags = 0u;
		if ( v2 ) {
			t3dVTDemandRecordV2_t source{};
			memcpy( &source, records + ( size_t ) i * stride, sizeof( source ) );
			record = source.Footprint;
			stableKey = source.nStableMaterialKey;
			flags = source.nFlags;
		} else {
			memcpy( &record, records + ( size_t ) i * stride, sizeof( record ) );
		}
		if ( stableKey == 0u || ( flags & ~1u ) != 0u || record.nReserved0 != 0u || record.nReserved1 != 0u ||
			 record.nCoverage == 0u || record.nViewMask == 0u || record.nUvMaxX < record.nUvMinX ||
			 record.nUvMaxY < record.nUvMinY ) {
			return false;
		}
	}
	m_bytes.assign( pBytes, pBytes + ( size_t ) nBytes );
	m_header = header;
	m_nHeaderBytes = headerBytes;
	m_nRecordStride = stride;
	return true;
}

bool Terrain3DVTSpatialDemand::isValidFor(
	const uint64_t nSignature ) const {
	return !m_bytes.empty() && nSignature != 0u && m_header.nPackageSignature == nSignature;
}

uint32_t Terrain3DVTSpatialDemand::getCellMeters() const {
	return m_header.nCellMeters;
}
uint32_t Terrain3DVTSpatialDemand::getCellCount() const {
	return m_header.nCellCount;
}

const t3dVTDemandRecord_t *Terrain3DVTSpatialDemand::findCellHot(
	const int32_t nX,
	const int32_t nZ,
	uint32_t	 &rCount ) {
	rCount = 0u;
	if ( m_bytes.empty() ) {
		return nullptr;
	}
	for ( uint32_t i = 0u; i < T3D_VT_DEMAND_MRU_SLOTS; ++i ) {
		CachedCell &entry = m_cache[i];
		if ( entry.nCount != 0u && entry.nX == nX && entry.nZ == nZ ) {
			entry.nLastAccess = ++m_accessClock;
			rCount = entry.nCount;
			return entry.Records;
		}
	}
	const uint8_t *const cells = m_bytes.data() + m_nHeaderBytes;
	uint32_t			 low = 0u, high = m_header.nCellCount;
	while ( low < high ) {
		const uint32_t	  mid = low + ( high - low ) / 2u;
		t3dVTDemandCell_t cell{};
		memcpy( &cell, cells + ( size_t ) mid * sizeof( cell ), sizeof( cell ) );
		if ( cell.nX < nX || ( cell.nX == nX && cell.nZ < nZ ) ) {
			low = mid + 1u;
		} else {
			high = mid;
		}
	}
	if ( low >= m_header.nCellCount ) {
		return nullptr;
	}
	t3dVTDemandCell_t found{};
	memcpy( &found, cells + ( size_t ) low * sizeof( found ), sizeof( found ) );
	if ( found.nX != nX || found.nZ != nZ ) {
		return nullptr;
	}
	uint32_t victim = 0u;
	for ( uint32_t i = 1u; i < T3D_VT_DEMAND_MRU_SLOTS; ++i ) {
		if ( m_cache[i].nLastAccess < m_cache[victim].nLastAccess ) {
			victim = i;
		}
	}
	CachedCell			&entry = m_cache[victim];
	const uint64_t		 cellBytes = ( uint64_t ) m_header.nCellCount * sizeof( t3dVTDemandCell_t );
	const uint8_t *const records = cells + cellBytes + ( size_t ) found.nFirstRecord * m_nRecordStride;
	if ( m_header.nVersion == T3D_VT_DEMAND_VERSION_V2 ) {
		for ( uint32_t i = 0; i < found.nCount; ++i ) {
			t3dVTDemandRecordV2_t record{};
			memcpy( &record, records + ( size_t ) i * m_nRecordStride, sizeof( record ) );
			entry.Records[i] = record.Footprint;
			entry.Records[i].nReserved1 = ( uint16_t ) record.nFlags;
			entry.Records[i].nAllocation =
				m_boundAllocations.empty() ? UINT32_MAX : m_boundAllocations[found.nFirstRecord + i];
		}
	} else {
		memcpy( entry.Records, records, ( size_t ) found.nCount * sizeof( t3dVTDemandRecord_t ) );
	}
	entry.nX = nX;
	entry.nZ = nZ;
	entry.nCount = found.nCount;
	entry.nLastAccess = ++m_accessClock;
	rCount = entry.nCount;
	return entry.Records;
}

uint32_t Terrain3DVTSpatialDemand::getVersion() const {
	return m_header.nVersion;
}
uint64_t Terrain3DVTSpatialDemand::getAssetIdentityHash() const {
	if ( m_header.nVersion != T3D_VT_DEMAND_VERSION_V2 || m_bytes.size() < sizeof( t3dVTDemandHeaderV2_t ) ) {
		return 0u;
	}
	t3dVTDemandHeaderV2_t h{};
	memcpy( &h, m_bytes.data(), sizeof( h ) );
	return h.nAssetIdentityHash;
}
uint32_t Terrain3DVTSpatialDemand::getMappedRecordCount() const {
	return m_nMappedRecords;
}
bool Terrain3DVTSpatialDemand::hasBoundPredictor() const {
	return m_header.nVersion == T3D_VT_DEMAND_VERSION_V2 && m_nMappedRecords != 0u;
}

bool Terrain3DVTSpatialDemand::bindStableKeysCold(
	const uint64_t *const pKeys,
	const uint32_t *const pRuntimeAllocations,
	const uint32_t		  nCount ) {
	m_boundAllocations.clear();
	m_nMappedRecords = 0u;
	for ( uint32_t i = 0u; i < T3D_VT_DEMAND_MRU_SLOTS; ++i ) {
		m_cache[i].nCount = 0u;
	}
	if ( m_header.nVersion != T3D_VT_DEMAND_VERSION_V2 || pKeys == nullptr || pRuntimeAllocations == nullptr ) {
		return false;
	}
	const uint64_t offset = m_nHeaderBytes + ( uint64_t ) m_header.nCellCount * sizeof( t3dVTDemandCell_t );
	m_boundAllocations.resize( m_header.nRecordCount, UINT32_MAX );
	for ( uint32_t i = 0u; i < m_header.nRecordCount; ++i ) {
		t3dVTDemandRecordV2_t record{};
		memcpy( &record, m_bytes.data() + offset + ( size_t ) i * m_nRecordStride, sizeof( record ) );
		uint32_t low = 0u, high = nCount;
		while ( low < high ) {
			const uint32_t mid = low + ( high - low ) / 2u;
			if ( pKeys[mid] < record.nStableMaterialKey ) {
				low = mid + 1u;
			} else {
				high = mid;
			}
		}
		if ( low < nCount && pKeys[low] == record.nStableMaterialKey ) {
			m_boundAllocations[i] = pRuntimeAllocations[low];
			++m_nMappedRecords;
		}
	}
	return m_nMappedRecords != 0u;
}

bool Terrain3DVTSpatialDemand::hasCoveredCellHot(
	const int32_t nX,
	const int32_t nZ ) {
	if ( !hasBoundPredictor() ) {
		return false;
	}
	uint32_t						 count = 0u;
	const t3dVTDemandRecord_t *const records = findCellHot( nX, nZ, count );
	for ( uint32_t i = 0u; records && i < count; ++i ) {
		if ( records[i].nAllocation != UINT32_MAX ) {
			return true;
		}
	}
	return false;
}

bool t3d_vt_demand_write_v2(
	const t3dVTDemandCell_t *const	   pCells,
	const uint32_t					   nCells,
	const t3dVTDemandRecordV2_t *const pRecords,
	const uint32_t					   nRecords,
	const uint64_t					   nSignature,
	const uint64_t					   nAssetHash,
	std::vector< uint8_t >			  &rBytes ) {
	rBytes.clear();
	if ( nSignature == 0u || nAssetHash == 0u || nCells == 0u || nCells > T3D_VT_DEMAND_MAX_CELLS || nRecords == 0u ||
		 nRecords > T3D_VT_DEMAND_MAX_RECORDS || !pCells || !pRecords ) {
		return false;
	}
	t3dVTDemandHeaderV2_t h{};
	h.Base.nMagic = T3D_VT_DEMAND_MAGIC;
	h.Base.nVersion = T3D_VT_DEMAND_VERSION_V2;
	h.Base.nCellMeters = 128u;
	h.Base.nCellCount = nCells;
	h.Base.nRecordCount = nRecords;
	h.Base.nPackageSignature = nSignature;
	h.nAssetIdentityHash = nAssetHash;
	h.nRecordStride = sizeof( t3dVTDemandRecordV2_t );
	h.nOriginCellX = pCells[0].nX;
	h.nOriginCellZ = pCells[0].nZ;
	const size_t cb = ( size_t ) nCells * sizeof( t3dVTDemandCell_t );
	const size_t rb = ( size_t ) nRecords * sizeof( t3dVTDemandRecordV2_t );
	rBytes.resize( sizeof( h ) + cb + rb );
	memcpy( rBytes.data() + sizeof( h ), pCells, cb );
	memcpy( rBytes.data() + sizeof( h ) + cb, pRecords, rb );
	h.Base.nPayloadHash = t3d_vt_demand_hash( rBytes.data() + sizeof( h ), cb + rb );
	memcpy( rBytes.data(), &h, sizeof( h ) );
	// Validate all ordering, lengths, field bounds and hashes before publishing.
	Terrain3DVTSpatialDemand verify;
	if ( !verify.loadCold( rBytes.data(), rBytes.size() ) ) {
		rBytes.clear();
		return false;
	}
	return true;
}

// T01: offline v3 paged 4x4-cell codec. V1/V2 production paths above remain
// completely unchanged; runtime selection is introduced later in T04.
#include <limits.h>

static void t3dV3Put32( uint8_t *p, uint32_t value ) {
	for ( uint32_t k = 0u; k < 4u; ++k ) p[k] = ( uint8_t ) ( value >> ( 8u * k ) );
}
static void t3dV3Put64( uint8_t *p, uint64_t value ) {
	for ( uint32_t k = 0u; k < 8u; ++k ) p[k] = ( uint8_t ) ( value >> ( 8u * k ) );
}
static uint32_t t3dV3Get32( const uint8_t *p ) {
	return ( uint32_t ) p[0] | ( ( uint32_t ) p[1] << 8u ) |
		( ( uint32_t ) p[2] << 16u ) | ( ( uint32_t ) p[3] << 24u );
}
static uint64_t t3dV3Get64( const uint8_t *p ) {
	uint64_t value = 0u;
	for ( uint32_t k = 0u; k < 8u; ++k ) value |= ( uint64_t ) p[k] << ( 8u * k );
	return value;
}
static int32_t t3dV3GetSigned( const uint8_t *p ) {
	const uint32_t bits = t3dV3Get32( p );
	int32_t value = 0;
	memcpy( &value, &bits, sizeof( value ) );
	return value;
}
static uint32_t t3dV3SignedBits( int32_t value ) {
	uint32_t bits;
	memcpy( &bits, &value, sizeof( bits ) );
	return bits;
}
static int32_t t3dV3Group( int32_t value ) {
	return value >= 0 ? value / 4 : ( int32_t ) ( -( ( -( int64_t ) value + 3 ) / 4 ) );
}
static bool t3dV3ValidConfig( const t3dVTDemandV3CookConfig_t &c ) {
	return c.m_nPackageSignature != 0u && c.m_nAssetIdentityHash != 0u &&
		c.m_nSourceSignature != 0u && c.m_nBakeConfigSignature != 0u &&
		c.m_nNominalTextureSize >= 16u && c.m_nNominalTextureSize <= 32768u &&
		( c.m_nNominalTextureSize & ( c.m_nNominalTextureSize - 1u ) ) == 0u &&
		c.m_nNominalRenderSize >= 16u && c.m_nNominalRenderSize <= 4096u &&
		c.m_nFovMilliDegrees > 0u && c.m_nFovMilliDegrees < 180000u &&
		c.m_nAlgorithmRevision > 0u;
}
static bool t3dV3ValidRecord( const t3dVTDemandV3Record_t &r ) {
	if ( r.m_nStableMaterialKey == 0u || r.m_nViewBin >= T3D_VT_DEMAND_V3_VIEW_BINS ||
		( r.m_nFlags & ~( T3D_VT_VISIBILITY_PARTIAL | T3D_VT_VISIBILITY_NO_VISIBLE ) ) != 0u ||
		r.m_nNominalMipBase >= 16u || r.m_nSourceRevision == 0u || r.m_nProbeCount == 0u ||
		r.m_nCoverageSampleCount > r.m_nSampleCount ) return false;
	uint64_t sum = 0u;
	for ( uint32_t i = 0u; i < 16u; ++i ) sum += r.m_szMipWeight[i];
	if ( ( r.m_nFlags & T3D_VT_VISIBILITY_NO_VISIBLE ) != 0u ) {
		return sum == 0u && r.m_nSampleCount == 0u && r.m_nCoverageSampleCount == 0u;
	}
	return sum > 0u && r.m_nSampleCount > 0u && r.m_nCoverageConfidence != 0u;
}
static bool t3dV3Before( const t3dVTDemandV3Cell_t &a, const t3dVTDemandV3Cell_t &b ) {
	const int32_t ax = t3dV3Group( a.m_nX ), az = t3dV3Group( a.m_nZ );
	const int32_t bx = t3dV3Group( b.m_nX ), bz = t3dV3Group( b.m_nZ );
	if ( ax != bx ) return ax < bx;
	if ( az != bz ) return az < bz;
	if ( a.m_nX != b.m_nX ) return a.m_nX < b.m_nX;
	return a.m_nZ < b.m_nZ;
}
static void t3dV3EncodeRecord( uint8_t *p, const t3dVTDemandV3Record_t &r ) {
	memset( p, 0, T3D_VT_DEMAND_V3_RECORD_BYTES );
	t3dV3Put64( p, r.m_nStableMaterialKey );
	p[8] = r.m_nViewBin; p[9] = r.m_nFlags;
	p[10] = ( uint8_t ) r.m_nCoverageConfidence;
	p[11] = ( uint8_t ) ( r.m_nCoverageConfidence >> 8u );
	t3dV3Put32( p + 12, r.m_nSampleCount );
	for ( uint32_t i = 0u; i < 16u; ++i ) {
		p[16u + i * 2u] = ( uint8_t ) r.m_szMipWeight[i];
		p[17u + i * 2u] = ( uint8_t ) ( r.m_szMipWeight[i] >> 8u );
	}
	p[48] = ( uint8_t ) r.m_nNominalMipBase;
	p[49] = ( uint8_t ) ( r.m_nNominalMipBase >> 8u );
	t3dV3Put32( p + 52, r.m_nSourceRevision );
	t3dV3Put32( p + 56, r.m_nProbeCount );
	t3dV3Put32( p + 60, r.m_nCoverageSampleCount );
}
static bool t3dV3DecodeRecord( const uint8_t *p, t3dVTDemandV3Record_t &r ) {
	if ( p[50] != 0u || p[51] != 0u ) return false;
	r.m_nStableMaterialKey = t3dV3Get64( p );
	r.m_nViewBin = p[8]; r.m_nFlags = p[9];
	r.m_nCoverageConfidence = ( uint16_t ) ( p[10] | ( ( uint16_t ) p[11] << 8u ) );
	r.m_nSampleCount = t3dV3Get32( p + 12 );
	for ( uint32_t i = 0u; i < 16u; ++i )
		r.m_szMipWeight[i] = ( uint16_t ) ( p[16u + i * 2u] | ( ( uint16_t ) p[17u + i * 2u] << 8u ) );
	r.m_nNominalMipBase = ( uint16_t ) ( p[48] | ( ( uint16_t ) p[49] << 8u ) );
	r.m_nSourceRevision = t3dV3Get32( p + 52 );
	r.m_nProbeCount = t3dV3Get32( p + 56 );
	r.m_nCoverageSampleCount = t3dV3Get32( p + 60 );
	return t3dV3ValidRecord( r );
}

// Hash the immutable v3 header AND both directories. Hash fields are treated
// as zero to avoid a self-reference. This rejects tampered bake settings,
// version/dimensions and directory offsets before any record allocation.
static uint64_t t3dV3DirectoryHash( const uint8_t *bytes, uint64_t payloadOffset ) {
	uint64_t hash = 14695981039346656037ull;
	for ( uint64_t i = 0u; i < payloadOffset; ++i ) {
		const uint8_t value = i >= 120u && i < 136u ? 0u : bytes[i];
		hash = ( hash ^ value ) * 1099511628211ull;
	}
	return hash;
}

// T03 deterministic PackBits bundles. Codec 0 remains the T01 raw format;
// codec 1 is used only when it shrinks the payload. Decoder always verifies
// exact uncompressed length before exposing the record stream.
static void t3dV3PackBitsEncode( const std::vector< uint8_t > &input,
                                 std::vector< uint8_t > &output ) {
    output.clear();
    for ( size_t i = 0u; i < input.size(); ) {
        size_t run = 1u;
        while ( run < 128u && i + run < input.size() && input[i + run] == input[i] ) ++run;
        if ( run >= 3u ) {
            output.push_back( ( uint8_t ) ( 257u - run ) );
            output.push_back( input[i] );
            i += run;
        } else {
            const size_t begin = i;
            i += run;
            while ( i < input.size() && i - begin < 128u ) {
                size_t next = 1u;
                while ( next < 128u && i + next < input.size() && input[i + next] == input[i] ) ++next;
                if ( next >= 3u ) break;
                i += std::min( next, 128u - ( i - begin ) );
            }
            output.push_back( ( uint8_t ) ( i - begin - 1u ) );
            output.insert( output.end(), input.begin() + begin, input.begin() + i );
        }
    }
}
static bool t3dV3PackBitsDecode( const uint8_t *src, const uint32_t encodedBytes,
                                 uint8_t *dst, const uint32_t decodedBytes ) {
    uint32_t in = 0u, out = 0u;
    while ( in < encodedBytes ) {
        const uint8_t code = src[in++];
        if ( code < 128u ) {
            const uint32_t count = ( uint32_t ) code + 1u;
            if ( count > encodedBytes - in || count > decodedBytes - out ) return false;
            memcpy( dst + out, src + in, count );
            in += count;
            out += count;
        } else if ( code > 128u ) {
            const uint32_t count = 257u - code;
            if ( in == encodedBytes || count > decodedBytes - out ) return false;
            memset( dst + out, src[in++], count );
            out += count;
        } else return false; // PackBits NOP is disallowed in canonical files
    }
    return out == decodedBytes;
}

struct t3dV3Group_t {
	int32_t x = 0, z = 0;
	uint32_t firstCell = 0u, cellCount = 0u, firstRecord = 0u, recordCount = 0u;
};

bool t3d_vt_demand_write_v3(
	const t3dVTDemandV3Cell_t *const cells, uint32_t cellCount,
	const t3dVTDemandV3Record_t *const records, uint32_t recordCount,
	const t3dVTDemandV3CookConfig_t &config, std::vector< uint8_t > &bytes ) {
	bytes.clear();
	if ( !t3dV3ValidConfig( config ) || !cells || !records || cellCount == 0u ||
		cellCount > T3D_VT_DEMAND_V3_MAX_CELLS || recordCount == 0u ||
		recordCount > T3D_VT_DEMAND_V3_MAX_RECORDS ) return false;
	std::vector< t3dV3Group_t > groups;
	uint32_t next = 0u;
	for ( uint32_t i = 0u; i < cellCount; ++i ) {
		const t3dVTDemandV3Cell_t &c = cells[i];
		if ( c.m_nFlags > 1u || c.m_nRecordCount == 0u || c.m_nFirstRecord != next ||
			c.m_nRecordCount > recordCount - next || ( i > 0 && !t3dV3Before( cells[i - 1u], c ) ) ) return false;
		const int32_t gx = t3dV3Group( c.m_nX ), gz = t3dV3Group( c.m_nZ );
		if ( groups.empty() || groups.back().x != gx || groups.back().z != gz ) {
			t3dV3Group_t g;
			g.x = gx; g.z = gz; g.firstCell = i; g.firstRecord = next;
			groups.push_back( g );
		}
		t3dV3Group_t &g = groups.back();
		++g.cellCount;
		g.recordCount += c.m_nRecordCount;
		if ( ( uint64_t ) g.recordCount * T3D_VT_DEMAND_V3_RECORD_BYTES >
			T3D_VT_DEMAND_V3_MAX_BUNDLE_BYTES ) return false;
		for ( uint32_t k = 0u; k < c.m_nRecordCount; ++k ) {
			const t3dVTDemandV3Record_t &r = records[next + k];
			if ( !t3dV3ValidRecord( r ) ) return false;
			if ( k > 0u ) {
				const t3dVTDemandV3Record_t &prev = records[next + k - 1u];
				if ( prev.m_nStableMaterialKey > r.m_nStableMaterialKey ||
					( prev.m_nStableMaterialKey == r.m_nStableMaterialKey && prev.m_nViewBin >= r.m_nViewBin ) )
					return false;
			}
		}
		next += c.m_nRecordCount;
	}
	if ( next != recordCount ) return false;
    std::vector< std::vector< uint8_t > > encodedGroups( groups.size() );
    std::vector< uint32_t > codecs( groups.size(), 0u );
    uint64_t encodedTotal = 0u;
    for ( size_t i = 0u; i < groups.size(); ++i ) {
        const auto &g = groups[i];
        std::vector< uint8_t > raw( ( size_t ) g.recordCount * T3D_VT_DEMAND_V3_RECORD_BYTES );
        for ( uint32_t k = 0u; k < g.recordCount; ++k )
            t3dV3EncodeRecord( raw.data() + ( size_t ) k * T3D_VT_DEMAND_V3_RECORD_BYTES,
                               records[g.firstRecord + k] );
        std::vector< uint8_t > compressed;
        t3dV3PackBitsEncode( raw, compressed );
        if ( compressed.size() + 8u < raw.size() ) {
            codecs[i] = 1u;
            encodedGroups[i].swap( compressed );
        } else encodedGroups[i].swap( raw );
        encodedTotal += encodedGroups[i].size();
    }
    const uint64_t bundleDirOffset = T3D_VT_DEMAND_V3_HEADER_BYTES +
        ( uint64_t ) cellCount * T3D_VT_DEMAND_V3_CELL_BYTES;
    const uint64_t payloadOffset = bundleDirOffset +
        ( uint64_t ) groups.size() * T3D_VT_DEMAND_V3_BUNDLE_BYTES;
    const uint64_t totalBytes = payloadOffset + encodedTotal;
    if ( totalBytes > T3D_VT_DEMAND_V3_MAX_FILE_BYTES ) return false;
	bytes.resize( ( size_t ) totalBytes, 0u );
	uint8_t *const head = bytes.data();
	t3dV3Put32( head, T3D_VT_DEMAND_MAGIC );
	t3dV3Put32( head + 4, T3D_VT_DEMAND_VERSION_V3 );
	t3dV3Put32( head + 8, T3D_VT_DEMAND_V3_ENDIAN_TAG );
	t3dV3Put32( head + 12, T3D_VT_DEMAND_V3_HEADER_BYTES );
	t3dV3Put32( head + 16, T3D_VT_DEMAND_V3_CELL_METERS );
	t3dV3Put32( head + 20, T3D_VT_DEMAND_V3_VIEW_BINS );
	t3dV3Put32( head + 24, T3D_VT_DEMAND_V3_MIP_BINS );
	t3dV3Put32( head + 32, config.m_nNominalTextureSize );
	t3dV3Put32( head + 36, config.m_nNominalRenderSize );
	t3dV3Put32( head + 40, config.m_nFovMilliDegrees );
	t3dV3Put32( head + 44, config.m_nAlgorithmRevision );
	t3dV3Put64( head + 48, config.m_nPackageSignature );
	t3dV3Put64( head + 56, config.m_nAssetIdentityHash );
	t3dV3Put64( head + 64, config.m_nSourceSignature );
	t3dV3Put64( head + 72, config.m_nBakeConfigSignature );
	t3dV3Put32( head + 80, cellCount );
	t3dV3Put32( head + 84, ( uint32_t ) groups.size() );
	t3dV3Put64( head + 88, T3D_VT_DEMAND_V3_HEADER_BYTES );
	t3dV3Put64( head + 96, bundleDirOffset );
	t3dV3Put64( head + 104, payloadOffset );
	t3dV3Put64( head + 112, totalBytes );
	uint32_t gIndex = 0u;
	for ( uint32_t i = 0u; i < cellCount; ++i ) {
		while ( gIndex + 1u < groups.size() && i >= groups[gIndex].firstCell + groups[gIndex].cellCount )
			++gIndex;
		const t3dVTDemandV3Cell_t &c = cells[i];
		uint8_t *const dst = head + T3D_VT_DEMAND_V3_HEADER_BYTES +
			( size_t ) i * T3D_VT_DEMAND_V3_CELL_BYTES;
		t3dV3Put32( dst, t3dV3SignedBits( c.m_nX ) );
		t3dV3Put32( dst + 4, t3dV3SignedBits( c.m_nZ ) );
		t3dV3Put32( dst + 8, gIndex );
		t3dV3Put32( dst + 12, ( c.m_nFirstRecord - groups[gIndex].firstRecord ) * T3D_VT_DEMAND_V3_RECORD_BYTES );
		t3dV3Put32( dst + 16, c.m_nRecordCount );
		t3dV3Put32( dst + 20, c.m_nRecordCount * T3D_VT_DEMAND_V3_RECORD_BYTES );
		t3dV3Put32( dst + 24, c.m_nFlags );
	}
    uint64_t cursor = payloadOffset;
    for ( uint32_t g = 0u; g < groups.size(); ++g ) {
        const t3dV3Group_t &group = groups[g];
        const auto &buffer = encodedGroups[g];
        const uint32_t encoded = ( uint32_t ) buffer.size();
        const uint32_t decoded = group.recordCount * T3D_VT_DEMAND_V3_RECORD_BYTES;
        uint8_t *const dir = head + bundleDirOffset + ( size_t ) g * T3D_VT_DEMAND_V3_BUNDLE_BYTES;
        t3dV3Put32( dir, t3dV3SignedBits( group.x ) );
        t3dV3Put32( dir + 4, t3dV3SignedBits( group.z ) );
        t3dV3Put64( dir + 8, cursor );
        t3dV3Put32( dir + 16, encoded );
        t3dV3Put32( dir + 20, decoded );
        t3dV3Put32( dir + 32, codecs[g] );
        t3dV3Put32( dir + 36, group.recordCount );
        memcpy( head + cursor, buffer.data(), encoded );
        t3dV3Put64( dir + 24, t3d_vt_demand_hash( head + cursor, encoded ) );
        cursor += encoded;
    }
	t3dV3Put64( head + 120, t3dV3DirectoryHash( head, payloadOffset ) );
	t3dV3Put64( head + 128, t3d_vt_demand_hash( head + payloadOffset, totalBytes - payloadOffset ) );
	std::vector< t3dVTDemandV3Cell_t > verifyCells;
	std::vector< t3dVTDemandV3Record_t > verifyRecords;
	if ( !t3d_vt_demand_read_v3( head, totalBytes, config.m_nPackageSignature,
		config.m_nSourceSignature, verifyCells, verifyRecords ) ) {
		bytes.clear();
		return false;
	}
	return true;
}

bool t3d_vt_demand_read_v3(
	const uint8_t *const bytes, const uint64_t size,
	const uint64_t expectedPackage, const uint64_t expectedSource,
	std::vector< t3dVTDemandV3Cell_t > &outCells,
	std::vector< t3dVTDemandV3Record_t > &outRecords,
	t3dVTDemandV3CookConfig_t *const outConfig ) {
	// Keep previous caller data intact on decode failure.
	if ( !bytes || size < T3D_VT_DEMAND_V3_HEADER_BYTES || size > T3D_VT_DEMAND_V3_MAX_FILE_BYTES ||
		expectedPackage == 0u || expectedSource == 0u ) return false;
	if ( t3dV3Get32( bytes ) != T3D_VT_DEMAND_MAGIC ||
		t3dV3Get32( bytes + 4 ) != T3D_VT_DEMAND_VERSION_V3 ||
		t3dV3Get32( bytes + 8 ) != T3D_VT_DEMAND_V3_ENDIAN_TAG ||
		t3dV3Get32( bytes + 12 ) != T3D_VT_DEMAND_V3_HEADER_BYTES ||
		t3dV3Get32( bytes + 16 ) != T3D_VT_DEMAND_V3_CELL_METERS ||
		t3dV3Get32( bytes + 20 ) != T3D_VT_DEMAND_V3_VIEW_BINS ||
		t3dV3Get32( bytes + 24 ) != T3D_VT_DEMAND_V3_MIP_BINS ||
		t3dV3Get32( bytes + 28 ) != 0u ||
		t3dV3Get64( bytes + 48 ) != expectedPackage ||
		t3dV3Get64( bytes + 64 ) != expectedSource ||
		t3dV3Get64( bytes + 112 ) != size ) return false;
	t3dVTDemandV3CookConfig_t config;
	config.m_nNominalTextureSize = t3dV3Get32( bytes + 32 );
	config.m_nNominalRenderSize = t3dV3Get32( bytes + 36 );
	config.m_nFovMilliDegrees = t3dV3Get32( bytes + 40 );
	config.m_nAlgorithmRevision = t3dV3Get32( bytes + 44 );
	config.m_nPackageSignature = t3dV3Get64( bytes + 48 );
	config.m_nAssetIdentityHash = t3dV3Get64( bytes + 56 );
	config.m_nSourceSignature = t3dV3Get64( bytes + 64 );
	config.m_nBakeConfigSignature = t3dV3Get64( bytes + 72 );
	if ( !t3dV3ValidConfig( config ) ) return false;
	const uint32_t cellCount = t3dV3Get32( bytes + 80 );
	const uint32_t groupCount = t3dV3Get32( bytes + 84 );
	if ( cellCount == 0u || cellCount > T3D_VT_DEMAND_V3_MAX_CELLS ||
		groupCount == 0u || groupCount > cellCount ) return false;
	const uint64_t groupDir = T3D_VT_DEMAND_V3_HEADER_BYTES +
		( uint64_t ) cellCount * T3D_VT_DEMAND_V3_CELL_BYTES;
	const uint64_t payloadOffset = groupDir + ( uint64_t ) groupCount * T3D_VT_DEMAND_V3_BUNDLE_BYTES;
	if ( t3dV3Get64( bytes + 88 ) != T3D_VT_DEMAND_V3_HEADER_BYTES ||
		t3dV3Get64( bytes + 96 ) != groupDir || t3dV3Get64( bytes + 104 ) != payloadOffset ||
		payloadOffset > size ||
		t3dV3DirectoryHash( bytes, payloadOffset ) != t3dV3Get64( bytes + 120 ) ||
		t3d_vt_demand_hash( bytes + payloadOffset, size - payloadOffset ) != t3dV3Get64( bytes + 128 ) ) return false;
	struct LoadedGroup {
        int32_t x = 0, z = 0;
        uint64_t offset = 0, decodedOffset = 0;
        uint32_t count = 0;
    };
	std::vector< LoadedGroup > groups;
	groups.reserve( groupCount );
	uint64_t payloadCursor = payloadOffset;
	uint64_t totalRecords = 0u;
    std::vector< uint8_t > decodedPayload;
    decodedPayload.reserve( std::min( ( uint64_t ) T3D_VT_DEMAND_V3_MAX_RECORDS *
        T3D_VT_DEMAND_V3_RECORD_BYTES, size * 2u ) );
	for ( uint32_t i = 0u; i < groupCount; ++i ) {
		const uint8_t *const dir = bytes + groupDir + ( size_t ) i * T3D_VT_DEMAND_V3_BUNDLE_BYTES;
		LoadedGroup g;
		g.x = t3dV3GetSigned( dir ); g.z = t3dV3GetSigned( dir + 4 );
		g.offset = t3dV3Get64( dir + 8 );
		const uint32_t encoded = t3dV3Get32( dir + 16 ), decoded = t3dV3Get32( dir + 20 );
		g.count = t3dV3Get32( dir + 36 );
        const uint32_t codec = t3dV3Get32( dir + 32 );
		if ( g.count == 0u || g.count > T3D_VT_DEMAND_V3_MAX_BUNDLE_BYTES / T3D_VT_DEMAND_V3_RECORD_BYTES ||
            decoded != g.count * T3D_VT_DEMAND_V3_RECORD_BYTES || !encoded ||
            codec > 1u || ( codec == 0u && encoded != decoded ) ||
			g.offset != payloadCursor || encoded > size - payloadCursor ||
			t3d_vt_demand_hash( bytes + g.offset, encoded ) != t3dV3Get64( dir + 24 ) ||
			( i > 0u && ( g.x < groups.back().x || ( g.x == groups.back().x && g.z <= groups.back().z ) ) ) ) return false;
        if ( totalRecords + g.count > T3D_VT_DEMAND_V3_MAX_RECORDS ) return false;
        g.decodedOffset = decodedPayload.size();
        decodedPayload.resize( decodedPayload.size() + decoded );
        if ( codec == 0u ) memcpy( decodedPayload.data() + g.decodedOffset, bytes + g.offset, decoded );
        else if ( !t3dV3PackBitsDecode( bytes + g.offset, encoded,
                                        decodedPayload.data() + g.decodedOffset, decoded ) ) return false;
		payloadCursor += encoded;
		totalRecords += g.count;
		groups.push_back( g );
	}
	if ( payloadCursor != size || totalRecords > T3D_VT_DEMAND_V3_MAX_RECORDS ) return false;
	std::vector< t3dVTDemandV3Cell_t > candidateCells;
	std::vector< t3dVTDemandV3Record_t > candidateRecords;
	candidateCells.reserve( cellCount );
	candidateRecords.reserve( ( size_t ) totalRecords );
	uint32_t activeGroup = 0u, nextGroupByte = 0u;
	for ( uint32_t i = 0u; i < cellCount; ++i ) {
		const uint8_t *const entry = bytes + T3D_VT_DEMAND_V3_HEADER_BYTES +
			( size_t ) i * T3D_VT_DEMAND_V3_CELL_BYTES;
		t3dVTDemandV3Cell_t c;
		c.m_nX = t3dV3GetSigned( entry ); c.m_nZ = t3dV3GetSigned( entry + 4 );
		const uint32_t group = t3dV3Get32( entry + 8 ), byteOffset = t3dV3Get32( entry + 12 );
		c.m_nRecordCount = t3dV3Get32( entry + 16 );
		const uint32_t byteCount = t3dV3Get32( entry + 20 );
		c.m_nFlags = t3dV3Get32( entry + 24 );
		if ( group >= groups.size() || c.m_nRecordCount == 0u ||
			c.m_nFlags > 1u || t3dV3Get32( entry + 28 ) != 0u ||
			c.m_nRecordCount > T3D_VT_DEMAND_V3_MAX_BUNDLE_BYTES / T3D_VT_DEMAND_V3_RECORD_BYTES ||
			byteCount != c.m_nRecordCount * T3D_VT_DEMAND_V3_RECORD_BYTES ||
			t3dV3Group( c.m_nX ) != groups[group].x || t3dV3Group( c.m_nZ ) != groups[group].z ||
			( i > 0u && !t3dV3Before( candidateCells.back(), c ) ) ) return false;
		if ( group != activeGroup ) {
			if ( group != activeGroup + 1u || nextGroupByte != groups[activeGroup].count * T3D_VT_DEMAND_V3_RECORD_BYTES )
				return false;
			activeGroup = group;
			nextGroupByte = 0u;
		}
		if ( byteOffset != nextGroupByte || byteCount > groups[group].count * T3D_VT_DEMAND_V3_RECORD_BYTES - nextGroupByte )
			return false;
		c.m_nFirstRecord = ( uint32_t ) candidateRecords.size();
		for ( uint32_t k = 0u; k < c.m_nRecordCount; ++k ) {
			t3dVTDemandV3Record_t record;
			if ( !t3dV3DecodeRecord( decodedPayload.data() + groups[group].decodedOffset + byteOffset +
				( size_t ) k * T3D_VT_DEMAND_V3_RECORD_BYTES, record ) ) return false;
			if ( k > 0u ) {
				const t3dVTDemandV3Record_t &prev = candidateRecords.back();
				if ( prev.m_nStableMaterialKey > record.m_nStableMaterialKey ||
					( prev.m_nStableMaterialKey == record.m_nStableMaterialKey && prev.m_nViewBin >= record.m_nViewBin ) )
					return false;
			}
			candidateRecords.push_back( record );
		}
		nextGroupByte += byteCount;
		candidateCells.push_back( c );
	}
	if ( activeGroup + 1u != groups.size() ||
		nextGroupByte != groups.back().count * T3D_VT_DEMAND_V3_RECORD_BYTES ||
		candidateRecords.size() != totalRecords ) return false;
	outCells.swap( candidateCells );
	outRecords.swap( candidateRecords );
	if ( outConfig ) *outConfig = config;
	return true;
}
