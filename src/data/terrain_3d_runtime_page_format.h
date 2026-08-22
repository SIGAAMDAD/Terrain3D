// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RUNTIME_PAGE_FORMAT_H
#define TERRAIN3D_RUNTIME_PAGE_FORMAT_H

#include "data/terrain_3d_runtime_page_constants.h"

#include <cstddef>
#include <cstdint>
#include <limits>

// T3PG, little endian.

typedef enum t3dRuntimePageChannel_e : uint32_t {
	TERRAIN3D_RUNTIME_PAGE_HEIGHT = 1u,
	TERRAIN3D_RUNTIME_PAGE_CONTROL = 2u,
	TERRAIN3D_RUNTIME_PAGE_COLOR = 3u,
	TERRAIN3D_RUNTIME_PAGE_DERIVED = 4u,
	TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT = 5u,
	TERRAIN3D_RUNTIME_PAGE_FAR_HEIGHT = 6u,
	TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT = 7u,
	TERRAIN3D_RUNTIME_PAGE_FAR_CONTROL = 8u,
	TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE = 9u,
	TERRAIN3D_RUNTIME_PAGE_FAR_ALBEDO_REFERENCE = 10u,
	TERRAIN3D_RUNTIME_PAGE_HOLE_MASK = 11u,
	TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT = 12u,
	TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS = 13u,
	TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES = 14u,
	TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS = 15u,
	TERRAIN3D_RUNTIME_PAGE_HLOD = 16u,
	TERRAIN3D_RUNTIME_PAGE_NAVIGATION = 17u,
	TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA = 18u,
	TERRAIN3D_RUNTIME_PAGE_LIGHTING_RECEIVERS = 19u,
	TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START = 20u,
	TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK = 21u,
} t3dRuntimePageChannel_t;

typedef enum t3dRuntimePageCompression_e : uint32_t {
	TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE = 0u,
	TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE = 1u,
} t3dRuntimePageCompression_t;

typedef struct t3dRuntimePageChannelInfo_s {
	uint32_t m_nType = 0u;
	uint32_t m_nFormat = 0u;
	uint32_t m_nCompression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	uint32_t m_iOffset = 0u;
	uint32_t m_nCompressedBytes = 0u;
	uint32_t m_nUncompressedBytes = 0u;
	uint32_t m_nWidth = 0u;
	uint32_t m_nHeight = 0u;
	uint32_t m_nMipCount = 0u;
	uint32_t m_nAlignment = s_nRuntimePageAlignment;
	uint32_t m_nChecksum = 0u;
	uint32_t m_nSignature = 0u;
} t3dRuntimePageChannelInfo_t;

typedef struct t3dRuntimePageHeader_s {
	uint32_t m_nMagic = s_nRuntimePageMagic;
	uint32_t m_nVersion = s_nRuntimePageVersion;
	uint32_t m_nHeaderBytes = s_nRuntimePageHeaderBytes;
	uint32_t m_nRegionSize = 0u;
	uint32_t m_nColorDivisor = 1u;
	uint32_t m_nDerivedDivisor = 1u;
	uint32_t m_nChannelFlags = 0u;
	uint32_t m_nTotalBytes = s_nRuntimePageHeaderBytes;
	uint32_t m_nChannelCount = 0u;
	uint32_t m_iChannelTableOffset = 40u;
	t3dRuntimePageChannelInfo_t m_szChannels[s_nRuntimePageChannelCount];
	uint32_t m_nSourceSignatureLo = 0u;
	uint32_t m_nSourceSignatureHi = 0u;
	uint32_t m_nBlendRecipeVersion = 0u;
	uint32_t m_nMaterialMask = 1u;
	uint32_t m_nMiddleHeightDivisor = 1u;
	uint32_t m_nWorldSignatureLo = 0u;
	uint32_t m_nWorldSignatureHi = 0u;
	int32_t m_nRegionX = 0;
	int32_t m_nRegionZ = 0;
	uint32_t m_nBoundsMinYBits = 0u;
	uint32_t m_nBoundsMaxYBits = 0u;
} t3dRuntimePageHeader_t;

