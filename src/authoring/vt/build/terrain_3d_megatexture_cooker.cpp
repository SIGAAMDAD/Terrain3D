// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/vt/build/terrain_3d_megatexture_cooker.h"


uint32_t t3d_megatexture_cooker_page_count( uint32_t nWidth, uint32_t nHeight ) {
	if ( nWidth == 0u || nHeight == 0u ) {
		return 0u;
	}

	uint64_t nPageCount = 0u;
	const uint32_t nMipCount = t3d_vt_page_builder_mip_count( nWidth, nHeight );
	for ( uint32_t iMip = 0u; iMip < nMipCount; ++iMip ) {
		nPageCount += ( uint64_t )t3d_vt_page_builder_pages_wide( nWidth ) * t3d_vt_page_builder_pages_high( nHeight );
		if ( nPageCount > UINT32_MAX ) {
			return 0u;
		}
		nWidth = nWidth > 1u ? nWidth >> 1u : 1u;
		nHeight = nHeight > 1u ? nHeight >> 1u : 1u;
	}
	return ( uint32_t )nPageCount;
}

bool t3d_megatexture_cooker_build_page_u8( const uint8_t *pMipPixels,
        const uint32_t nMipWidth,
        const uint32_t nMipHeight,
        const uint32_t nComponents,
        const t3dMegatexturePageAddress_t &rAddress,
        const t3dVTCookWrapMode_t eWrapMode,
        uint8_t *pDestination,
        const uint64_t nDestinationBytes ) {
	return t3d_vt_page_builder_build_tile_u8( pMipPixels,
	        nMipWidth,
	        nMipHeight,
	        nComponents,
	        rAddress.m_nPageX,
	        rAddress.m_nPageY,
	        eWrapMode,
	        pDestination,
	        nDestinationBytes );
}
