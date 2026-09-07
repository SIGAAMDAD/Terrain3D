// Keep this regression executable independent of the Godot runtime library.
#define TERRAIN3D_CONSTANTS_H
#define s_nGpuCacheMaxPageTableBytes ( 64u * 64u * 3u )
#define s_nGpuCacheMinSpareLayers ( 2 )
#define s_nRegionSize ( 128 )
#define s_nRegionMapSize ( 32 )
#define s_nRegionWorldSizeMeters ( 128 )

#include "memory/terrain_3d_memory_budget.h"
#include "streaming/terrain_3d_gpu_cache_policy.h"

#define T3D_TEST_REGION_SIZE ( 128 )
#define T3D_TEST_REGION_MAP_SIZE ( 32 )
#define T3D_TEST_REGION_SLOTS ( 1024 )
#define T3D_TEST_GPU_RUNTIME_MAX_LAYERS ( 64 )
#define T3D_TEST_GPU_DEFAULT_SPARE_LAYERS ( 12 )

#include <stdio.h>

static int fail( const char *pMessage ) {
	fprintf( stderr, "FAIL: %s\n", pMessage );
	return 1;
}

static t3dMemoryBudgetInput_t baseInput( uint64_t nTotal, uint64_t nFixed, uint64_t nAllocator, uint64_t nTransient ) {
	t3dMemoryBudgetInput_t input;
	input.m_nTotalBudgetBytes = nTotal;
	input.m_nFixedBytes = nFixed;
	input.m_nAllocatorReserveBytes = nAllocator;
	input.m_nTransientReserveBytes = nTransient;
	for ( int i = 0; i < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++i ) {
		input.m_szSubsystems[i].m_bEnabled = false;
	}
	return input;
}

