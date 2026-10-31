// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_ST_PUBLICATION_H
#define TERRAIN3D_ST_PUBLICATION_H

#include "rendering/rd/superterrain/terrain_3d_st_region_contract.h"
#include "rendering/rd/vhm/terrain_3d_vhm_residency.h"
#include "rendering/rd/vt/cutout/terrain_3d_cutout_reader.h"
#include <stdint.h>
#include <stddef.h>
#include <vector>

// One cold authoring transaction. The sidecars, composed allocation and
// generation must refer to the same committed source revision; no individual
// family can publish early. The caller retains bytes only for the duration of
// prepareCold(), which makes an owned immutable copy of the GPU-facing tables.
typedef struct t3dStPublicationRegionInput_s
{
    uint32_t m_nRegionIndex = T3D_ST_INVALID_ID;
    uint32_t m_nComposedAllocation = T3D_ST_INVALID_ID;
    uint32_t m_nGeneration = 0u;
    uint32_t m_nPackedPhysicalFlags = 0u;
    uint32_t m_nMegaVTAllocation = T3D_ST_INVALID_ID;
    uint32_t m_nMegaVTGeneration = 0u;
    uint32_t m_nMegaVTFlags = 0u;
    float m_vMegaVTWorldToUv[4] = {};
    uint64_t m_nSourceSignature = 0u;
    uint64_t m_nRecipeSignature = 0u;
    const uint8_t *m_pVhmBytes = nullptr;
    size_t m_nVhmBytes = 0u;
    const uint8_t *m_pMaskBytes = nullptr;
    size_t m_nMaskBytes = 0u;
} t3dStPublicationRegionInput_t;

// A single completely built CPU publication, to be uploaded as a unit by the
// render owner. The frame table is never built by modifying an active record.
class Terrain3DStPublicationImage
{
    std::vector<t3dStRegionRecord_t> m_regions;
    std::vector<t3dStRegionVhmPageEntry_t> m_heightPages;
    std::vector<t3dStRegionVhmTail_t> m_heightTails;
    std::vector<uint8_t> m_heightAtlas;
    std::vector<t3dStMaskPageEntry_t> m_maskPages;
    std::vector<uint32_t> m_maskWords;
    // Cold-selected residency snapshot. Each 68x68 R16 page occupies one
    // contiguous slot; immutable after the publication candidate is sealed.
    std::vector<uint8_t> m_finePages;
    uint32_t m_nFinePageCount = 0u;
    uint64_t m_nTransactionSignature = 0u;
    uint32_t m_nPresentRegions = 0u;
    bool m_bValid = false;

public:
    bool prepareCold( const t3dStPublicationRegionInput_t *pInputs,
                      uint32_t nRegionCount, uint32_t nMaxPinnedMaskWords,
                      uint32_t nFinePageBudget = 0u );
    bool valid( void ) const { return m_bValid; }
    uint64_t signature( void ) const { return m_nTransactionSignature; }
    uint32_t regionCount( void ) const { return m_nPresentRegions; }
    const std::vector<t3dStRegionRecord_t> &regions( void ) const { return m_regions; }
    const std::vector<t3dStRegionVhmPageEntry_t> &heightPages( void ) const { return m_heightPages; }
    const std::vector<t3dStRegionVhmTail_t> &heightTails( void ) const { return m_heightTails; }
    const std::vector<uint8_t> &heightAtlas( void ) const { return m_heightAtlas; }
    const std::vector<t3dStMaskPageEntry_t> &maskPages( void ) const { return m_maskPages; }
    const std::vector<uint32_t> &maskWords( void ) const { return m_maskWords; }
    const std::vector<uint8_t> &finePages( void ) const { return m_finePages; }
    uint32_t finePageCount( void ) const { return m_nFinePageCount; }
    void swap( Terrain3DStPublicationImage &rOther );
};

// The owner swaps only a fully prepared candidate. A failure preserves every
// active family from its previous generation, including pinned exact masks.
class Terrain3DStPublicationOwner
{
    Terrain3DStPublicationImage m_active;
    uint64_t m_nCommitEpoch = 0u;
public:
    bool commitCold( Terrain3DStPublicationImage &rCandidate );
    const Terrain3DStPublicationImage &active( void ) const { return m_active; }
    uint64_t commitEpoch( void ) const { return m_nCommitEpoch; }
};

#endif // TERRAIN3D_ST_PUBLICATION_H
