// Copyright 2026 Terrain3D contributors.
#ifndef TERRAIN3D_VHM_COOKER_H
#define TERRAIN3D_VHM_COOKER_H

#include <stdint.h>
#include <vector>

struct t3dStVhmCookInput_t
{
    // R16_UNORM little-endian, [-384, 768] meters, 128x128 each. [1][1]
    // is the center. Neighbors are optional and used only for 2-texel gutters
    // when their committed generation matches the authoring transaction.
    const uint8_t *m_pHeightLe[3][3] = {};
    uint32_t m_nNeighborGeneration[3][3] = {};
    uint32_t m_nRegionIndex = 0u;
    uint32_t m_nGeneration = 0u;
    uint64_t m_nSourceSignature = 0u;
    uint64_t m_nRecipeSignature = 0u;
};

class Terrain3DVhmCooker
{
public:
    static bool cookRegionCold( const t3dStVhmCookInput_t &rSource, std::vector<uint8_t> &rOutput );
};

#endif // TERRAIN3D_VHM_COOKER_H
