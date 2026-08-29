// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_plateau.h"

bool t3d_render_world_plateau_snapshot_equal_reference_debug(
        const T3DRenderWorldPlateauSnapshot &pBaseline, const T3DRenderWorldPlateauSnapshot &pCurrent ) {
	if ( pBaseline.nLimitsSignature == 0u || pBaseline.nLimitsSignature != pCurrent.nLimitsSignature ) {
		return false;
	}
	if ( pBaseline.nGpuCreateCount != pCurrent.nGpuCreateCount ) {
		return false;
	}
	if ( pBaseline.nGpuFreeCount != pCurrent.nGpuFreeCount ) {
		return false;
	}
	if ( pBaseline.nGpuBufferBytes != pCurrent.nGpuBufferBytes ) {
		return false;
	}
	if ( pBaseline.nGpuTextureBytes != pCurrent.nGpuTextureBytes ) {
		return false;
	}
	if ( pBaseline.nVTPhysicalBytes != pCurrent.nVTPhysicalBytes ) {
		return false;
	}
	if ( pBaseline.nVisibilityCapacity != pCurrent.nVisibilityCapacity ) {
		return false;
	}
	if ( pBaseline.nCpuAllocationCount != pCurrent.nCpuAllocationCount ) {
		return false;
	}
	if ( pCurrent.nSyncWaitCount != 0u || pCurrent.nSyncReadbackCount != 0u ) {
		return false;
	}
	if ( pBaseline.nLiveResourceCount != pCurrent.nLiveResourceCount ) {
		return false;
	}
	if ( pBaseline.nHotViolationCount != pCurrent.nHotViolationCount ) {
		return false;
	}
	return true;
}
