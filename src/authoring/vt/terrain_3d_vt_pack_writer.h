// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_PACK_WRITER_H
#define TERRAIN3D_VT_PACK_WRITER_H

#include "rendering/rd/vt/terrain_3d_vt_contract.h"

#include <stdint.h>

#define T3D_VT_PACK_V3_HEADER_BYTES 256u
#define T3D_VT_PACK_ALIGNMENT 16u
#define T3D_VT_PACK_CODEC_NONE 0u
#define T3D_VT_PACK_CODEC_ZSTD 1u
#define T3D_VT_PACK_FLAG_CLUSTERED_PAYLOADS ( 1u << 0u )
#define T3D_VT_PACK_FLAG_TAILS_PRESENT ( 1u << 1u )
#define T3D_VT_PACK_FLAG_GPU_READY ( 1u << 2u )
#define T3D_VT_PACK_FLAG_OUTER_COMPRESSION ( 1u << 3u )
#define T3D_VT_PACK_FLAG_QUALITY_VIEWS ( 1u << 4u )
#define T3D_VT_PACK_FLAG_FORMAT_FAMILY_MAP ( 1u << 5u )
#define T3D_VT_PACK_PAYLOAD_FLAG_TAIL ( 1u << 0u )
#define T3D_VT_PACK_TAIL_LAYOUT_HORIZONTAL_V1 1u

typedef struct t3dVTPackAllocationKeyRecordV3_s {
	uint64_t m_nStableKey;
	uint32_t m_nAllocationId;
	uint32_t m_nContentId;
} t3dVTPackAllocationKeyRecordV3_t;

typedef struct t3dVTPackPayloadRecordV3_s {
	uint64_t m_nContentSignature;
	uint32_t m_nFormat;
	uint32_t m_nClusterId;
	uint32_t m_nClusterOffset;
	uint32_t m_nStoredBytes;
	uint32_t m_nDecodedBytes;
	uint32_t m_nCodec;
	uint32_t m_nChecksum;
	uint32_t m_nFlags;
	uint32_t m_nReserved;
} t3dVTPackPayloadRecordV3_t;

typedef struct t3dVTPackClusterRecordV3_s {
	uint64_t m_nPayloadDataOffset;
	uint64_t m_nContentSignature;
	uint32_t m_nStoredBytes;
	uint32_t m_nDecodedBytes;
	uint32_t m_nFirstPayload;
	uint32_t m_nPayloadCount;
	uint32_t m_nCodec;
	uint32_t m_nChecksum;
} t3dVTPackClusterRecordV3_t;

