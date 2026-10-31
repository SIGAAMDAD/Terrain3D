// Copyright 2026 Terrain3D contributors.
#include "rendering/rd/superterrain/terrain_3d_st_publication.h"

#include <string.h>
#include <utility>

static uint64_t t3dStMixSignature( uint64_t nHash, uint64_t nValue )
{
    // Ordered deterministic transaction fingerprint, not a cryptographic digest.
    nHash ^= nValue;
    nHash *= UINT64_C( 1099511628211 );
    nHash ^= nValue >> 32u;
    return nHash * UINT64_C( 1099511628211 );
}

void Terrain3DStPublicationImage::swap( Terrain3DStPublicationImage &rOther )
{
    m_regions.swap( rOther.m_regions );
    m_heightPages.swap( rOther.m_heightPages );
    m_heightTails.swap( rOther.m_heightTails );
    m_heightAtlas.swap( rOther.m_heightAtlas );
    m_maskPages.swap( rOther.m_maskPages );
    m_maskWords.swap( rOther.m_maskWords );
    m_finePages.swap( rOther.m_finePages );
    std::swap( m_nFinePageCount, rOther.m_nFinePageCount );
    std::swap( m_nTransactionSignature, rOther.m_nTransactionSignature );
    std::swap( m_nPresentRegions, rOther.m_nPresentRegions );
    std::swap( m_bValid, rOther.m_bValid );
}

