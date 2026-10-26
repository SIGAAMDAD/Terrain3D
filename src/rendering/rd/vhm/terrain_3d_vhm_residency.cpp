// Copyright 2026 Terrain3D contributors.
#include "rendering/rd/vhm/terrain_3d_vhm_residency.h"
#include <string.h>

bool Terrain3DVhmRootAtlas::initializeCold( void )
{
    std::vector<uint8_t> candidate( size_t( T3D_ST_VHM_TAIL_ATLAS_WIDTH ) *
                                    T3D_ST_VHM_TAIL_ATLAS_HEIGHT * 2u, 0u );
    std::vector<t3dStVhmTail_t> tails( 1024u );
    m_pixels.swap( candidate );
    m_tails.swap( tails );
    return true;
}

bool Terrain3DVhmRootAtlas::stageRootCold( const Terrain3DVhmRegionView &rRegion )
{
    if ( !rRegion.valid() || m_pixels.size() != size_t( T3D_ST_VHM_TAIL_ATLAS_WIDTH ) *
                                            T3D_ST_VHM_TAIL_ATLAS_HEIGHT * 2u ||
         m_tails.size() != 1024u || rRegion.getRootIndex() >= 1024u )
    {
        return false;
    }
    const t3dStVhmTail_t &tail = rRegion.getTail();
    if ( tail.m_nGeneration == 0u ||
         tail.m_nAtlasX + T3D_ST_VHM_TAIL_SLOT_WIDTH > T3D_ST_VHM_TAIL_ATLAS_WIDTH ||
         tail.m_nAtlasY + T3D_ST_VHM_TAIL_SLOT_HEIGHT > T3D_ST_VHM_TAIL_ATLAS_HEIGHT )
    {
        return false;
    }
    const uint8_t *pSlot = rRegion.getTailSlotBytes();
    if ( pSlot == nullptr )
    {
        return false;
    }
    for ( uint32_t y = 0u; y < T3D_ST_VHM_TAIL_SLOT_HEIGHT; ++y )
    {
        const size_t atlasOffset = size_t( tail.m_nAtlasY + y ) * T3D_ST_VHM_TAIL_ATLAS_WIDTH * 2u +
                                   size_t( tail.m_nAtlasX ) * 2u;
        memcpy( m_pixels.data() + atlasOffset, pSlot + size_t( y ) * T3D_ST_VHM_TAIL_SLOT_WIDTH * 2u,
                T3D_ST_VHM_TAIL_SLOT_WIDTH * 2u );
    }
    m_tails[rRegion.getRootIndex()] = tail;
    return true;
}

bool Terrain3DVhmFinePagePool::initializeCold( const uint32_t nPhysicalPages )
{
    if ( nPhysicalPages == 0u || nPhysicalPages > 4096u )
    {
        return false;
    }
    std::vector<t3dStVhmPhysicalSlot_t> slots( nPhysicalPages );
    std::vector<uint8_t> payload( size_t( nPhysicalPages ) * T3D_ST_VHM_PAGE_BYTES );
    std::vector<uint32_t> freeSlots( nPhysicalPages );
    std::vector<uint32_t> lookup( 1024u * T3D_ST_VHM_PAGES_PER_REGION, T3D_ST_VHM_INVALID );
    for ( uint32_t i = 0u; i < nPhysicalPages; ++i )
    {
        freeSlots[i] = nPhysicalPages - 1u - i;
    }
    m_slots.swap( slots );
    m_payload.swap( payload );
    m_freeSlots.swap( freeSlots );
    m_lookup.swap( lookup );
    m_nFreeCount = nPhysicalPages;
    m_nNextTicket = 0u;
    return true;
}

bool Terrain3DVhmFinePagePool::preparePageHot( const Terrain3DVhmRegionView &rRegion,
                                               const uint32_t nLocalPage, const uint32_t nGeneration,
                                               uint32_t &rSlot, uint32_t &rTicket )
{
    if ( !rRegion.valid() || nLocalPage >= T3D_ST_VHM_FINE_PAGE_COUNT || nGeneration == 0u ||
         rRegion.getTail().m_nGeneration != nGeneration || m_lookup.empty() )
    {
        return false;
    }
    const uint32_t pageId = rRegion.getRootIndex() * T3D_ST_VHM_PAGES_PER_REGION + nLocalPage;
    const uint32_t existing = m_lookup[pageId];
    if ( existing != T3D_ST_VHM_INVALID )
    {
        const t3dStVhmPhysicalSlot_t &r = m_slots[existing];
        if ( r.m_nGeneration != nGeneration || r.m_nState == 0u )
        {
            return false; // old generation still potentially read by GPU
        }
        rSlot = existing;
        rTicket = r.m_nUploadTicket;
        return true;
    }
    if ( m_nFreeCount == 0u )
    {
        return false; // bounded: caller prioritizes or evicts fence-safe slots
    }
    const uint8_t *pPage = rRegion.getFinePageBytes( nLocalPage );
    if ( pPage == nullptr )
    {
        return false;
    }
    const uint32_t slot = m_freeSlots[--m_nFreeCount];
    if ( ++m_nNextTicket == 0u )
    {
        ++m_nNextTicket; // zero is never an actionable completion ticket
    }
    memcpy( m_payload.data() + size_t( slot ) * T3D_ST_VHM_PAGE_BYTES, pPage,
            T3D_ST_VHM_PAGE_BYTES );
    t3dStVhmPhysicalSlot_t &r = m_slots[slot];
    r.m_nGlobalPage = pageId;
    r.m_nGeneration = nGeneration;
    r.m_nUploadTicket = m_nNextTicket;
    r.m_nState = 1u;
    m_lookup[pageId] = slot;
    rSlot = slot;
    rTicket = r.m_nUploadTicket;
    return true;
}

