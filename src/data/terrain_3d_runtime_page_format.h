// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RUNTIME_PAGE_FORMAT_H
#define TERRAIN3D_RUNTIME_PAGE_FORMAT_H

#include "data/terrain_3d_runtime_page_constants.h"

#include <cstddef>
#include <cstdint>
#include <limits>

 // T3PG, little endian.

typedef enum t3druntimePageChannel_e : uint32_t {
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
} t3druntimePageChannel_t;

typedef enum t3druntimePageCompression_e : uint32_t {
	TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE = 0u,
	TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE = 1u,
} t3druntimePageCompression_t;

typedef struct t3druntimePageChannelInfo_s {
	uint32_t type = 0u;
	uint32_t format = 0u;
	uint32_t compression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	uint32_t iOffset = 0u;
	uint32_t compressedBytes = 0u;
	uint32_t uncompressedBytes = 0u;
	uint32_t nWidth = 0u;
	uint32_t height = 0u;
	uint32_t mipCount = 0u;
	uint32_t alignment = TERRAIN3D_RUNTIME_PAGE_ALIGNMENT;
	uint32_t checksum = 0u;
	uint32_t signature = 0u;
} t3druntimePageChannelInfo_t;

typedef struct t3druntimePageHeader_s {
	uint32_t magic = TERRAIN3D_RUNTIME_PAGE_MAGIC;
	uint32_t version = TERRAIN3D_RUNTIME_PAGE_VERSION;
	uint32_t nHeaderBytes = TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES;
	uint32_t nRegionSize = 0u;
	uint32_t colorDivisor = 1u;
	uint32_t derivedDivisor = 1u;
	uint32_t channelFlags = 0u;
	uint32_t nTotalBytes = TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES;
	uint32_t channelCount = 0u;
	uint32_t channelTableOffset = 40u;
	t3druntimePageChannelInfo_t szChannels[TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT];
	uint32_t sourceSignatureLo = 0u;
	uint32_t sourceSignatureHi = 0u;
	uint32_t blendRecipeVersion = 0u;
	uint32_t materialMask = 1u;
	uint32_t middleHeightDivisor = 1u;
	uint32_t worldSignatureLo = 0u;
	uint32_t worldSignatureHi = 0u;
	int32_t regionX = 0;
	int32_t regionZ = 0;
	uint32_t boundsMinYBits = 0u;
	uint32_t boundsMaxYBits = 0u;
} t3druntimePageHeader_t;

inline const t3druntimePageChannelInfo_t *terrain3dRuntimePageFindChannel(
	const t3druntimePageHeader_t &rHeader, const t3druntimePageChannel_t channel
) {
	for ( uint32_t index = 0u; index < rHeader.channelCount; ++index ) {
		if ( rHeader.szChannels[index].type == ( uint32_t ) ( channel ) ) {
			return &rHeader.szChannels[index];
		}
	}
	return nullptr;
}

inline uint32_t terrain3dRuntimePageAlign( const uint32_t nValue ) {
	const uint32_t mask = TERRAIN3D_RUNTIME_PAGE_ALIGNMENT - 1u;
	if ( nValue > std::numeric_limits<uint32_t>::max() - mask ) {
		return 0u;
	}
	return ( nValue + mask ) & ~mask;
}

inline uint32_t terrain3dRuntimePageChecksum( const uint8_t *pData, const size_t nSize ) {
	uint32_t hash = 2166136261u;
	if ( !pData && nSize > 0u ) {
		return 0u;
	}
	for ( size_t index = 0u; index < nSize; ++index ) {
		hash ^= ( uint32_t ) ( pData[index] );
		hash *= 16777619u;
	}
	return hash;
}

inline bool terrain3dRuntimePageValidateHeader(
	const t3druntimePageHeader_t &rHeader,
	const uint64_t nFileBytes
) {
	if ( rHeader.magic != TERRAIN3D_RUNTIME_PAGE_MAGIC ||
		rHeader.version != TERRAIN3D_RUNTIME_PAGE_VERSION ||
		rHeader.nHeaderBytes != TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ||
		rHeader.nRegionSize == 0u || rHeader.colorDivisor == 0u ||
		rHeader.derivedDivisor == 0u || rHeader.nTotalBytes < rHeader.nHeaderBytes ||
		rHeader.channelCount == 0u || rHeader.channelCount > TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT ||
		rHeader.channelTableOffset != 40u ||
		rHeader.nTotalBytes > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ||
		( uint64_t ) ( rHeader.nTotalBytes ) != nFileBytes ||
		( rHeader.colorDivisor != 1u && rHeader.colorDivisor != 2u && rHeader.colorDivisor != 4u ) ||
		( rHeader.derivedDivisor != 1u && rHeader.derivedDivisor != 2u &&
			rHeader.derivedDivisor != 4u ) ||
		( rHeader.channelFlags & ~7u ) != 0u ) {
		return false;
	}
	if ( rHeader.blendRecipeVersion == 0u || rHeader.materialMask == 0u ||
		( rHeader.worldSignatureLo == 0u && rHeader.worldSignatureHi == 0u ) ||
		( rHeader.middleHeightDivisor != 1u && rHeader.middleHeightDivisor != 2u &&
			rHeader.middleHeightDivisor != 4u ) ) {
		return false;
	}
	uint32_t previous_end = rHeader.nHeaderBytes;
	for ( uint32_t channel = 0u; channel < rHeader.channelCount; ++channel ) {
		const t3druntimePageChannelInfo_t &info = rHeader.szChannels[channel];
		if ( info.type == 0u || info.compressedBytes == 0u || info.uncompressedBytes == 0u ||
			( info.compression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE &&
				info.compression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE ) ||
			( info.compression == TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE &&
				info.compressedBytes != info.uncompressedBytes ) || info.alignment != TERRAIN3D_RUNTIME_PAGE_ALIGNMENT ) {
			return false;
		}
		for ( uint32_t previous = 0u; previous < channel; ++previous ) {
			if ( rHeader.szChannels[previous].type == info.type ) {
				return false;
			}
		}
		if ( info.iOffset < rHeader.nHeaderBytes ||
			( info.iOffset & ( info.alignment - 1u ) ) != 0u ||
			info.iOffset < previous_end || info.nWidth == 0u || info.height == 0u || info.mipCount == 0u ||
			info.compressedBytes > rHeader.nTotalBytes - info.iOffset ) {
			return false;
		}
		previous_end = info.iOffset + info.compressedBytes;
	}
	return previous_end <= rHeader.nTotalBytes;
}

#endif // TERRAIN3D_RUNTIME_PAGE_FORMAT_H
