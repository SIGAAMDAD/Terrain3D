// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RUNTIME_PAGE_FORMAT_H
#define TERRAIN3D_RUNTIME_PAGE_FORMAT_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "core/utility/terrain_3d_checked_math.h"

#include <EASTL/numeric_limits.h>
#include <stddef.h>
#include <stdint.h>

// T3PG, little endian.

// t3dRuntimePageChannel_e centralized in Terrain3DTypes.h.

// t3dRuntimePageCompression_e centralized in Terrain3DTypes.h.

// t3dRuntimePageChannelInfo_s centralized in Terrain3DTypes.h.

// t3dRuntimePageHeader_s centralized in Terrain3DTypes.h.

inline const t3dRuntimePageChannelInfo_t *t3dRuntimePageFindChannel(
        const t3dRuntimePageHeader_t &rHeader, const t3dRuntimePageChannel_t channel ) {
	for ( uint32_t index = 0u; index < rHeader.m_nChannelCount; ++index ) {
		if ( rHeader.m_szChannels[index].m_nType == ( uint32_t )( channel ) ) {
			return &rHeader.m_szChannels[index];
		}
	}
	return NULL;
}

inline uint32_t t3dRuntimePageAlign( const uint32_t nValue ) {
	uint32_t aligned = 0u;
	return t3dCheckedAlignUp<uint32_t>( nValue, T3D_RUNTIME_PAGE_ALIGNMENT, aligned ) ? aligned : 0u;
}

inline uint32_t t3dRuntimePageChecksum( const uint8_t *pData, const size_t nSize ) {
	uint32_t hash = 2166136261u;
	if ( !pData && nSize > 0u ) {
		return 0u;
	}
	return t3dHashFnv1a32( pData, nSize, hash );
}

bool t3dRuntimePageValidateHeader( const t3dRuntimePageHeader_t &rHeader, const uint64_t nFileBytes );

#endif // TERRAIN3D_RUNTIME_PAGE_FORMAT_H
