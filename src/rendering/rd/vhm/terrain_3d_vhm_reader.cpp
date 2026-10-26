// Copyright 2026 Terrain3D contributors.
#include "rendering/rd/vhm/terrain_3d_vhm_reader.h"
#include "rendering/rd/vhm/terrain_3d_st_sidecar_wire.h"

bool Terrain3DVhmRegionView::open( const uint8_t *pData, const size_t nBytes,
                                   const uint32_t nRegion, const uint32_t nGeneration,
                                   const uint64_t nSourceSignature, const uint64_t nRecipeSignature )
{
    t3dStSidecarInfo_t info = {};
    if ( !t3dStReadSidecar( pData, nBytes, T3D_ST_SIDECAR_VHM_MAGIC, nRegion,
                            nGeneration, nSourceSignature, nRecipeSignature, info ) ||
         info.m_nEntryCount != T3D_ST_VHM_PAGES_PER_REGION ||
         info.m_nPayloadBytes != T3D_ST_VHM_PAYLOAD_BYTES )
    {
        return false;
    }
    const uint8_t *p = pData + T3D_ST_SIDECAR_HEADER_BYTES;
    t3dStVhmPageEntry_t pages[T3D_ST_VHM_PAGES_PER_REGION] = {};
    for ( uint32_t page = 0u; page < T3D_ST_VHM_PAGES_PER_REGION; ++page )
    {
        const uint8_t *e = p + size_t( page ) * 16u;
        pages[page].m_nPhysicalSlot = t3dStLoadLe32( e );
        pages[page].m_nParentPage = t3dStLoadLe32( e + 4u );
        pages[page].m_nGeneration = t3dStLoadLe32( e + 8u );
        pages[page].m_nPackedMipFlags = t3dStLoadLe32( e + 12u );
        const uint32_t mip = page < 4u ? 0u : page - 3u;
        const uint32_t parent = mip == 7u ? T3D_ST_VHM_INVALID :
            nRegion * T3D_ST_VHM_PAGES_PER_REGION + t3dStVhmPageIdRegion( mip + 1u, 0u, 0u );
        const uint32_t expectedFlags = mip | T3D_ST_VHM_VALID | ( mip >= 3u ? T3D_ST_VHM_PINNED : 0u );
        if ( pages[page].m_nPhysicalSlot != T3D_ST_VHM_INVALID ||
             pages[page].m_nParentPage != parent ||
             pages[page].m_nGeneration != nGeneration ||
             pages[page].m_nPackedMipFlags != expectedFlags )
        {
            return false;
        }
    }
    t3dStVhmTail_t tail = {};
    const uint8_t *t = p + T3D_ST_VHM_PAGE_TABLE_BYTES;
    tail.m_nAtlasX = t3dStLoadLe32( t );
    tail.m_nAtlasY = t3dStLoadLe32( t + 4u );
    tail.m_nFirstMip = t3dStLoadLe32( t + 8u );
    tail.m_nMipCount = t3dStLoadLe32( t + 12u );
    tail.m_nGeneration = t3dStLoadLe32( t + 16u );
    tail.m_nFlags = t3dStLoadLe32( t + 20u );
    tail.m_nReserved0 = t3dStLoadLe32( t + 24u );
    tail.m_nReserved1 = t3dStLoadLe32( t + 28u );
    if ( tail.m_nAtlasX != ( nRegion % 32u ) * T3D_ST_VHM_TAIL_SLOT_WIDTH ||
         tail.m_nAtlasY != ( nRegion / 32u ) * T3D_ST_VHM_TAIL_SLOT_HEIGHT ||
         tail.m_nFirstMip != T3D_ST_VHM_TAIL_FIRST_MIP ||
         tail.m_nMipCount != T3D_ST_VHM_MIP_COUNT - T3D_ST_VHM_TAIL_FIRST_MIP ||
         tail.m_nGeneration != nGeneration || tail.m_nFlags != T3D_ST_VHM_TAIL_VALID ||
         tail.m_nReserved0 != 0u || tail.m_nReserved1 != 0u )
    {
        return false;
    }
    for ( uint32_t i = 0u; i < T3D_ST_VHM_PAGES_PER_REGION; ++i )
    {
        m_pages[i] = pages[i];
    }
    m_tail = tail;
    m_pPayload = p;
    m_nGeneration = nGeneration;
    m_nRegionIndex = nRegion;
    return true;
}

