// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MEGATEXTURE_COOKER_H
#define TERRAIN3D_MEGATEXTURE_COOKER_H

#include "Terrain3DTypes.h"

#include "authoring/vt/build/terrain_3d_vt_page_builder.h"

#include <stdint.h>

// t3dMegatexturePageAddress_s centralized in Terrain3DTypes.h.


uint32_t t3d_megatexture_cooker_page_count( uint32_t nWidth, uint32_t nHeight );
bool t3d_megatexture_cooker_build_page_u8( const uint8_t *pMipPixels,
        uint32_t nMipWidth,
        uint32_t nMipHeight,
        uint32_t nComponents,
        const t3dMegatexturePageAddress_t &rAddress,
        t3dVTCookWrapMode_t eWrapMode,
        uint8_t *pDestination,
        uint64_t nDestinationBytes );

#endif // TERRAIN3D_MEGATEXTURE_COOKER_H