// Frozen Sparse VT V3 package ABI.
typedef struct t3dVTPackHeaderV3_s {
	uint32_t m_nMagic;
	uint32_t m_nVersion;
	uint32_t m_nHeaderBytes;
	uint32_t m_nFlags;
	uint32_t m_nAllocationCount;
	uint32_t m_nChannelAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nPayloadCount;
	uint32_t m_nClusterCount;
	uint32_t m_nTailCount;
	uint32_t m_nChannelCount;
	uint32_t m_nInteriorTexels;
	uint32_t m_nGutterTexels;
	uint32_t m_nPhysicalTexels;
	uint32_t m_nPackageGeneration;
	uint32_t m_nHeaderChecksum;
	uint32_t m_nQualityViewCount;
	uint32_t m_nChannelPageCount[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nChannelTailCount[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nFormatFamilyCount;
	uint32_t m_nFullySupportedProfileMask;
	uint64_t m_nAllocationOffset;
	uint64_t m_nAllocationKeyOffset;
	uint64_t m_nChannelAllocationOffset;
	uint64_t m_nPageOffset;
	uint64_t m_nPayloadDirectoryOffset;
	uint64_t m_nClusterDirectoryOffset;
	uint64_t m_nTailMetadataOffset;
	uint64_t m_nQualityViewOffset;
	uint64_t m_nFormatFamilyOffset;
	uint64_t m_nPayloadDataOffset;
	uint64_t m_nPayloadDataBytes;
	uint64_t m_nContentSignature;
	uint64_t m_nPackageBytes;
	uint64_t m_nReserved[4];
} t3dVTPackHeaderV3_t;

typedef struct t3dVTPackSourceV3_s {
	const t3dVTPackAllocationRecordV3_t *m_pAllocations;
	uint32_t m_nAllocationCount;
	const t3dVTPackAllocationKeyRecordV3_t *m_pAllocationKeys;
	const t3dVTChannelAllocation_t *m_pChannelAllocations;
	uint32_t m_nChannelAllocationCount;
	const t3dVTPackPageRecordV3_t *m_pPages;
	uint32_t m_nPageCount;
	const t3dVTPackPayloadRecordV3_t *m_pPayloads;
	uint32_t m_nPayloadCount;
	const t3dVTPackClusterRecordV3_t *m_pClusters;
	uint32_t m_nClusterCount;
	const t3dVTPackTailRecordV3_t *m_pTails;
	uint32_t m_nTailCount;
	const t3dVTPackQualityViewV3_t *m_pQualityViews;
	uint32_t m_nQualityViewCount;
	const t3dVTPackFormatFamilyRecordV3_t *m_pFormatFamilies;
	uint32_t m_nFormatFamilyCount;
	const uint8_t *m_pPayloadData;
	uint64_t m_nPayloadDataBytes;
	uint32_t m_nPackageGeneration;
} t3dVTPackSourceV3_t;

typedef struct t3dVTPackViewV3_s {
	const t3dVTPackHeaderV3_t *m_pHeader;
	const t3dVTPackAllocationRecordV3_t *m_pAllocations;
	const t3dVTPackAllocationKeyRecordV3_t *m_pAllocationKeys;
	const t3dVTChannelAllocation_t *m_pChannelAllocations;
	const t3dVTPackPageRecordV3_t *m_pPages;
	const t3dVTPackPayloadRecordV3_t *m_pPayloads;
	const t3dVTPackClusterRecordV3_t *m_pClusters;
	const t3dVTPackTailRecordV3_t *m_pTails;
	const t3dVTPackQualityViewV3_t *m_pQualityViews;
	const t3dVTPackFormatFamilyRecordV3_t *m_pFormatFamilies;
	const uint8_t *m_pPayloadData;
} t3dVTPackViewV3_t;

uint64_t t3d_vt_pack_hash64( const void *pBytes, uint64_t nBytes );
uint32_t t3d_vt_pack_checksum32( const void *pBytes, uint64_t nBytes );
uint32_t t3d_vt_pack_expected_tile_bytes( uint32_t nFormat );
uint32_t t3d_vt_pack_expected_physical_tile_bytes_v3( uint32_t nPhysicalFamily );
bool t3d_vt_pack_init_format_family_records_v3( t3dVTPackFormatFamilyRecordV3_t *pRecords, uint32_t nRecordCount );
bool t3d_vt_pack_validate_source_v3( const t3dVTPackSourceV3_t &rSource );
uint64_t t3d_vt_pack_compute_bytes_v3( const t3dVTPackSourceV3_t &rSource );
bool t3d_vt_pack_write_v3( const t3dVTPackSourceV3_t &rSource, void *pDestination, uint64_t nDestinationBytes, uint64_t &rWrittenBytes );
bool t3d_vt_pack_open_view_v3( const void *pPackage, uint64_t nPackageBytes, t3dVTPackViewV3_t &rView );

static_assert( sizeof( t3dVTPackHeaderV3_t ) == T3D_VT_PACK_V3_HEADER_BYTES, "VT pack v3 header ABI changed" );
static_assert( sizeof( t3dVTPackAllocationRecordV3_t ) == 16u, "VT pack v3 allocation ABI changed" );
static_assert( sizeof( t3dVTPackPageRecordV3_t ) == 36u, "VT pack v3 page ABI changed" );
static_assert( sizeof( t3dVTPackTailRecordV3_t ) == 40u, "VT pack v3 tail ABI changed" );
static_assert( sizeof( t3dVTPackQualityViewV3_t ) == 16u, "VT pack v3 quality-view ABI changed" );
static_assert( sizeof( t3dVTPackFormatFamilyRecordV3_t ) == 8u, "VT pack v3 format-family ABI changed" );
static_assert( sizeof( t3dVTPackAllocationKeyRecordV3_t ) == 16u, "VT pack v3 allocation-key ABI changed" );
static_assert( sizeof( t3dVTPackPayloadRecordV3_t ) == 48u, "VT pack v3 payload ABI changed" );
static_assert( sizeof( t3dVTPackClusterRecordV3_t ) == 40u, "VT pack v3 cluster ABI changed" );

#endif // TERRAIN3D_VT_PACK_WRITER_H
