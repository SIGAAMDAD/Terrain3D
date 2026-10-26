// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_TYPES_H
#define TERRAIN3D_TYPES_H
#include <string.h>
#include <cstddef>

// Central POD/data-contract authority. Keep behavior, ownership, and executable logic out of this header.
// Constants are a one-way dependency because fixed-size records and enum defaults consume T3D_* macros.
#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Callback signatures embedded in POD interfaces.
typedef void ( *t3dParallelTileTask_t )( void *, size_t, size_t, size_t, size_t, int32_t );

typedef void *( *t3dVTCookAllocateFn )( void *pUser, uint64_t nBytes, uint32_t nAlignment );
typedef void ( *t3dVTCookFreeFn )( void *pUser, void *pMemory );
typedef uint64_t ( *t3dVTCookClusterBoundFn )( void *pUser, uint32_t nCodec, uint64_t nDecodedBytes );
typedef bool ( *t3dVTCookClusterEncodeFn )( void		  *pUser,
											uint32_t	   nCodec,
											const uint8_t *pSource,
											uint32_t	   nSourceBytes,
											uint8_t		  *pDestination,
											uint32_t	   nDestinationCapacity,
											uint32_t	  &rWrittenBytes );
typedef void *( *t3dVTPackRuntimeAllocateFn_t )( void *pUser, size_t nBytes, size_t nAlignment );
typedef void ( *t3dVTPackRuntimeFreeFn_t )( void *pUser, void *pMemory );

typedef void ( *t3dWorldReleaseConvertedSceneColdFn_t )( void *pUser );
typedef bool ( *t3dWorldIsConvertedSceneReadyColdFn_t )( void *pUser );
typedef bool ( *t3dWorldResolveMaterialVTColdFn_t )( void	  *pUser,
													 uint64_t  nStableMaterialKey,
													 uint32_t &rAllocationId );

// Enumerated type contracts live in a dedicated header; all legacy type names and values remain unchanged.
#include "Terrain3DEnums.h"

// ENV-P01: fixed native owner-frame contract. No Godot types, heap storage or GPU ABI.
#define T3D_ENVIRONMENT_MAX_IMPULSES 128u

typedef struct t3dEnvironmentForcing_s
{
    float m_fRainRate = 0.0f;
    float m_fSnowRate = 0.0f;
    float m_fDryRate = 0.0f;
    float m_fWindX = 0.0f;
    float m_fWindZ = 0.0f;
    float m_fGustStrength = 0.0f;
    float m_fDeltaTime = 0.0f;
    uint32_t m_nFlags = 0u;
} t3dEnvironmentForcing_t;

typedef struct t3dEnvironmentImpulse_s
{
    float m_fPositionX = 0.0f;
    float m_fPositionY = 0.0f;
    float m_fPositionZ = 0.0f;
    float m_fRadius = 0.0f;
    float m_fForceX = 0.0f;
    float m_fForceZ = 0.0f;
    float m_fMagnitude = 0.0f;
    uint32_t m_nType = 0u;
} t3dEnvironmentImpulse_t;

static_assert( sizeof( t3dEnvironmentForcing_t ) == 32u, "Environment forcing ABI changed" );
static_assert( sizeof( t3dEnvironmentImpulse_t ) == 32u, "Environment impulse ABI changed" );

// ENV-P07 owner-thread region climate cache: one 128m cell per logical terrain region.
// Stored as fixed-point Q0.16; serialized separately from cooked terrain/VT assets.
typedef struct t3dEnvironmentRegionClimate_s
{
    uint16_t m_nWetness = 0u;
    uint16_t m_nSnow = 0u;
    uint16_t m_nGroundCover = 0u;
    uint16_t m_nFlags = 0u;
} t3dEnvironmentRegionClimate_t;
static_assert( sizeof( t3dEnvironmentRegionClimate_t ) == 8u, "Region climate ABI changed" );

// POD/data records.
typedef struct t3dPerformanceProfileSetting_s
{
	t3dPerformanceProfileOwner_t	 m_owner;
	t3dPerformanceProfileValueKind_t m_kind;
	const char						*m_pszProperty;
	int64_t							 m_nIntValue;
	double							 m_fFloatValue;
	bool							 m_bBoolValue;
} t3dPerformanceProfileSetting_t;

typedef struct t3dRegionCoord_s
{
	int8_t m_x;
	int8_t m_y;
} t3dRegionCoord_t;

typedef struct t3dTextureSettingsBlock_s
{
	Color	m_szColors[T3D_MAX_TEXTURE_ASSETS]{};
	float	m_szNormalDepths[T3D_MAX_TEXTURE_ASSETS]{};
	float	m_szAoStrengths[T3D_MAX_TEXTURE_ASSETS]{};
	float	m_szAoLightAffects[T3D_MAX_TEXTURE_ASSETS]{};
	float	m_szRoughnessMods[T3D_MAX_TEXTURE_ASSETS]{};
	float	m_szMetallics[T3D_MAX_TEXTURE_ASSETS]{};
	float	m_szUvScales[T3D_MAX_TEXTURE_ASSETS]{};
	Vector2 m_szDetiles[T3D_MAX_TEXTURE_ASSETS]{};
	Vector2 m_szDisplacements[T3D_MAX_TEXTURE_ASSETS]{};
	int		m_nCount = 0;
} t3dTextureSettingsBlock_t;

typedef struct t3dTextureArrayLayout_s
{
	Vector2i	  m_vSize = Vector2i( 0, 0 );
	Image::Format m_eFormat = Image::FORMAT_MAX;
	bool		  m_bMipmaps = true;
	bool		  m_bNeedsNormalization = false;
} t3dTextureArrayLayout_t;

typedef struct t3dMetricNeeds_s
{
	bool m_bSlope = false;
	bool m_bCurvature = false;
	bool m_bConcavity = false;
	bool m_bAspect = false;
	bool m_bFlow = false;
	bool m_bDeposition = false;
	bool m_bWaterDistance = false;
	bool m_bNoise = false;
	bool m_bBiome = false;
} t3dMetricNeeds_t;

typedef struct t3dMetricBuildConfig_s
{
	float	 m_fCurvatureScale = 1.0f;
	float	 m_fWaterLevel = 0.0f;
	float	 m_fNoiseScale = 0.01f;
	int		 m_nNoiseOctaves = 4;
	float	 m_fNoiseLacunarity = 2.0f;
	float	 m_fNoiseGain = 0.5f;
	uint32_t m_nNoiseSeed = 1337u;
} t3dMetricBuildConfig_t;

typedef struct t3dAxisSample_s
{
	real_t	m_fOffset = 0.f;
	real_t	m_fGlobalPosition = 0.f;
	int32_t m_nRegionLocation = 0;
	int32_t m_nMapPixel = 0;
	real_t	m_fCenteredUv = 0.f;
} t3dAxisSample_t;

typedef struct t3dComputeWrite_s
{
	size_t	 m_iRegionIndex = 0u;
	Vector2i m_mapPixel = Vector2i( 0, 0 );
	uint32_t m_nSourceValue = 0u;
	Vector3	 m_worldPosition = Vector3( 0.f, 0.f, 0.f );
} t3dComputeWrite_t;

typedef struct t3dErosionFinalStatistics_s
{
	double m_fEroded = 0.0;
	double m_fDeposited = 0.0;
	float  m_fMaximumLowering = 0.0f;
	float  m_fMaximumRaising = 0.0f;
} t3dErosionFinalStatistics_t;

typedef struct t3dTransitionScore_s
{
	t3dMaterialTransitionKind_t m_kind;
	float						m_fScore;
} t3dTransitionScore_t;

typedef struct t3dTextureErosionProfile_s
{
	bool	m_bValid = false;
	bool	m_bEnabled = true;
	float	m_fHardness = -1.0f;
	float	m_fHydraulicErodibility = 1.0f;
	float	m_fWindErodibility = 1.0f;
	float	m_fWaveErodibility = 1.0f;
	float	m_fWeatheringSusceptibility = 1.0f;
	float	m_fOxidationSusceptibility = 1.0f;
	float	m_fSaltSusceptibility = 1.0f;
	float	m_fWetDrySusceptibility = 1.0f;
	float	m_fInfiltrationMultiplier = 1.0f;
	float	m_fTrafficErodibility = 1.0f;
	float	m_fCohesion = 0.0f;
	float	m_fGrainSize = 0.5f;
	int32_t m_nExposedTextureId = -1;
	int32_t m_nWeatheredTextureId = -1;
	int32_t m_nOxidizedTextureId = -1;
	int32_t m_nCoarseDepositTextureId = -1;
	int32_t m_nFineDepositTextureId = -1;
	int32_t m_nSaltWeatheredTextureId = -1;
	int32_t m_nPavementTextureId = -1;
	int32_t m_nVarnishedTextureId = -1;
} t3dTextureErosionProfile_t;

struct RegionNeighbors
{
	int16_t m_ids[T3D_EROSION_DIRECTION_COUNT]{};
};

typedef struct t3dBasinProfile_s
{
	float m_fPathLength = 0.0f;
	float m_fHeightSlope = 0.0f;
	float m_fHeightIntercept = 0.0f;
	bool  m_bValid = false;
} t3dBasinProfile_t;

typedef struct t3dCollisionSection_s
{
	Vector3 m_top;
	Vector3 m_bottom;
} t3dCollisionSection_t;

typedef struct t3dCrossSection_s
{
	Vector3 m_highGround;
	Vector3 m_frontTop;
	Vector3 m_frontUpper;
	Vector3 m_frontLower;
	Vector3 m_frontToe;
	Vector3 m_lowGround;
	float	m_fU = 0.0f;
	float	m_fFaceHeight = 1.0f;
} t3dCrossSection_t;

typedef struct t3dMegatexturePageAddress_s
{
	uint32_t m_nMip;
	uint32_t m_nPageX;
	uint32_t m_nPageY;
} t3dMegatexturePageAddress_t;

typedef struct t3dVTCookStagedPayload_s
{
	uint64_t m_nContentSignature;
	uint8_t *m_pBytes;
	uint32_t m_nBytes;
	uint32_t m_nFormat;
	uint32_t m_nFlags;
	uint32_t m_nChecksum;
} t3dVTCookStagedPayload_t;

typedef struct t3dVTCookRawTileCacheEntryV1_s
{
	uint64_t m_nHashA;
	uint64_t m_nHashB;
	uint32_t m_nRawBytes;
	uint32_t m_nFamily;
	uint32_t m_nChannel;
	uint32_t m_nPayloadId;
	uint32_t m_nFlags;
} t3dVTCookRawTileCacheEntryV1_t;

typedef struct t3dVTCookerSettings_s
{
	uint32_t m_nPageInterior;
	uint32_t m_nPageGutter;
	uint32_t m_nClusterTargetPages;
	uint32_t m_nClusterTargetBytes;
	uint32_t m_nOuterCompression;
	uint32_t m_nMipQuality;
	uint32_t m_nCompressionQuality;
} t3dVTCookerSettings_t;

typedef struct t3dVTEncodeTileRequest_s
{
	const uint8_t *m_pSourcePixels;
	uint32_t	   m_nSourceBytes;
	uint32_t	   m_nChannel;
	uint32_t	   m_nFormat;
	uint32_t	   m_nWidth;
	uint32_t	   m_nHeight;
	uint32_t	   m_nComponents;
	uint32_t	   m_nQuality;
	uint32_t	   m_nContractVersion;
	uint8_t		   m_nAlphaThreshold = 0u; // Zero disables cutoff-constrained encoding.
	uint8_t		  *m_pDestination;
	uint32_t	   m_nDestinationCapacity;
} t3dVTEncodeTileRequest_t;

typedef bool ( *t3dVTEncodeTileFn )( void *pUser, const t3dVTEncodeTileRequest_s &rRequest, uint32_t &rWrittenBytes );

typedef struct t3dVTEncoderInterface_s
{
	void			 *m_pUser;
	t3dVTEncodeTileFn m_pEncodeTile;
} t3dVTEncoderInterface_t;

typedef struct t3dVTCookAllocator_s
{
	void			   *m_pUser;
	t3dVTCookAllocateFn m_pAllocate;
	t3dVTCookFreeFn		m_pFree;
} t3dVTCookAllocator_t;

typedef struct t3dVTCookClusterCodec_s
{
	void					*m_pUser;
	t3dVTCookClusterBoundFn	 m_pBound;
	t3dVTCookClusterEncodeFn m_pEncode;
} t3dVTCookClusterCodec_t;

