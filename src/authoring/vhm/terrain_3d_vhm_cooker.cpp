// Copyright 2026 Terrain3D contributors.
#include "authoring/vhm/terrain_3d_vhm_cooker.h"
#include "rendering/rd/vhm/terrain_3d_vhm_contract.h"
#include "rendering/rd/vhm/terrain_3d_st_sidecar_wire.h"

#include <array>
#include <cmath>

struct t3dStHeightPyramid_t
{
    std::array<std::vector<uint16_t>, T3D_ST_VHM_MIP_COUNT> m_mips;
    bool m_bValid = false;
};

// The global signed height interval, never a per-region renormalization.
// This decode/re-encode preserves the canonical R16 quantization grid.
static uint16_t t3dStHeightAverageU16( const uint16_t *pValues )
{
    double total = 0.0;
    for ( uint32_t i = 0u; i < 4u; ++i )
    {
        total += -384.0 + double( pValues[i] ) * ( 1152.0 / 65535.0 );
    }
    const double average = total * 0.25;
    const double encoded = ( average + 384.0 ) * ( 65535.0 / 1152.0 );
    return uint16_t( std::floor( encoded + 0.5 ) );
}

static void t3dStBuildHeightPyramid( const uint8_t *pHeights, t3dStHeightPyramid_t &rOut )
{
    std::vector<uint16_t> &base = rOut.m_mips[0u];
    base.resize( 128u * 128u );
    for ( uint32_t i = 0u; i < 128u * 128u; ++i )
    {
        base[i] = t3dStLoadLe16( pHeights + size_t( i ) * 2u );
    }
    for ( uint32_t mip = 1u; mip < T3D_ST_VHM_MIP_COUNT; ++mip )
    {
        const uint32_t side = T3D_ST_VHM_SOURCE_SIDE >> mip;
        const uint32_t parentSide = side * 2u;
        const std::vector<uint16_t> &previous = rOut.m_mips[mip - 1u];
        std::vector<uint16_t> &next = rOut.m_mips[mip];
        next.resize( side * side );
        for ( uint32_t y = 0u; y < side; ++y )
        {
            for ( uint32_t x = 0u; x < side; ++x )
            {
                const uint32_t pos = ( y * 2u ) * parentSide + x * 2u;
                const uint16_t samples[4] =
                {
                    previous[pos], previous[pos + 1u],
                    previous[pos + parentSide], previous[pos + parentSide + 1u]
                };
                next[y * side + x] = t3dStHeightAverageU16( samples );
            }
        }
    }
    rOut.m_bValid = true;
}

static uint16_t t3dStSamplePyramid( const t3dStHeightPyramid_t (&rNeighbors)[3][3],
                                    const uint32_t nMip, int x, int y )
{
    const int side = int( T3D_ST_VHM_SOURCE_SIDE >> nMip );
    const int dx = x < 0 ? -1 : ( x >= side ? 1 : 0 );
    const int dy = y < 0 ? -1 : ( y >= side ? 1 : 0 );
    const int neighborX = 1 + dx;
    const int neighborY = 1 + dy;
    const bool bNeighborValid = rNeighbors[neighborY][neighborX].m_bValid;
    if ( bNeighborValid )
    {
        x -= dx * side;
        y -= dy * side;
    }
    // The two-pixel gutter exceeds the 1-texel mip7 root. Even with a
    // valid immediate neighbor, clamp excursions beyond its footprint.
    // A second-neighbor lookup is deliberately outside the 3x3 source scope.
    if ( x < 0 ) x = 0;
    if ( y < 0 ) y = 0;
    if ( x >= side ) x = side - 1;
    if ( y >= side ) y = side - 1;
    const t3dStHeightPyramid_t &pyramid = bNeighborValid ?
        rNeighbors[neighborY][neighborX] : rNeighbors[1][1];
    return pyramid.m_mips[nMip][size_t( y ) * side + x];
}

