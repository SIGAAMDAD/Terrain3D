// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_ST_REGION_PACKAGE_H
#define TERRAIN3D_ST_REGION_PACKAGE_H

#include "rendering/rd/superterrain/terrain_3d_st_publication.h"
#include <stdint.h>
#include <stddef.h>
#include <vector>

// One immutable cold-cooked transaction for the mandatory height and exact
// cutout siblings of a composed V1 package. Every entry is validated before
// any caller is allowed to prepare a GPU publication. All offsets are LE.
typedef bool (*t3dStResolveComposedAllocationFn)( void *pContext, uint32_t nRegionIndex,
                                                        uint32_t &rAllocation, uint32_t &rGeneration );

class Terrain3DStRegionPackage
{
    std::vector<uint8_t> m_bytes;
    std::vector<t3dStPublicationRegionInput_t> m_inputs;
    uint32_t m_nVtPackageGeneration = 0u;

public:
    Terrain3DStRegionPackage() = default;
    Terrain3DStRegionPackage( const Terrain3DStRegionPackage & ) = delete;
    Terrain3DStRegionPackage &operator=( const Terrain3DStRegionPackage & ) = delete;
    Terrain3DStRegionPackage( Terrain3DStRegionPackage && ) = delete;
    Terrain3DStRegionPackage &operator=( Terrain3DStRegionPackage && ) = delete;

    static bool buildCold( const t3dStPublicationRegionInput_t *pInputs,
                           uint32_t nCount, uint32_t nVtPackageGeneration,
                           std::vector<uint8_t> &rBytes );
    bool openCold( const uint8_t *pData, size_t nBytes,
                   uint32_t nExpectedVtPackageGeneration,
                   t3dStResolveComposedAllocationFn pResolve, void *pContext );
    bool preparePublicationCold( Terrain3DStPublicationImage &rCandidate,
                                  uint32_t nPinnedMaskWords, uint32_t nFinePageBudget ) const;
    const t3dStPublicationRegionInput_t *inputs( void ) const
    {
        return m_inputs.empty() ? nullptr : m_inputs.data();
    }
    uint32_t regionCount( void ) const { return ( uint32_t ) m_inputs.size(); }
    uint32_t vtPackageGeneration( void ) const { return m_nVtPackageGeneration; }
};

#endif // TERRAIN3D_ST_REGION_PACKAGE_H
