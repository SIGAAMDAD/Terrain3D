// BR18: portable, independent cold capability gate oracle.
#include "rendering/rd/forward/terrain_3d_fragment_subgroup_policy.h"
#include <assert.h>
#include <stdio.h>

int main()
{
    t3dFragmentSubgroupSubgroupCapabilities_t caps{};
    assert( !t3dFragmentSubgroupFragmentSubgroupEligible( true, caps ) );
    caps.m_nPropertiesValid = 1u;
    caps.m_nStageMask = T3D_FRAGMENT_SUBGROUP_VK_FRAGMENT_STAGE_BIT;
    caps.m_nOperationMask = T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BASIC_BIT |
                            T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_VOTE_BIT |
                            T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BALLOT_BIT;
    caps.m_nSubgroupSize = 32u;
    assert( t3dFragmentSubgroupFragmentSubgroupEligible( true, caps ) );
    assert( !t3dFragmentSubgroupFragmentSubgroupEligible( false, caps ) );
    caps.m_nStageMask = 0u;
    assert( !t3dFragmentSubgroupFragmentSubgroupEligible( true, caps ) );
    caps.m_nStageMask = T3D_FRAGMENT_SUBGROUP_VK_FRAGMENT_STAGE_BIT;
    caps.m_nOperationMask &= ~T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BALLOT_BIT;
    assert( !t3dFragmentSubgroupFragmentSubgroupEligible( true, caps ) );
    caps.m_nOperationMask |= T3D_FRAGMENT_SUBGROUP_VK_SUBGROUP_BALLOT_BIT;
    caps.m_nSubgroupSize = 0u;
    assert( !t3dFragmentSubgroupFragmentSubgroupEligible( true, caps ) );
    puts( "BR18 cold fragment subgroup policy PASS" );
    return 0;
}