bool Terrain3DVhmCooker::cookRegionCold( const t3dStVhmCookInput_t &rSource,
                                        std::vector<uint8_t> &rOutput )
{
    if ( rSource.m_pHeightLe[1][1] == nullptr ||
         rSource.m_nRegionIndex >= 1024u || rSource.m_nGeneration == 0u ||
         rSource.m_nSourceSignature == 0u || rSource.m_nRecipeSignature == 0u )
    {
        return false;
    }
    t3dStHeightPyramid_t neighbors[3][3];
    for ( uint32_t ny = 0u; ny < 3u; ++ny )
    {
        for ( uint32_t nx = 0u; nx < 3u; ++nx )
        {
            if ( rSource.m_pHeightLe[ny][nx] != nullptr &&
                 rSource.m_nNeighborGeneration[ny][nx] == rSource.m_nGeneration )
            {
                t3dStBuildHeightPyramid( rSource.m_pHeightLe[ny][nx], neighbors[ny][nx] );
            }
        }
    }
    if ( !neighbors[1][1].m_bValid )
    {
        return false;
    }
    std::vector<uint8_t> payload( T3D_ST_VHM_PAYLOAD_BYTES, 0u );
    // Global page ID is stable across edits and independent of allocated GPU
    // fine slots. Physical slots are assigned only after GPU upload.
    for ( uint32_t mip = 0u; mip < T3D_ST_VHM_MIP_COUNT; ++mip )
    {
        const uint32_t sidePages = mip == 0u ? 2u : 1u;
        for ( uint32_t py = 0u; py < sidePages; ++py )
        {
            for ( uint32_t px = 0u; px < sidePages; ++px )
            {
                const uint32_t localPage = t3dStVhmPageIdRegion( mip, px, py );
                const uint32_t nextLocal = mip == 7u ? T3D_ST_VHM_INVALID :
                                           t3dStVhmPageIdRegion( mip + 1u, px >> 1u, py >> 1u );
                const uint32_t parent = nextLocal == T3D_ST_VHM_INVALID ?
                    T3D_ST_VHM_INVALID : rSource.m_nRegionIndex * T3D_ST_VHM_PAGES_PER_REGION + nextLocal;
                const size_t off = size_t( localPage ) * 16u;
                t3dStStoreLe32( payload.data() + off, T3D_ST_VHM_INVALID );
                t3dStStoreLe32( payload.data() + off + 4u, parent );
                t3dStStoreLe32( payload.data() + off + 8u, rSource.m_nGeneration );
                t3dStStoreLe32( payload.data() + off + 12u,
                                mip | T3D_ST_VHM_VALID | ( mip >= 3u ? T3D_ST_VHM_PINNED : 0u ) );
            }
        }
    }
    const uint32_t atlasX = ( rSource.m_nRegionIndex % 32u ) * T3D_ST_VHM_TAIL_SLOT_WIDTH;
    const uint32_t atlasY = ( rSource.m_nRegionIndex / 32u ) * T3D_ST_VHM_TAIL_SLOT_HEIGHT;
    const size_t meta = T3D_ST_VHM_PAGE_TABLE_BYTES;
    t3dStStoreLe32( payload.data() + meta, atlasX );
    t3dStStoreLe32( payload.data() + meta + 4u, atlasY );
    t3dStStoreLe32( payload.data() + meta + 8u, T3D_ST_VHM_TAIL_FIRST_MIP );
    t3dStStoreLe32( payload.data() + meta + 12u, T3D_ST_VHM_MIP_COUNT - T3D_ST_VHM_TAIL_FIRST_MIP );
    t3dStStoreLe32( payload.data() + meta + 16u, rSource.m_nGeneration );
    t3dStStoreLe32( payload.data() + meta + 20u, T3D_ST_VHM_TAIL_VALID );

    const size_t fineBase = T3D_ST_VHM_PAGE_TABLE_BYTES + T3D_ST_VHM_TAIL_META_BYTES;
    for ( uint32_t localPage = 0u; localPage < T3D_ST_VHM_FINE_PAGE_COUNT; ++localPage )
    {
        const uint32_t mip = localPage < 4u ? 0u : ( localPage == 4u ? 1u : 2u );
        const int originX = localPage < 4u ? int( localPage & 1u ) * 64 : 0;
        const int originY = localPage < 4u ? int( localPage >> 1u ) * 64 : 0;
        const int activeMax = int( T3D_ST_VHM_SOURCE_SIDE >> mip ) + 1;
        const size_t dest = fineBase + size_t( localPage ) * T3D_ST_VHM_PAGE_BYTES;
        for ( int y = 0; y < int( T3D_ST_VHM_PAGE_TILE ); ++y )
        {
            for ( int x = 0; x < int( T3D_ST_VHM_PAGE_TILE ); ++x )
            {
                int sourceX = originX + x - int( T3D_ST_VHM_PAGE_GUTTER );
                int sourceY = originY + y - int( T3D_ST_VHM_PAGE_GUTTER );
                // Fine mip2 uses only its upper-left 32x32 interior. Its
                // unused pad texels never read unrelated neighbor regions.
                if ( sourceX > activeMax ) sourceX = activeMax;
                if ( sourceY > activeMax ) sourceY = activeMax;
                const uint16_t h = t3dStSamplePyramid( neighbors, mip, sourceX, sourceY );
                t3dStStoreLe16( payload.data() + dest + size_t( y * int( T3D_ST_VHM_PAGE_TILE ) + x ) * 2u, h );
            }
        }
    }
    const size_t tailBase = fineBase + size_t( T3D_ST_VHM_FINE_PAGE_COUNT ) * T3D_ST_VHM_PAGE_BYTES;
    for ( uint32_t mip = T3D_ST_VHM_TAIL_FIRST_MIP; mip < T3D_ST_VHM_MIP_COUNT; ++mip )
    {
        const int side = int( T3D_ST_VHM_SOURCE_SIDE >> mip );
        const int originX = int( t3dStVhmTailMipX( mip ) );
        for ( int y = 0; y < side + 4; ++y )
        {
            for ( int x = 0; x < side + 4; ++x )
            {
                const uint16_t h = t3dStSamplePyramid( neighbors, mip, x - 2, y - 2 );
                const size_t index = size_t( y * int( T3D_ST_VHM_TAIL_SLOT_WIDTH ) + originX + x );
                t3dStStoreLe16( payload.data() + tailBase + index * 2u, h );
            }
        }
    }
    t3dStSidecarInfo_t info = {};
    info.m_nMagic = T3D_ST_SIDECAR_VHM_MAGIC;
    info.m_nVersion = T3D_ST_SIDECAR_VERSION;
    info.m_nRegionIndex = rSource.m_nRegionIndex;
    info.m_nGeneration = rSource.m_nGeneration;
    info.m_nSourceSignature = rSource.m_nSourceSignature;
    info.m_nRecipeSignature = rSource.m_nRecipeSignature;
    info.m_nEntryCount = T3D_ST_VHM_PAGES_PER_REGION;
    return t3dStWriteSidecar( info, payload, rOutput );
}
