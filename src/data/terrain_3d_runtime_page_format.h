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
	TERRAIN3D_RUNTIME_PAGE_HEIGHT = 0u,
	TERRAIN3D_RUNTIME_PAGE_CONTROL = 1u,
	TERRAIN3D_RUNTIME_PAGE_COLOR = 2u,
	TERRAIN3D_RUNTIME_PAGE_DERIVED = 3u,
	TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT = 4u,
} t3druntimePageChannel_t;

typedef struct t3druntimePageChannelInfo_s {
	uint32_t iOffset = 0u;
	uint32_t nSize = 0u;
	uint32_t checksum = 0u;
	uint32_t format = 0u;
	uint32_t nWidth = 0u;
	uint32_t height = 0u;
	uint32_t mipmaps = 0u;
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
	t3druntimePageChannelInfo_t szChannels[TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT];
	uint32_t sourceSignatureLo = 0u;
	uint32_t sourceSignatureHi = 0u;
	uint32_t blendRecipeVersion = 0u;
	uint32_t materialMask = 1u;
	uint32_t middleHeightDivisor = 1u;
} t3druntimePageHeader_t;

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
		rHeader.nTotalBytes > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ||
		( uint64_t ) ( rHeader.nTotalBytes ) != nFileBytes ||
		( rHeader.colorDivisor != 1u && rHeader.colorDivisor != 2u && rHeader.colorDivisor != 4u ) ||
		( rHeader.derivedDivisor != 1u && rHeader.derivedDivisor != 2u &&
			rHeader.derivedDivisor != 4u ) ||
		( rHeader.channelFlags & ~7u ) != 0u ) {
		return false;
	}
	if ( rHeader.blendRecipeVersion == 0u || rHeader.materialMask == 0u ||
		( rHeader.middleHeightDivisor != 1u && rHeader.middleHeightDivisor != 2u &&
			rHeader.middleHeightDivisor != 4u ) ) {
		return false;
	}
	uint32_t previous_end = rHeader.nHeaderBytes;
	for ( uint32_t channel = 0u; channel < TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT; ++channel ) {
		const t3druntimePageChannelInfo_t &info = rHeader.szChannels[channel];
		if ( info.nSize == 0u ) {
			if ( info.iOffset != 0u || info.checksum != 0u || info.format != 0u || info.nWidth != 0u ||
				info.height != 0u || info.mipmaps != 0u ) {
				return false;
			}
			continue;
		}
		if ( info.iOffset < rHeader.nHeaderBytes ||
			( info.iOffset & ( TERRAIN3D_RUNTIME_PAGE_ALIGNMENT - 1u ) ) != 0u ||
			info.iOffset < previous_end || info.nWidth == 0u || info.height == 0u || info.mipmaps > 1u ||
			info.nSize > rHeader.nTotalBytes - info.iOffset ) {
			return false;
		}
		previous_end = info.iOffset + info.nSize;
	}
	return previous_end <= rHeader.nTotalBytes;
}

#endif // TERRAIN3D_RUNTIME_PAGE_FORMAT_H
