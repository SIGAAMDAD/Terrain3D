#include "rendering/rd/vt/core/terrain_3d_composed_surface_acceptance.h"
#include "memory/policy/terrain_3d_composed_surface_budget.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

int main()
{
    t3dComposedSurfaceRetirementEvidence_t e{};
    assert( !t3d_cs_may_retire_legacy_middle_atlas( e ) );
    e.m_bCanonicalSurfaceParity = true;
    e.m_bEditorDirtyRecookPasses = true;
    e.m_bMiddleCoverageComplete = true;
    e.m_bDepthDnrColorCoherent = true;
    e.m_bLowBudgetFallbackPasses = true;
    e.m_bVulkanPerformanceAcceptance = true;
    assert( !t3d_cs_may_retire_legacy_middle_atlas( e ) );
    e.m_bStableResourcesTenThousandFrames = true;
    assert( t3d_cs_may_retire_legacy_middle_atlas( e ) );

    for ( uint32_t mb : { 256u, 512u, 1024u, 2048u } ) {
        assert( t3d_cs_plan_budget( false, mb, false, UINT64_MAX ).m_nPages == 0u );
        const uint32_t cap = t3d_cs_profile_page_cap( mb, false );
        assert( cap != 0u );
        const auto denied = t3d_cs_plan_budget( true, mb, false, 0u );
        assert( denied.m_nPages == 0u && !denied.m_nTotalBytes );
        const auto admitted = t3d_cs_plan_budget( true, mb, false, UINT64_MAX );
        assert( admitted.m_nPages == cap );
        assert( admitted.m_nTotalBytes == admitted.m_nGpuBytes + admitted.m_nCpuBytes );
        assert( admitted.m_nGpuBytes > 0u && admitted.m_nCpuBytes > 0u );
    }
    puts( "CS08 retirement/budgets: PASS" );
}
