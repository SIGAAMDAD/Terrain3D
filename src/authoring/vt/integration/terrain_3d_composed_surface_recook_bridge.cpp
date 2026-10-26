#include "authoring/vt/integration/terrain_3d_composed_surface_recook_bridge.h"
#include <string.h>

bool t3d_cs_make_recook_flags_cold(
    const t3dComposedSurfaceEditTicket_t &ticket, const uint64_t nRegionKey,
    const uint32_t nMip0Width, const uint32_t nMip0Height,
    const t3dComposedSurfaceCookPage_t *pPages, const uint32_t nPages,
    uint8_t *pFlags, const uint32_t nCapacity )
{
    if ( !pPages || !pFlags || nCapacity < nPages || !nPages || !nRegionKey ||
         !ticket.m_nRevision || !ticket.m_nSourceFingerprint ||
         !nMip0Width || !nMip0Height || nMip0Width > 4u || nMip0Height > 4u ||
         ( nMip0Width & ( nMip0Width - 1u ) ) ||
         ( nMip0Height & ( nMip0Height - 1u ) ) ) return false;
    // Reject partially constructed page lists rather than issuing an
    // incremental cook which silently omits a page affected by a source edit.
    uint32_t expectedMask = ticket.m_nDirtyClosure;
    memset( pFlags, 0, nPages );
    for ( uint32_t i = 0u; i < nPages; ++i ) {
        const t3dComposedSurfaceCookPage_t &page = pPages[i];
        if ( page.m_nRegionKey != nRegionKey || !page.m_nCompositionRevision || !page.m_nGeneration ) return false;
        uint32_t width = nMip0Width, height = nMip0Height, offset = 0u;
        for ( uint32_t mip = 0u; mip < page.m_nMip; ++mip ) {
            offset += width * height;
            width = width > 1u ? width / 2u : 1u;
            height = height > 1u ? height / 2u : 1u;
        }
        if ( offset + width * height > 32u || page.m_nPageX >= width || page.m_nPageY >= height ) return false;
        const uint32_t bit = 1u << ( offset + page.m_nPageY * width + page.m_nPageX );
        if ( expectedMask & bit ) {
            pFlags[i] = 1u;
            expectedMask &= ~bit;
        }
    }
    // Invalidation must cover every affected logical page exactly once.
    return expectedMask == 0u;
}

bool t3d_cs_recook_journal_ticket_cold(
    const Terrain3DComposedSurfaceEditJournal &journal,
    const t3dComposedSurfaceEditTicket_t &ticket,
    const uint64_t nRegionKey, const uint32_t nMip0Width, const uint32_t nMip0Height,
    const t3dComposedSurfaceCookPage_t *pPages, const uint32_t nPages,
    const t3dComposedSurfaceCookSource_t &source,
    const uint8_t *pPreviousFile, const uint64_t nPreviousBytes,
    uint8_t *pDirtyFlags, const uint32_t nDirtyCapacity,
    uint8_t *pScratch, const uint32_t nScratchBytes,
    uint8_t *pOutput, const uint64_t nOutputCapacity, uint64_t &rWrittenBytes )
{
    rWrittenBytes = 0u;
    if ( !source.m_bCanonicalMaterialParityAccepted ||
         source.m_nSourceFingerprint != ticket.m_nSourceFingerprint ||
         !journal.completionCurrentCold( ticket, source.m_nSourceFingerprint,
                                          ticket.m_nTopologyGeneration ) ||
         !t3d_cs_make_recook_flags_cold( ticket, nRegionKey, nMip0Width,
                                         nMip0Height, pPages, nPages, pDirtyFlags,
                                         nDirtyCapacity ) ) return false;
    if ( !t3d_cs_recook_incremental_cold( pPages, nPages, source,
                                          pPreviousFile, nPreviousBytes,
                                          pDirtyFlags, pScratch, nScratchBytes,
                                          pOutput, nOutputCapacity, rWrittenBytes ) ) return false;
    if ( !journal.completionCurrentCold( ticket, source.m_nSourceFingerprint,
                                          ticket.m_nTopologyGeneration ) ) {
        rWrittenBytes = 0u;
        return false;
    }
    return true;
}
