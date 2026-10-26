// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_PACK_WRITER_H
#define TERRAIN3D_VT_PACK_WRITER_H

#include "Terrain3DTypes.h"

#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"

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

// t3dVTPackAllocationKeyRecordV3_s centralized in Terrain3DTypes.h.


// t3dVTPackPayloadRecordV3_s centralized in Terrain3DTypes.h.


// t3dVTPackClusterRecordV3_s centralized in Terrain3DTypes.h.


// Frozen Sparse VT V3 package ABI.
// t3dVTPackHeaderV3_s centralized in Terrain3DTypes.h.


// t3dVTPackSourceV3_s centralized in Terrain3DTypes.h.


// t3dVTPackViewV3_s centralized in Terrain3DTypes.h.


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
