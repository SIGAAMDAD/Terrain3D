// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_PAGE_BUILDER_H
#define TERRAIN3D_VT_PAGE_BUILDER_H

#include "rendering/rd/vt/terrain_3d_vt_contract.h"

#include <stdint.h>

typedef enum t3dVTCookWrapMode_e : uint32_t {
	T3D_VT_COOK_WRAP_REPEAT = 0u,
	T3D_VT_COOK_WRAP_CLAMP = 1u,
} t3dVTCookWrapMode_t;

typedef enum t3dVTSurfaceSourceLayout_e : uint32_t {
	T3D_VT_SURFACE_SOURCE_ORM = 0u,
	T3D_VT_SURFACE_SOURCE_ROUGHNESS_R = 1u,
	T3D_VT_SURFACE_SOURCE_ROUGHNESS_G = 2u,
	T3D_VT_SURFACE_SOURCE_ROUGHNESS_B = 3u,
	T3D_VT_SURFACE_SOURCE_ROUGHNESS_A = 4u,
} t3dVTSurfaceSourceLayout_t;

typedef struct t3dVTCookImageView_s {
	const uint8_t *m_pPixels;
	uint32_t m_nWidth;
	uint32_t m_nHeight;
	uint32_t m_nComponents;
	uint32_t m_nRowBytes;
} t3dVTCookImageView_t;

typedef struct t3dVTMipLevelLayout_s {
	uint32_t m_nWidth;
	uint32_t m_nHeight;
	uint64_t m_nOffset;
	uint64_t m_nBytes;
} t3dVTMipLevelLayout_t;

typedef struct t3dVTMipChainLayout_s {
	t3dVTMipLevelLayout_t m_Levels[T3D_VT_MAX_MIPS];
	uint32_t m_nMipCount;
	uint32_t m_nComponents;
	uint64_t m_nTotalBytes;
} t3dVTMipChainLayout_t;

uint32_t t3d_vt_page_builder_channel_components( uint32_t nChannel );
uint32_t t3d_vt_page_builder_logical_channel_components( t3dVTLogicalChannel_t Channel );
bool t3d_vt_page_builder_fill_logical_neutral(
        t3dVTLogicalChannel_t Channel, uint32_t nWidth, uint32_t nHeight, uint8_t *pDestination, uint64_t nDestinationBytes );
bool t3d_vt_page_builder_normalize_logical_channel_u8( t3dVTLogicalChannel_t Channel,
        uint32_t nOrmSourceLayout,
        const t3dVTCookImageView_t &rSource,
        uint8_t *pDestination,
        uint64_t nDestinationBytes );
bool t3d_vt_page_builder_resample_logical_channel_u8( t3dVTLogicalChannel_t Channel,
        uint32_t nOrmSourceLayout,
        bool bAlphaScissor,
        uint8_t nAlphaThreshold,
        const t3dVTCookImageView_t &rSource,
        uint32_t nTargetWidth,
        uint32_t nTargetHeight,
        uint8_t *pDestination,
        uint64_t nDestinationBytes );
bool t3d_vt_page_builder_build_logical_channel_mips_u8( t3dVTLogicalChannel_t Channel,
        bool bAlphaScissor,
        uint8_t nAlphaThreshold,
        uint8_t *pImageBytes,
        uint64_t nImageBytes,
        const t3dVTMipChainLayout_t &rLayout );
uint32_t t3d_vt_page_builder_mip_count( uint32_t nWidth, uint32_t nHeight );
bool t3d_vt_page_builder_layout_mips( uint32_t nWidth, uint32_t nHeight, uint32_t nComponents, t3dVTMipChainLayout_t &rLayout );
bool t3d_vt_page_builder_fill_neutral(
        uint32_t nChannel, uint32_t nWidth, uint32_t nHeight, uint8_t *pDestination, uint64_t nDestinationBytes );
bool t3d_vt_page_builder_normalize_channel_u8( uint32_t nChannel,
        uint32_t nSurfaceSourceLayout,
        const t3dVTCookImageView_t &rSource,
        uint8_t *pDestination,
        uint64_t nDestinationBytes );
bool t3d_vt_page_builder_resample_u8( const t3dVTCookImageView_t &rSource,
        uint32_t nTargetWidth,
        uint32_t nTargetHeight,
        uint8_t *pDestination,
        uint64_t nDestinationBytes );
bool t3d_vt_page_builder_build_mips_u8( uint8_t *pImageBytes, uint64_t nImageBytes, const t3dVTMipChainLayout_t &rLayout );
bool t3d_vt_page_builder_build_channel_mips_u8(
        uint32_t nChannel, uint32_t nQuality, uint8_t *pImageBytes, uint64_t nImageBytes, const t3dVTMipChainLayout_t &rLayout );
bool t3d_vt_page_builder_build_tile_u8( const uint8_t *pMipPixels,
        uint32_t nMipWidth,
        uint32_t nMipHeight,
        uint32_t nComponents,
        uint32_t nPageX,
        uint32_t nPageY,
        t3dVTCookWrapMode_t eWrapMode,
        uint8_t *pDestination,
        uint64_t nDestinationBytes );
uint32_t t3d_vt_page_builder_pages_wide( uint32_t nMipWidth );
uint32_t t3d_vt_page_builder_pages_high( uint32_t nMipHeight );

#endif // TERRAIN3D_VT_PAGE_BUILDER_H