bool Terrain3DStPublicationImage::prepareCold( const t3dStPublicationRegionInput_t *pInputs,
                                               const uint32_t nRegionCount,
                                               const uint32_t nMaxPinnedMaskWords,
                                               const uint32_t nFinePageBudget )
{
    if ( ( nRegionCount != 0u && pInputs == nullptr ) || nRegionCount > T3D_ST_REGION_COUNT ||
         nMaxPinnedMaskWords == 0u ||
         nMaxPinnedMaskWords > T3D_ST_MASK_MAX_PINNED_BYTES / 4u ||
         nFinePageBudget > 4096u )
    {
        return false;
    }
    Terrain3DStPublicationImage staged;
    staged.m_regions.resize( T3D_ST_REGION_COUNT );
    staged.m_heightPages.resize( T3D_ST_REGION_COUNT * T3D_ST_VHM_PAGES_PER_REGION );
    staged.m_maskPages.resize( T3D_ST_REGION_COUNT * T3D_ST_MASK_PAGES_PER_REGION );
    staged.m_maskWords.reserve( nMaxPinnedMaskWords );
    Terrain3DVhmRootAtlas roots;
    if ( !roots.initializeCold() )
    {
        return false;
    }
    for ( uint32_t i = 0u; i < T3D_ST_REGION_COUNT; ++i )
    {
        t3dStRegionRecord_t &r = staged.m_regions[i];
        r.m_nComposedVTAllocation = T3D_ST_INVALID_ID;
        r.m_nMegaVTAllocation = T3D_ST_INVALID_ID;
        r.m_nVhmRootIndex = T3D_ST_INVALID_ID;
        r.m_nMaskRootIndex = T3D_ST_INVALID_ID;
        for ( uint32_t p = 0u; p < T3D_ST_VHM_PAGES_PER_REGION; ++p )
        {
            staged.m_heightPages[i * T3D_ST_VHM_PAGES_PER_REGION + p].m_nPhysicalSlot = T3D_ST_INVALID_ID;
            staged.m_heightPages[i * T3D_ST_VHM_PAGES_PER_REGION + p].m_nParentPage = T3D_ST_INVALID_ID;
        }
        for ( uint32_t p = 0u; p < T3D_ST_MASK_PAGES_PER_REGION; ++p )
        {
            staged.m_maskPages[i * T3D_ST_MASK_PAGES_PER_REGION + p].m_nPhysicalSlot = T3D_ST_INVALID_ID;
            staged.m_maskPages[i * T3D_ST_MASK_PAGES_PER_REGION + p].m_nPinnedWordOffset = T3D_ST_INVALID_ID;
        }
    }
    uint64_t signature = UINT64_C( 1469598103934665603 );
    for ( uint32_t i = 0u; i < nRegionCount; ++i )
    {
        const t3dStPublicationRegionInput_t &input = pInputs[i];
        if ( input.m_nRegionIndex >= T3D_ST_REGION_COUNT ||
             ( input.m_nPackedPhysicalFlags & 0x80000000u ) == 0u ||
             input.m_nGeneration == 0u || input.m_nComposedAllocation == T3D_ST_INVALID_ID ||
             input.m_nSourceSignature == 0u || input.m_nRecipeSignature == 0u ||
             input.m_pVhmBytes == nullptr || input.m_pMaskBytes == nullptr ||
             staged.m_regions[input.m_nRegionIndex].m_nPublicationGeneration != 0u )
        {
            return false;
        }
        Terrain3DVhmRegionView vhm;
        Terrain3DCutoutRegionView mask;
        if ( !vhm.open( input.m_pVhmBytes, input.m_nVhmBytes, input.m_nRegionIndex,
                        input.m_nGeneration, input.m_nSourceSignature, input.m_nRecipeSignature ) ||
             !mask.open( input.m_pMaskBytes, input.m_nMaskBytes, input.m_nRegionIndex,
                         input.m_nGeneration, input.m_nSourceSignature, input.m_nRecipeSignature ) ||
             !roots.stageRootCold( vhm ) )
        {
            return false;
        }
        t3dStRegionVhmPageEntry_t *pPages = staged.m_heightPages.data() +
            input.m_nRegionIndex * T3D_ST_VHM_PAGES_PER_REGION;
        for ( uint32_t j = 0u; j < T3D_ST_VHM_PAGES_PER_REGION; ++j )
        {
            const t3dStVhmPageEntry_t &page = vhm.getPages()[j];
            // Fine pages are not resident until the owner uploads them. The
            // pinned root alone is guaranteed and already contains its data.
            pPages[j].m_nPhysicalSlot = T3D_ST_INVALID_ID;
            pPages[j].m_nParentPage = page.m_nParentPage;
            pPages[j].m_nGeneration = page.m_nGeneration;
            pPages[j].m_nPackedMipFlags = page.m_nPackedMipFlags & ~T3D_ST_VHM_PAGE_RESIDENT_FLAG;
        }
        t3dStCutoutEntry_t stagedMask[ T3D_ST_MASK_PAGES_PER_REGION ] = {};
        if ( !mask.appendGpuPayload( staged.m_maskWords, stagedMask, nMaxPinnedMaskWords ) )
        {
            return false;
        }
        for ( uint32_t j = 0u; j < T3D_ST_MASK_PAGES_PER_REGION; ++j )
        {
            t3dStMaskPageEntry_t &rMask = staged.m_maskPages[
                input.m_nRegionIndex * T3D_ST_MASK_PAGES_PER_REGION + j ];
            rMask.m_nPhysicalSlot = stagedMask[j].m_nPhysicalSlot;
            rMask.m_nPinnedWordOffset = stagedMask[j].m_nPinnedWordOffset;
            rMask.m_nGeneration = stagedMask[j].m_nGeneration;
            rMask.m_nPackedState = stagedMask[j].m_nPackedState;
        }
        t3dStRegionRecord_t &r = staged.m_regions[input.m_nRegionIndex];
        r.m_nPackedPhysicalFlags = input.m_nPackedPhysicalFlags;
        r.m_nMegaVTAllocation = input.m_nMegaVTAllocation;
        r.m_nMegaVTGeneration = input.m_nMegaVTGeneration;
        r.m_nMegaVTFlags = input.m_nMegaVTFlags;
        memcpy( r.m_vMegaVTWorldToUv, input.m_vMegaVTWorldToUv, sizeof( r.m_vMegaVTWorldToUv ) );
        r.m_nComposedVTAllocation = input.m_nComposedAllocation;
        r.m_nComposedVTGeneration = input.m_nGeneration;
        r.m_nVhmRootIndex = input.m_nRegionIndex;
        r.m_nVhmGeneration = input.m_nGeneration;
        r.m_nMaskRootIndex = input.m_nRegionIndex * T3D_ST_MASK_PAGES_PER_REGION;
        r.m_nMaskGeneration = input.m_nGeneration;
        r.m_nPublicationGeneration = input.m_nGeneration;
        r.m_nRegionStateFlags = T3D_ST_REGION_VALID_MASK;
        if ( !t3dStRegionReady( &r ) )
        {
            return false;
        }
        signature = t3dStMixSignature( signature, input.m_nRegionIndex );
        signature = t3dStMixSignature( signature, input.m_nGeneration );
        signature = t3dStMixSignature( signature, input.m_nComposedAllocation );
        signature = t3dStMixSignature( signature, input.m_nSourceSignature );
        signature = t3dStMixSignature( signature, input.m_nRecipeSignature );
    }
    // Coarse-to-fine admission ensures the available pool spans more regions
    // before assigning expensive mip0 quadrants. Resident bits are published
    // only alongside the exact payload uploaded by the ST GPU image owner.
    // Empty/invalid region slots are omitted. All three scans are cold-only.
    if ( nFinePageBudget != 0u )
    {
        staged.m_finePages.reserve( size_t( nFinePageBudget ) * T3D_ST_VHM_PAGE_BYTES );
        const uint32_t admissionOrder[6] = { 5u, 4u, 0u, 1u, 2u, 3u };
        for ( uint32_t priority = 0u; priority < 6u &&
                                     staged.m_nFinePageCount < nFinePageBudget; ++priority )
        {
            const uint32_t localPage = admissionOrder[priority];
            for ( uint32_t i = 0u; i < nRegionCount &&
                                   staged.m_nFinePageCount < nFinePageBudget; ++i )
            {
                const t3dStPublicationRegionInput_t &input = pInputs[i];
                Terrain3DVhmRegionView vhm;
                if ( !vhm.open( input.m_pVhmBytes, input.m_nVhmBytes, input.m_nRegionIndex,
                                input.m_nGeneration, input.m_nSourceSignature,
                                input.m_nRecipeSignature ) )
                {
                    return false;
                }
                const uint8_t *pPage = vhm.getFinePageBytes( localPage );
                if ( pPage == nullptr )
                {
                    return false;
                }
                const uint32_t slot = staged.m_nFinePageCount++;
                staged.m_finePages.insert( staged.m_finePages.end(), pPage,
                                           pPage + T3D_ST_VHM_PAGE_BYTES );
                t3dStRegionVhmPageEntry_t &entry = staged.m_heightPages[
                    input.m_nRegionIndex * T3D_ST_VHM_PAGES_PER_REGION + localPage ];
                if ( entry.m_nGeneration != input.m_nGeneration ||
                     ( entry.m_nPackedMipFlags & T3D_ST_VHM_PAGE_VALID_FLAG ) == 0u ||
                     ( entry.m_nPackedMipFlags & T3D_ST_VHM_PAGE_RESIDENT_FLAG ) != 0u )
                {
                    return false;
                }
                entry.m_nPhysicalSlot = slot;
                entry.m_nPackedMipFlags |= T3D_ST_VHM_PAGE_RESIDENT_FLAG;
                signature = t3dStMixSignature( signature,
                    input.m_nRegionIndex * T3D_ST_VHM_PAGES_PER_REGION + localPage );
            }
        }
    }
    staged.m_heightTails.resize( roots.getTailCount() );
    static_assert( sizeof( t3dStRegionVhmTail_t ) == sizeof( t3dStVhmTail_t ), "ST05 VHM tail ABI" );
    for ( uint32_t i = 0u; i < roots.getTailCount(); ++i )
    {
        const t3dStVhmTail_t &source = roots.getTailEntries()[i];
        t3dStRegionVhmTail_t &dest = staged.m_heightTails[i];
        dest.m_nAtlasX = source.m_nAtlasX;
        dest.m_nAtlasY = source.m_nAtlasY;
        dest.m_nFirstMip = source.m_nFirstMip;
        dest.m_nMipCount = source.m_nMipCount;
        dest.m_nGeneration = source.m_nGeneration;
        dest.m_nFlags = source.m_nFlags;
        dest.m_nReserved0 = source.m_nReserved0;
        dest.m_nReserved1 = source.m_nReserved1;
    }
    staged.m_heightAtlas.assign( roots.getAtlasPixels(), roots.getAtlasPixels() + roots.getAtlasBytes() );
    staged.m_nPresentRegions = nRegionCount;
    staged.m_nTransactionSignature = t3dStMixSignature(
        t3dStMixSignature( signature, staged.m_nFinePageCount ), nRegionCount );
    staged.m_bValid = true;
    swap( staged );
    return true;
}

bool Terrain3DStPublicationOwner::commitCold( Terrain3DStPublicationImage &rCandidate )
{
    if ( !rCandidate.valid() )
    {
        return false;
    }
    if ( m_active.valid() && rCandidate.signature() == m_active.signature() )
    {
        return true;
    }
    m_active.swap( rCandidate );
    ++m_nCommitEpoch;
    if ( m_nCommitEpoch == 0u )
    {
        ++m_nCommitEpoch;
    }
    return true;
}