typedef struct t3dVTCookSparseStatsV1_s
{
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nTailCount;
	uint32_t m_nUniquePayloadCount;
	uint32_t m_nRawDedupHits;
	uint32_t m_nEncodeCalls;
	uint32_t m_nChannelPageCount[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nPhysicalFamilyPayloadCount[T3D_VT_PHYSICAL_FORMAT_COUNT];
	uint64_t m_nContentSignature;
} t3dVTCookSparseStatsV1_t;

typedef struct t3dVTGodotSourceChannelV1_s
{
	uint32_t m_nSourceWidth;
	uint32_t m_nSourceHeight;
	uint32_t m_nSourceExtent;
	uint32_t m_nReserved0;
	uint64_t m_nSourceFingerprint;
	uint8_t	 m_bAuthored;
	uint8_t	 m_nLogicalChannel;
	uint8_t	 m_nConsumer;
	uint8_t	 m_nWrapMode;
	uint8_t	 m_bAlphaScissor;
	uint8_t	 m_nAlphaThreshold;
	uint8_t	 m_nReserved1[2];
} t3dVTGodotSourceChannelV1_t;

typedef struct t3dVTEditorCookTelemetryV1_s
{
	uint32_t m_nStage;
	uint32_t m_nDirtyAllocations;
	uint32_t m_nAllocationCacheHits;
	uint32_t m_nAllocationCacheMisses;
	uint32_t m_nChannelCacheHits;
	uint32_t m_nChannelCacheMisses;
	uint64_t m_nExtractUsec;
	uint64_t m_nFilterUsec;
	uint64_t m_nPageUsec;
	uint64_t m_nEncodeUsec;
	uint64_t m_nPackUsec;
} t3dVTEditorCookTelemetryV1_t;

typedef struct t3dVTPackAllocationKeyRecordV1_s
{
	uint64_t m_nStableKey;
	uint32_t m_nAllocationId;
	uint32_t m_nContentId;
} t3dVTPackAllocationKeyRecordV1_t;

typedef struct t3dVTPackPayloadRecordV1_s
{
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
} t3dVTPackPayloadRecordV1_t;

typedef struct t3dVTPackClusterRecordV1_s
{
	uint64_t m_nPayloadDataOffset;
	uint64_t m_nContentSignature;
	uint32_t m_nStoredBytes;
	uint32_t m_nDecodedBytes;
	uint32_t m_nFirstPayload;
	uint32_t m_nPayloadCount;
	uint32_t m_nCodec;
	uint32_t m_nChecksum;
} t3dVTPackClusterRecordV1_t;

typedef struct t3dVTPackHeaderV1_s
{
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
	uint64_t m_nReserved[3];
} t3dVTPackHeaderV1_t;

typedef struct t3dVTChannelAllocation_s
{
	uint32_t m_nBaseVirtualPage;
	uint32_t m_nPageCount;
	uint32_t m_nTailIndex;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nFlags;
} t3dVTChannelAllocation_t;

typedef struct t3dVTPackAllocationRecordV1_s
{
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint16_t m_nFlags;
	uint16_t m_nChannelMask;
	uint8_t	 m_nHighestAvailableProfile;
	uint8_t	 m_nAvailableProfileMask;
	uint8_t	 m_nConsumer;
	uint8_t	 m_nReserved0;
} t3dVTPackAllocationRecordV1_t;

typedef struct t3dVTPackPageRecordV1_s
{
	uint32_t m_nVirtualPage;
	uint32_t m_nParentPage;
	uint32_t m_nAllocationId;
	uint32_t m_nPayloadId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint16_t m_nMip;
	uint16_t m_nFlags;
	uint16_t m_nPageX;
	uint16_t m_nPageY;
	uint16_t m_nChannel;
	uint16_t m_nReserved;
} t3dVTPackPageRecordV1_t;

typedef struct t3dVTPackTailRecordV1_s
{
	uint64_t m_nContentSignature;
	uint32_t m_nCanonicalAllocationId;
	uint32_t m_nPayloadId;
	uint32_t m_nContentId;
	uint16_t m_nFirstMip;
	uint16_t m_nMipCount;
	uint16_t m_nFirstMipWidth;
	uint16_t m_nFirstMipHeight;
	uint16_t m_nAtlasInteriorWidth;
	uint16_t m_nAtlasInteriorHeight;
	uint16_t m_nChannel;
	uint16_t m_nFlags;
	uint32_t m_nLayout;
} t3dVTPackTailRecordV1_t;

typedef struct t3dVTPackQualityViewV1_s
{
	uint32_t m_nAllocationId;
	uint8_t	 m_nProfile;
	uint8_t	 m_nChannelMask;
	uint8_t	 m_nBaseMip[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint8_t	 m_nMaxMip[T3D_VT_LOGICAL_CHANNEL_COUNT];
} t3dVTPackQualityViewV1_t;

typedef struct t3dVTPackFormatFamilyRecordV1_s
{
	uint8_t	 m_nLogicalChannel;
	uint8_t	 m_nPhysicalFamily;
	uint8_t	 m_nSemantic;
	uint8_t	 m_nFlags;
	uint32_t m_nReserved;
} t3dVTPackFormatFamilyRecordV1_t;

typedef struct t3dVTPackSourceV1_s
{
	const t3dVTPackAllocationRecordV1_s	   *m_pAllocations;
	uint32_t								m_nAllocationCount;
	const t3dVTPackAllocationKeyRecordV1_t *m_pAllocationKeys;
	const t3dVTChannelAllocation_s		   *m_pChannelAllocations;
	uint32_t								m_nChannelAllocationCount;
	const t3dVTPackPageRecordV1_s		   *m_pPages;
	uint32_t								m_nPageCount;
	const t3dVTPackPayloadRecordV1_t	   *m_pPayloads;
	uint32_t								m_nPayloadCount;
	const t3dVTPackClusterRecordV1_t	   *m_pClusters;
	uint32_t								m_nClusterCount;
	const t3dVTPackTailRecordV1_s		   *m_pTails;
	uint32_t								m_nTailCount;
	const t3dVTPackQualityViewV1_s		   *m_pQualityViews;
	uint32_t								m_nQualityViewCount;
	const t3dVTPackFormatFamilyRecordV1_s  *m_pFormatFamilies;
	uint32_t								m_nFormatFamilyCount;
	const uint8_t						   *m_pPayloadData;
	uint64_t								m_nPayloadDataBytes;
	uint32_t								m_nPackageGeneration;
} t3dVTPackSourceV1_t;

// File-backed V1 directories are byte spans, never typed object arrays.
// The reader validates each complete directory before exposing these handles.
template< typename T > struct t3dVTPackByteRecords_t
{
	const uint8_t *m_pBytes = nullptr;
	T			   operator[](
		 size_t nIndex ) const {
		T Record{};
		memcpy( &Record, m_pBytes + nIndex * sizeof( T ), sizeof( T ) );
		return Record;
	}
	bool operator==(
		std::nullptr_t ) const {
		return m_pBytes == nullptr;
	}
	bool operator!=(
		std::nullptr_t ) const {
		return m_pBytes != nullptr;
	}
};

struct t3dVTPackHeaderValue_t
{
	t3dVTPackHeaderV1_t		   m_Value{};
	bool					   m_bValid = false;
	const t3dVTPackHeaderV1_t *operator->() const {
		return &m_Value;
	}
	const t3dVTPackHeaderV1_t &operator*() const {
		return m_Value;
	}
	bool operator==(
		std::nullptr_t ) const {
		return !m_bValid;
	}
	bool operator!=(
		std::nullptr_t ) const {
		return m_bValid;
	}
};

typedef struct t3dVTPackViewV1_s
{
	t3dVTPackHeaderValue_t									   m_pHeader;
	t3dVTPackByteRecords_t< t3dVTPackAllocationRecordV1_t >	   m_pAllocations;
	t3dVTPackByteRecords_t< t3dVTPackAllocationKeyRecordV1_t > m_pAllocationKeys;
	t3dVTPackByteRecords_t< t3dVTChannelAllocation_t >		   m_pChannelAllocations;
	t3dVTPackByteRecords_t< t3dVTPackPageRecordV1_t >		   m_pPages;
	t3dVTPackByteRecords_t< t3dVTPackPayloadRecordV1_t >	   m_pPayloads;
	t3dVTPackByteRecords_t< t3dVTPackClusterRecordV1_t >	   m_pClusters;
	t3dVTPackByteRecords_t< t3dVTPackTailRecordV1_t >		   m_pTails;
	t3dVTPackByteRecords_t< t3dVTPackQualityViewV1_t >		   m_pQualityViews;
	t3dVTPackByteRecords_t< t3dVTPackFormatFamilyRecordV1_t >  m_pFormatFamilies;
	const uint8_t											  *m_pPayloadData = nullptr;
	uint32_t												   m_nAllocationCount = 0u;
	uint32_t												   m_nChannelAllocationCount = 0u;
	uint32_t												   m_nPageCount = 0u;
	uint32_t												   m_nPayloadCount = 0u;
	uint32_t												   m_nClusterCount = 0u;
	uint32_t												   m_nTailCount = 0u;
	uint32_t												   m_nQualityViewCount = 0u;
	uint32_t												   m_nFormatFamilyCount = 0u;
	uint64_t												   m_nPayloadDataBytes = 0u;
} t3dVTPackViewV1_t;

typedef struct t3dVTCookImageView_s
{
	const uint8_t *m_pPixels;
	uint32_t	   m_nWidth;
	uint32_t	   m_nHeight;
	uint32_t	   m_nComponents;
	uint32_t	   m_nRowBytes;
} t3dVTCookImageView_t;

typedef struct t3dVTMipLevelLayout_s
{
	uint32_t m_nWidth;
	uint32_t m_nHeight;
	uint64_t m_nOffset;
	uint64_t m_nBytes;
} t3dVTMipLevelLayout_t;

typedef struct t3dVTReferenceBitWriter_s
{
	uint8_t *m_pBytes;
	uint32_t m_nBit;
} t3dVTReferenceBitWriter_t;

typedef struct t3dWorldConversionPolicy_s
{
	t3dWorldMode_t				 eMode;
	uint32_t					 nDefaultTier;
	t3dWorldMaterialPolicy_t	 eMaterialPolicy;
	t3dWorldTransparencyPolicy_t eTransparencyPolicy;
	bool						 bConvertMeshInstances;
	bool						 bConvertMultiMeshInstances;
	bool						 bConvertDynamicObjects;
	bool						 bConvertHiddenObjects;
	float						 fCellSize;
} t3dWorldConversionPolicy_t;

typedef struct t3dWorldCandidateDesc_s
{
	uint64_t				nSourceId;
	t3dWorldCandidateKind_t eKind;
	uint32_t				nTier;
	bool					bExplicitInclude;
	bool					bExplicitExclude;
	bool					bVisible;
	bool					bStatic;
	bool					bHasMesh;
	bool					bMaterialSupported;
	bool					bSortedTransparency;
	bool					bTwoSided;
} t3dWorldCandidateDesc_t;

typedef struct t3dWorldClassification_s
{
	t3dWorldOwnership_t		   eOwnership;
	t3dWorldConversionReason_t eReason;
	uint32_t				   nTier;
} t3dWorldClassification_t;

typedef struct t3dWorldOwnershipEntry_s
{
	uint64_t				   nSourceId;
	uint32_t				   nPrototypeId;
	uint32_t				   nInstanceId;
	uint32_t				   nGeneration;
	t3dWorldOwnership_t		   eOwnership;
	t3dWorldConversionReason_t eReason;
} t3dWorldOwnershipEntry_t;

typedef struct t3dWorldOwnershipRegistry_s
{
	t3dWorldOwnershipEntry_t *pEntries;
	uint32_t				  nCapacity;
	uint32_t				  nCount;
	uint32_t				  nGeneration;
} t3dWorldOwnershipRegistry_t;

typedef struct t3dWorldGodotMaterialResolver_s
{
	t3dWorldResolveMaterialVTColdFn_t pResolveMaterialVT;
	void							 *pUser;
} t3dWorldGodotMaterialResolver_t;

typedef struct alignas(
	16 ) t3dWorldVertex_s
{
	float	 m_fPosition[3];
	uint32_t m_nPackedNormal;
	uint32_t m_nPackedTangent;
	uint32_t m_nPackedUv0;
	uint32_t m_nPackedUv1;
	uint32_t m_nColorRgba8;
} t3dWorldVertex_t;

typedef struct t3dWorldGodotScratch_s
{
	t3dWorldVertex_s *pVertices;
	uint32_t		  nVertexCapacity;
	uint32_t		 *pIndices;
	uint32_t		  nIndexCapacity;
	uint32_t		 *pTriangleMaterialOffsets;
	uint8_t			 *pTriangleTiers;
	uint32_t		  nTriangleCapacity;
} t3dWorldGodotScratch_t;

typedef struct t3dWorldPrototypeEntry_s
{
	uint64_t nKey;
	uint32_t nMeshId;
	uint32_t nGeneration;
} t3dWorldPrototypeEntry_t;

typedef struct t3dWorldMaterialEntry_s
{
	uint64_t nKey;
	uint32_t nMaterialId;
	uint32_t nGeneration;
} t3dWorldMaterialEntry_t;

typedef struct t3dWorldNativeVisualEntry_s
{
	uint64_t			nSourceId;
	t3dWorldOwnership_t eOwnership;
} t3dWorldNativeVisualEntry_t;

typedef struct t3dWorldSceneBuildStats_s
{
	uint32_t nVisitedNodes;
	uint32_t nMeshCandidates;
	uint32_t nConvertedInstances;
	uint32_t nUniquePrototypes;
	uint32_t nUniqueMaterials;
	uint32_t nCompatibilityInstances;
	uint32_t nNativeInstances;
	uint32_t nFailureCount;
} t3dWorldSceneBuildStats_t;

typedef struct t3dWorldPrototypeBounds_s
{
	float vCenter[3];
	float fRadius;
} t3dWorldPrototypeBounds_t;

typedef struct t3dWorldCellHashEntry_s
{
	int32_t	 iCellX;
	int32_t	 iCellZ;
	uint32_t nCellIndex;
	uint32_t nInstanceCount;
	float	 vMin[3];
	float	 vMax[3];
	bool	 bOccupied;
} t3dWorldCellHashEntry_t;

typedef struct alignas(
	16 ) t3dWorldTransform_s
{
	float m_fRow0[4];
	float m_fRow1[4];
	float m_fRow2[4];
} t3dWorldTransform_t;

typedef struct alignas(
	16 ) t3dWorldInstance_s
{
	uint32_t m_nClusterOffset;
	uint32_t m_nClusterCount;
	uint32_t m_nTransformIndex;
	uint32_t m_nPreviousTransformIndex;
	uint32_t m_nVisibilityLayers;
	uint32_t m_nFlags;
	uint32_t m_nGeometryKind;
	uint32_t m_nSourceId;
	uint32_t m_nMaterialOverride;
	uint32_t m_nCellId;
	float	 m_fLodBegin;
	float	 m_fLodEnd;
	float	 m_fFadeMargin;
	uint32_t m_nReserved0;
	uint32_t m_nReserved1;
	uint32_t m_nReserved2;
} t3dWorldInstance_t;

typedef struct t3dWorldCellBuildScratch_s
{
	t3dWorldInstance_s		*pReorderedInstances;
	t3dWorldTransform_s		*pReorderedTransforms;
	uint64_t				*pReorderedSourceIds;
	uint32_t				 nInstanceCapacity;
	uint32_t				*pInstanceCellIds;
	uint32_t				*pCellCounts;
	uint32_t				*pCellWriteCursor;
	uint32_t				 nCellScratchCapacity;
	t3dWorldCellHashEntry_t *pCellHash;
	uint32_t				 nCellHashCapacity;
} t3dWorldCellBuildScratch_t;

typedef struct t3dStartupDomainSnapshot_s
{
	t3dStartupStage_t m_eStage = T3D_STARTUP_IDLE;
	uint32_t		  m_nGeneration = 0u;
	uint32_t		  m_nCompletedUnits = 0u;
	uint32_t		  m_nTotalUnits = 0u;
	uint64_t		  m_nStartUsec = 0u;
	uint64_t		  m_nEndUsec = 0u;
} t3dStartupDomainSnapshot_t;

typedef struct t3dDisplacementBuildContext_s
{
	const uint8_t *m_pSource = NULL;
	uint8_t		  *m_pDestination = NULL;
	int			   m_nChannel = 0;
} t3dDisplacementBuildContext_t;

typedef struct t3dDerivedMapUpdate_s
{
	Vector2i m_location = Vector2i( INT32_MAX, INT32_MAX );
	Rect2i	 m_dirtyRect;
	bool	 m_bSuccess = false;
} t3dDerivedMapUpdate_t;

typedef struct t3dDerivedMapTask_s
{
	Vector2i m_location = Vector2i( INT32_MAX, INT32_MAX );
	Rect2i	 m_dirtyRect;
	size_t	 m_iOutputOffset = 0u;
	size_t	 m_nOutputBytes = 0u;
	bool	 m_bAvailable = false;
	bool	 m_bSuccess = false;
} t3dDerivedMapTask_t;

typedef struct t3dStartupLoadStats_s
{
	uint64_t m_nTotalUsecLast = 0u;
	uint64_t m_nEnumerationUsecLast = 0u;
	uint64_t m_nDeserializeUsecLast = 0u;
	uint64_t m_nStagingUsecLast = 0u;
	uint64_t m_nPublishUsecLast = 0u;
	uint64_t m_nFinalizeUsecLast = 0u;
	int		 m_nRegionFilesLast = 0;
	int		 m_nRegionsPublishedLast = 0;
	uint64_t m_nBytesReadLast = 0u;
	bool	 m_bSucceededLast = false;
} t3dStartupLoadStats_t;

typedef struct t3dRuntimeCollisionHeightView_s
{
	const uint8_t *m_pHeights = NULL;
	uint32_t	   m_nCount = 0u;
	int			   m_nWidth = 0;
} t3dRuntimeCollisionHeightView_t;

typedef struct t3dRuntimeInstanceCellMetadata_s
{
	uint16_t m_nMeshId = 0u;
	Vector2i m_cell = Vector2i( 0, 0 );
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nOrdinal = 0u;
	AABB	 m_originBounds;
} t3dRuntimeInstanceCellMetadata_t;

typedef struct t3dMinMaxLevelView_s
{
	const uint8_t *m_pRanges = NULL;
	uint32_t	   m_nRangeCount = 0u;
	uint16_t	   m_nSourceSide = 0u;
	uint16_t	   m_nLevelSide = 0u;
} t3dMinMaxLevelView_t;

typedef struct t3dInstanceMetadataView_s
{
	const uint8_t *m_pRecords = NULL;
	uint32_t	   m_nCellCount = 0u;
	uint32_t	   m_nInstanceCount = 0u;
	uint16_t	   m_nMeshCount = 0u;
} t3dInstanceMetadataView_t;

typedef struct t3dLightingReceiverCursor_s
{
	const uint8_t *m_pData = NULL;
	size_t		   m_nSize = 0u;
	size_t		   m_iOffset = 0u;
	uint32_t	   m_nCount = 0u;
	uint32_t	   m_iIndex = 0u;
} t3dLightingReceiverCursor_t;

typedef struct t3dRuntimePageChannelInfo_s
{
	uint32_t m_nType = 0u;
	uint32_t m_nFormat = 0u;
	uint32_t m_nCompression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	uint32_t m_iOffset = 0u;
	uint32_t m_nCompressedBytes = 0u;
	uint32_t m_nUncompressedBytes = 0u;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
	uint32_t m_nMipCount = 0u;
	uint32_t m_nAlignment = T3D_RUNTIME_PAGE_ALIGNMENT;
	uint32_t m_nChecksum = 0u;
	uint32_t m_nSignature = 0u;
} t3dRuntimePageChannelInfo_t;

typedef struct t3dRdPublicationExchangeState_s
{
	uint64_t	   *m_pSlotGenerations;
	t3dAtomicU64_t *m_pExchangeToken;
	uint32_t	   *m_pWriteIndex;
	uint32_t	   *m_pRenderIndex;
	uint64_t	   *m_pRenderGeneration;
} t3dRdPublicationExchangeState_t;

typedef struct t3dRdPublicationPayload_s
{
	uint64_t m_nGeneration;
	uint64_t m_nCheck;
} t3dRdPublicationPayload_t;

typedef struct t3dRdPublicationThreadContext_s
{
	t3dRdPublicationPayload_t *m_pSlots;
	t3dAtomicU64_t			  *m_pToken;
	t3dAtomicBool_t			  *m_pProducerDone;
	t3dAtomicBool_t			  *m_pValid;
	t3dAtomicU64_t			  *m_pAcquires;
} t3dRdPublicationThreadContext_t;

typedef struct t3dEastlAllocationStats_s
{
	size_t m_nLiveBytes = 0u;
	size_t m_nPeakBytes = 0u;
	size_t m_nLiveAllocations = 0u;
	size_t m_nTotalAllocations = 0u;
	size_t m_nFailedAllocations = 0u;
	size_t m_nOverflowAllocations = 0u;
	size_t m_nOverflowLiveBytes = 0u;
} t3dEastlAllocationStats_t;

typedef struct t3dHLODTriangle_s
{
	Vector3 m_a;
	Vector3 m_b;
	Vector3 m_c;
	Vector3 m_center;
	AABB	m_bounds;
} t3dHLODTriangle_t;

typedef struct t3dHLODBvhNode_s
{
	AABB m_bounds;
	int	 m_nLeft = -1;
	int	 m_nRight = -1;
	int	 m_nBegin = 0;
	int	 m_nCount = 0;
} t3dHLODBvhNode_t;

typedef struct t3dHLODRuntimeNodeTemp_s
{
	uint16_t m_nRegion;
	int64_t	 m_nSpawn;
	uint8_t	 m_nLevel;
} t3dHLODRuntimeNodeTemp_t;

typedef struct t3dHLODRuntimeNodePolicyRecord_s
{
	int64_t m_nSpawnId = 0;
	uint8_t m_nMinLevel = 0;
} t3dHLODRuntimeNodePolicyRecord_t;

// Shared with the HLOD codec: identical persisted policy record.
typedef t3dHLODRuntimeNodePolicyRecord_t t3dNodePolicy_t;

typedef struct t3dHLODInstancerRepresentationState_s
{
	uint8_t m_nMinimumLevel = 0;
	uint8_t m_nActiveLevel = 0;
	uint8_t m_nResidentLevel = 0;
	uint8_t m_nRetiringLevel = 0;
} t3dHLODInstancerRepresentationState_t;

typedef struct t3dInstanceCellCapacityPlan_s
{
	size_t m_nRequired = 0u;
	size_t m_nHeadroom = 0u;
	size_t m_nDesired = 0u;
	size_t m_nTarget = 0u;
} t3dInstanceCellCapacityPlan_t;

typedef struct t3dInstanceOcclusionCell_s
{
	AABB	 m_worldBounds;
	AABB	 m_localProxyBounds;
	Vector2i m_region;
	Vector2i m_cell;
	int32_t	 m_nMeshId = -1;
	uint32_t m_nSourceRevision = 0u;
	uint32_t m_nProxyGeneration = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint16_t m_nMirroredInstanceCount = 0u;
	uint8_t	 m_nProxyPolicy = 0u;
	uint8_t	 m_nFlags = T3D_INSTANCE_OCCLUSION_RECORD_NONE;
} t3dInstanceOcclusionCell_t;

typedef struct t3dInstancePublicationCategoryDiagnostics_s
{
	uint64_t m_nPublishedLod0Cells = 0u;
	uint64_t m_nPublishedLod1Cells = 0u;
	uint64_t m_nPublishedHigherLodCells = 0u;
	uint64_t m_nLodFallbacks = 0u;
	uint64_t m_nLodMissingSourceSkips = 0u;
	uint64_t m_nVisibilityLayerFilteredCells = 0u;
	uint64_t m_nFadeActiveCells = 0u;
	uint64_t m_nHlodEligibleSources = 0u;
	uint64_t m_nHlodActiveReplacements = 0u;
	uint64_t m_nHlodDetailSuppressedCells = 0u;
	uint64_t m_nHlodDetailRetainedMissingProxy = 0u;
	uint64_t m_nHlodTransitionOverlapCells = 0u;
	uint64_t m_nHlodTransitionGapErrors = 0u;
} t3dInstancePublicationCategoryDiagnostics_t;

typedef struct t3dRenderCell_s
{
	Vector2i m_region;
	Vector2i m_cell;
	int		 m_nMeshId = -1;
	int		 m_nLod = 0;
	uint32_t m_nRegionListNode = UINT32_MAX;
	int		 m_nUploadedCount = 0;
	uint32_t m_nUploadedRevision = 0;
	uint32_t m_nSettingsRevision = 0;
	uint32_t m_nAabbGeneration = 0;
	bool	 m_bUsesColors = false;
} t3dRenderCell_t;

typedef struct t3dUsableHeight_s
{
	bool	m_bValid = false;
	real_t	m_fHeight = 0.f;
	bool	m_bRaycastHit = false;
	Vector3 m_normal = Vector3( 0.f, 1.f, 0.f );
} t3dUsableHeight_t;

typedef struct t3dLogicalCellState_s
{
	uint32_t m_nCount = 0;
	uint32_t m_nRegionListNode = UINT32_MAX;
} t3dLogicalCellState_t;

typedef struct t3dRdCellRange_s
{
	uint32_t m_nFirst = 0u;
	uint32_t m_nCount = 0u;
} t3dRdCellRange_t;

typedef struct t3dRegionList_s
{
	uint32_t m_nHead = UINT32_MAX;
	uint32_t m_nCount = 0u;
} t3dRegionList_t;

typedef struct t3dRenderRegionNode_s
{
	size_t	 m_iRenderCell = 0u;
	uint32_t m_nNext = UINT32_MAX;
	uint32_t m_nPrev = UINT32_MAX;
} t3dRenderRegionNode_t;

typedef struct t3dTransitionBucketState_s
{
	uint32_t m_nHead = UINT32_MAX;
	uint32_t m_nCount = 0u;
} t3dTransitionBucketState_t;

typedef struct t3dRuntimeAllocationPhaseStats_s
{
	uint64_t m_szEastlEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szEastlBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szZoneEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szZoneBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szRawEvents[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
	uint64_t m_szRawBytes[T3D_RUNTIME_ALLOCATION_PHASE_COUNT] = {};
} t3dRuntimeAllocationPhaseStats_t;

typedef struct t3dMemorySubsystemInput_s
{
	bool	 m_bEnabled = true;
	bool	 m_bAutomatic = true;
	uint64_t m_nManualBytes = 0u;
	uint64_t m_nMinimumBytes = 0u;
	uint64_t m_nHardCapBytes = UINT64_MAX;
	uint32_t m_nWeight = 1u;
} t3dMemorySubsystemInput_t;

typedef struct t3dMemorySubsystemAllocation_s
{
	uint64_t			   m_nRequestedBytes = 0u;
	uint64_t			   m_nEffectiveBytes = 0u;
	t3dMemoryLimitReason_t m_limitingReason = TERRAIN3D_MEMORY_LIMIT_NONE;
	bool				   m_bBelowMinimum = false;
} t3dMemorySubsystemAllocation_t;

typedef struct t3dMemoryZoneStats_s
{
	size_t	 m_nReservedBytes = 0u;
	size_t	 m_nLiveRequestedBytes = 0u;
	size_t	 m_nLivePhysicalBytes = 0u;
	size_t	 m_nLiveAllocationCount = 0u;
	size_t	 m_nExtentFreeBytes = 0u;
	size_t	 m_nSmallPageReservedBytes = 0u;
	size_t	 m_nSmallPageReusablePayloadBytes = 0u;
	size_t	 m_nSmallPageReusablePhysicalBytes = 0u;
	size_t	 m_nLargeAllocationPhysicalBytes = 0u;
	size_t	 m_nMetadataBytes = 0u;
	size_t	 m_nAlignmentWasteBytes = 0u;
	size_t	 m_nSizeClassWasteBytes = 0u;
	size_t	 m_nInternalFragmentationBytes = 0u;
	size_t	 m_nFailedAllocations = 0u;
	size_t	 m_nExtentTableSaturationEvents = 0u;
	size_t	 m_nLostFreePreventionEvents = 0u;
	uint32_t m_nActiveSmallPages = 0u;
	uint32_t m_nPeakActiveSmallPages = 0u;
	uint32_t m_nFreeExtentCount = 0u;
	uint32_t m_nPeakFreeExtentCount = 0u;
} t3dMemoryZoneStats_t;

typedef struct t3dMemoryZoneExtent_s
{
	size_t m_iOffset = 0u;
	size_t m_nSize = 0u;
} t3dMemoryZoneExtent_t;

typedef struct t3dMemoryZoneSmallPage_s
{
	size_t	 m_iOffset = 0u;
	uint32_t m_nBytes = 0u;
	uint32_t m_nBlockStride = 0u;
	uint32_t m_iFirstFree = UINT32_MAX;
	uint16_t m_iClass = UINT16_MAX;
	uint16_t m_nLiveBlocks = 0u;
	uint16_t m_nTotalBlocks = 0u;
	size_t	 m_nLiveRequestedBytes = 0u;
	bool	 m_bActive = false;
} t3dMemoryZoneSmallPage_t;

typedef struct t3dMemoryZoneAllocationHeader_s
{
	uint64_t m_nMagic = 0u;
	size_t	 m_iExtentOffset = 0u;
	size_t	 m_nExtentBytes = 0u;
	size_t	 m_nRequestedBytes = 0u;
	uint32_t m_nAlignment = 0u;
	uint16_t m_iPage = UINT16_MAX;
	uint16_t m_iClass = UINT16_MAX;
	uint64_t m_nReserved = 0u;
} t3dMemoryZoneAllocationHeader_t;

typedef struct t3dSlotLayout_s
{
	size_t m_nHeightSlotBytes = 0u;
	size_t m_nControlSlotBytes = 0u;
	size_t m_nColorSlotBytes = 0u;
	size_t m_nDerivedSlotBytes = 0u;
	size_t m_nStride = 0u;
} t3dSlotLayout_t;

struct t3dLocalComputeBufferStats_t
{
	uint64_t bufferCreates = 0u;
	uint64_t uniformSetCreates = 0u;
	uint64_t pipelineCreates = 0u;
	uint64_t resourceFailures = 0u;
	uint64_t uploadCalls = 0u;
	uint64_t bytesUploaded = 0u;
	uint64_t readbackCalls = 0u;
	uint64_t bytesReadBack = 0u;
};

struct t3dRdHiZDispatchInfo_t
{
	uint32_t m_nMip = 0u;
	uint32_t m_nSourceWidth = 0u;
	uint32_t m_nSourceHeight = 0u;
	uint32_t m_nAuxiliaryWidth = 0u;
	uint32_t m_nAuxiliaryHeight = 0u;
	uint32_t m_nDestinationWidth = 0u;
	uint32_t m_nDestinationHeight = 0u;
	bool	 m_bAuxiliarySourceBound = false;
};

typedef struct t3dRdForwardViewportState_s
{
	uint64_t m_nViewportKey = 0u;
	Vector2i m_renderSize;
	Rect2i	 m_renderRegion;
	uint64_t m_nLastRenderFrame = UINT64_MAX;
	uint64_t m_nReconfiguredRenderFrame = UINT64_MAX;
	uint32_t m_nViewCount = 0u;
	uint32_t m_nSampleCount = 0u;
	bool	 m_bValid = false;
} t3dRdForwardViewportState_t;

typedef struct t3dRdForwardDrawBaseline_s
{
	uint64_t m_nSubmitCallbacks = 0u;
	uint64_t m_nTerrainDraws = 0u;
	uint64_t m_nInstanceDraws = 0u;
	uint64_t m_nOceanDraws = 0u;
	uint64_t m_nWorldDraws = 0u;
} t3dRdForwardDrawBaseline_t;

typedef struct t3dRdFreedResource_s
{
	uint64_t m_nRid = 0u;
	uint64_t m_nDeviceGeneration = 0u;
} t3dRdFreedResource_t;

struct t3dRdResourceEpoch_t
{
	uint64_t nCreates = 0u;
	uint64_t nFrees = 0u;
	uint64_t nLive = 0u;
	uint64_t nLiveBytes = 0u;
	uint64_t nSteadyStateViolations = 0u;
};

typedef struct alignas(
	16 ) t3dRdForwardLightFrame_s
{
	uint32_t m_nClusterShift = 0u;
	uint32_t m_nClusterWidth = 0u;
	uint32_t m_nClusterTypeSize = 0u;
	uint32_t m_nClusterMaxElementsDiv32 = 0u;
	uint32_t m_nDirectionalLightCount = 0u;
	uint32_t m_nDefaultSurfaceMask = UINT32_MAX;
	uint32_t m_nShadowFlags = 0u;
	uint32_t m_nDirectionalSoftShadows = 0u;
	float	 m_fCameraZNear = 0.0f;
	float	 m_fCameraZFar = 0.0f;
	float	 m_fInvCameraZFar = 0.0f;
	float	 m_fReflectionAtlasBorderSize = 0.0f;
	uint32_t m_nRenderWidth = 0u;
	uint32_t m_nRenderHeight = 0u;
	uint32_t m_nClusterMaxElements = 0u;
	uint32_t m_nSceneFeatureFlags = 0u;
	float	 m_szAmbientLightColorEnergy[4];
	float	 m_szEnvironmentParams0[4];
	float	 m_szEnvironmentParams1[4];
	float	 m_szRadianceInverseXform[12];
} t3dRdForwardLightFrame_t;

typedef struct alignas(
	8 ) t3dRdForwardBridgeFramePacket_s
{
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint64_t m_nGeneration;
	uint64_t m_nScenePass;
	uint64_t m_nRenderFrame;
	uint64_t m_nViewportKey;
	uint32_t m_nViewCount;
	uint32_t m_nSampleCount;
	uint32_t m_nCameraVisibleLayers;
	uint32_t m_nFlags;
	int32_t	 m_nRenderRegion[4];
	int32_t	 m_nRenderSize[2];
	uint32_t m_nClusterMaxElements;
	uint32_t m_nClusterShift;
	uint32_t m_nClusterWidth;
	uint32_t m_nClusterTypeSize;
	uint32_t m_nClusterMaxElementsDiv32;
	uint32_t m_nDirectionalLightCount;
	float	 m_fCameraZNear;
	float	 m_fCameraZFar;
	float	 m_fReflectionAtlasBorderSize;
	float	 m_mCameraTransform[12];
	float	 m_mViewProjection[T3D_FORWARD_BRIDGE_MAX_VIEWS][16];
	float	 m_mPreviousViewProjection[T3D_FORWARD_BRIDGE_MAX_VIEWS][16];
	float	 m_vEyeOffset[T3D_FORWARD_BRIDGE_MAX_VIEWS][4];
	float	 m_vMotionJitter[4];
	uint32_t m_nAbi10ReservedTail;
	uint32_t m_nEnvironmentFlags;
	uint32_t m_nEnvironmentReserved0;
	float	 m_szAmbientLightColorEnergy[4];
	float	 m_fAmbientColorSkyMix;
	float	 m_fEmissiveExposureNormalization;
	float	 m_fIblExposureNormalization;
	float	 m_fRadiancePixelSize;
	float	 m_fRadianceBorderSize;
	float	 m_fEnvironmentReserved1;
	float	 m_szRadianceInverseXform[12];
} t3dRdForwardBridgeFramePacket_t;

typedef struct alignas(
	8 ) t3dRdForwardBridgeResourcePacket_s
{
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nGeneration;
	uint32_t m_nReserved0;
	uint64_t m_hDirectionalLightBuffer;
	uint64_t m_hOmniLightBuffer;
	uint64_t m_hSpotLightBuffer;
	uint64_t m_hAreaLightBuffer;
	uint64_t m_hClusterBuffer;
	uint64_t m_hShadowAtlas;
	uint64_t m_hDirectionalShadowAtlas;
	uint64_t m_hShadowSampler;
	uint64_t m_hReflectionBuffer;
	uint64_t m_hReflectionAtlas;
	uint64_t m_hDecalBuffer;
	uint64_t m_hDecalAtlas;
	uint64_t m_hDecalAtlasSrgb;
	uint64_t m_hRadianceTexture;
	uint64_t m_hPreviousDepthTexture;
} t3dRdForwardBridgeResourcePacket_t;

typedef struct alignas(
	8 ) t3dRdForwardBridgePassPacket_s
{
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nPassKind;
	uint32_t m_nNativePassMode;
	uint32_t m_nColorFlags;
	uint32_t m_nFlags;
	int64_t	 m_nFramebufferFormat;
	uint64_t m_nSerial;
	uint64_t m_hNativeFramebuffer;
} t3dRdForwardBridgePassPacket_t;

typedef struct alignas(
	8 ) t3dRdForwardBridgeShadowPacket_s
{
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nNativePassMode;
	uint32_t m_nFlags;
	uint32_t m_nSampleCount;
	uint32_t m_nShadowCasterMask;
	uint32_t m_nShadowLightCullMask;
	uint32_t m_nReserved0;
	int64_t	 m_nFramebufferFormat;
	uint64_t m_nSerial;
	uint64_t m_nScenePass;
	uint64_t m_nRenderFrame;
	uint64_t m_nViewportKey;
	float	 m_mCameraTransform[12];
	float	 m_mViewProjection[16];
	float	 m_vEyeOffset[4];
} t3dRdForwardBridgeShadowPacket_t;

typedef struct t3dRdFrustum_s
{
	Vector4 m_szPlanes[6];
} t3dRdFrustum_t;

typedef struct alignas(
	16 ) t3dRdInstanceFrame_s
{
	float m_szViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16];
	float m_szView[16];
	float m_szPreviousViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16];
	float m_szCamera[T3D_RD_FORWARD_MAX_VIEWS][4];
	float m_szMotionJitter[4];
	float m_szMotionParams[4];
	float m_szNomadSunShape[4];
	float m_szNomadSunDiffuse[4];
	float m_szNomadSunSpecular[4];
	float m_szNomadSunRim[4];
	float m_szInstanceTextureMipBias[4]; // xyz: mip bias; w: packed category filter overrides
} t3dRdInstanceFrame_t;

typedef struct alignas(
	16 ) t3dRdInstancePush_s
{
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nOverlayMaterial = UINT16_MAX;
	uint32_t m_nVisibleBase = 0u;
	uint32_t m_nFlags = 0u;
	uint32_t m_nVisibilityLayers = UINT32_MAX;
	uint32_t m_nReserved0 = 0u;
	uint32_t m_nReserved1 = 0u;
	uint32_t m_nReserved2 = 0u;
} t3dRdInstancePush_t;

typedef struct t3dRdHlodCullPush_s
{
	uint32_t m_nDescriptorCount = 0u;
	uint32_t m_nVisibilityMask = UINT32_MAX;
	uint32_t m_nCommandCount = 0u;
	uint32_t m_nMainCommandBase = 0u;
} t3dRdHlodCullPush_t;

typedef struct t3dRdInstanceCullPush_s
{
	uint32_t m_nDescriptorBase = 0u;
	uint32_t m_nDescriptorCount = 0u;
	uint32_t m_nCommandCount = 0u;
	uint32_t m_nVisibilityMask = UINT32_MAX;
	uint32_t m_nViewCount = 1u;
	uint32_t m_nShadowPass = 0u;
	uint32_t m_nMode = 0u;
	uint32_t m_nMainCommandBase = 0u;
	uint32_t m_nShadowCommandBase = 0u;
	uint32_t m_nReserved0 = 0u;
	uint32_t m_nReserved1 = 0u;
	uint32_t m_nReserved2 = 0u;
} t3dRdInstanceCullPush_t;

typedef struct t3dRdInstanceDirtyRange_s
{
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
} t3dRdInstanceDirtyRange_t;

typedef struct t3dRdInstanceDescriptorSpan_s
{
	uint32_t m_nFirst = 0u;
	uint32_t m_nCount = 0u;
} t3dRdInstanceDescriptorSpan_t;

typedef struct t3dRdPacketBuildEntry_s
{
	uint32_t m_nPacketIndex = 0u;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nCellIndex = 0u;
} t3dRdPacketBuildEntry_t;

struct AttributeDesc
{
	uint32_t					location;
	RenderingDevice::DataFormat format;
	uint32_t					offset;
};

typedef struct t3dRdMeshSourceSurface_s
{
	uint64_t m_nSourceKey = 0u;
	uint16_t m_nAssetId = 0u;
	int16_t	 m_nLod = 0;
	uint16_t m_nSurface = 0u;
	uint16_t m_nMaterial = UINT16_MAX;
	uint16_t m_nOverlayMaterial = UINT16_MAX;
	uint16_t m_nFlags = 0u;
	uint32_t m_nVertexOffset = 0u;
	uint32_t m_nVertexCount = 0u;
	uint32_t m_nIndexOffset = 0u;
	uint32_t m_nIndexCount = 0u;
	uint32_t m_nWorldVertexOffset = 0u;
	uint32_t m_nWorldVertexCount = 0u;
	uint32_t m_nWorldClusterOffset = 0u;
	uint32_t m_nWorldClusterCount = 0u;
	AABB	 m_bounds;
	// CPU prediction metadata; never serialized or uploaded as a shader record.
	float m_szUvMin[2] = { 0.0f, 0.0f };
	float m_szUvMax[2] = { 0.0f, 0.0f };
} t3dRdMeshSourceSurface_t;

typedef struct t3dRdMeshSourceLod_s
{
	uint64_t m_nSourceKey = 0u;
	uint16_t m_nAssetId = 0u;
	int16_t	 m_nLod = 0;
	uint32_t m_nFirstSurface = 0u;
	uint32_t m_nSurfaceCount = 0u;
	AABB	 m_bounds;
} t3dRdMeshSourceLod_t;

typedef struct alignas(
	16 ) t3dRdOceanFrame_s
{
	float m_mViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16];
	float m_mView[16];
	float m_szCameraTime[4];
	float m_szLightDirection[4];
	float m_szLightColor[4];
	float m_szMeshParams[4];
	float m_szTargetTime[4];
	float m_szWaterColor[4];
	float m_szDepthParams[4];
	float m_szWaveParams[4];
	float m_szSurfaceParams[4];
	float m_szFoamParams0[4];
	float m_szFoamParams1[4];
	float m_szScatterParams[4];
	float m_szModeParams[4];
} t3dRdOceanFrame_t;