int main( void ) {
	const uint64_t mib = 1024u * 1024u;
	const uint64_t fixed_far_atlas_bytes = 64u * mib + 32u * 1024u;
	const uint64_t fixed_other_bytes = 992u * 1024u;

	// Default-like 128 MiB ownership: far atlas is fixed, never part of the managed GPU grant.
	t3dMemoryBudgetInput_t input = baseInput( 128u * mib, fixed_far_atlas_bytes + fixed_other_bytes, 16u * mib, 16u * mib );
	t3dMemorySubsystemInput_t &gpu = input.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.m_bEnabled = true;
	gpu.m_bAutomatic = true;
	gpu.m_nMinimumBytes = 4u * mib;
	gpu.m_nHardCapBytes = 128u * mib;
	gpu.m_nWeight = 1u;
	t3dMemoryBudgetResult_t result = Terrain3DMemoryBudgetCoordinator::calculate( input );
	const uint64_t managed_gpu_page_budget_bytes = result.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].m_nEffectiveBytes;
	if ( result.m_nCommittedBytes > input.m_nTotalBudgetBytes ) {
		return fail( "default known residency exceeds total budget" );
	}
	if ( managed_gpu_page_budget_bytes + fixed_far_atlas_bytes <= managed_gpu_page_budget_bytes ) {
		return fail( "far-atlas ownership arithmetic overflow" );
	}
	// A constrained exact-byte page grant must not gain capacity merely because the
	// independently-owned far atlas exists. The old combined-budget path does.
	const uint64_t constrained_page_budget_bytes = 2u * mib + 17u;
	const int managed_capacity = t3dGpuCacheCalculateCapacity( constrained_page_budget_bytes,
	        T3D_TEST_GPU_RUNTIME_MAX_LAYERS, T3D_TEST_REGION_SLOTS, true, true, false, 1, 1, T3D_TEST_GPU_DEFAULT_SPARE_LAYERS );
	const int legacy_combined_capacity = t3dGpuCacheCalculateCapacity( constrained_page_budget_bytes + fixed_far_atlas_bytes,
	        T3D_TEST_GPU_RUNTIME_MAX_LAYERS, T3D_TEST_REGION_SLOTS, true, true, false, 1, 1, T3D_TEST_GPU_DEFAULT_SPARE_LAYERS );
	if ( legacy_combined_capacity <= managed_capacity ) {
		return fail( "test did not detect legacy far-atlas capacity inflation" );
	}

	// Exact resident-layer accounting includes Middle composite + terrain shadow.
	// Mapped pages outrank the configured spare target; only the legal minimum spare
	// set is mandatory when the grant is constrained.
	const size_t exact_layer_bytes = t3dGpuCacheEstimateResidentLayerBytes( false, false, true, 1, 1 );
	const size_t exact_shared_bytes = t3dGpuCacheEstimateSharedBytes( false, false, 1 );
	const uint64_t exact_budget = ( uint64_t )( exact_shared_bytes ) + ( uint64_t )( exact_layer_bytes ) * 11u + exact_layer_bytes / 2u;
	const int exact_mapped = t3dGpuCacheCalculateCapacity( exact_budget, 64, 49, false, false, true, 1, 1, 12 );
	const int exact_budget_layers = t3dGpuCacheCalculateBudgetPhysicalLayers( exact_budget, 64, false, false, true, 1, 1 );
	const int exact_physical = t3dGpuCacheCalculatePhysicalLayers( exact_mapped, 12, 64, exact_budget_layers );
	if ( exact_mapped != 9 || exact_physical != 11 ) {
		return fail( "mapped terrain did not outrank optional spare-page target" );
	}
	if ( ( uint64_t )( exact_shared_bytes ) + ( uint64_t )( exact_layer_bytes ) * ( uint64_t )( exact_physical ) > exact_budget ) {
		return fail( "physical GPU page allocation exceeds exact managed grant" );
	}

	// Regression: the reported 4,950,062-byte grant with half-resolution color/derived
	// supports 26 mapped pages plus only 2 spare pages. The old policy incorrectly
	// added all 12 configured spares and allocated 38 layers (~6.55 MiB).
	const uint64_t observed_grant = 4950062u;
	const size_t observed_layer_bytes = t3dGpuCacheEstimateResidentLayerBytes( false, false, true, 2, 2 );
	const size_t observed_shared_bytes = t3dGpuCacheEstimateSharedBytes( false, false, 2 );
	const int observed_mapped = t3dGpuCacheCalculateCapacity( observed_grant, 64, 96, false, false, true, 2, 2, 12 );
	const int observed_budget_layers = t3dGpuCacheCalculateBudgetPhysicalLayers( observed_grant, 64, false, false, true, 2, 2 );
	const int observed_physical = t3dGpuCacheCalculatePhysicalLayers( observed_mapped, 12, 64, observed_budget_layers );
	if ( observed_mapped != 26 || observed_physical != 28 ) {
		return fail( "observed GPU cache grant regression did not resolve to 26 mapped + 2 spare pages" );
	}
	if ( ( uint64_t )( observed_shared_bytes ) + ( uint64_t )( observed_layer_bytes ) * ( uint64_t )( observed_physical ) > observed_grant ) {
		return fail( "observed GPU cache grant still over-allocates physical layers" );
	}

	// Minimum fits exactly.
	input = baseInput( 40u * mib, 8u * mib, 16u * mib, 12u * mib );
	gpu = input.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.m_bEnabled = true;
	gpu.m_bAutomatic = true;
	gpu.m_nMinimumBytes = 4u * mib;
	gpu.m_nHardCapBytes = 128u * mib;
	result = Terrain3DMemoryBudgetCoordinator::calculate( input );
	if ( result.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].m_nEffectiveBytes != 4u * mib || result.m_nCommittedBytes != 40u * mib ) {
		return fail( "minimum allocation case" );
	}

	// Manual override is exact bytes, not rounded MiB internally.
	input = baseInput( 64u * mib, 8u * mib, 16u * mib, 8u * mib );
	gpu = input.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.m_bEnabled = true;
	gpu.m_bAutomatic = false;
	gpu.m_nManualBytes = 12u * mib + 12345u;
	gpu.m_nHardCapBytes = 128u * mib;
	result = Terrain3DMemoryBudgetCoordinator::calculate( input );
	if ( result.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].m_nEffectiveBytes != 12u * mib + 12345u ) {
		return fail( "manual exact-byte override" );
	}

	// Hard-overcommit: an automatic subsystem minimum is advisory when fixed +
	// allocator + transient ownership consumes the complete aggregate budget. This
	// is why render-tier validity must never depend on the current page grant.
	input = baseInput( 32u * mib, 16u * mib, 8u * mib, 8u * mib );
	gpu = input.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.m_bEnabled = true;
	gpu.m_bAutomatic = true;
	gpu.m_nMinimumBytes = 4u * mib;
	gpu.m_nHardCapBytes = 128u * mib;
	result = Terrain3DMemoryBudgetCoordinator::calculate( input );
	if ( result.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].m_nEffectiveBytes != 0u
	        || !result.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].m_bBelowMinimum || !result.m_bHardOvercommit ) {
		return fail( "hard-overcommit did not demonstrate zero-byte automatic minimum grant" );
	}

	// Near-overcommit: fixed reserves still fit, but the requested minimum is one byte too large.
	input = baseInput( 32u * mib, 8u * mib, 16u * mib, 4u * mib );
	gpu = input.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.m_bEnabled = true;
	gpu.m_bAutomatic = true;
	gpu.m_nMinimumBytes = 4u * mib + 1u;
	gpu.m_nHardCapBytes = 128u * mib;
	result = Terrain3DMemoryBudgetCoordinator::calculate( input );
	if ( !result.m_bHardOvercommit || !result.m_szSubsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].m_bBelowMinimum ) {
		return fail( "near-overcommit was not reported" );
	}
	if ( result.m_nCommittedBytes > input.m_nTotalBudgetBytes ) {
		return fail( "near-overcommit granted bytes beyond total budget" );
	}

	const size_t page_cache_owned_fixed_metadata_bytes = t3dGpuCacheEstimateSharedBytes( true, true, 1 );
	if ( page_cache_owned_fixed_metadata_bytes >= fixed_far_atlas_bytes ) {
		return fail( "managed GPU helper still appears to own far-atlas memory" );
	}

	printf( "PASS: Terrain3D P01 memory ownership contract\n" );
	return 0;
}
