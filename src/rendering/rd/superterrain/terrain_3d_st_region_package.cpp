// Copyright 2026 Terrain3D contributors.
#include "rendering/rd/superterrain/terrain_3d_st_region_package.h"
#include "rendering/rd/vhm/terrain_3d_st_sidecar_wire.h"
#include <string.h>

#define T3D_ST_PACKAGE_MAGIC 0x31535453u
#define T3D_ST_PACKAGE_VERSION 1u
#define T3D_ST_PACKAGE_HEADER_BYTES 32u
#define T3D_ST_PACKAGE_ENTRY_BYTES 52u
#define T3D_ST_PACKAGE_MAX_BYTES ( 128u * 1024u * 1024u )

static bool t3dStRegionPackageInputValid( const t3dStPublicationRegionInput_t &rInput )
{
    if ( rInput.m_nRegionIndex >= 1024u || rInput.m_nGeneration == 0u ||
         rInput.m_nComposedAllocation == T3D_ST_INVALID_ID ||
         ( rInput.m_nPackedPhysicalFlags & 0x80000000u ) == 0u ||
         rInput.m_nSourceSignature == 0u || rInput.m_nRecipeSignature == 0u ||
         rInput.m_pVhmBytes == nullptr || rInput.m_pMaskBytes == nullptr )
    {
        return false;
    }
    Terrain3DVhmRegionView vhm;
    Terrain3DCutoutRegionView mask;
    return vhm.open( rInput.m_pVhmBytes, rInput.m_nVhmBytes,
                     rInput.m_nRegionIndex, rInput.m_nGeneration,
                     rInput.m_nSourceSignature, rInput.m_nRecipeSignature ) &&
           mask.open( rInput.m_pMaskBytes, rInput.m_nMaskBytes,
                      rInput.m_nRegionIndex, rInput.m_nGeneration,
                      rInput.m_nSourceSignature, rInput.m_nRecipeSignature );
}

bool Terrain3DStRegionPackage::buildCold( const t3dStPublicationRegionInput_t *pInputs,
                                          const uint32_t nCount,
                                          const uint32_t nVtPackageGeneration,
                                          std::vector<uint8_t> &rBytes )
{
    if ( pInputs == nullptr || nCount == 0u || nCount > 1024u ||
         nVtPackageGeneration == 0u ) return false;
    bool seen[1024] = {};
    uint64_t total = T3D_ST_PACKAGE_HEADER_BYTES + ( uint64_t ) nCount * T3D_ST_PACKAGE_ENTRY_BYTES;
    for ( uint32_t i = 0u; i < nCount; ++i )
    {
        const t3dStPublicationRegionInput_t &input = pInputs[i];
        if ( !t3dStRegionPackageInputValid( input ) || seen[input.m_nRegionIndex] ) return false;
        seen[input.m_nRegionIndex] = true;
        total += input.m_nVhmBytes + input.m_nMaskBytes;
        if ( total > T3D_ST_PACKAGE_MAX_BYTES ) return false;
    }
    std::vector<uint8_t> result( ( size_t ) total, 0u );
    t3dStStoreLe32( result.data(), T3D_ST_PACKAGE_MAGIC );
    t3dStStoreLe32( result.data() + 4u, T3D_ST_PACKAGE_VERSION );
    t3dStStoreLe32( result.data() + 8u, nCount );
    t3dStStoreLe32( result.data() + 12u, nVtPackageGeneration );
    t3dStStoreLe32( result.data() + 16u, ( uint32_t ) total );
    uint32_t cursor = T3D_ST_PACKAGE_HEADER_BYTES + nCount * T3D_ST_PACKAGE_ENTRY_BYTES;
    for ( uint32_t i = 0u; i < nCount; ++i )
    {
        const t3dStPublicationRegionInput_t &input = pInputs[i];
        uint8_t *const entry = result.data() + T3D_ST_PACKAGE_HEADER_BYTES + i * T3D_ST_PACKAGE_ENTRY_BYTES;
        const uint32_t vhmBytes = ( uint32_t ) input.m_nVhmBytes;
        const uint32_t maskBytes = ( uint32_t ) input.m_nMaskBytes;
        t3dStStoreLe32( entry, input.m_nRegionIndex );
        t3dStStoreLe32( entry + 4u, input.m_nGeneration );
        t3dStStoreLe32( entry + 8u, input.m_nComposedAllocation );
        t3dStStoreLe32( entry + 12u, input.m_nPackedPhysicalFlags );
        t3dStStoreLe64( entry + 16u, input.m_nSourceSignature );
        t3dStStoreLe64( entry + 24u, input.m_nRecipeSignature );
        t3dStStoreLe32( entry + 32u, cursor );
        t3dStStoreLe32( entry + 36u, vhmBytes );
        memcpy( result.data() + cursor, input.m_pVhmBytes, vhmBytes );
        cursor += vhmBytes;
        t3dStStoreLe32( entry + 40u, cursor );
        t3dStStoreLe32( entry + 44u, maskBytes );
        memcpy( result.data() + cursor, input.m_pMaskBytes, maskBytes );
        cursor += maskBytes;
        t3dStStoreLe32( entry + 48u,
            t3dStPayloadCrc32( result.data() + t3dStLoadLe32( entry + 32u ),
                               ( size_t ) vhmBytes + maskBytes ) );
    }
    t3dStStoreLe32( result.data() + 20u,
                    t3dStPayloadCrc32( result.data() + T3D_ST_PACKAGE_HEADER_BYTES,
                                      result.size() - T3D_ST_PACKAGE_HEADER_BYTES ) );
    rBytes.swap( result );
    return true;
}

