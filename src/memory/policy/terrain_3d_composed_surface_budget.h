// CS01 cold-only planning. No residency allocations or render-server calls.
#ifndef TERRAIN3D_COMPOSED_SURFACE_BUDGET_H
#define TERRAIN3D_COMPOSED_SURFACE_BUDGET_H

#include "rendering/rd/vt/core/terrain_3d_composed_surface_contract.h"
#include <stdint.h>

#define T3D_COMP_SURF_ENABLED_BY_DEFAULT 0
#define T3D_COMP_SURF_MAX_SCRATCH_LANES 2u
#define T3D_COMP_SURF_SCRATCH_OUTPUTS 3u
#define T3D_COMP_SURF_PAGE_METADATA_BYTES 192u // 4 x 48B hashed GPU mappings per physical page
#define T3D_COMP_SURF_STAGING_PAGES 2u
#define T3D_COMP_SURF_PENDING_BYTES_PER_PAGE 128u // pending GPU jobs, CPU cache slots and requests

struct t3dComposedSurfaceBudget_t
{
    uint32_t m_nPages = 0u;
    uint32_t m_nScratchLanes = 0u;
    uint64_t m_nGpuBytes = 0u;
    uint64_t m_nCpuBytes = 0u;
    uint64_t m_nTotalBytes = 0u;
};

inline uint32_t t3d_comp_surf_profile_page_cap( const uint32_t nConfiguredBudgetMb, const bool bEditorAuthoring )
{
    if ( bEditorAuthoring )
{
        return 2048u;
    }
    if ( nConfiguredBudgetMb < 256u ) return 0u;
    if ( nConfiguredBudgetMb < 512u ) return 128u;
    if ( nConfiguredBudgetMb < 1024u ) return 256u;
    if ( nConfiguredBudgetMb < 2048u ) return 512u;
    return 1024u;
}

// Caller supplies spare *accounted* bytes after source VT, regions and mandatory
// resources have been protected by policy. No budgets are redistributed.
// Returns zeroed plan on denial; a live allocator must separately reserve
// m_nGpuBytes and m_nCpuBytes in COMPOSED_SURFACE before creating resources.
inline t3dComposedSurfaceBudget_t t3d_comp_surf_plan_budget( const bool bEnabled,
                                                     const uint32_t nConfiguredBudgetMb,
                                                     const bool bEditorAuthoring,
                                                     const uint64_t nSpareAccountedBytes )
{
    t3dComposedSurfaceBudget_t result;
    if ( !bEnabled ) return result;
    const uint32_t page_cap = t3d_comp_surf_profile_page_cap( nConfiguredBudgetMb, bEditorAuthoring );
    const uint64_t scratch_bytes = uint64_t( T3D_COMP_SURF_MAX_SCRATCH_LANES ) * T3D_COMP_SURF_SCRATCH_OUTPUTS *
                                   T3D_COMP_SURF_PHYSICAL_TILE * T3D_COMP_SURF_PHYSICAL_TILE * 4u;
    const uint64_t staging_bytes = uint64_t( T3D_COMP_SURF_STAGING_PAGES ) * T3D_COMP_SURF_PAGE_PAYLOAD_BYTES;
    // Metadata covers page-table staging plus the fixed upload bookkeeping.
    const uint64_t cpu_fixed = scratch_bytes + staging_bytes + uint64_t( page_cap ) * ( T3D_COMP_SURF_PAGE_METADATA_BYTES + T3D_COMP_SURF_PENDING_BYTES_PER_PAGE );
    const uint64_t gpu_per_page = T3D_COMP_SURF_PAGE_PAYLOAD_BYTES + T3D_COMP_SURF_PAGE_METADATA_BYTES;
    if ( page_cap == 0u || nSpareAccountedBytes < cpu_fixed + gpu_per_page * 32u ) return result;
    uint32_t pages = page_cap;
    while ( pages >= 32u && cpu_fixed + gpu_per_page * pages > nSpareAccountedBytes ) {
        pages /= 2u;
    }
    if ( pages < 32u ) return result;
    result.m_nPages = pages;
    result.m_nScratchLanes = T3D_COMP_SURF_MAX_SCRATCH_LANES;
    result.m_nGpuBytes = gpu_per_page * pages;
    result.m_nCpuBytes = cpu_fixed;
    result.m_nTotalBytes = result.m_nGpuBytes + result.m_nCpuBytes;
    return result;
}
// Future CS05 resource owner uses these transaction helpers when the feature
// is enabled. Two kind reservations share the one composed-surface domain.
#include "memory/policy/terrain_3d_memory_account.h"
inline bool t3d_comp_surf_reserve_budget( Terrain3DMemoryAccount &account,
                                  const t3dComposedSurfaceBudget_t &budget )
{
    if ( budget.m_nPages == 0u || budget.m_nTotalBytes != budget.m_nGpuBytes + budget.m_nCpuBytes ||
         !account.reserve( budget.m_nGpuBytes, Terrain3DMemoryAccount::GPU,
                           Terrain3DMemoryAccount::COMPOSED_SURFACE ) ) return false;
    if ( !account.reserve( budget.m_nCpuBytes, Terrain3DMemoryAccount::CPU,
                           Terrain3DMemoryAccount::COMPOSED_SURFACE ) ) {
        account.release( budget.m_nGpuBytes, Terrain3DMemoryAccount::GPU,
                         Terrain3DMemoryAccount::COMPOSED_SURFACE );
        return false;
    }
    return true;
}
inline void t3d_comp_surf_release_budget( Terrain3DMemoryAccount &account,
                                   const t3dComposedSurfaceBudget_t &budget )
{
    account.release( budget.m_nCpuBytes, Terrain3DMemoryAccount::CPU,
                     Terrain3DMemoryAccount::COMPOSED_SURFACE );
    account.release( budget.m_nGpuBytes, Terrain3DMemoryAccount::GPU,
                     Terrain3DMemoryAccount::COMPOSED_SURFACE );
}

#endif // TERRAIN3D_COMPOSED_SURFACE_BUDGET_H
