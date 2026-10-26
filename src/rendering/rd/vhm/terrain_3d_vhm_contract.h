// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_VHM_CONTRACT_H
#define TERRAIN3D_VHM_CONTRACT_H

#include <stdint.h>
#include <stddef.h>

#define T3D_ST_VHM_SOURCE_SIDE 128u
#define T3D_ST_VHM_MIP_COUNT 8u
#define T3D_ST_VHM_PAGES_PER_REGION 11u
#define T3D_ST_VHM_PAGE_INTERIOR 64u
#define T3D_ST_VHM_PAGE_GUTTER 2u
#define T3D_ST_VHM_PAGE_TILE 68u
#define T3D_ST_VHM_PAGE_BYTES 9248u
#define T3D_ST_VHM_FINE_PAGE_COUNT 6u
#define T3D_ST_VHM_TAIL_FIRST_MIP 3u
#define T3D_ST_VHM_TAIL_SLOT_WIDTH 52u
#define T3D_ST_VHM_TAIL_SLOT_HEIGHT 20u
#define T3D_ST_VHM_TAIL_SLOT_BYTES 2080u
#define T3D_ST_VHM_TAIL_ATLAS_WIDTH 1664u
#define T3D_ST_VHM_TAIL_ATLAS_HEIGHT 640u
#define T3D_ST_VHM_INVALID 0xffffffffu
#define T3D_ST_VHM_VALID 0x00010000u
#define T3D_ST_VHM_RESIDENT 0x00020000u
#define T3D_ST_VHM_PINNED 0x00040000u
#define T3D_ST_VHM_TAIL_VALID 1u
#define T3D_ST_VHM_PAGE_TABLE_BYTES 176u
#define T3D_ST_VHM_TAIL_META_BYTES 32u
#define T3D_ST_VHM_PAYLOAD_BYTES ( T3D_ST_VHM_PAGE_TABLE_BYTES + T3D_ST_VHM_TAIL_META_BYTES + T3D_ST_VHM_FINE_PAGE_COUNT * T3D_ST_VHM_PAGE_BYTES + T3D_ST_VHM_TAIL_SLOT_BYTES )

struct t3dStVhmPageEntry_t
{
    uint32_t m_nPhysicalSlot;
    uint32_t m_nParentPage;
    uint32_t m_nGeneration;
    uint32_t m_nPackedMipFlags;
};
struct t3dStVhmTail_t
{
    uint32_t m_nAtlasX;
    uint32_t m_nAtlasY;
    uint32_t m_nFirstMip;
    uint32_t m_nMipCount;
    uint32_t m_nGeneration;
    uint32_t m_nFlags;
    uint32_t m_nReserved0;
    uint32_t m_nReserved1;
};
static_assert( sizeof( t3dStVhmPageEntry_t ) == 16u, "ST04 frozen page entry" );
static_assert( sizeof( t3dStVhmTail_t ) == 32u, "ST04 frozen tail entry" );
static_assert( T3D_ST_VHM_TAIL_ATLAS_WIDTH * T3D_ST_VHM_TAIL_ATLAS_HEIGHT * 2u == 2129920u,
               "ST04 pinned root atlas footprint" );

static inline uint32_t t3dStVhmPageIdRegion( const uint32_t nMip,
                                             const uint32_t nX, const uint32_t nY )
{
    const uint32_t side = nMip == 0u ? 2u : 1u;
    if ( nMip >= T3D_ST_VHM_MIP_COUNT || nX >= side || nY >= side )
    {
        return T3D_ST_VHM_INVALID;
    }
    return nMip == 0u ? nY * 2u + nX : nMip + 3u;
}
static inline uint32_t t3dStVhmTailMipX( const uint32_t nMip )
{
    static const uint32_t s_offsets[5] = { 0u, 20u, 32u, 40u, 46u };
    return nMip >= 3u && nMip <= 7u ? s_offsets[nMip - 3u] : T3D_ST_VHM_INVALID;
}
#endif // TERRAIN3D_VHM_CONTRACT_H