inline const t3dRuntimePageChannelInfo_t *t3dRuntimePageFindChannel( const t3dRuntimePageHeader_t &rHeader,
		const t3dRuntimePageChannel_t channel ) {
	for ( uint32_t index = 0u; index < rHeader.m_nChannelCount; ++index ) {
		if ( rHeader.m_szChannels[index].m_nType == ( uint32_t )( channel ) ) {
			return &rHeader.m_szChannels[index];
		}
	}
	return nullptr;
}

inline uint32_t t3dRuntimePageAlign( const uint32_t nValue ) {
	const uint32_t mask = s_nRuntimePageAlignment - 1u;
	if ( nValue > std::numeric_limits<uint32_t>::max() - mask ) {
		return 0u;
	}
	return ( nValue + mask ) & ~mask;
}

inline uint32_t t3dRuntimePageChecksum( const uint8_t *pData, const size_t nSize ) {
	uint32_t hash = 2166136261u;
	if ( !pData && nSize > 0u ) {
		return 0u;
	}
	for ( size_t index = 0u; index < nSize; ++index ) {
		hash ^= ( uint32_t )( pData[index] );
		hash *= 16777619u;
	}
	return hash;
}

inline bool t3dRuntimePageValidateHeader( const t3dRuntimePageHeader_t &rHeader, const uint64_t nFileBytes ) {
	if ( rHeader.m_nMagic != s_nRuntimePageMagic || rHeader.m_nVersion != s_nRuntimePageVersion ||
			rHeader.m_nHeaderBytes != s_nRuntimePageHeaderBytes || rHeader.m_nRegionSize == 0u ||
			rHeader.m_nColorDivisor == 0u || rHeader.m_nDerivedDivisor == 0u ||
			rHeader.m_nTotalBytes < rHeader.m_nHeaderBytes || rHeader.m_nChannelCount == 0u ||
			rHeader.m_nChannelCount > s_nRuntimePageChannelCount || rHeader.m_iChannelTableOffset != 40u ||
			rHeader.m_nTotalBytes > s_nRuntimePageMaxBytes || ( uint64_t )( rHeader.m_nTotalBytes ) != nFileBytes ||
			( rHeader.m_nColorDivisor != 1u && rHeader.m_nColorDivisor != 2u && rHeader.m_nColorDivisor != 4u ) ||
			( rHeader.m_nDerivedDivisor != 1u && rHeader.m_nDerivedDivisor != 2u && rHeader.m_nDerivedDivisor != 4u ) ||
			( rHeader.m_nChannelFlags & ~7u ) != 0u ) {
		return false;
	}
	if ( rHeader.m_nBlendRecipeVersion == 0u || rHeader.m_nMaterialMask == 0u ||
			( rHeader.m_nWorldSignatureLo == 0u && rHeader.m_nWorldSignatureHi == 0u ) ||
			( rHeader.m_nMiddleHeightDivisor != 1u && rHeader.m_nMiddleHeightDivisor != 2u &&
					rHeader.m_nMiddleHeightDivisor != 4u ) ) {
		return false;
	}
	uint32_t previous_end = rHeader.m_nHeaderBytes;
	for ( uint32_t channel = 0u; channel < rHeader.m_nChannelCount; ++channel ) {
		const t3dRuntimePageChannelInfo_t &info = rHeader.m_szChannels[channel];
		if ( info.m_nType == 0u || info.m_nCompressedBytes == 0u || info.m_nUncompressedBytes == 0u ||
				( info.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE &&
						info.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE ) ||
				( info.m_nCompression == TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE &&
						info.m_nCompressedBytes != info.m_nUncompressedBytes ) ||
				info.m_nAlignment != s_nRuntimePageAlignment ) {
			return false;
		}
		for ( uint32_t previous = 0u; previous < channel; ++previous ) {
			if ( rHeader.m_szChannels[previous].m_nType == info.m_nType ) {
				return false;
			}
		}
		if ( info.m_iOffset < rHeader.m_nHeaderBytes || ( info.m_iOffset & ( info.m_nAlignment - 1u ) ) != 0u ||
				info.m_iOffset < previous_end || info.m_nWidth == 0u || info.m_nHeight == 0u ||
				info.m_nMipCount == 0u || info.m_nCompressedBytes > rHeader.m_nTotalBytes - info.m_iOffset ) {
			return false;
		}
		previous_end = info.m_iOffset + info.m_nCompressedBytes;
	}
	return previous_end <= rHeader.m_nTotalBytes;
}

#endif // TERRAIN3D_RUNTIME_PAGE_FORMAT_H
