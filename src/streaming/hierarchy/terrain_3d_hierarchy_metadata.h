#ifndef TERRAIN3D_HIERARCHY_METADATA_H
#define TERRAIN3D_HIERARCHY_METADATA_H

#include <stdint.h>

// F01 T3RH v2 persistent node ABI. This header is deliberately independent
// of Godot classes so the owner-thread F03 policy can be tested in isolation.
typedef struct t3dRasterHierarchyV2Node_s
{
    uint64_t m_nHeightOffset = 0u;
    uint64_t m_nControlOffset = 0u;
    uint64_t m_nColorOffset = 0u;
    uint64_t m_nHeightHash = 0u;
    uint64_t m_nControlHash = 0u;
    uint64_t m_nColorHash = 0u;
    uint32_t m_nRevision = 0u;
    uint32_t m_nGeneration = 0u;
    uint32_t m_nLeafCount = 0u;
    float m_fMinHeight = 0.0f;
    float m_fMaxHeight = 0.0f;
    float m_fMaxHeightErrorM = 0.0f;
    uint8_t m_nLevel = 0u;
    uint8_t m_nChildMask = 0u;
    uint8_t m_nCoverageMask = 0u;
} t3dRasterHierarchyV2Node_t;

#endif