bool Terrain3DVhmFinePagePool::finishUpload( const uint32_t nSlot, const uint32_t nTicket )
{
    if ( nSlot >= m_slots.size() || nTicket == 0u ||
         m_slots[nSlot].m_nUploadTicket != nTicket || m_slots[nSlot].m_nState != 1u )
    {
        return false;
    }
    m_slots[nSlot].m_nState = 2u;
    return true;
}

bool Terrain3DVhmFinePagePool::retireAfterFence( const uint32_t nSlot, const uint32_t nTicket )
{
    if ( nSlot >= m_slots.size() || nTicket == 0u || m_nFreeCount >= m_freeSlots.size() ||
         m_slots[nSlot].m_nUploadTicket != nTicket || m_slots[nSlot].m_nState == 0u )
    {
        return false;
    }
    const uint32_t pageId = m_slots[nSlot].m_nGlobalPage;
    if ( pageId >= m_lookup.size() || m_lookup[pageId] != nSlot )
    {
        return false;
    }
    m_lookup[pageId] = T3D_ST_VHM_INVALID;
    m_slots[nSlot] = t3dStVhmPhysicalSlot_t();
    m_freeSlots[m_nFreeCount++] = nSlot;
    return true;
}

bool Terrain3DVhmFinePagePool::isResident( const uint32_t nGlobalPage,
                                           const uint32_t nGeneration, uint32_t &rSlot ) const
{
    if ( nGlobalPage >= m_lookup.size() || nGeneration == 0u )
    {
        return false;
    }
    const uint32_t slot = m_lookup[nGlobalPage];
    if ( slot == T3D_ST_VHM_INVALID || slot >= m_slots.size() ||
         m_slots[slot].m_nState != 2u || m_slots[slot].m_nGeneration != nGeneration )
    {
        return false;
    }
    rSlot = slot;
    return true;
}

const uint8_t *Terrain3DVhmFinePagePool::getUploadBytes( const uint32_t nSlot,
                                                         const uint32_t nTicket ) const
{
    if ( nSlot >= m_slots.size() || nTicket == 0u ||
         m_slots[nSlot].m_nUploadTicket != nTicket || m_slots[nSlot].m_nState == 0u )
    {
        return nullptr;
    }
    return m_payload.data() + size_t( nSlot ) * T3D_ST_VHM_PAGE_BYTES;
}

bool Terrain3DVhmFinePagePool::buildPageEntries( const Terrain3DVhmRegionView &rRegion,
                                                  const uint32_t nGeneration,
                                                  t3dStVhmPageEntry_t *pElevenEntries ) const
{
    if ( !rRegion.valid() || pElevenEntries == nullptr || nGeneration == 0u ||
         rRegion.getTail().m_nGeneration != nGeneration )
    {
        return false;
    }
    const t3dStVhmPageEntry_t *pSource = rRegion.getPages();
    const uint32_t regionBase = rRegion.getRootIndex() * T3D_ST_VHM_PAGES_PER_REGION;
    for ( uint32_t i = 0u; i < T3D_ST_VHM_PAGES_PER_REGION; ++i )
    {
        if ( pSource[i].m_nGeneration != nGeneration )
        {
            return false;
        }
    }
    for ( uint32_t i = 0u; i < T3D_ST_VHM_PAGES_PER_REGION; ++i )
    {
        pElevenEntries[i] = pSource[i];
        if ( i < T3D_ST_VHM_FINE_PAGE_COUNT )
        {
            uint32_t slot = T3D_ST_VHM_INVALID;
            if ( isResident( regionBase + i, nGeneration, slot ) )
            {
                pElevenEntries[i].m_nPhysicalSlot = slot;
                pElevenEntries[i].m_nPackedMipFlags |= T3D_ST_VHM_RESIDENT;
            }
        }
    }
    return true;
}
