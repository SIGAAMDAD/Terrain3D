// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_VHM_READER_H
#define TERRAIN3D_VHM_READER_H

#include "rendering/rd/vhm/terrain_3d_vhm_contract.h"
#include <stdint.h>
#include <stddef.h>

// Immutable checked sidecar view. Page-table + pinned tail are one generation;
// caller keeps bytes alive through ST05's atomic bank publication.
class Terrain3DVhmRegionView
{
    const uint8_t *m_pPayload = nullptr;
    t3dStVhmPageEntry_t m_pages[T3D_ST_VHM_PAGES_PER_REGION] = {};
    t3dStVhmTail_t m_tail = {};
    uint32_t m_nGeneration = 0u;
    uint32_t m_nRegionIndex = T3D_ST_VHM_INVALID;
public:
    bool open( const uint8_t *pData, size_t nBytes, uint32_t nRegion,
               uint32_t nGeneration, uint64_t nSourceSignature, uint64_t nRecipeSignature );
    bool valid( void ) const { return m_pPayload != nullptr; }
    uint32_t getRootIndex( void ) const { return m_nRegionIndex; }
    const t3dStVhmPageEntry_t *getPages( void ) const { return m_pages; }
    const t3dStVhmTail_t &getTail( void ) const { return m_tail; }
    // Returns exact stored height bits from mip0, a downsampled mip, or the
    // closest committed ancestor. nFineResidencyMask bits 0..5 describe the
    // optional physical fine-page pool. Tail mips 3..7 are always pinned.
    bool sampleR16( uint32_t nX, uint32_t nY, uint32_t nPreferredMip,
                    uint32_t nFineResidencyMask, uint32_t nGeneration,
                    uint16_t &rHeight, uint32_t &rResolvedMip ) const;
    // Full 68x68 fine-page payload and 52x20 compact root payload can be
    // uploaded without allocations or format conversion in the hot path.
    const uint8_t *getFinePageBytes( uint32_t nLocalPage ) const;
    const uint8_t *getTailSlotBytes( void ) const;
};

#endif // TERRAIN3D_VHM_READER_H