const uint8_t *Terrain3DVhmRegionView::getFinePageBytes( const uint32_t nLocalPage ) const
{
    return m_pPayload != nullptr && nLocalPage < T3D_ST_VHM_FINE_PAGE_COUNT ?
        m_pPayload + T3D_ST_VHM_PAGE_TABLE_BYTES + T3D_ST_VHM_TAIL_META_BYTES +
        size_t( nLocalPage ) * T3D_ST_VHM_PAGE_BYTES : nullptr;
}

const uint8_t *Terrain3DVhmRegionView::getTailSlotBytes( void ) const
{
    return m_pPayload != nullptr ?
        m_pPayload + T3D_ST_VHM_PAGE_TABLE_BYTES + T3D_ST_VHM_TAIL_META_BYTES +
        T3D_ST_VHM_FINE_PAGE_COUNT * T3D_ST_VHM_PAGE_BYTES : nullptr;
}

bool Terrain3DVhmRegionView::sampleR16( const uint32_t nX, const uint32_t nY,
                                        const uint32_t nPreferredMip, const uint32_t nFineResidencyMask,
                                        const uint32_t nGeneration, uint16_t &rHeight,
                                        uint32_t &rResolvedMip ) const
{
    if ( m_pPayload == nullptr || nGeneration == 0u || nGeneration != m_nGeneration ||
         nX >= T3D_ST_VHM_SOURCE_SIDE || nY >= T3D_ST_VHM_SOURCE_SIDE ||
         nPreferredMip >= T3D_ST_VHM_MIP_COUNT )
    {
        return false;
    }
    for ( uint32_t mip = nPreferredMip; mip < T3D_ST_VHM_MIP_COUNT; ++mip )
    {
        const uint32_t logicalX = nX >> mip;
        const uint32_t logicalY = nY >> mip;
        const uint32_t pageX = mip == 0u ? logicalX >> 6u : 0u;
        const uint32_t pageY = mip == 0u ? logicalY >> 6u : 0u;
        const uint32_t localPage = t3dStVhmPageIdRegion( mip, pageX, pageY );
        const t3dStVhmPageEntry_t &entry = m_pages[localPage];
        if ( entry.m_nGeneration != nGeneration || ( entry.m_nPackedMipFlags & T3D_ST_VHM_VALID ) == 0u )
        {
            return false;
        }
        if ( mip < T3D_ST_VHM_TAIL_FIRST_MIP )
        {
            if ( ( nFineResidencyMask & ( 1u << localPage ) ) == 0u )
            {
                continue;
            }
            const uint8_t *pPage = getFinePageBytes( localPage );
            const uint32_t x = ( logicalX & 63u ) + T3D_ST_VHM_PAGE_GUTTER;
            const uint32_t y = ( logicalY & 63u ) + T3D_ST_VHM_PAGE_GUTTER;
            rHeight = t3dStLoadLe16( pPage + ( y * T3D_ST_VHM_PAGE_TILE + x ) * 2u );
        }
        else
        {
            if ( m_tail.m_nGeneration != nGeneration || ( m_tail.m_nFlags & T3D_ST_VHM_TAIL_VALID ) == 0u )
            {
                return false;
            }
            const uint8_t *pTail = getTailSlotBytes();
            const uint32_t x = t3dStVhmTailMipX( mip ) + logicalX + T3D_ST_VHM_PAGE_GUTTER;
            const uint32_t y = logicalY + T3D_ST_VHM_PAGE_GUTTER;
            rHeight = t3dStLoadLe16( pTail + ( y * T3D_ST_VHM_TAIL_SLOT_WIDTH + x ) * 2u );
        }
        rResolvedMip = mip;
        return true;
    }
    return false;
}
