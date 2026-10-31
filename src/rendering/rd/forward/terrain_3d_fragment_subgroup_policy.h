// BR18: optional Vulkan 1.1+ fragment subgroup capability policy.
// The engine-owned Forward+ bridge supplies the actual VkPhysicalDevice
// supportedStages and supportedOperations masks; no vendor/width guesses.
#ifndef TERRAIN3D_FRAGMENT_SUBGROUP_POLICY_H
#define TERRAIN3D_FRAGMENT_SUBGROUP_POLICY_H

#include <stdint.h>

#define T3D_FRAGMENT_SUBGROUP_VK_FRAGMENT_STAGE_BIT 0x00000010u
#define T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BASIC_BIT 0x00000001u
#define T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_VOTE_BIT 0x00000002u
#define T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BALLOT_BIT 0x00000008u

typedef struct t3dFragmentSubgroupSubgroupCapabilities_s
{
    uint32_t m_nStageMask;
    uint32_t m_nOperationMask;
    uint32_t m_nSubgroupSize;
    uint32_t m_nPropertiesValid;
} t3dFragmentSubgroupSubgroupCapabilities_t;

static inline bool t3dFragmentSubgroupFragmentSubgroupEligible(
    const bool bColdRequested,
    const t3dFragmentSubgroupSubgroupCapabilities_t &rCapabilities )
{
    return bColdRequested && rCapabilities.m_nPropertiesValid != 0u &&
           ( rCapabilities.m_nStageMask & T3D_FRAGMENT_SUBGROUP_VK_FRAGMENT_STAGE_BIT ) != 0u &&
           ( rCapabilities.m_nOperationMask &
             ( T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BASIC_BIT | T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_VOTE_BIT |
               T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BALLOT_BIT ) ) ==
               ( T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BASIC_BIT | T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_VOTE_BIT |
               T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BALLOT_BIT ) &&
           rCapabilities.m_nSubgroupSize != 0u;
}

#endif // TERRAIN3D_FRAGMENT_SUBGROUP_POLICY_H