typedef struct t3dPreviewReadbackSlot_s
{
	bool	 bActive = false;
	bool	 bReadbackInFlight = false;
	uint16_t nAssetId = 0u;
	Vector2i size;
	uint64_t nRequestGeneration = 0u;
	uint64_t nLifecycleGeneration = 0u;
	uint64_t nMeshSourceGeneration = 0u;
	uint64_t nAssetObjectId = 0u;
	uint64_t nReadbackSubmitUsec = 0u;
	uint64_t nCompletionOwnerId = 0u;
	uint64_t nCompletionGeneration = 0u;
} t3dPreviewReadbackSlot_t;

typedef struct t3dRdShaderCatalogEntry_s
{
	t3dRdShaderId_t		  m_id;
	const char			 *m_path;
	const char			 *m_version;
	t3dRdShaderDomain_t	  m_domain;
	t3dRdShaderCategory_t m_category;
	bool				  m_requiredAtStartup;
} t3dRdShaderCatalogEntry_t;

typedef struct t3dRdPatch_s
{
	float	 m_fOriginX = 0.0f;
	float	 m_fOriginZ = 0.0f;
	float	 m_fScale = 1.0f;
	uint32_t m_nLod = 0u;
	uint32_t m_nTier = 0u;
	uint32_t m_nStitchX = 1u;
	uint32_t m_nStitchZ = 1u;
	uint32_t m_nFlags = 0u;
	float	 m_fBoundsMinY = 0.0f;
	float	 m_fBoundsMaxY = 0.0f;
	float	 m_fBoundsHalfExtent = 0.0f;
	float	 m_fPreviousOriginX = 0.0f;
	float	 m_fPreviousOriginZ = 0.0f;
	float	 m_fPreviousScale = 1.0f;
	float	 m_fPadding0 = 0.0f;
	float	 m_fPadding1 = 0.0f;
} t3dRdPatch_t;

typedef struct t3dRdMaterialLayer_s
{
	float	 m_fColorR = 1.0f;
	float	 m_fColorG = 1.0f;
	float	 m_fColorB = 1.0f;
	float	 m_fColorA = 1.0f;
	float	 m_fUvScale = 0.1f;
	float	 m_fNormalDepth = 1.0f;
	float	 m_fRoughness = 1.0f;
	float	 m_fMetallic = 0.0f;
	float	 m_fAoStrength = 1.0f;
	float	 m_fAoLightAffect = 0.0f;
	float	 m_fDetileRotation = 0.0f;
	float	 m_fDetileShift = 0.0f;
	float	 m_fDisplacementOffset = 0.0f;
	float	 m_fDisplacementScale = 0.0f;
	uint32_t m_nPhysicalLayer = 0u;
	uint32_t m_nFlags = 0u;
	uint32_t m_nMicroVTAllocation = T3D_VT_INVALID_ALLOCATION;
	uint32_t m_nMicroVTGeneration = 0u;
	uint32_t m_nMicroVTFlags = 0u;
	uint32_t m_nMicroVTReserved = 0u;
} t3dRdMaterialLayer_t;

typedef struct alignas(
	16 ) t3dRdTierMaterialConfig_s
{
	uint32_t m_nSamplingMode = 3u;
	uint32_t m_nNormalMode = 3u;
	uint32_t m_nColorMode = 2u;
	uint32_t m_nSurfaceResponse = 1u;
	uint32_t m_nTextureSurfaceMode = 0u;
	uint32_t m_nLightingModel = 3u;
	uint32_t m_nDetilingMode = 1u;
	uint32_t m_nFiltering = 1u;
	float	 m_fAuthoredColorContribution = 1.0f;
	float	 m_fWetnessContribution = 1.0f;
	float	 m_fMaterialLodBias = 0.0f;
	float	 m_fQualityLodOffset = 0.0f;
} t3dRdTierMaterialConfig_t;

typedef struct alignas(
	16 ) t3dRdEditorDecal_s
{
	float	 m_fPositionX = 0.0f;
	float	 m_fPositionY = 0.0f;
	float	 m_fSize = 0.0f;
	float	 m_fRotation = 0.0f;
	float	 m_fColorR = 0.0f;
	float	 m_fColorG = 0.0f;
	float	 m_fColorB = 0.0f;
	float	 m_fColorA = 0.0f;
	uint32_t m_nVisible = 0u;
	uint32_t m_nPartFlags = 0u;
	// IEEE-754 cos/sin of the authored rotation, published in the existing
	// state.zw lanes. Keep the 48-byte editor overlay ABI unchanged.
	uint32_t m_nRotationCosBits = 0x3f800000u;
	uint32_t m_nRotationSinBits = 0u;
} t3dRdEditorDecal_t;