bool Terrain3DStRegionPackage::openCold( const uint8_t *pData, const size_t nBytes,
                                         const uint32_t nExpectedVtPackageGeneration,
                                         t3dStResolveComposedAllocationFn pResolve, void *pContext )
{
    if ( pResolve == nullptr || pData == nullptr || nBytes < T3D_ST_PACKAGE_HEADER_BYTES ||
         nBytes > T3D_ST_PACKAGE_MAX_BYTES || nExpectedVtPackageGeneration == 0u ||
         t3dStLoadLe32( pData ) != T3D_ST_PACKAGE_MAGIC ||
         t3dStLoadLe32( pData + 4u ) != T3D_ST_PACKAGE_VERSION ||
         t3dStLoadLe32( pData + 12u ) != nExpectedVtPackageGeneration ||
         t3dStLoadLe32( pData + 16u ) != nBytes ) return false;
    const uint32_t count = t3dStLoadLe32( pData + 8u );
    if ( count == 0u || count > 1024u ||
         ( uint64_t ) T3D_ST_PACKAGE_HEADER_BYTES +
             ( uint64_t ) count * T3D_ST_PACKAGE_ENTRY_BYTES > nBytes ||
         t3dStLoadLe32( pData + 20u ) !=
             t3dStPayloadCrc32( pData + T3D_ST_PACKAGE_HEADER_BYTES,
                                nBytes - T3D_ST_PACKAGE_HEADER_BYTES ) ) return false;
    std::vector<uint8_t> staged( pData, pData + nBytes );
    std::vector<t3dStPublicationRegionInput_t> inputs;
    inputs.reserve( count );
    bool seen[1024] = {};
    size_t cursor = T3D_ST_PACKAGE_HEADER_BYTES + ( size_t ) count * T3D_ST_PACKAGE_ENTRY_BYTES;
    for ( uint32_t i = 0u; i < count; ++i )
    {
        const uint8_t *const entry = staged.data() + T3D_ST_PACKAGE_HEADER_BYTES +
                                     i * T3D_ST_PACKAGE_ENTRY_BYTES;
        t3dStPublicationRegionInput_t input = {};
        input.m_nRegionIndex = t3dStLoadLe32( entry );
        input.m_nGeneration = t3dStLoadLe32( entry + 4u );
        input.m_nComposedAllocation = t3dStLoadLe32( entry + 8u );
        input.m_nPackedPhysicalFlags = t3dStLoadLe32( entry + 12u );
        input.m_nSourceSignature = t3dStLoadLe64( entry + 16u );
        input.m_nRecipeSignature = t3dStLoadLe64( entry + 24u );
        const uint32_t vhmOffset = t3dStLoadLe32( entry + 32u );
        const uint32_t vhmBytes = t3dStLoadLe32( entry + 36u );
        const uint32_t maskOffset = t3dStLoadLe32( entry + 40u );
        const uint32_t maskBytes = t3dStLoadLe32( entry + 44u );
        if ( input.m_nRegionIndex >= 1024u || seen[input.m_nRegionIndex] ||
             vhmBytes == 0u || maskBytes == 0u ||
             vhmOffset != cursor || ( uint64_t ) cursor + vhmBytes > nBytes ||
             maskOffset != ( size_t ) vhmOffset + vhmBytes ||
             ( uint64_t ) maskOffset + maskBytes > nBytes ||
             t3dStLoadLe32( entry + 48u ) !=
                 t3dStPayloadCrc32( staged.data() + vhmOffset,
                                    ( size_t ) vhmBytes + maskBytes ) ) return false;
        cursor = ( size_t ) maskOffset + maskBytes;
        input.m_pVhmBytes = staged.data() + vhmOffset;
        input.m_nVhmBytes = vhmBytes;
        input.m_pMaskBytes = staged.data() + maskOffset;
        input.m_nMaskBytes = maskBytes;
        if ( !t3dStRegionPackageInputValid( input ) ) return false;
        uint32_t resolvedAllocation = T3D_ST_INVALID_ID;
        uint32_t resolvedGeneration = 0u;
        if ( !pResolve( pContext, input.m_nRegionIndex, resolvedAllocation, resolvedGeneration ) ||
             resolvedAllocation != input.m_nComposedAllocation ||
             resolvedGeneration != input.m_nGeneration ) return false;
        seen[input.m_nRegionIndex] = true;
        inputs.push_back( input );
    }
    if ( cursor != nBytes ) return false;
    // The inputs borrow m_bytes, never the caller's buffer. This class is
    // immutable after opening; reject a bad replacement without changing state.
    m_bytes.swap( staged );
    m_inputs.swap( inputs );
    m_nVtPackageGeneration = nExpectedVtPackageGeneration;
    return true;
}

bool Terrain3DStRegionPackage::preparePublicationCold(
    Terrain3DStPublicationImage &rCandidate, const uint32_t nPinnedMaskWords,
    const uint32_t nFinePageBudget ) const
{
    if ( m_nVtPackageGeneration == 0u || m_inputs.empty() ) return false;
    return rCandidate.prepareCold( m_inputs.data(), ( uint32_t ) m_inputs.size(),
                                   nPinnedMaskWords, nFinePageBudget );
}
