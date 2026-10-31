#define TERRAIN3D_STANDALONE_ALLOCATOR_TEST 1
#include "memory/terrain_3d_memory.h"

int main( void ) {
	void *pHotFrame = NULL;
	void *pHotRender = NULL;
	{
		Terrain3DAllocationPhaseScope phase( TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME );
		pHotFrame = t3dMemoryAllocate( 64u );
		if ( !pHotFrame ) {
			return 1;
		}
	}
	{
		Terrain3DAllocationPhaseScope phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
		pHotRender = t3dMemoryAllocate( 96u );
		if ( !pHotRender ) {
			return 2;
		}
	}
	{
		Terrain3DAllocationPhaseScope phase( TERRAIN3D_ALLOCATION_PHASE_COLD_REBUILD );
		void *const pCold = t3dMemoryAllocate( 32u );
		if ( !pCold ) {
			return 3;
		}
		t3dMemoryRelease( pCold );
	}
	t3dMemoryRelease( pHotFrame );
	t3dMemoryRelease( pHotRender );
	const t3dRuntimeAllocationPhaseStats_t stats = t3dGetRuntimeAllocationPhaseStats();
	if ( stats.m_szRawEvents[TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME] != 1u
	        || stats.m_szRawEvents[TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER] != 1u
	        || stats.m_szRawBytes[TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME] != 64u
	        || stats.m_szRawBytes[TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER] != 96u ) {
		return 4;
	}
	return 0;
}