typedef struct alignas(
	16 ) t3dRdMeshVertex_s
{
	float	 m_szPosition[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t m_nColorRgba8 = 0xFFFFFFFFu;
	int16_t	 m_szNormal[4] = { 0, 32767, 0, 0 };
	int16_t	 m_szTangent[4] = { 32767, 0, 0, 32767 };
	uint16_t m_szUv[2] = { 0u, 0u };
	uint16_t m_szUv2[2] = { 0u, 0u };
	uint32_t m_nPadding0 = 0u;
	uint32_t m_nPadding1 = 0u;
} t3dRdMeshVertex_t;

typedef struct alignas(
	16 ) t3dRdInstanceRecord_s
{
	float	 m_szPosition[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t m_nQuaternion01 = 0u;
	uint32_t m_nQuaternion23 = 0u;
	uint32_t m_nScale01 = 0u;
	uint32_t m_nScale2Flags = 0u;
	uint32_t m_nColorRgba8 = 0xFFFFFFFFu;
} t3dRdInstanceRecord_t;

typedef struct t3dRdInstanceCell_s
{
	uint64_t m_nMeshSourceKey = 0u;
	int32_t	 m_nRegionX = 0;
	int32_t	 m_nRegionZ = 0;
	int16_t	 m_nCellX = 0;
	int16_t	 m_nCellZ = 0;
	uint16_t m_nMeshAssetId = 0u;
	int16_t	 m_nLod = 0;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nRevision = 0u;
	uint32_t m_nSettingsRevision = 0u;
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nFlags = 0u;
	float	 m_fLodBegin = 0.0f;
	float	 m_fLodEnd = 0.0f;
	float	 m_fFadeMargin = 0.0f;
	float	 m_fPadding0 = 0.0f;
	AABB	 m_bounds;
	AABB	 m_occlusionBounds;
	uint32_t m_nOcclusionSourceRevision = 0u;
	uint32_t m_nOcclusionProxyGeneration = 0u;
	uint32_t m_nOcclusionProxyPolicy = 0u;
} t3dRdInstanceCell_t;

typedef struct alignas(
	16 ) t3dRdHlodCullDescriptor_s
{
	float	 m_szBoundsCenter[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float	 m_szBoundsExtent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nCommandIndex = 0u;
	uint32_t m_nFirstVisible = 0u;
	uint32_t m_nReserved0 = 0u;
} t3dRdHlodCullDescriptor_t;

typedef struct alignas(
	16 ) t3dRdInstanceCullDescriptor_s
{
	float	 m_szBoundsCenter[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float	 m_szBoundsExtent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t m_nDrawSlot = UINT32_MAX;
	uint32_t m_nCommandIndex = 0u;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nFirstVisible = 0u;
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nFlags = 0u;
	float	 m_fLodBegin = 0.0f;
	float	 m_fLodEnd = 0.0f;
	alignas( 16 ) float m_szLocalBoundsCenter[4] = {};
	alignas( 16 ) float m_szLocalBoundsExtent[4] = {};
} t3dRdInstanceCullDescriptor_t;

typedef struct alignas(
	16 ) t3dRdInstanceMaterial_s
{
	float	 m_szAlbedo[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	float	 m_fMetallic = 0.0f;
	float	 m_fRoughness = 1.0f;
	float	 m_fAo = 1.0f;
	float	 m_fAlphaScissor = 0.5f;
	float	 m_szEmission[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t m_nFlags = 0u;
	float	 m_fNormalScale = 1.0f;
	uint32_t m_nReserved0 = 0u;
} t3dRdInstanceMaterial_t;

typedef struct t3dRdTerrainGenerationTuple_s
{
	uint64_t m_nRegionTopologyGeneration = 0u;
	uint64_t m_nPhysicalRegionMapGeneration = 0u;
	uint64_t m_nPatchGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nRegionLightingSignature = 0u;
} t3dRdTerrainGenerationTuple_t;

typedef struct alignas(
	16 ) t3dRdFrameUniforms_s
{
	float m_mViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16]{};
	float m_mView[16]{};
	float m_mPreviousViewProjection[T3D_RD_FORWARD_MAX_VIEWS][16]{};
	float m_szMotionJitter[4]{};
	float m_szMotionParams[4]{};
	float m_fLightDirectionX = 0.0f;
	float m_fLightDirectionY = -1.0f;
	float m_fLightDirectionZ = 0.0f;
	float m_fAmbient = 0.28f;
	float m_fLightColorR = 1.0f;
	float m_fLightColorG = 1.0f;
	float m_fLightColorB = 1.0f;
	float m_fLightColorA = 1.0f;
	float m_fTerrainPadding0 = 0.0f;
	float m_fTerrainPadding1 = 0.0f;
	float m_fHeightMin = s_fRegionHeightMin;
	float m_fHeightRange = s_fRegionHeightRange;
	float m_fVertexSpacing = 1.0f;
	float m_fMeshSize = 48.0f;
	float m_fDebugBlend = 0.0f;
	float m_fMaterialLayerCount = 1.0f;
	float m_fTargetX = 0.0f;
	float m_fTargetZ = 0.0f;
	float m_fSubdiv = 1.0f;
	float m_fTargetPadding = 0.0f;
	float m_szCamera[T3D_RD_FORWARD_MAX_VIEWS][4]{};
	float m_fFogR = 0.0f;
	float m_fFogG = 0.0f;
	float m_fFogB = 0.0f;
	float m_fFogDensity = 0.0f;
	float m_fFogStart = 0.0f;
	float m_fFogEnd = 1000000.0f;
	float m_fFogCurve = 1.0f;
	float m_fFogMode = 0.0f;
	float m_fDisplacementScale = 1.0f;
	float m_fDisplacementEnabled = 0.0f;
	float m_fDisplacementPadding0 = 0.0f;
	float m_fDisplacementPadding1 = 0.0f;
	float m_szInstanceTextureMipBias[4]; // xyz: mip bias; w: packed category filter overrides
} t3dRdFrameUniforms_t;

typedef struct t3dRdDrawIndirectCommand_s
{
	uint32_t m_nVertexCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nFirstVertex = 0u;
	uint32_t m_nFirstInstance = 0u;
} t3dRdDrawIndirectCommand_t;

typedef struct t3dRdIndexedIndirectCommand_s
{
	uint32_t m_nIndexCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nFirstIndex = 0u;
	int32_t	 m_nVertexOffset = 0;
	uint32_t m_nFirstInstance = 0u;
} t3dRdIndexedIndirectCommand_t;

typedef struct alignas(
	16 ) t3dInstanceDrawMeta_s
{
	uint32_t m_nMaterial = T3D_RD_INSTANCE_DRAW_META_INVALID_MATERIAL;
	uint32_t m_nOverlayMaterial = T3D_RD_INSTANCE_DRAW_META_INVALID_MATERIAL;
	uint32_t m_nFirstIndex = 0u;
	uint32_t m_nIndexCount = 0u;
	int32_t	 m_nVertexOffset = 0;
	uint32_t m_nFlags = 0u;
	uint32_t m_nVisibleBase = T3D_RD_INSTANCE_DRAW_META_DYNAMIC_VISIBLE_BASE;
	uint32_t m_nReserved = 0u;
} t3dInstanceDrawMeta_t;

typedef struct alignas(
	16 ) t3dInstanceVTPredictMeta_s
{
	uint32_t m_nBasePrediction = UINT32_MAX;
	uint32_t m_nOverlayPrediction = UINT32_MAX;
	uint32_t m_nFlags = 0u;
	uint32_t m_nReserved = 0u;
} t3dInstanceVTPredictMeta_t;

typedef struct t3dRdCullPushConstants_s
{
	uint32_t m_nPatchCount = 0u;
	uint32_t m_nLod0PartCount = 0u;
	uint32_t m_nOuterPartCount = 0u;
	uint32_t m_nMaxPatches = T3D_MAX_CLIPMAP_LEVELS;
	uint32_t m_nStripCommandBase = 0u;
	uint32_t m_nIndexedCommandBase = 0u;
	uint32_t m_nHotIndex = 0u;
	uint32_t m_nCopyOnly = 0u;
} t3dRdCullPushConstants_t;

typedef struct t3dRdDrawPushConstants_s
{
	uint32_t m_nDirectPatchIndex = 0u;
	uint32_t m_nDirectMode = 0u;
	uint32_t m_nPadding0 = 0u;
	uint32_t m_nPadding1 = 0u;
	uint32_t m_nHotIndex = 0u;
} t3dRdDrawPushConstants_t;

typedef struct t3dRenderLifecycleStatus_s
{
	uint32_t				  m_nStartupGeneration = 0u;
	uint32_t				  m_nReadyMask = T3D_RENDER_READY_NONE;
	t3dRenderLifecycleStage_t m_eStage = T3D_RENDER_LIFECYCLE_IDLE;
	uint32_t				  m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	uint32_t				  m_nSharedResourceClass = 0u;
	uint64_t				  m_nPreparedWorldPublicationGeneration = 0u;
	uint64_t				  m_nRecoveryRevision = 0u;
	uint64_t				  m_nRevision = 0u;
} t3dRenderLifecycleStatus_t;

typedef struct t3dWorldRenderAuthorityPolicy_s
{
	bool bShippingUnifiedRequired;
	bool bUnifiedWorldPass;
	bool bEditorPreviewPass;
} t3dWorldRenderAuthorityPolicy_t;

typedef struct t3dTerrainMegaVTRegion_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nFlags;
	float	 m_vWorldToUv[4]; // scale X/Z, bias X/Z
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nTexelsPerMeter;
	uint32_t m_nPageCount;
	uint32_t m_nReserved;
} t3dTerrainMegaVTRegion_t;

typedef struct t3dTerrainMegaVTCookRequest_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nAllocationFlags;
	int32_t	 m_nRegionX;
	int32_t	 m_nRegionZ;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nTexelsPerMeter;
	uint32_t m_nPageCount;
	uint32_t m_nCookFlags;
	uint32_t m_nPageInterior;
	uint32_t m_nPageGutter;
	uint32_t m_nPhysicalTile;
	float	 m_vWorldToUv[4];
} t3dTerrainMegaVTCookRequest_t;

typedef struct t3dTerrainMegaVTEditBounds_s
{
	float m_fMinWorldX;
	float m_fMinWorldZ;
	float m_fMaxWorldX;
	float m_fMaxWorldZ;
} t3dTerrainMegaVTEditBounds_t;

typedef struct t3dTerrainMegaVTEditorState_s
{
	uint32_t m_nDirtyMip0Mask;
	uint32_t m_nDirtyPageMask;
	uint32_t m_nInFlightGeneration;
	uint32_t m_nLastPublishedGeneration;
} t3dTerrainMegaVTEditorState_t;

typedef struct t3dTerrainMegaVTCookTicket_s
{
	uint32_t m_nAllocationId;
	uint32_t m_nRegionMapIndex;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nPageMask;
	uint64_t m_nClaimId;
} t3dTerrainMegaVTCookTicket_t;

typedef struct t3dRdTerrainRegionGpu_s
{
	uint32_t m_nPackedPhysicalFlags;
	uint32_t m_nMegaVTAllocation;
	uint32_t m_nMegaVTGeneration;
	uint32_t m_nMegaVTFlags;
	float	 m_vMegaVTWorldToUv[4];
    uint32_t m_nFarVTAllocation;
    uint32_t m_nFarVTGeneration;
    uint32_t m_nComposedVTAllocation;
    uint32_t m_nComposedVTGeneration;
} t3dRdTerrainRegionGpu_t;

typedef struct t3dVirtualTextureStats_s
{
	uint64_t nInitializeCount;
	uint64_t nShutdownCount;
	uint64_t nCpuImageBytes;
	uint64_t nPhysicalCacheCreateEpoch;
	uint64_t nPhysicalCacheFreeEpoch;
	uint32_t nPhysicalPageSetCapacity;
	uint32_t nPhysicalCacheAllocationEvents;
	uint32_t nPhysicalCacheAllocationEventsAfterSeal;
	uint32_t nHotViolationCount;
	uint64_t nQualityDemandResetCount;
} t3dVirtualTextureStats_t;

typedef struct t3dVTFinalTelemetry_s
{
	uint64_t nAllocations;
	uint64_t nVirtualPages;
	uint64_t nPhysicalCapacity;
	uint64_t nPhysicalResident;
	uint64_t nPhysicalFree;
	uint64_t nPhysicalRetiring;
	uint64_t nRequestCapacity;
	uint64_t nRequestAttempts;
	uint64_t nRequestUnique;
	uint64_t nRequestSameKey;
	uint64_t nRequestHashOverflow;
	uint64_t nRequestListOverflow;
	uint64_t nRequestDropped;
	uint64_t nPageHits;
	uint64_t nPageMisses;
	uint64_t nParentFallbackHits;
	uint64_t nTailFallbackHits;
	uint64_t nResolverDepth0Samples;
	uint64_t nResolverDepth1Samples;
	uint64_t nResolverDepth2To3Samples;
	uint64_t nResolverDepth4PlusSamples;
	uint64_t nResolverTailSamples;
	uint64_t nIoJobsPending;
	uint64_t nIoBytesTotal;
	uint64_t nIoBytesPerSecond;
	uint64_t nUploadPageSetsFrame;
	uint64_t nUploadBytesFrame;
	uint64_t nUploadBytesTotal;
	uint64_t nEvictionsFrame;
	uint64_t nEvictionsTotal;
	uint64_t nThrashCount;
	uint64_t nResidentTerrain;
	uint64_t nResidentClutter;
	uint64_t nResidentGroundCover;
	uint64_t nResidentOccluder;
	uint64_t nResidentHlod;
	uint64_t nDescriptorBindsInstancerPass;
	uint64_t nDescriptorRebuildsRuntime;
	uint64_t nGpuResourceCreatesAfterSeal;
} t3dVTFinalTelemetry_t;

typedef struct t3dVTAllocation_s
{
	uint32_t m_nBaseVirtualPage;
	uint32_t m_nPageCount;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nFlags;
	uint32_t m_nTailIndex;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nReserved;
} t3dVTAllocation_t;

typedef struct t3dVTPageEntry_s
{
	uint32_t m_nPhysicalSlot;
	uint32_t m_nParentPage;
	uint32_t m_nGeneration;
	uint16_t m_nResolvedMip;
	uint16_t m_nFlags;
} t3dVTPageEntry_t;

typedef struct t3dVTMipLayout_s
{
	uint32_t m_nBaseVirtualPage;
	uint16_t m_nWidthPages;
	uint16_t m_nHeightPages;
} t3dVTMipLayout_t;

typedef struct t3dVTTailMetadata_s
{
	uint32_t m_nAtlasX;
	uint32_t m_nAtlasY;
	uint16_t m_nWidthTexels;
	uint16_t m_nHeightTexels;
	uint16_t m_nFirstMip;
	uint16_t m_nMipCount;
	uint32_t m_nGeneration;
	uint32_t m_nChannelMask;
	uint32_t m_nContentId;
	uint32_t m_nFlags;
} t3dVTTailMetadata_t;

typedef struct t3dVTResolveResult_s
{
	uint32_t m_nAllocation;
	uint32_t m_nRequestedPage;
	uint32_t m_nResolvedPage;
	uint32_t m_nPhysicalSlot;
	uint32_t m_nTailIndex;
	uint16_t m_nRequestedMip;
	uint16_t m_nResolvedMip;
	uint16_t m_nFallbackDepth;
	uint16_t m_nFlags;
	uint32_t m_nReserved;
} t3dVTResolveResult_t;

typedef struct t3dVTPhysicalSlot_s
{
	uint32_t m_nVirtualPage;
	uint32_t m_nGeneration;
	uint32_t m_nLastVisibleFrame;
	uint32_t m_nLastRequestedFrame;
	uint16_t m_nFlags;
	uint16_t m_nClockAge;
	uint16_t m_nPinCount;
	uint16_t m_nRetireFrame;
	uint32_t m_nPayloadId;
	uint32_t m_nReserved;
} t3dVTPhysicalSlot_t;

typedef struct t3dVTRequest_s
{
	uint32_t m_nVirtualPage;
	uint16_t m_nWantedMip;
	uint16_t m_nPriority;
} t3dVTRequest_t;

typedef struct t3dRenderingMaterialVTBindings_s
{
	uint32_t m_nVtAllocation;
	uint32_t m_nOverlayVtAllocation;
} t3dRenderingMaterialVTBindings_t;

typedef struct t3dRenderingMaterialGpu_s
{
	uint32_t m_nVtAllocation;
	uint32_t m_nOverlayVtAllocation;
	uint32_t m_nFlags;
	uint32_t m_nPackedScalars0;
	float	 m_vBaseColor[4];
	float	 m_vUvScaleBias[4];
	uint32_t m_nPackedEmission;
	uint32_t m_nPackedScalars1;
	uint32_t m_nSamplerClass;
	uint32_t m_nReserved;
} t3dRenderingMaterialGpu_t;


typedef struct t3dVTRequestGpuCounters_s
{
	uint32_t m_nCompactedRequestCount;
	uint32_t m_nRawAttemptCount;
	uint32_t m_nUniqueAcceptedCount;
	uint32_t m_nSameKeyMergeCount;
	uint32_t m_nProbeOverflowCount;
	uint32_t m_nRequestListOverflowCount;
	uint32_t m_nVisiblePredictionCount;
	uint32_t m_nFuturePredictionCount;
	uint32_t m_nMipCoarsenCount;
	uint32_t m_nBaseAllocationCount;
	uint32_t m_nOverlayAllocationCount;
	uint32_t m_nFragmentMissCount;
	uint32_t m_nFrameId;
	uint32_t m_nGeneration;
	uint32_t m_nHashCapacity;
	uint32_t m_nRequestCapacity;
} t3dVTRequestGpuCounters_t;

typedef struct t3dVTFragmentFeedbackParams_s
{
	uint32_t m_nHashCapacity;
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nFlags;
	uint32_t m_nPredictedTerrainMaterials; // one bit per material layer, never a runtime allocation ID
	uint32_t m_nReserved0;
	uint32_t m_nReserved1;
	uint32_t m_nReserved2;
} t3dVTFragmentFeedbackParams_t;

typedef struct t3dVTRequestHashEntry_s
{
	uint32_t m_nVirtualPage;
	uint32_t m_nWantedMip;
	uint32_t m_nPriority;
	uint32_t m_nProducerMask;
} t3dVTRequestHashEntry_t;

typedef struct t3dVTPredictionMetadata_s
{
	float	 m_vWorldSphere[4];
	float	 m_vUvBounds[4];
	uint32_t m_nBaseVtAllocation;
	uint32_t m_nOverlayVtAllocation;
	uint32_t m_nMaterialId;
	uint32_t m_nDrawSlot;
	float	 m_fTexelDensity;
	float	 m_fObjectScale;
	uint16_t m_nGeometryLod;
	uint8_t	 m_nCategory;
	int8_t	 m_nMipBias;
	uint32_t m_nFlags;
} t3dVTPredictionMetadata_t;

typedef struct t3dVTPredictionDispatch_s
{
	uint32_t m_nHashCapacity;
	uint32_t m_nRequestCapacity;
	uint32_t m_nFrameId;
	uint32_t m_nGeneration;
	float	 m_vCameraLinearVelocity[3];
	float	 m_fPredictionHorizon;
	float	 m_fCameraAngularSpeed;
	float	 m_fViewportWidth;
	float	 m_fViewportHeight;
	float	 m_fFuturePriorityScale;
	uint32_t m_nDrawSlotCount;
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nFlags;
} t3dVTPredictionDispatch_t;

typedef struct t3dVTFixedProfileDesc_s
{
	uint32_t m_nExtent[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nSupportedChannelMask;
} t3dVTFixedProfileDesc_t;

typedef struct t3dVTGpuStats_s
{
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nDispatchCount;
	uint64_t nComputeListBarrierCount;
	uint64_t nAsyncReadbackCount;
	uint64_t nUploadBytes;
	uint64_t nReadbackConsumeCount;
	uint64_t nAcceptedRequestCount;
	uint64_t nReadbackBytes;
	uint64_t nVariableTierDispatchCount;
	uint64_t nRetireRetryCount;
	uint64_t nPageTableUpdateBytes;
	uint64_t nPageTableUpdateCalls;
	uint64_t nPageTableBatchCount;
	uint64_t nPageTableBatchEntries;
	uint64_t nRequestAttempts;
	uint64_t nRequestUnique;
	uint64_t nRequestSameKey;
	uint64_t nRequestHashOverflow;
	uint64_t nRequestListOverflow;
	uint64_t nFragmentFaultCount;
	uint32_t nReadbackRingBusyCount;
	uint32_t nReadbackRejectCount;
	uint32_t nReadbackStaleGenerationCount;
	uint32_t nReadbackInvalidPayloadCount;
	uint32_t nReadbackCadenceSkipCount;
	uint32_t nHotViolationCount;
	uint32_t nReserved;
	// P07: source-side transfer diagnostics; these counters never cause an RD sync.
	uint64_t nUploadPreflightRejected;
	uint64_t nNeutralStageCopyBytesSaved;
} t3dVTGpuStats_t;

typedef struct t3dVTPackRuntimeAllocator_s
{
	void						*pUser;
	t3dVTPackRuntimeAllocateFn_t pAllocate;
	t3dVTPackRuntimeFreeFn_t	 pFree;
} t3dVTPackRuntimeAllocator_t;

typedef struct t3dVTQualityRuntimeStats_s
{
	uint32_t nRequestedProfile;
	uint32_t nPackageFullySupportedProfileMask;
	uint32_t nFallbackAllocationCount;
	uint32_t nAllocationsByHighestProfile[T3D_VT_QUALITY_PROFILE_COUNT];
	uint32_t nAllocationsByResolvedProfile[T3D_VT_QUALITY_PROFILE_COUNT];
} t3dVTQualityRuntimeStats_t;

typedef struct t3dVTResidencyPolicy_s
{
	uint32_t nHotHoldFrames[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	uint32_t nWarmHoldFrames[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	// GPU-P01: capacity-scaled cold-page protection. Percentages deliberately
	// sum below 100 so categories can borrow unused physical cache from peers.
	uint32_t nProtectedResidentPercent[T3D_VT_RESIDENCY_CATEGORY_COUNT];
	uint32_t nThrashWindowFrames;
	uint32_t nRetireDelayFrames;
} t3dVTResidencyPolicy_t;

typedef struct t3dVTFileHeader_s
{
	uint32_t nMagic;
	uint32_t nVersion;
	uint32_t nHeaderBytes;
	uint32_t nFlags;
	uint32_t nAllocationCount;
	uint32_t nPageCount;
	uint32_t nPayloadCount;
	uint32_t nChannelCount;
	uint64_t nAllocationOffset;
	uint64_t nPageOffset;
	uint64_t nPayloadDirectoryOffset;
	uint64_t nPayloadDataOffset;
	uint64_t nPayloadDataBytes;
	uint64_t nContentSignature;
	uint32_t nInteriorTexels;
	uint32_t nGutterTexels;
	uint32_t nPhysicalTexels;
	uint32_t nReserved0;
	uint64_t nReserved[4];
} t3dVTFileHeader_t;

typedef struct t3dVTPageFileRecord_s
{
	uint32_t nVirtualPage;
	uint32_t nParentPage;
	uint32_t nAllocationId;
	uint32_t nPayloadId;
	uint16_t nMip;
	uint16_t nChannel;
	uint16_t nFlags;
	uint16_t nReserved0;
	uint32_t nGeneration;
	uint32_t nReserved1;
} t3dVTPageFileRecord_t;

typedef struct t3dVTPayloadRecord_s
{
	uint64_t nOffset;
	uint32_t nCompressedBytes;
	uint32_t nDecodedBytes;
	uint32_t nCodec;
	uint32_t nChecksum;
	uint32_t nFlags;
	uint32_t nReserved;
} t3dVTPayloadRecord_t;

typedef struct t3dVTRuntimePayloadRecord_s
{
	uint64_t nOffset;
	uint32_t nBytes;
	uint32_t nChannel;
	uint32_t nContentId;
	uint32_t nChecksum;
} t3dVTRuntimePayloadRecord_t;

typedef struct t3dVTRuntimeTailSourceRecord_s
{
	uint32_t nPayloadId[T3D_VT_CHANNEL_COUNT];
	uint16_t nAtlasInteriorWidth;
	uint16_t nAtlasInteriorHeight;
} t3dVTRuntimeTailSourceRecord_t;

typedef struct t3dVTAllocationRecordV1_s
{
	uint32_t nBaseVirtualPage;
	uint32_t nPageCount;
	uint32_t nMip0WidthPages;
	uint32_t nMip0HeightPages;
	uint16_t nMipCount;
	uint16_t nChannel;
	uint16_t nFlags;
	uint16_t nReserved0;
	uint32_t nRootPage;
	uint32_t nReserved1;
} t3dVTAllocationRecordV1_t;

typedef struct t3dVTPackSource_s
{
	const t3dVTAllocationRecordV1_s *pAllocations;
	uint32_t						 nAllocationCount;
	const t3dVTPageFileRecord_t		*pPages;
	uint32_t						 nPageCount;
	const t3dVTPayloadRecord_t		*pPayloads;
	uint32_t						 nPayloadCount;
	const uint8_t					*pPayloadData;
	uint64_t						 nPayloadDataBytes;
} t3dVTPackSource_t;

struct t3dVTFileSource_s;
typedef struct t3dVTDatabaseView_s
{
	const t3dVTFileSource_s									 *pFileSource;
	const t3dVTFileHeader_t									 *pHeader;
	const t3dVTAllocationRecordV1_s							 *pAllocations;
	const t3dVTPageFileRecord_t								 *pPages;
	const t3dVTPayloadRecord_t								 *pPayloads;
	const uint8_t											 *pPayloadData;
	uint64_t												  nFileBytes;
	const t3dVTAllocation_t									 *pRuntimeAllocations;
	const t3dVTTailMetadata_t								 *pRuntimeTailMetadata;
	const t3dVTRuntimeTailSourceRecord_t					 *pRuntimeTailSources;
	const t3dVTRuntimePayloadRecord_t						 *pRuntimePayloads;
	uint32_t												  nRuntimePayloadCount;
	uint32_t												  nRuntimeTailCount;
	t3dVTPackByteRecords_t< t3dVTPackAllocationRecordV1_t >	  pV1Allocations;
	t3dVTPackByteRecords_t< t3dVTChannelAllocation_t >		  pV1ChannelAllocations;
	t3dVTPackByteRecords_t< t3dVTPackPageRecordV1_t >		  pV1Pages;
	t3dVTPackByteRecords_t< t3dVTPackTailRecordV1_t >		  pV1Tails;
	t3dVTPackByteRecords_t< t3dVTPackQualityViewV1_t >		  pV1QualityViews;
	t3dVTPackByteRecords_t< t3dVTPackFormatFamilyRecordV1_t > pV1FormatFamilies;
	uint32_t												  nV1ChannelAllocationCount;
	uint32_t												  nV1PageCount;
	uint32_t												  nV1TailCount;
	uint32_t												  nV1QualityViewCount;
	uint32_t												  nV1FormatFamilyCount;
	uint32_t												  nV1FullySupportedProfileMask;
	uint32_t												  nPackageGeneration;
	uint32_t												  nRuntimeKind;
	uint64_t												  nPackageContentSignature;
} t3dVTDatabaseView_t;

typedef struct t3dVTRetiredPageSet_s
{
	uint32_t nPhysicalSlot;
	uint32_t nVirtualPage;
	uint32_t nGeneration;
	uint32_t nReleaseFrame;
} t3dVTRetiredPageSet_t;

typedef struct t3dVTPageSetPayloadView_s
{
	const uint8_t *pChannelBytes[T3D_VT_CHANNEL_COUNT];
	uint32_t	   nChannelBytes[T3D_VT_CHANNEL_COUNT];
	uint32_t	   nPresentMask;
} t3dVTPageSetPayloadView_t;

typedef struct t3dVTPageTableEntry_s
{
	uint32_t nPhysicalSlot;
	uint32_t nParentPage;
	uint32_t nGeneration;
	uint16_t nResolvedMip;
	uint16_t nFlags;
} t3dVTPageTableEntry_t;

typedef struct t3dVTResidencyBuffers_s
{
	t3dVTAllocation_t	  *pAllocations;
	uint32_t			   nAllocationCapacity;
	t3dVTPageTableEntry_s *pPageTable;
	uint32_t			   nPageTableCapacity;
	t3dVTTailMetadata_t	  *pTailMetadata;
	uint32_t			   nTailMetadataCapacity;
	t3dVTPhysicalSlot_t	  *pPhysicalPageSets;
	uint32_t			   nPhysicalPageSetCapacity;
	uint32_t			  *pFreeSlots;
	t3dVTRetiredPageSet_t *pRetiredPageSets;
	uint32_t			  *pLastEvictedFrame;
} t3dVTResidencyBuffers_t;

typedef struct t3dVTUploadReservation_s
{
	uint32_t nVirtualPage;
	uint32_t nPhysicalSlot;
	uint32_t nVictimPage;
	uint32_t nExpectedGeneration;
	uint32_t nDatabaseGeneration;
	uint16_t nWantedMip;
	uint16_t nReserved0;
} t3dVTUploadReservation_t;

typedef struct t3dVTResidencyStats_s
{
	uint64_t nTouchCount;
	uint64_t nFallbackCount;
	uint64_t nAncestorFallbackCount;
	uint64_t nTailFallbackCount;
	uint64_t nUploadReservationCount;
	uint64_t nUploadCommitCount;
	uint64_t nEvictionCount;
	uint64_t nClockScanCount;
	uint64_t nReservationFailureCount;
	uint64_t nRetireFinalizeCount;
	uint64_t nRetireDeferredCount;
	uint64_t nThrashCount;
	uint64_t nHotScanCount;
	uint64_t nWarmScanCount;
	uint64_t nColdScanCount;
	uint64_t nPinnedProtectionCount;
	uint32_t nFreeSlotCount;
	uint32_t nResidentCount;
	uint32_t nPendingSlotCount;
	uint32_t nRetiredSlotCount;
} t3dVTResidencyStats_t;

typedef struct t3dVTPredictedPageRect_s
{
	uint32_t nMip;
	uint32_t nMinPageX;
	uint32_t nMinPageY;
	uint32_t nMaxPageX;
	uint32_t nMaxPageY;
	uint32_t nPageCount;
} t3dVTPredictedPageRect_t;

typedef struct t3dVTGpuProfile_s
{
	uint32_t nVtAllocationRecordBytes;
	uint32_t nVtPageTableEntryBytes;
	uint32_t nVtPhysicalTileSize;
} t3dVTGpuProfile_t;

typedef struct t3dVTBankLayout_s
{
	uint32_t nBankCount;
	uint32_t nLayers[T3D_VT_MAX_TEXTURE_BANKS];
} t3dVTBankLayout_t;

typedef struct t3dVTGpuResourcePlan_s
{
	uint64_t nBufferBytes[T3D_VT_GPU_BUFFER_COUNT];
	uint64_t nVtPhysicalBytes[T3D_VT_CHANNEL_COUNT];
	uint64_t nVtTailBytes[T3D_VT_CHANNEL_COUNT];
	uint32_t nVtTailAtlasWidth;
	uint32_t nVtTailAtlasHeight;
	uint32_t nVtTailEntryCapacity;
	uint32_t nReserved0;
	uint64_t nTotalBufferBytes;
	uint64_t nTotalTextureBytes;
	uint64_t nTotalBytes;
} t3dVTGpuResourcePlan_t;

typedef struct t3dVTGpuResourceStats_s
{
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nLiveBufferBytes;
	uint64_t nLiveTextureBytes;
	uint64_t nResourceGeneration;
	uint32_t nLiveResourceCount;
	uint32_t nHotSealViolationCount;
} t3dVTGpuResourceStats_t;

typedef struct t3dVTGpuCounters_s
{
	uint32_t nRequestCount;
	uint32_t nPredictedRequestCount;
	uint32_t nDedupRequestCount;
	uint32_t nHashOverflowCount;
	uint32_t nListOverflowCount;
	uint32_t nFragmentFaultCount;
	uint32_t nReadbackIssueCount;
	uint32_t nReserved;
} t3dVTGpuCounters_t;

typedef struct t3dVTRuntimeRequest_s
{
	uint32_t nVirtualPage;
	uint16_t nWantedMip;
	uint16_t nPriority;
} t3dVTRuntimeRequest_t;

typedef struct t3dVTReadbackSlotHeader_s
{
	uint32_t nFrameId;
	uint32_t nDatabaseGeneration;
	uint32_t nRequestCapacity;
	uint32_t nReadbackBytes;
} t3dVTReadbackSlotHeader_t;

typedef struct t3dVTStreamJob_s
{
	uint64_t nReadOffset;
	uint32_t nReadBytes;
	uint32_t nExpectedDecodedBytes;
	uint32_t nVirtualPage;
	uint32_t nAllocationId;
	uint32_t nPayloadId;
	uint32_t nContentId;
	uint32_t nPageGeneration;
	uint32_t nAllocationGeneration;
	uint32_t nDatabaseGeneration;
	uint32_t nRequestFrame;
	uint32_t nDecodedBytes;
	uint32_t nScratchLane;
	uint16_t nWantedMip;
	uint16_t nPriority;
	uint16_t nPriorityBucket;
	uint16_t nChannel;
	uint16_t nState;
	uint16_t nReserved;
	uint32_t nResultCode;
} t3dVTStreamJob_t;

typedef struct t3dVTStreamingLimits_s
{
	uint64_t nMaxUploadBytesPerFrame;
	uint32_t nMaxPageSetsPerFrame;
	uint32_t nMaxInFlightIoJobs;
	uint32_t nMaxAdmittedRequestsPerFrame;
} t3dVTStreamingLimits_t;

typedef struct t3dVTStreamingStats_s
{
	uint64_t nQueuedCount;
	uint64_t nClaimedCount;
	uint64_t nCompletedCount;
	uint64_t nUploadedPageCount;
	uint64_t nUploadedBytes;
	uint64_t nStaleCompletionCount;
	uint64_t nStaleReadbackCount;
	uint64_t nInvalidReadbackCount;
	uint64_t nDroppedRequestCount;
	uint64_t nDuplicateMergeCount;
	uint64_t nResidentRequestCount;
	uint64_t nPriorityPromotionCount;
	uint64_t nReplacedQueuedJobCount;
	uint64_t nBudgetDeferredCount;
	uint64_t nAdmissionBudgetDeferredCount;
	uint64_t nIoBudgetDeferredCount;
	uint64_t nPageSetBudgetDeferredCount;
	uint64_t nUploadByteBudgetDeferredCount;
	uint64_t nOversizedReadCount;
	uint64_t nDecodeFailureCount;
	uint64_t nPageHitCount;
	uint64_t nPageMissCount;
	uint64_t nIoBytesTotal;
	uint64_t nAdmissionStatusCount[T3D_VT_ADMISSION_STATUS_COUNT];
	uint64_t nPriorityBucketClaimCount[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint32_t nPriorityBucketHighWater[T3D_VT_PRIORITY_BUCKET_COUNT];
	uint32_t nQueueHighWater;
	uint32_t nActiveJobCount;
	uint32_t nInFlightIoHighWater;
} t3dVTStreamingStats_t;

typedef struct t3dWorldClusterCookSource_s
{
	const t3dWorldVertex_s *m_pVertices;
	const uint32_t		   *m_pIndices;
	uint32_t				m_nVertexBase;
	uint32_t				m_nVertexCount;
	uint32_t				m_nIndexCount;
	uint32_t				m_nMaterial;
	uint32_t				m_nLod;
	uint32_t				m_nGeometryKind;
	uint32_t				m_nClusterFlags;
	uint32_t				m_nCookFlags;
	uint32_t				m_nSourceId;
} t3dWorldClusterCookSource_t;

typedef struct alignas(
	16 ) t3dWorldCluster_s
{
	float	 m_fCenter[3];
	float	 m_fRadius;
	float	 m_fConeAxis[3];
	float	 m_fConeCutoff;
	uint32_t m_nVertexIndexOffset;
	uint32_t m_nTriangleOffset;
	uint32_t m_nPackedCounts;
	uint32_t m_nPackedLodGeometryKind;
	uint32_t m_nMaterial;
	uint32_t m_nFlags;
	uint32_t m_nSourceId;
	uint32_t m_nReserved;
} t3dWorldCluster_t;

typedef struct alignas(
	16 ) t3dWorldClusterVTMetadata_s
{
	uint32_t m_nPackedUvMin;
	uint32_t m_nPackedUvMax;
	uint32_t m_nTexelDensity;
	uint32_t m_nReserved;
} t3dWorldClusterVTMetadata_t;

typedef struct t3dWorldClusterCookBuffers_s
{
	uint32_t					*m_pClusterVertexIndices;
	uint32_t					 m_nClusterVertexIndexCapacity;
	uint32_t					 m_nClusterVertexIndexCount;
	uint32_t					*m_pTriangleWords;
	uint32_t					 m_nTriangleWordCapacity;
	uint32_t					 m_nTriangleWordCount;
	t3dWorldCluster_s			*m_pClusters;
	t3dWorldClusterVTMetadata_s *m_pVtMetadata;
	uint32_t					 m_nClusterCapacity;
	uint32_t					 m_nClusterCount;
} t3dWorldClusterCookBuffers_t;

typedef struct t3dWorldClusterCookStats_s
{
	uint32_t m_nSourceTriangleCount;
	uint32_t m_nRasterTriangleCount;
	uint32_t m_nClusterCount;
	uint32_t m_nPaddedTriangleSlots;
} t3dWorldClusterCookStats_t;

typedef struct alignas(
	16 ) t3dWorldTerrainCluster_s
{
	uint32_t m_nPatchId;
	uint32_t m_nPackedGridOrigin;
	uint32_t m_nPackedGridExtent;
	uint32_t m_nPackedVertexCountLod;
	uint32_t m_nFlags;
	uint32_t m_nReserved0;
	uint32_t m_nReserved1;
	uint32_t m_nReserved2;
} t3dWorldTerrainCluster_t;

typedef struct alignas(
	16 ) t3dWorldVisibleItem_s
{
	uint32_t m_nClusterOrTerrainId;
	uint32_t m_nInstanceOrPatchId;
	uint32_t m_nGeometryKind;
	uint32_t m_nFlags;
} t3dWorldVisibleItem_t;

typedef struct alignas(
	16 ) t3dWorldDrawIndirectArgs_s
{
	uint32_t m_nVertexCount;
	uint32_t m_nInstanceCount;
	uint32_t m_nFirstVertex;
	uint32_t m_nFirstInstance;
} t3dWorldDrawIndirectArgs_t;

typedef struct alignas(
	16 ) t3dWorldMeshClusterSpan_s
{
	uint32_t m_nVertexOffset;
	uint32_t m_nVertexCount;
	uint32_t m_nClusterOffset;
	uint32_t m_nClusterCount;
} t3dWorldMeshClusterSpan_t;

typedef struct alignas(
	16 ) t3dWorldFrame_s
{
	float	 m_fViewProjection[16];
	float	 m_fPreviousViewProjection[16];
	float	 m_fFrustumPlanes[24];
	float	 m_fCameraAndMaxDistance[4];
	float	 m_fViewportAndInvSize[4];
	float	 m_fMotionJitter[4];
	uint32_t m_nClusterCount;
	uint32_t m_nTerrainClusterCount;
	uint32_t m_nTransformCount;
	uint32_t m_nMaterialCount;
	uint32_t m_nVisibleCapacity;
	uint32_t m_nPassKind;
	uint32_t m_nViewIndex;
	uint32_t m_nFrameId;
	uint32_t m_nPublicationGeneration;
	uint32_t m_nGeometryGeneration;
	uint32_t m_nMaterialGeneration;
	uint32_t m_nVtGeneration;
	uint32_t m_nHiZWidth;
	uint32_t m_nHiZHeight;
	uint32_t m_nHiZMipCount;
	uint32_t m_nFlags;
} t3dWorldFrame_t;

typedef struct alignas(
	16 ) t3dWorldResourceLimits_s
{
	uint32_t m_nMaxSourceVertices;
	uint32_t m_nMaxClusters;
	uint32_t m_nMaxTerrainClusters;
	uint32_t m_nMaxTransforms;
	uint32_t m_nMaxVisibleItems;
	uint32_t m_nMaxMaterials;
	uint32_t m_nMaxClusterVertexIndices;
	uint32_t m_nMaxTriangleSlots;
	uint64_t m_nGeometryHeapBytes;
	uint64_t m_nTriangleHeapBytes;
	uint64_t m_nVisibilityBytes;
	uint64_t m_nWorldUploadBytesPerFrame;
	uint64_t m_nPublicationScratchBytes;
	uint32_t m_nHiZMaxWidth;
	uint32_t m_nHiZMaxHeight;
	uint32_t m_nHiZMaxMipCount;
	uint32_t m_nVtPageRequestBudget;
	uint32_t m_nVtPageAdmissionBudget;
	uint32_t m_nReserved0;
} t3dWorldResourceLimits_t;

typedef struct t3dWorldGeometryRequirements_s
{
	uint64_t nSourceTriangleCount;
	uint64_t nClusterVertexReferenceCount;
	uint64_t nTriangleSlotCount;
	uint32_t nClusterCount;
	uint32_t nMinimumClusterTriangleCount;
	uint32_t nMaximumClusterTriangleCount;
} t3dWorldGeometryRequirements_t;

typedef struct t3dWorldPackedClusterTriangle_s
{
	uint32_t nLocalIndices;
} t3dWorldPackedClusterTriangle_t;

typedef struct t3dWorldMeshRecord_s
{
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	uint32_t nLodOffset;
	uint32_t nLodCount;
	uint32_t nMaterialBase;
	uint32_t nFlags;
	uint32_t nReserved0;
	uint32_t nReserved1;
} t3dWorldMeshRecord_t;

typedef struct t3dWorldMeshLodRecord_s
{
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	float	 fMinDistance;
	float	 fMaxDistance;
	float	 fScreenError;
	uint32_t nFlags;
	uint32_t nReserved0;
	uint32_t nReserved1;
} t3dWorldMeshLodRecord_t;

typedef struct t3dWorldClusterRecord_s
{
	float	 vCenter[3];
	float	 fRadius;
	float	 vConeAxis[3];
	float	 fConeCutoff;
	uint32_t nVertexIndexOffset;
	uint32_t nTriangleOffset;
	uint16_t nVertexCount;
	uint16_t nTriangleCount;
	uint16_t nLod;
	uint16_t nTier;
	uint32_t nMaterialId;
	uint32_t nPackedUvMin;
	uint32_t nPackedUvMax;
	uint32_t nTexelDensity;
} t3dWorldClusterRecord_t;

typedef struct t3dWorldGeometrySourceLod_s
{
	const uint32_t *pIndices;
	const uint32_t *pTriangleMaterialOffsets;
	const uint8_t  *pTriangleTiers;
	uint32_t		nIndexCount;
	uint32_t		nDefaultTier;
	float			fMinDistance;
	float			fMaxDistance;
	float			fScreenError;
	uint32_t		nFlags;
} t3dWorldGeometrySourceLod_t;

typedef struct t3dWorldGeometrySourceMesh_s
{
	const t3dWorldVertex_t			  *pVertices;
	const t3dWorldGeometrySourceLod_t *pLods;
	uint32_t						   nVertexCount;
	uint32_t						   nLodCount;
	uint32_t						   nMaterialBase;
	uint32_t						   nFlags;
} t3dWorldGeometrySourceMesh_t;

typedef struct t3dWorldGeometryDatabaseBuffers_s
{
	t3dWorldVertex_t				*pVertexHeap;
	uint32_t						 nVertexCapacity;
	uint32_t						*pClusterVertexIndexHeap;
	uint32_t						 nClusterVertexIndexCapacity;
	t3dWorldPackedClusterTriangle_t *pTriangleHeap;
	uint32_t						 nTriangleCapacity;
	t3dWorldMeshRecord_t			*pMeshRecords;
	uint32_t						 nMeshCapacity;
	t3dWorldMeshLodRecord_t			*pLodRecords;
	uint32_t						 nLodCapacity;
	t3dWorldClusterRecord_t			*pClusterRecords;
	t3dWorldCluster_t				*pWorldClusterRecords;
	t3dWorldClusterVTMetadata_t		*pWorldClusterVtMetadata;
	uint32_t						 nClusterCapacity;
} t3dWorldGeometryDatabaseBuffers_t;

typedef struct t3dWorldGeometryCookedMeshSpan_s
{
	uint32_t nMeshId;
	uint32_t nVertexOffset;
	uint32_t nVertexCount;
	uint32_t nLodOffset;
	uint32_t nLodCount;
	uint32_t nClusterOffset;
	uint32_t nClusterCount;
	uint32_t nReserved;
} t3dWorldGeometryCookedMeshSpan_t;

typedef struct t3dWorldGeometryCookStats_s
{
	uint64_t nSourceTriangleCount;
	uint64_t nClusterTriangleSlotCount;
	uint64_t nClusterVertexReferenceCount;
	uint32_t nClusterCount;
	uint32_t nMinimumClusterTriangleCount;
	uint32_t nMaximumClusterTriangleCount;
	uint32_t nReserved;
} t3dWorldGeometryCookStats_t;

typedef struct t3dWorldGeometryDerivedCluster_s
{
	float	 vCenter[3];
	float	 fRadius;
	float	 vConeAxis[3];
	float	 fConeCutoff;
	uint32_t nPackedUvMin;
	uint32_t nPackedUvMax;
	uint32_t nTexelDensity;
} t3dWorldGeometryDerivedCluster_t;

typedef struct t3dWorldHiZReducePush_s
{
	uint32_t m_nSourceWidth;
	uint32_t m_nSourceHeight;
	uint32_t m_nDestinationWidth;
	uint32_t m_nDestinationHeight;
} t3dWorldHiZReducePush_t;

typedef struct alignas(
	16 ) t3dWorldCell_s
{
	float	 m_fCenter[3];
	float	 m_fRadius;
	uint32_t m_nFirstInstance;
	uint32_t m_nInstanceCount;
	uint32_t m_nVisibilityLayers;
	uint32_t m_nFlags;
} t3dWorldCell_t;

typedef struct alignas(
	16 ) t3dWorldHlodRecord_s
{
	uint32_t m_nFirstInstance;
	uint32_t m_nInstanceCount;
	uint32_t m_nClusterOffset;
	uint32_t m_nClusterCount;
	uint32_t m_nProxySourceId;
	uint32_t m_nDetailSourceId;
	uint32_t m_nFlags;
	uint32_t m_nReserved;
} t3dWorldHlodRecord_t;

typedef struct alignas(
	16 ) t3dWorldTerrainClusterBounds_s
{
	float m_fCenter[3];
	float m_fRadius;
} t3dWorldTerrainClusterBounds_t;

typedef struct t3dWorldPublicationCapacity_s
{
	size_t m_nVertices;
	size_t m_nClusterVertexIndices;
	size_t m_nTriangleWords;
	size_t m_nClusters;
	size_t m_nClusterVtMetadata;
	size_t m_nTerrainStripClusters;
	size_t m_nTransforms;
	size_t m_nInstances;
	size_t m_nCells;
	size_t m_nHlodRecords;
	size_t m_nMaterials;
	size_t m_nVisibilityCandidates;
} t3dWorldPublicationCapacity_t;

typedef struct t3dWorldGeometryDatabase_s
{
	t3dWorldGeometryDatabaseBuffers_t Buffers;
	uint32_t						  nVertexCount;
	uint32_t						  nClusterVertexIndexCount;
	uint32_t						  nTriangleCount;
	uint32_t						  nMeshCount;
	uint32_t						  nLodCount;
	uint32_t						  nClusterCount;
	uint64_t						  nContentSignature;
} t3dWorldGeometryDatabase_t;

struct t3dWorldStaticBuildSource_t
{
	const t3dWorldGeometryDatabase_s *m_pGeometry = NULL;
	const t3dWorldTransform_t		 *m_pTransforms = NULL;
	const t3dWorldInstance_t		 *m_pInstances = NULL;
	const t3dWorldCell_t			 *m_pCells = NULL;
	const t3dRenderingMaterialGpu_t	 *m_pMaterials = NULL;
	uint32_t						  m_nTransformCount = 0u;
	uint32_t						  m_nInstanceCount = 0u;
	uint32_t						  m_nCellCount = 0u;
	uint32_t						  m_nMaterialCount = 0u;
};

typedef struct t3dWorldLimits_s
{
	uint32_t nMaxCells;
	uint32_t nMaxInstances;
	uint32_t nMaxMeshes;
	uint32_t nMaxMeshLods;
	uint32_t nMaxClusters;
	uint32_t nMaxMaterials;
	uint32_t nMaxVisibleCells;
	uint32_t nMaxVisibleInstances;
	uint32_t nMaxVisibleClusters[T3D_WORLD_TIER_COUNT];
	uint32_t nMaxVtVirtualPages;
	uint32_t nMaxVtPhysicalPageSets;
	uint32_t nMaxVtRequests;
	uint32_t nMaxStreamJobs;
	uint32_t nStreamScratchLaneCount;
	uint64_t nMaxStreamPayloadBytes;
	uint64_t nMaxUploadBytesPerFrame;
	uint64_t nMaxReadbackBytesPerFrame;
	uint64_t nGeometryVertexHeapBytes;
	uint64_t nGeometryClusterVertexIndexHeapBytes;
	uint64_t nGeometryTriangleHeapBytes;
	uint64_t nSnapshotBytesPerFrame;
	uint64_t nFrameScratchBytesPerFrame;
	uint64_t nStreamScratchBytesPerLane;
	uint32_t nOperationEventCapacity;
	uint32_t nReserved;
} t3dWorldLimits_t;

typedef struct t3dVisibilityPush_s
{
	uint32_t m_nCandidateCount;
	uint32_t m_nVisibilityMask;
	uint32_t m_nFlags;
	uint32_t m_nPassKind;
	uint32_t m_nTerrainBucketCounts[T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT];
	uint32_t m_nReserved0;
	uint32_t m_nReserved1;
	uint32_t m_nHotIndex;
} t3dVisibilityPush_t;

typedef struct t3dClipmapVerticalBounds_s
{
	double m_fMinY = -0.1;
	double m_fSizeY = 0.2;
} t3dClipmapVerticalBounds_t;

typedef struct t3dClipmapStripPart_s
{
	int16_t	 m_nOriginX;
	int16_t	 m_nOriginZ;
	uint16_t m_nCellWidth;
	uint16_t m_nCellHeight;
	uint16_t m_nPartCode;
	uint16_t m_nFlags;
} t3dClipmapStripPart_t;

typedef struct alignas(
	16 ) t3dMaterialData_s
{
	float m_fUvScale;
	float m_fOffset;
	float m_fScale;
	float m_fPad;
} t3dMaterialData_t;

typedef struct t3dDisplacementParams_s
{
	uint32_t m_nWidth, m_nHeight;
	float	 m_fVertexSpacing, m_fVertexDensity, m_fMeshSize, m_fTessellationLevel;
	float	 m_fTargetX, m_fTargetZ, m_fBlendSharpness, m_fDisplacementSharpness;
	float	 m_fAutoSlope, m_fAutoHeightReduction;
	uint32_t m_nAutoBaseTexture, m_nAutoOverlayTexture, m_bAutoEnabled, m_nSamplingMode;
	uint32_t m_bFastBlendCurve, m_nBackgroundMode;
	float	 m_fGroundLevel, m_fRegionBlend;
	float	 m_fHeightMin, m_fHeightRange;
	uint32_t m_bControlMapsImplicit, m_nTextureLayerCount, m_nPadding1, m_nPadding2;
} t3dDisplacementParams_t;

typedef struct t3dDerivedParams_s
{
	uint32_t m_nWidth;
	uint32_t m_nHeight;
	uint32_t m_nHaloStride;
	float	 m_fVertexSpacing;
} t3dDerivedParams_t;

typedef struct t3dBrushElement_s
{
	uint32_t m_nValue = 0u;	 // Height float bits, packed pControl, or RGBA8.
	uint32_t m_nAlpha = 0u;	 // Float bits.
	uint32_t m_nTarget = 0u; // Gradient/height m_nTarget float bits.
	uint32_t m_nFlags = 0u;	 // Bit 0: eligible.
} t3dBrushElement_t;

typedef struct t3dBrushParams_s
{
	uint32_t m_nMapType = 0u;
	uint32_t m_nTool = 0u;
	uint32_t m_nOperation = 0u;
	uint32_t m_nCount = 0u;
	float	 m_fStrength = 0.0f;
	float	 m_fSelectedHeight = 0.0f;
	float	 m_fBrushCenterHeight = 0.0f;
	float	 m_fRoughness = 0.0f;
	uint32_t m_nColorRgba8 = 0xffffffffu;
	uint32_t m_nAssetId = 0u;
	uint32_t m_nUvRotation = 0u;
	uint32_t m_nUvScale = 0u;
	uint32_t m_nFeatureFlags = 0u;
} t3dBrushParams_t;

typedef struct t3dDerivedMapComputeJob_s
{
	const float *m_pHeightHalo = NULL;
	uint8_t		*m_pRgbaOutput = NULL;
	uint32_t	 m_nHaloElements = 0u;
	uint32_t	 m_nOutputBytes = 0u;
	uint32_t	 m_nWidth = 0u;
	uint32_t	 m_nHeight = 0u;
	float		 m_fVertexSpacing = 1.0f;
	bool		 m_bSuccess = false;
} t3dDerivedMapComputeJob_t;

typedef struct t3dFarSurfaceBakeData_s
{
	uint8_t m_populated[T3D_MAX_TEXTURE_ASSETS] = {};
	int		m_nCount = 0;
	int		m_nFallbackTexture = 0;
} t3dFarSurfaceBakeData_t;

typedef struct t3dFarRegionalCookStats_s
{
	int		 m_nHeightUpdatesLastTick = 0;
	int		 m_nColorUpdatesLastTick = 0;
	int		 m_nHeightRegionsLastTick = 0;
	int		 m_nColorRegionsLastTick = 0;
	int		 m_nHeightDirtyRegions = 0;
	int		 m_nColorDirtyRegions = 0;
	uint64_t m_nHeightProcessingUsecLast = 0u;
	uint64_t m_nColorProcessingUsecLast = 0u;
	uint64_t m_nHeightDirtyTexelsLast = 0u;
	uint64_t m_nColorDirtyTexelsLast = 0u;
	uint64_t m_nHeightRebuiltBytesLast = 0u;
	uint64_t m_nColorRebuiltBytesLast = 0u;
	uint64_t m_nHeightUploadedBytesLast = 0u;
	uint64_t m_nColorUploadedBytesLast = 0u;
	uint64_t m_nHeightPublicationUsecLast = 0u;
	uint64_t m_nColorPublicationUsecLast = 0u;
	uint64_t m_nHeightPublicationCount = 0u;
	uint64_t m_nColorPublicationCount = 0u;
	uint64_t m_nHeightUploadedBytesTotal = 0u;
	uint64_t m_nColorUploadedBytesTotal = 0u;
	uint64_t m_nCookIoUsecLast = 0u;
	bool	 m_bHeightDirtyAll = true;
	bool	 m_bColorDirtyAll = true;
	bool	 m_bCookedLoaded = false;
	bool	 m_bCookedSaved = false;
} t3dFarRegionalCookStats_t;

typedef struct t3dShaderVariantState_s
{
	t3dWorldBackground_t		  m_worldBackground = FLAT;
	t3dTextureFiltering_t		  m_textureFiltering = LINEAR;
	t3dTerrainSamplingMode_t	  m_terrainSamplingMode = TERRAIN_SAMPLING_ADAPTIVE;
	t3dTerrainNormalMode_t		  m_terrainNormalMode = TERRAIN_NORMAL_ADAPTIVE;
	t3dColorSamplingMode_t		  m_colorSamplingMode = COLOR_SAMPLING_AUTO;
	t3dProjectionMode_t			  m_projectionMode = PROJECTION_DISABLED;
	t3dTessellationMaterialMode_t m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
	t3dSurfaceDataMode_t		  m_surfaceDataMode = SURFACE_DATA_AUTO;
	t3dTextureSurfaceMode_t		  m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	t3dTerrainShadingMode_t		  m_terrainShadingMode = SHADING_LAMBERT_GGX;
	t3dDetilingMode_t			  m_detilingMode = DETILING_CONTINUOUS;
	t3dWorldNoiseShaderMode_t	  m_worldNoiseShaderMode = WORLD_NOISE_DYNAMIC;
	bool						  m_bAutoShaderEnabled = false;
	bool						  m_bDualScalingEnabled = false;
	bool						  m_bMacroVariationEnabled = false;
	bool						  m_bOutputAlbedoEnabled = true;
	bool						  m_bOutputRoughnessEnabled = true;
	bool						  m_bOutputSpecularEnabled = true;
	bool						  m_bOutputNormalMapEnabled = true;
	bool						  m_bOutputAmbientOcclusionEnabled = true;
	bool						  m_bFastBlendCurve = false;
	bool						  m_bTessellationEnabled = false;
	bool						  m_bSurfacePacked = false;
	bool						  m_bControlMapsImplicit = false;
	bool						  m_bColorMapsImplicit = false;
} t3dShaderVariantState_t;

typedef struct t3dTierQualityConfig_s
{
	t3dTextureFiltering_t		 m_textureFiltering = LINEAR;
	t3dMaterialSamplingQuality_t m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
	t3dTerrainNormalQuality_t	 m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
	t3dTerrainColorQuality_t	 m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
	t3dSurfaceResponseQuality_t	 m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
	t3dTextureSurfaceMode_t		 m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	t3dTierLightingModel_t		 m_lightingModel = TIER_LIGHTING_LAMBERT;
	t3dTierDetilingQuality_t	 m_detilingQuality = TIER_DETILING_DISABLED;
	real_t						 m_fAuthoredColorContribution = 1.0f;
	real_t						 m_fWetnessContribution = 1.0f;
	real_t						 m_fMaterialLodBias = 0.0f;
} t3dTierQualityConfig_t;

typedef struct t3dMesherRuntimeConfig_s
{
	int	 m_nNearShaderLodCount = T3D_MAX_CLIPMAP_LEVELS;
	int	 m_nMiddleShaderLodCount = 0;
	int	 m_nImmediateLodCount = T3D_MAX_CLIPMAP_LEVELS;
	int	 m_nClipmapUpdateBudget = T3D_MAX_CLIPMAP_LEVELS;
	int	 m_nShadowLodCount = 0;
	int	 m_szTierShadowLodCounts[3] = { -1, -1, -1 };
	bool m_szTierShadowEnabled[3] = { true, true, true };
} t3dMesherRuntimeConfig_t;

typedef struct t3dOceanSettingsState_s
{
	float m_fSeaLevel = 5.0f;
	float m_szWaterColor[4] = { 0.1058f, 0.1568f, 0.2117f, 1.0f };
	float m_fVisibleDepth = 256.0f;
	float m_fDepthCurve = 0.1f;
	float m_fShoreBlend = 1.0f;
	float m_fRefractionStrength = 0.02f;
	float m_fTimeScale = 8.0f;
	float m_fHeightScale = 3.0f;
	float m_fOceanScale = 2.0f;
	float m_fFresnelScale = 0.5f;
	float m_fFresnelPower = 1.0f;
	float m_fSpecular = 1.0f;
	float m_fFoamHeight = 1.35f;
	float m_fFoamTopIntensity = 2.0f;
	float m_fFoamDistance = 3072.0f;
	float m_fFoamShoreIntensity = 1.3f;
	float m_fFoamNoiseScale = 1.5f;
	float m_fHeightScattering = 6.0f;
	float m_fHeightThreshold = 0.1f;
	float m_fHeightAngleThreshold = 0.21f;
	float m_fFresnelScattering = 20.0f;
	float m_fDirectScattering = 10.0f;
} t3dOceanSettingsState_t;

typedef struct t3dTerrainShaderPipelineKey_s
{
	uint8_t m_nPassClass = T3D_TERRAIN_SHADER_PASS_COLOR_GENERAL;
	uint8_t m_nTier = 0u;
	uint8_t m_nShaderPath = T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL;
	uint8_t m_nMaterialBackend = T3D_TERRAIN_MATERIAL_BACKEND_UNIFIED_COOKED_V1;
	uint8_t m_nShippingFeatureClass = T3D_TERRAIN_SHIPPING_FEATURE_RUNTIME;
	uint8_t m_nLightingModelClass = T3D_TERRAIN_LIGHTING_MODEL_RUNTIME;
	uint8_t m_nSceneIntegrationClass = T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME;
	uint8_t m_nDnrOutputClass = T3D_TERRAIN_DNR_OUTPUT_RUNTIME;
	uint8_t m_nMiddleSurfaceClass = T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME;
	uint8_t m_nEditorDiagnosticClass = T3D_TERRAIN_EDITOR_DIAGNOSTIC_RUNTIME;
	uint8_t m_nNoiseSourceClass = T3D_TERRAIN_NOISE_SOURCE_RUNTIME;
	bool	m_bDisplacement = false;
	bool	m_bWorldNoiseFragmentNormal = false;
	bool	m_bClusteredDecals = true;
	bool	m_bReflectionProbes = true;
	bool	m_bReceiveShadows = true;
} t3dTerrainShaderPipelineKey_t;

typedef struct t3dParallelTileDispatchContext_s
{
	size_t				  m_nWidth = 0u;
	size_t				  m_nHeight = 0u;
	size_t				  m_nTileWidth = 1u;
	size_t				  m_nTileHeight = 1u;
	size_t				  m_nTilesX = 0u;
	t3dParallelTileTask_t m_task = NULL;
	void				 *m_pContext = NULL;
} t3dParallelTileDispatchContext_t;

typedef struct t3dSimdCapabilities_s
{
	bool m_bSse2 = false;
	bool m_bAvx = false;
	bool m_bF16c = false;
	bool m_bAvx2 = false;
	bool m_bNeon = false;
} t3dSimdCapabilities_t;

typedef struct t3dActiveHeightResidencyStats_s
{
	int m_nReconciliationsLastTick = 0;
	int m_nSlotsExaminedLastTick = 0;
	int m_nRequiredLastTick = 0;
	int m_nMissingRequiredLastTick = 0;
	int m_nPublishedLastTick = 0;
	int m_nEvictedLastTick = 0;
	int m_nPredictedRequestsLastTick = 0;
} t3dActiveHeightResidencyStats_t;

typedef struct t3dAdaptiveBudgetValues_s
{
	int m_nGpuPages = 4;
	int m_nCpuPages = 4;
	int m_nInstancePages = 4;
	int m_nInstancerCells = 32;
	int m_nNodeLoadRegions = 2;
	int m_nNodeUnloadRegions = 4;
	int m_nWorldUploadKb = 4096;
	int m_nWorldClusters = 8192;
	int m_nVtRequests = 2048;
	int m_nVtAdmissions = 64;
} t3dAdaptiveBudgetValues_t;

typedef struct t3dAdaptiveBudgetPressure_s
{
	bool m_bGpuLimited = false;
	bool m_bCpuLimited = false;
	bool m_bInstanceLimited = false;
	bool m_bInstancerLimited = false;
	bool m_bNodeLoadLimited = false;
	bool m_bNodeUnloadLimited = false;
	bool m_bWorldLimited = false;
	bool m_bVtLimited = false;
} t3dAdaptiveBudgetPressure_t;

typedef struct t3dCpuRegionCacheStats_s
{
	bool	 m_bEnabled = false;
	bool	 m_bSelectionPending = true;
	int		 m_nCapacity = 0;
	int		 m_nResident = 0;
	int		 m_nDesiredLastTick = 0;
	int		 m_nLoadsLastTick = 0;
	int		 m_nEvictionsLastTick = 0;
	int		 m_nPinnedLastTick = 0;
	uint32_t m_nStallFlagsLastTick = 0u;
	size_t	 m_nLoadBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
} t3dCpuRegionCacheStats_t;

typedef struct t3dGpuCacheCoord_s
{
	int m_nX = 0;
	int m_nY = 0;
} t3dGpuCacheCoord_t;

typedef struct t3dGpuCacheSelectionStats_s
{
	int m_nRequired = 0;
	int m_nGuard = 0;
	int m_nPrefetch = 0;
	int m_nWritten = 0;
} t3dGpuCacheSelectionStats_t;

typedef struct t3dGpuRegionCacheStats_s
{
	bool	 m_bEnabled = false;
	bool	 m_bTexturesReady = false;
	bool	 m_bSelectionPending = true;
	bool	 m_bCameraMovedLastTick = false;
	uint64_t m_nBudgetBytes = 0u;
	int		 m_nBudgetMb = 0;
	int		 m_nCapacity = 0;
	int		 m_nPhysicalLayers = 0;
	int		 m_nSpareLayers = 0;
	int		 m_nResident = 0;
	int		 m_nRetired = 0;
	int		 m_nDesiredLastTick = 0;
	int		 m_nRequiredLastTick = 0;
	int		 m_nGuardLastTick = 0;
	int		 m_nPrefetchLastTick = 0;
	int		 m_nHysteresisLastTick = 0;
	int		 m_nMissingRequiredLastTick = 0;
	int		 m_nRequestedPages = 0;
	int		 m_nRequiredRadius = 0;
	int		 m_nUploadsLastTick = 0;
	int		 m_nEvictionsLastTick = 0;
	int		 m_nCookedHitsLastTick = 0;
	int		 m_nCookedMissesLastTick = 0;
	int		 m_nRuntimePreparesLastTick = 0;
	int		 m_nReusedLayersLastTick = 0;
	int		 m_nPageTableUpdatesLastTick = 0;
	int		 m_nHeightUpdatesLastTick = 0;
	int		 m_nControlUpdatesLastTick = 0;
	int		 m_nColorUpdatesLastTick = 0;
	int		 m_nDerivedUpdatesLastTick = 0;
	int		 m_nMiddleCompositeUpdatesLastTick = 0;
	int		 m_nTerrainShadowUpdatesLastTick = 0;
	int		 m_nRuntimeColorMipsLastTick = 0;
	int		 m_nRuntimeDerivedBuildsLastTick = 0;
	int		 m_nPreparedPagesLastTick = 0;
	int		 m_nPrepareWorkersLastTick = 0;
	int		 m_nEffectiveUploadLimitLastTick = 0;
	uint32_t m_nStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	size_t	 m_nUploadBytesLastTick = 0u;
	size_t	 m_nHeightBytesLastTick = 0u;
	size_t	 m_nControlBytesLastTick = 0u;
	size_t	 m_nColorBytesLastTick = 0u;
	size_t	 m_nDerivedBytesLastTick = 0u;
	size_t	 m_nMiddleCompositeBytesLastTick = 0u;
	size_t	 m_nTerrainShadowBytesLastTick = 0u;
	uint64_t m_nServiceUsecLast = 0u;
	uint64_t m_nPreparePublicationUsecLast = 0u;
	uint64_t m_nPrepareEndToEndUsecLast = 0u;
} t3dGpuRegionCacheStats_t;

typedef struct t3dGpuRegionCacheUpdateResult_s
{
	bool m_bPageTableChanged = false;
} t3dGpuRegionCacheUpdateResult_t;

typedef struct t3dMiddleHeightRegionCacheConfig_s
{
	bool m_bAuthoring = false;
	int	 m_nHotRadius = 0;
	int	 m_nResolutionDivisor = 1;
	int	 m_nRetireFrames = T3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES;
} t3dMiddleHeightRegionCacheConfig_t;

typedef struct t3dMiddleHeightRegionCacheStats_s
{
	int	   m_nResident = 0;
	int	   m_nMappedCapacity = 0;
	int	   m_nPhysicalLayers = 0;
	int	   m_nDesiredLastTick = 0;
	int	   m_nMissingRequiredLastTick = 0;
	int	   m_nUploadsLastTick = 0;
	int	   m_nEvictionsLastTick = 0;
	int	   m_nResolutionDivisor = 1;
	size_t m_nReservedBytes = 0u;
	bool   m_bPageTableValid = false;
	bool   m_bHeightArrayValid = false;
} t3dMiddleHeightRegionCacheStats_t;

typedef struct t3dRuntimePageNumericConfig_s
{
	int m_nColorDivisor = 1;
	int m_nDerivedDivisor = 1;
	int m_nMiddleHeightDivisor = 1;
	int m_nPrepareThreads = 2;
} t3dRuntimePageNumericConfig_t;

typedef struct t3dRuntimePageProbe_s
{
	t3dRuntimePageProbeState_t m_state = RUNTIME_PAGE_PROBE_MISSING;
	uint32_t				   m_nVersion = 0u;
	uint32_t				   m_nHeaderBytes = 0u;
	uint32_t				   m_nTotalBytes = 0u;
	uint32_t				   m_nChannelCount = 0u;
	uint64_t				   m_nFileBytes = 0u;
} t3dRuntimePageProbe_t;

typedef struct t3dRuntimePageStats_s
{
	// IO00: logical FileAccess-layer operations, not physical SSD traffic.
	uint64_t m_nSourceFileOpenCalls = 0u;
	uint64_t m_nSourceRangeReadCalls = 0u;
	uint64_t m_nSourceRangeReadBytes = 0u;
	uint64_t m_nSourceDistinctRangeBytes = 0u;
	uint64_t m_nSourceDuplicateRangeReads = 0u;
	uint64_t m_nSourceDuplicateRangeBytes = 0u;
	uint64_t m_nSourceCacheHits = 0u;
	uint64_t m_nSourceCacheHitBytes = 0u;
	uint64_t m_nSourceInFlightJoins = 0u;
	uint64_t m_nSourceCacheResidentBytes = 0u;
	uint64_t m_nSourceCacheEvictions = 0u;
	uint64_t m_nSourceMetadataHits = 0u;
	uint64_t m_nSourceMetadataReads = 0u;
	uint64_t m_nSourcePackHits = 0u;
	uint64_t m_nSourcePackOpens = 0u;
	uint64_t m_nSourceReadChunks = 0u;
	uint64_t m_nSourceReadWaits = 0u;
	uint64_t m_nSourceReadPromotions = 0u;
	int		 m_nPreparedPages = 0;
	int		 m_nPreparedLoadedPages = 0;
	int		 m_nPendingPages = 0;
	int		 m_nQueuedPages = 0;
	int		 m_nPrepareWorkers = 0;
	bool	 m_bPrepareJobActive = false;
	uint64_t m_nPrepareLatencyUsecLast = 0u;
	uint64_t m_nPrepareQueueWaitUsecLast = 0u;
	uint64_t m_nPrepareFileOpenUsecLast = 0u;
	uint64_t m_nPrepareFileReadUsecLast = 0u;
	uint64_t m_nPrepareValidationUsecLast = 0u;
	uint64_t m_nPrepareMaterializeUsecLast = 0u;
	uint64_t m_nPrepareWorkerUsecLast = 0u;
	uint64_t m_nPrepareReadyWaitUsecLast = 0u;
	uint64_t m_nPreparedByteBudget = 0u;
	uint64_t m_nPreparedReservedBytes = 0u;
	uint64_t m_nPayloadBytesPerSlot = 0u;
	uint64_t m_nWorkerScratchReservedBytes = 0u;
	int		 m_nConfiguredPrepareWorkers = 0;
	int		 m_nEffectivePrepareWorkers = 0;
	bool	 m_bCapacityFailureLatched = false;
} t3dRuntimePageStats_t;

typedef struct t3dRuntimePageMeta_s
{
	Vector2i m_location = Vector2i( INT32_MAX, INT32_MAX );
	int16_t	 m_iSourceIndex = -1;
	int16_t	 m_iPayloadSlot = -1;
	uint32_t m_nExpectedChannelFlags = 0u;
	uint64_t m_nExpectedSourceSignature = 0u;
	int		 m_nColorDivisor = 1;
	int		 m_nDerivedDivisor = 1;
	int		 m_nMiddleHeightDivisor = 1;
	uint32_t m_nMaterialMask = 1u;
	uint16_t m_nConsumerMask = 0u;
	uint16_t m_nAttemptedConsumerMask = 0u;
	uint16_t m_nMaterializedConsumerMask = 0u;
	uint16_t m_nLoadedConsumerMask = 0u;
	uint64_t m_nRequestedUsec = 0u;
	uint64_t m_nWorkerStartedUsec = 0u;
	uint64_t m_nFileOpenUsec = 0u;
	uint64_t m_nFileReadUsec = 0u;
	uint64_t m_nValidationUsec = 0u;
	uint64_t m_nMaterializeUsec = 0u;
	uint64_t m_nWorkerFinishedUsec = 0u;
	bool	 m_bAttempted = false;
	bool	 m_bLoaded = false;
	bool	 m_bInstanceAttempted = false;
	bool	 m_bInstanceLoaded = false;
	bool	 m_bLoadMinMax = false;
	bool	 m_bEditorAuthoring = false;
} t3dRuntimePageMeta_t;

typedef struct t3dRuntimeProfileSample_s
{
	uint64_t m_nFrameSequence = 0u;
	uint64_t m_nPhysicsTotalUsec = 0u;
	uint64_t m_nPreHotUsec = 0u;
	uint64_t m_nDataResidencyUsec = 0u;
	uint64_t m_nHlodStreamingUsec = 0u;
	uint64_t m_nNodeStreamingUsec = 0u;
	uint64_t m_nClipmapUsec = 0u;
	uint64_t m_nOceanUsec = 0u;
	uint64_t m_nInstancerUsec = 0u;
	uint64_t m_nLightSyncUsec = 0u;
	uint64_t m_nCollisionUsec = 0u;
	uint64_t m_nStreamingUsec = 0u;
	float	 m_fTargetX = 0.0f;
	float	 m_fTargetZ = 0.0f;
	float	 m_fMovementX = 0.0f;
	float	 m_fMovementZ = 0.0f;
	int		 m_nTargetRegionX = 0;
	int		 m_nTargetRegionZ = 0;
	uint32_t m_nEventFlags = TERRAIN3D_PROFILE_EVENT_NONE;
	uint32_t m_nStallFlags = TERRAIN3D_PROFILE_STALL_NONE;
	int		 m_nGpuResident = 0;
	int		 m_nGpuCapacity = 0;
	int		 m_nGpuUploads = 0;
	int		 m_nGpuEvictions = 0;
	int		 m_nGpuMissingRequired = 0;
	uint64_t m_nGpuUploadBytes = 0u;
	int		 m_nGpuPreparedPages = 0;
	int		 m_nGpuPrepareWorkers = 0;
	int		 m_nGpuEffectiveUploadLimit = 0;
	int		 m_nGpuCameraMoved = 0;
	int		 m_nGpuRequestedPages = 0;
	int		 m_nGpuRequiredPages = 0;
	int		 m_nGpuGuardPages = 0;
	int		 m_nGpuPrefetchPages = 0;
	int		 m_nGpuHysteresisPages = 0;
	int		 m_nGpuPreparedQueue = 0;
	int		 m_nGpuPreparePendingPages = 0;
	int		 m_nGpuPrepareJobActive = 0;
	int		 m_nGpuCookedHits = 0;
	int		 m_nGpuCookedMisses = 0;
	int		 m_nGpuRuntimePrepares = 0;
	int		 m_nGpuFarDirtyRegions = 0;
	int		 m_nGpuFarRegionsProcessed = 0;
	uint64_t m_nGpuPrepareLatencyUsec = 0u;
	uint64_t m_nGpuPrepareQueueWaitUsec = 0u;
	uint64_t m_nGpuPrepareFileOpenUsec = 0u;
	uint64_t m_nGpuPrepareFileReadUsec = 0u;
	uint64_t m_nGpuPrepareValidationUsec = 0u;
	uint64_t m_nGpuPrepareMaterializeUsec = 0u;
	uint64_t m_nGpuPrepareWorkerUsec = 0u;
	uint64_t m_nGpuPrepareReadyWaitUsec = 0u;
	uint64_t m_nGpuPreparePublicationUsec = 0u;
	uint64_t m_nGpuPrepareEndToEndUsec = 0u;
	uint64_t m_nGpuCacheServiceUsec = 0u;
	int		 m_nClipmapNearUpdates = 0;
	int		 m_nClipmapMiddleUpdates = 0;
	int		 m_nClipmapFarUpdates = 0;
	int		 m_nClipmapTeleports = 0;
	int		 m_nClipmapBufferUploads = 0;
	int		 m_nCpuResident = 0;
	int		 m_nCpuLoads = 0;
	int		 m_nCpuEvictions = 0;
	uint64_t m_nCpuLoadBytes = 0u;
	uint64_t m_nCpuCacheServiceUsec = 0u;
	int		 m_nCpuDesired = 0;
	int		 m_nCpuSelectionPending = 0;
	int		 m_nCpuPinned = 0;
	int		 m_nInstanceResident = 0;
	int		 m_nInstanceLoads = 0;
	int		 m_nInstanceEvictions = 0;
	uint64_t m_nInstanceLoadBytes = 0u;
	uint64_t m_nInstanceCacheServiceUsec = 0u;
	int		 m_nInstanceDesired = 0;
	int		 m_nInstanceSelectionPending = 0;
	int		 m_nRenderCells = 0;
	int		 m_nPendingCellUpdates = 0;
	int		 m_nNodeActiveRegions = 0;
	int		 m_nNodeActiveNodes = 0;
	int		 m_nNodeLoads = 0;
	int		 m_nNodeUnloads = 0;
	int		 m_nNodeLoadFailures = 0;
	uint64_t m_nLightmapStreamingUsec = 0u;
} t3dRuntimeProfileSample_t;

// A region coordinate has the same ABI in GPU cache and streaming policy.
typedef t3dGpuCacheCoord_t t3dStreamingCoord_t;

typedef struct t3dRegionStreamingConfig_s
{
	int	   m_nActiveRadius = 1;
	int	   m_nHotRadius = 8;
	int	   m_nColdRadius = 12;
	int	   m_nUnloadGuard = 2;
	bool   m_bPredictionEnabled = true;
	double m_fPredictionLookaheadSeconds = 0.75;
	int	   m_nPredictionMaximumRegions = 4;
} t3dRegionStreamingConfig_t;

typedef struct t3dRuntimeStreamingConfig_s
{
	int	   m_nActiveRadius = 1;
	int	   m_nHotRadius = 8;
	int	   m_nColdRadius = 12;
	int	   m_nUnloadRadius = 14;
	int	   m_nUnloadGuard = 2;
	int	   m_nPredictionMaximumRegions = 4;
	double m_fPredictionLookaheadSeconds = 0.75;
	bool   m_bPredictionEnabled = true;
} t3dRuntimeStreamingConfig_t;

typedef struct t3dBakeRowTask_s
{
	int32_t m_nXStart = 0;
	int32_t m_nXEnd = 0;
	int32_t m_nZStart = 0;
	int32_t m_nZEnd = 0;
	size_t	m_iOutputOffset = 0u;
	size_t	m_nOutputCapacity = 0u;
} t3dBakeRowTask_t;

typedef struct t3dVTEditorTimedEncoderV1_s
{
	t3dVTEncoderInterface_t m_Base;
	uint64_t				m_nEncodeUsec;
} t3dVTEditorTimedEncoderV1_t;

typedef struct t3dVTMipChainLayout_s
{
	t3dVTMipLevelLayout_t m_Levels[T3D_VT_MAX_MIPS];
	uint32_t			  m_nMipCount;
	uint32_t			  m_nComponents;
	uint64_t			  m_nTotalBytes;
} t3dVTMipChainLayout_t;

typedef struct t3dWorldSceneBuildBuffers_s
{
	t3dWorldGeometryDatabase_s	*pGeometry;
	t3dRenderingMaterialGpu_t	*pMaterials;
	uint32_t					 nMaterialCapacity;
	uint32_t					 nMaterialCount;
	t3dWorldCell_t				*pCells;
	uint32_t					 nCellCapacity;
	uint32_t					 nCellCount;
	t3dWorldInstance_t			*pInstances;
	t3dWorldTransform_t			*pTransforms;
	uint32_t					 nInstanceCapacity;
	uint32_t					 nInstanceCount;
	uint64_t					*pInstanceSourceIds;
	t3dWorldOwnershipRegistry_t *pOwnership;
	t3dWorldPrototypeEntry_t	*pPrototypeEntries;
	t3dWorldPrototypeBounds_t	*pPrototypeBoundsByMeshId;
	uint32_t					 nPrototypeBoundsCapacity;
	uint32_t					 nPrototypeCapacity;
	uint32_t					 nPrototypeCount;
	t3dWorldMaterialEntry_t		*pMaterialEntries;
	uint32_t					 nMaterialEntryCapacity;
	uint32_t					 nMaterialEntryCount;
	t3dWorldNativeVisualEntry_t *pNativeVisuals;
	uint32_t					 nNativeVisualCapacity;
	uint32_t					 nNativeVisualCount;
	t3dWorldGodotScratch_t		 Scratch;
	t3dWorldCellBuildScratch_t	 CellScratch;
} t3dWorldSceneBuildBuffers_t;

typedef bool ( *t3dWorldPublishConvertedSceneColdFn_t )( void							   *pUser,
														 const t3dWorldSceneBuildBuffers_s &rBuffers,
														 const t3dWorldSceneBuildStats_s   &rStats );

typedef struct t3dWorldAuthoringBackend_s
{
	t3dWorldPublishConvertedSceneColdFn_t pPublishSceneCold = NULL;
	t3dWorldReleaseConvertedSceneColdFn_t pReleaseSceneCold = NULL;
	void								 *pUser = NULL;
	// CPU acceptance is not a GPU-ready acknowledgement. Required for handoff.
	t3dWorldIsConvertedSceneReadyColdFn_t pIsSceneReadyCold = NULL;
} t3dWorldAuthoringBackend_t;

typedef struct t3dRuntimePageHeader_s
{
	uint32_t					m_nMagic = T3D_RUNTIME_PAGE_MAGIC;
	uint32_t					m_nVersion = T3D_RUNTIME_PAGE_VERSION;
	uint32_t					m_nHeaderBytes = T3D_RUNTIME_PAGE_HEADER_BYTES;
	uint32_t					m_nReservedGeometry = 0u;
	uint32_t					m_nColorDivisor = 1u;
	uint32_t					m_nDerivedDivisor = 1u;
	uint32_t					m_nChannelFlags = 0u;
	uint32_t					m_nTotalBytes = T3D_RUNTIME_PAGE_HEADER_BYTES;
	uint32_t					m_nChannelCount = 0u;
	uint32_t					m_iChannelTableOffset = 40u;
	t3dRuntimePageChannelInfo_t m_szChannels[T3D_RUNTIME_PAGE_CHANNEL_COUNT];
	uint32_t					m_nSourceSignatureLo = 0u;
	uint32_t					m_nSourceSignatureHi = 0u;
	uint32_t					m_nBlendRecipeVersion = 0u;
	uint32_t					m_nMaterialMask = 1u;
	uint32_t					m_nMiddleHeightDivisor = 1u;
	uint32_t					m_nWorldSignatureLo = 0u;
	uint32_t					m_nWorldSignatureHi = 0u;
	int32_t						m_nRegionX = 0;
	int32_t						m_nRegionZ = 0;
	uint32_t					m_nBoundsMinYBits = 0u;
	uint32_t					m_nBoundsMaxYBits = 0u;
} t3dRuntimePageHeader_t;

typedef struct t3dIndexedNodePolicy_s
{
	int				m_nRegion;
	t3dNodePolicy_t m_policy;
} t3dIndexedNodePolicy_t;

typedef struct t3dMemoryBudgetInput_s
{
	uint64_t				  m_nTotalBudgetBytes = 0u;
	uint64_t				  m_nFixedBytes = 0u;
	uint64_t				  m_nAllocatorContainedFixedBytes = 0u;
	uint64_t				  m_nAllocatorReserveBytes = 0u;
	uint64_t				  m_nTransientReserveBytes = 0u;
	t3dMemorySubsystemInput_t m_szSubsystems[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT]{};
} t3dMemoryBudgetInput_t;

typedef struct t3dMemoryBudgetResult_s
{
	uint64_t					   m_nTotalBudgetBytes = 0u;
	uint64_t					   m_nFixedBytes = 0u;
	uint64_t					   m_nAllocatorContainedFixedBytes = 0u;
	uint64_t					   m_nAllocatorReserveBytes = 0u;
	uint64_t					   m_nTransientReserveBytes = 0u;
	uint64_t					   m_nManagedBudgetBytes = 0u;
	uint64_t					   m_nManualReservedBytes = 0u;
	uint64_t					   m_nAutoMinimumBytes = 0u;
	uint64_t					   m_nManagedEffectiveBytes = 0u;
	uint64_t					   m_nCommittedBytes = 0u;
	uint64_t					   m_nHeadroomBytes = 0u;
	bool						   m_bHardOvercommit = false;
	t3dMemorySubsystemAllocation_t m_szSubsystems[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT]{};
} t3dMemoryBudgetResult_t;

typedef struct alignas(
	16 ) t3dRdOceanState_s
{
	uint32_t	 m_nEnabled = 0u;
	uint32_t	 m_nPatchCount = 0u;
	uint32_t	 m_nRenderLayers = 1u;
	uint32_t	 m_nCastShadows = 0u;
	uint32_t	 m_nVertexIterations = 1u;
	uint32_t	 m_nFragmentIterations = 3u;
	uint32_t	 m_nRefractionMode = 1u;
	uint32_t	 m_nFoamMode = 1u;
	uint32_t	 m_nScatteringMode = 1u;
	uint32_t	 m_nShadingMode = 1u;
	uint32_t	 m_nReserved0 = 0u;
	uint32_t	 m_nReserved1 = 0u;
	float		 m_fTimeSeconds = 0.0f;
	float		 m_fMeshSize = 32.0f;
	float		 m_fVertexSpacing = 4.0f;
	float		 m_fSubdiv = 1.0f;
	float		 m_fTargetX = 0.0f;
	float		 m_fTargetY = 0.0f;
	float		 m_fTargetZ = 0.0f;
	float		 m_fSeaLevel = 5.0f;
	float		 m_szWaterColor[4] = { 0.1058f, 0.1568f, 0.2117f, 1.0f };
	float		 m_fVisibleDepth = 256.0f;
	float		 m_fDepthCurve = 0.1f;
	float		 m_fShoreBlend = 1.0f;
	float		 m_fRefractionStrength = 0.02f;
	float		 m_fTimeScale = 8.0f;
	float		 m_fHeightScale = 3.0f;
	float		 m_fOceanScale = 2.0f;
	float		 m_fFresnelScale = 0.5f;
	float		 m_fFresnelPower = 1.0f;
	float		 m_fSpecular = 1.0f;
	float		 m_fFoamHeight = 1.35f;
	float		 m_fFoamTopIntensity = 2.0f;
	float		 m_fFoamDistance = 3072.0f;
	float		 m_fFoamShoreIntensity = 1.3f;
	float		 m_fFoamNoiseScale = 1.5f;
	float		 m_fHeightScattering = 6.0f;
	float		 m_fHeightThreshold = 0.1f;
	float		 m_fHeightAngleThreshold = 0.21f;
	float		 m_fFresnelScattering = 20.0f;
	float		 m_fDirectScattering = 10.0f;
	float		 m_fCullMargin = 0.0f;
	float		 m_fReserved2 = 0.0f;
	uint64_t	 m_nGeneration = 0u;
	t3dRdPatch_t m_szPatches[T3D_MAX_CLIPMAP_LEVELS]{};
} t3dRdOceanState_t;

typedef struct alignas(
	16 ) t3dRdMaterialConfig_s
{
	uint32_t m_nFeatureFlags = 0u;
	uint32_t m_nDebugFlags = 0u;
	uint32_t m_nOutputFlags = T3D_RD_MATERIAL_OUTPUT_ALBEDO | T3D_RD_MATERIAL_OUTPUT_ROUGHNESS |
							  T3D_RD_MATERIAL_OUTPUT_SPECULAR | T3D_RD_MATERIAL_OUTPUT_NORMAL |
							  T3D_RD_MATERIAL_OUTPUT_AO;
	uint32_t				  m_nBackgroundMode = 1u;
	uint32_t				  m_nProjectionMode = 0u;
	uint32_t				  m_nTessellationMaterialMode = 0u;
	uint32_t				  m_nWorldNoiseMode = 0u;
	uint32_t				  m_nTextureLayerCounts = 0u; // 8 bits each: albedo, normal, surface, displacement.
	float					  m_fDisplacementSharpness = 0.5f;
	float					  m_fBlendSharpness = 0.5f;
	float					  m_fDistantNormalScale = 2.0f;
	float					  m_fGroundLevel = 0.0f;
	float					  m_fRegionBlend = 0.25f;
	float					  m_fBiasDistance = 512.0f;
	float					  m_fMipmapBias = 0.0f;
	float					  m_fDepthBlur = 0.0f;
	float					  m_fAutoSlope = 1.0f;
	float					  m_fAutoHeightReduction = 0.1f;
	uint32_t				  m_nAutoBaseTexture = 0u;
	uint32_t				  m_nAutoOverlayTexture = 1u;
	uint32_t				  m_nDualScaleTexture = 0u;
	uint32_t				  m_nFarAlbedoLayerCount = 0u;
	float					  m_fDualScaleReduction = 0.3f;
	float					  m_fTriScaleReduction = 0.3f;
	float					  m_fDualScaleFar = 170.0f;
	float					  m_fDualScaleNear = 100.0f;
	float					  m_fNoise1Scale = 0.04f;
	float					  m_fNoise2Scale = 0.076f;
	float					  m_fNoise1RotationX = 1.0f;
	float					  m_fNoise1RotationY = 0.0f;
	float					  m_fMacroVariation1R = 1.0f;
	float					  m_fMacroVariation1G = 1.0f;
	float					  m_fMacroVariation1B = 1.0f;
	float					  m_fMacroVariationSlope = 0.333f;
	float					  m_fMacroVariation2R = 1.0f;
	float					  m_fMacroVariation2G = 1.0f;
	float					  m_fMacroVariation2B = 1.0f;
	float					  m_fMacroVariationPadding = 0.0f;
	float					  m_fNoise1OffsetX = 0.5f;
	float					  m_fNoise1OffsetY = 0.5f;
	float					  m_fWorldNoiseCachePeriod = 64.0f;
	float					  m_fWorldNoiseLodDistance = 7500.0f;
	float					  m_fWorldNoiseScale = 5.0f;
	float					  m_fWorldNoiseHeight = 32.0f;
	uint32_t				  m_nWorldNoiseMaxOctaves = 4u;
	uint32_t				  m_nWorldNoiseMinOctaves = 2u;
	uint32_t				  m_nWorldNoiseCacheEnabled = 1u;
	uint32_t				  m_nWorldNoiseFragmentNormals = 0u;
	float					  m_fWorldNoiseOffsetX = 0.0f;
	float					  m_fWorldNoiseOffsetY = 0.0f;
	float					  m_fWorldNoiseOffsetZ = 0.0f;
	float					  m_fWorldNoiseCacheMaxLod = 0.0f;
	float					  m_fSilhouetteR = 0.04f;
	float					  m_fSilhouetteG = 0.04f;
	float					  m_fSilhouetteB = 0.04f;
	float					  m_fSilhouetteA = 1.0f;
	float					  m_fContourStep = 1.0f;
	float					  m_fEditorOverlayOpacity = 0.55f;
	float					  m_fReserved2 = 0.0f;
	float					  m_fReserved3 = 0.0f;
	t3dRdTierMaterialConfig_t m_szTier[1]{};
} t3dRdMaterialConfig_t;

typedef struct alignas(
	16 ) t3dRdEditorOverlayConfig_s
{
	t3dRdEditorDecal_t m_szDecals[3]{};
	float			   m_fHeightmapBlackHeight = -100.0f;
	float			   m_fHeightmapWhiteHeight = 300.0f;
	float			   m_fContourInterval = 1.0f;
	float			   m_fContourThickness = 1.0f;
	float			   m_fContourColorR = 0.85f;
	float			   m_fContourColorG = 0.85f;
	float			   m_fContourColorB = 0.19f;
	float			   m_fContourColorA = 1.0f;
	float			   m_fInstancerCellSize = 32.0f;
	float			   m_fPadding0 = 0.0f;
	float			   m_fPadding1 = 0.0f;
	float			   m_fPadding2 = 0.0f;
} t3dRdEditorOverlayConfig_t;

typedef struct t3dRdCpuCullResult_s
{
	uint32_t					  m_szVisibleIndices[T3D_RD_DRAW_GROUP_COUNT * T3D_MAX_CLIPMAP_LEVELS];
	t3dRdIndexedIndirectCommand_t m_szCommands[T3D_RD_DRAW_GROUP_COUNT];
	uint32_t					  m_nVisibleCount;
	uint32_t					  m_nInvalidPatchCount;
} t3dRdCpuCullResult_t;

// Fixed CPU-only scratch records for bounded visible-material prediction.
typedef struct t3dRdInstanceVtCellPrediction_s
{
	uint32_t m_nCell = 0u;
	float	 m_fDistanceSq = 0.0f;
} t3dRdInstanceVtCellPrediction_t;

typedef struct t3dRdInstanceVtMaterialPrediction_s
{
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nCategory = 0u;
	float	 m_szUvMin[2]{};
	float	 m_szUvMax[2]{};
	float	 m_szUvPerPixel[2]{};
	float	 m_fDistanceSq = 0.0f;
} t3dRdInstanceVtMaterialPrediction_t;

typedef struct t3dRdInstanceDrawPacket_s
{
	uint32_t					  m_nSurfaceSlot = UINT32_MAX;
	uint32_t					  m_nDrawSlot = UINT32_MAX;
	uint32_t					  m_nFirstVisible = 0u;
	uint32_t					  m_nVisibleCount = 0u;
	uint32_t					  m_nMaterial = UINT16_MAX;
	uint32_t					  m_nOverlayMaterial = UINT16_MAX;
	uint32_t					  m_nFlags = 0u;
	float						  m_fSortDistanceSq = 0.0f;
	AABB						  m_cullBounds;
	uint32_t					  m_nVisibilityLayers = 1u;
	float						  m_fLodBegin = 0.0f;
	float						  m_fLodEnd = 0.0f;
	float						  m_fFadeMargin = 0.0f;
	float						  m_fReserved0 = 0.0f;
	t3dRdIndexedIndirectCommand_t m_command{};
} t3dRdInstanceDrawPacket_t;

typedef struct t3dTerrainMegaVTIncrementalCookRequest_s
{
	t3dTerrainMegaVTCookRequest_t m_base;
	uint32_t					  m_nDirtyMip0Mask;
	uint32_t					  m_nDirtyPageMask;
	uint32_t					  m_nSourceGeneration;
	uint32_t					  m_nIncrementalFlags;
} t3dTerrainMegaVTIncrementalCookRequest_t;

typedef struct t3dVTCookedSparsePackageV1_s
{
	t3dVTCookAllocator_t			  m_Allocator;
	t3dVTPackAllocationRecordV1_t	 *m_pAllocations;
	t3dVTPackAllocationKeyRecordV1_t *m_pAllocationKeys;
	t3dVTChannelAllocation_t		 *m_pChannelAllocations;
	t3dVTPackPageRecordV1_t			 *m_pPages;
	t3dVTPackPayloadRecordV1_t		 *m_pPayloads;
	t3dVTPackClusterRecordV1_t		 *m_pClusters;
	t3dVTPackTailRecordV1_t			 *m_pTails;
	t3dVTPackQualityViewV1_t		 *m_pQualityViews;
	uint8_t							 *m_pPayloadData;
	t3dVTPackFormatFamilyRecordV1_t	  m_FormatFamilies[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t						  m_nAllocationCount;
	uint32_t						  m_nChannelAllocationCount;
	uint32_t						  m_nPageCount;
	uint32_t						  m_nPayloadCount;
	uint32_t						  m_nClusterCount;
	uint32_t						  m_nTailCount;
	uint32_t						  m_nQualityViewCount;
	uint32_t						  m_nFullySupportedProfileMask;
	uint32_t						  m_nPackageGeneration;
	uint64_t						  m_nPayloadDataBytes;
	t3dVTCookSparseStatsV1_t		  m_Stats;
} t3dVTCookedSparsePackageV1_t;

typedef struct t3dVTPackRuntimeStorage_s
{
	t3dVTFileSource_s			   *pFileSource;
	void						   *pOwnedBlock;
	uint64_t						nOwnedBytes;
	uint8_t						   *pPackageBytes;
	uint64_t						nPackageBytes;
	t3dVTPackViewV1_t				PackViewV1;
	t3dVTDatabaseView_t				Database;
	t3dVTFileHeader_t			   *pRuntimeHeader;
	t3dVTAllocationRecordV1_t	   *pLegacyAllocations;
	t3dVTPageFileRecord_t		   *pLegacyPages;
	t3dVTPayloadRecord_t		   *pLegacyPayloads;
	t3dVTAllocation_t			   *pRuntimeAllocations;
	t3dVTRuntimePayloadRecord_t	   *pRuntimePayloads;
	t3dVTTailMetadata_t			   *pRuntimeTails;
	t3dVTRuntimeTailSourceRecord_t *pRuntimeTailSources;
} t3dVTPackRuntimeStorage_t;

typedef struct t3dVTResidencyState_s
{
	const t3dVTDatabaseView_t *pDatabase;
	t3dVTResidencyBuffers_t	   Buffers;
	t3dVTResidencyStats_t	   Stats;
	t3dVTResidencyPolicy_t	   Policy;
	uint32_t				   nNeutralRootPage[T3D_VT_CHANNEL_COUNT];
	uint32_t				   nClockHand;
	uint32_t				   nRetireHead;
	uint32_t				   nRetireTail;
	uint32_t				   nRetirementDelayFrames;
	uint32_t				   nDatabaseGeneration;
	uint32_t				   nPageTableGeneration;
	uint32_t				   nAllocationCount;
	uint32_t				   nTailCount;
	bool					   bInitialized;
	bool					   bNeutralPublished;
} t3dVTResidencyState_t;

typedef struct t3dWorldGeometryClusterBuilder_s
{
	uint32_t						aSourceVertex[T3D_WORLD_CLUSTER_MAX_VERTICES];
	t3dWorldPackedClusterTriangle_t aTriangles[T3D_WORLD_CLUSTER_MAX_TRIANGLES];
	uint32_t						nVertexCount;
	uint32_t						nTriangleCount;
	uint32_t						nMaterialId;
	uint32_t						nTier;
} t3dWorldGeometryClusterBuilder_t;

typedef struct t3dRdOceanStripSnapshot_s
{
	uint64_t			  m_nGeneration = 0u;
	t3dClipmapStripPart_t m_szStripParts[MESH_TYPE_COUNT][T3D_CLIPMAP_STRIP_PART_CAPACITY]{};
	uint16_t			  m_szStripPartCounts[MESH_TYPE_COUNT]{};
} t3dRdOceanStripSnapshot_t;

typedef struct t3dAdaptiveBudgetLimits_s
{
	t3dAdaptiveBudgetValues_t m_minimum;
	t3dAdaptiveBudgetValues_t m_maximum;
	uint64_t				  m_nTargetUsec = 1000u;
	uint64_t				  m_nHardMaxUsec = 2000u;
	int						  m_nAdjustmentWindowFrames = 4;
} t3dAdaptiveBudgetLimits_t;

typedef struct t3dAdaptiveBudgetState_s
{
	t3dAdaptiveBudgetValues_t m_effective;
	int						  m_nOverBudgetStreak = 0;
	int						  m_nUnderBudgetStreak = 0;
	uint64_t				  m_nAdjustmentCount = 0u;
	t3dAdaptiveBudgetReason_t m_reason = TERRAIN3D_ADAPTIVE_REASON_DISABLED;
	bool					  m_bInitialized = false;
} t3dAdaptiveBudgetState_t;

typedef struct t3dRegionStreamingPrediction_s
{
	double				m_fVelocityXRegionsPerSecond = 0.0;
	double				m_fVelocityYRegionsPerSecond = 0.0;
	t3dStreamingCoord_t m_predictedCenter;
} t3dRegionStreamingPrediction_t;

typedef struct t3dVTCookPreparedChannelV1_s
{
	const uint8_t		 *m_pMipBytes;
	t3dVTMipChainLayout_t m_Layout;
	uint32_t			  m_nTargetExtent;
	uint32_t			  m_nReserved0;
} t3dVTCookPreparedChannelV1_t;

typedef struct t3dVTEditorChannelCacheDiskHeaderV1_s
{
	uint32_t			  m_nMagic;
	uint32_t			  m_nVersion;
	uint32_t			  m_nLogicalChannel;
	uint32_t			  m_nTargetExtent;
	uint64_t			  m_nKey;
	t3dVTMipChainLayout_t m_Layout;
	uint64_t			  m_nPayloadBytes;
	uint32_t			  m_nPayloadChecksum;
	uint32_t			  m_nReserved0;
} t3dVTEditorChannelCacheDiskHeaderV1_t;

typedef struct t3dRdPacketBuildSlot_s
{
	t3dRdInstanceDrawPacket_t m_packet;
	uint32_t				  m_nVisibleCount = 0u;
	uint32_t				  m_nWriteOffset = 0u;
	uint32_t				  m_nCommandIndex = UINT32_MAX;
	bool					  m_bOneSubmitCompatible = false;
} t3dRdPacketBuildSlot_t;

typedef struct t3dRdOceanStripBank_s
{
	uint64_t				  m_nGeneration = 0u;
	t3dRdOceanStripSnapshot_t m_snapshot;
} t3dRdOceanStripBank_t;

typedef struct t3dVTCookSparseSourceV1_s
{
	uint64_t					 m_nStableKey;
	t3dVTCookImageView_t		 m_Channels[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint64_t					 m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_COUNT];
	t3dVTCookPreparedChannelV1_t m_PreparedChannels[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t					 m_nAuthoredChannelMask;
	uint32_t					 m_nOrmSourceLayout;
	uint32_t					 m_nWrapMode;
	uint32_t					 m_nConsumer;
	uint16_t					 m_nAllocationFlags;
	uint8_t						 m_bAlphaScissor;
	uint8_t						 m_nAlphaThreshold;
	uint32_t					 m_nGenerationSeed;
} t3dVTCookSparseSourceV1_t;

#endif // TERRAIN3D_TYPES_H
