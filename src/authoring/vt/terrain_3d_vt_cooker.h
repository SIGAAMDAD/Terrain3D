// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_COOKER_H
#define TERRAIN3D_VT_COOKER_H

#include "authoring/vt/terrain_3d_vt_page_builder.h"
#include "authoring/vt/terrain_3d_vt_pack_writer.h"
#include "rendering/rd/vt/terrain_3d_vt_fixed_profile.h"

#include <stdint.h>

typedef enum t3dVTOuterCompression_e : uint32_t {
	T3D_VT_OUTER_COMPRESSION_NONE = 0u,
	T3D_VT_OUTER_COMPRESSION_ZSTD = 1u,
} t3dVTOuterCompression_t;

typedef struct t3dVTCookerSettings_s {
	uint32_t m_nPageInterior;
	uint32_t m_nPageGutter;
	uint32_t m_nClusterTargetPages;
	uint32_t m_nClusterTargetBytes;
	uint32_t m_nOuterCompression;
	uint32_t m_nMipQuality;
	uint32_t m_nCompressionQuality;
} t3dVTCookerSettings_t;

typedef struct t3dVTEncodeTileRequest_s {
	const uint8_t *m_pSourcePixels;
	uint32_t m_nSourceBytes;
	uint32_t m_nChannel;
	uint32_t m_nFormat;
	uint32_t m_nWidth;
	uint32_t m_nHeight;
	uint32_t m_nComponents;
	uint32_t m_nQuality;
	uint32_t m_nContractVersion;
	uint8_t *m_pDestination;
	uint32_t m_nDestinationCapacity;
} t3dVTEncodeTileRequest_t;

typedef bool ( *t3dVTEncodeTileFn )( void *pUser, const t3dVTEncodeTileRequest_t &rRequest, uint32_t &rWrittenBytes );

typedef struct t3dVTEncoderInterface_s {
	void *m_pUser;
	t3dVTEncodeTileFn m_pEncodeTile;
} t3dVTEncoderInterface_t;

typedef void *( *t3dVTCookAllocateFn )( void *pUser, uint64_t nBytes, uint32_t nAlignment );
typedef void ( *t3dVTCookFreeFn )( void *pUser, void *pMemory );

typedef struct t3dVTCookAllocator_s {
	void *m_pUser;
	t3dVTCookAllocateFn m_pAllocate;
	t3dVTCookFreeFn m_pFree;
} t3dVTCookAllocator_t;

bool t3d_vt_cooker_normalize_channel_to_profile( const t3dVTCookImageView_t &rSource,
        t3dVTLogicalChannel_t Channel,
        uint32_t nSourceLayout,
        uint32_t nTargetExtent,
        bool bAlphaScissor,
        uint8_t nAlphaThreshold,
        const t3dVTCookAllocator_t &rAllocator,
        uint8_t *pDestination,
        uint64_t nDestinationBytes );

typedef uint64_t ( *t3dVTCookClusterBoundFn )( void *pUser, uint32_t nCodec, uint64_t nDecodedBytes );
typedef bool ( *t3dVTCookClusterEncodeFn )( void *pUser,
        uint32_t nCodec,
        const uint8_t *pSource,
        uint32_t nSourceBytes,
        uint8_t *pDestination,
        uint32_t nDestinationCapacity,
        uint32_t &rWrittenBytes );

typedef struct t3dVTCookClusterCodec_s {
	void *m_pUser;
	t3dVTCookClusterBoundFn m_pBound;
	t3dVTCookClusterEncodeFn m_pEncode;
} t3dVTCookClusterCodec_t;

typedef struct t3dVTCookPreparedChannelV3_s {
	const uint8_t *m_pMipBytes;
	t3dVTMipChainLayout_t m_Layout;
	uint32_t m_nTargetExtent;
	uint32_t m_nReserved0;
} t3dVTCookPreparedChannelV3_t;

