#define GDEXTENSION 1
#include "memory/terrain_3d_memory_budget.h"

int main( void ) {
	t3dMemoryBudgetInput_t input;
	input.m_nTotalBudgetBytes = 512u * 1024u * 1024u;
	input.m_nFixedBytes = 100u * 1024u * 1024u;
	input.m_nAllocatorContainedFixedBytes = 20u * 1024u * 1024u;
	input.m_nAllocatorReserveBytes = 64u * 1024u * 1024u;
	input.m_nTransientReserveBytes = 32u * 1024u * 1024u;
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		input.m_szSubsystems[index].m_bEnabled = false;
	}

	const t3dMemoryBudgetResult_t result = Terrain3DMemoryBudgetCoordinator::calculate( input );
	if ( result.m_nFixedBytes != 80u * 1024u * 1024u ) {
		return 1;
	}
	if ( result.m_nAllocatorContainedFixedBytes != 20u * 1024u * 1024u ) {
		return 2;
	}
	if ( result.m_nManagedBudgetBytes != 336u * 1024u * 1024u ) {
		return 3;
	}
	if ( result.m_nCommittedBytes != 176u * 1024u * 1024u ) {
		return 4;
	}
	if ( result.m_nHeadroomBytes != 336u * 1024u * 1024u ) {
		return 5;
	}

	input.m_nFixedBytes = 8u * 1024u * 1024u;
	input.m_nAllocatorContainedFixedBytes = 16u * 1024u * 1024u;
	const t3dMemoryBudgetResult_t clamped = Terrain3DMemoryBudgetCoordinator::calculate( input );
	if ( clamped.m_nAllocatorContainedFixedBytes != input.m_nFixedBytes || clamped.m_nFixedBytes != 0u ) {
		return 6;
	}

	input.m_nFixedBytes = 100u * 1024u * 1024u;
	input.m_nAllocatorContainedFixedBytes = 80u * 1024u * 1024u;
	input.m_nAllocatorReserveBytes = 64u * 1024u * 1024u;
	const t3dMemoryBudgetResult_t reserveClamped = Terrain3DMemoryBudgetCoordinator::calculate( input );
	if ( reserveClamped.m_nAllocatorContainedFixedBytes != input.m_nAllocatorReserveBytes
	        || reserveClamped.m_nFixedBytes != 36u * 1024u * 1024u ) {
		return 7;
	}
	return 0;
}