typedef struct t3dVTCookSparseSourceV3_s {
	uint64_t m_nStableKey;
	t3dVTCookImageView_t m_Channels[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint64_t m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_COUNT];
	t3dVTCookPreparedChannelV3_t m_PreparedChannels[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nAuthoredChannelMask;
	uint32_t m_nOrmSourceLayout;
	uint32_t m_nWrapMode;
	uint32_t m_nConsumer;
	uint16_t m_nAllocationFlags;
	uint8_t m_bAlphaScissor;
	uint8_t m_nAlphaThreshold;
	uint32_t m_nGenerationSeed;
} t3dVTCookSparseSourceV3_t;

typedef struct t3dVTCookSparseStatsV3_s {
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nTailCount;
	uint32_t m_nUniquePayloadCount;
	uint32_t m_nRawDedupHits;
	uint32_t m_nEncodeCalls;
	uint32_t m_nChannelPageCount[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nPhysicalFamilyPayloadCount[T3D_VT_PHYSICAL_FORMAT_COUNT];
	uint64_t m_nContentSignature;
} t3dVTCookSparseStatsV3_t;

typedef struct t3dVTCookedSparsePackageV3_s {
	t3dVTCookAllocator_t m_Allocator;
	t3dVTPackAllocationRecordV3_t *m_pAllocations;
	t3dVTPackAllocationKeyRecordV3_t *m_pAllocationKeys;
	t3dVTChannelAllocation_t *m_pChannelAllocations;
	t3dVTPackPageRecordV3_t *m_pPages;
	t3dVTPackPayloadRecordV3_t *m_pPayloads;
	t3dVTPackClusterRecordV3_t *m_pClusters;
	t3dVTPackTailRecordV3_t *m_pTails;
	t3dVTPackQualityViewV3_t *m_pQualityViews;
	uint8_t *m_pPayloadData;
	t3dVTPackFormatFamilyRecordV3_t m_FormatFamilies[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nAllocationCount;
	uint32_t m_nChannelAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nPayloadCount;
	uint32_t m_nClusterCount;
	uint32_t m_nTailCount;
	uint32_t m_nQualityViewCount;
	uint32_t m_nFullySupportedProfileMask;
	uint32_t m_nPackageGeneration;
	uint64_t m_nPayloadDataBytes;
	t3dVTCookSparseStatsV3_t m_Stats;
} t3dVTCookedSparsePackageV3_t;

void t3d_vt_cooker_default_settings( t3dVTCookerSettings_t &rSettings );
bool t3d_vt_cooker_validate_settings( const t3dVTCookerSettings_t &rSettings );
bool t3d_vt_cooker_encode_tile_v3(
        const t3dVTEncoderInterface_t &rEncoder, const t3dVTEncodeTileRequest_t &rRequest, uint32_t &rWrittenBytes );
bool t3d_vt_cooker_build_channel_pyramid_v3( const t3dVTCookAllocator_t &rAllocator,
        const t3dVTCookSparseSourceV3_t &rSource,
        t3dVTFixedProfile_t Profile,
        t3dVTLogicalChannel_t Channel,
        uint8_t *&rpMipBytes,
        t3dVTMipChainLayout_t &rLayout );
bool t3d_vt_cooker_cook_sparse_pages_v3( const t3dVTCookerSettings_t &rSettings,
        const t3dVTCookSparseSourceV3_t *pSources,
        uint32_t nSourceCount,
        const t3dVTEncoderInterface_t &rEncoder,
        const t3dVTCookAllocator_t &rAllocator,
        const t3dVTCookClusterCodec_t *pClusterCodec,
        t3dVTCookedSparsePackageV3_t &rPackage );
void t3d_vt_cooker_release_sparse_package_v3( t3dVTCookedSparsePackageV3_t &rPackage );
bool t3d_vt_cooker_build_pack_source_v3( const t3dVTCookedSparsePackageV3_t &rPackage, t3dVTPackSourceV3_t &rSource );

#endif // TERRAIN3D_VT_COOKER_H
