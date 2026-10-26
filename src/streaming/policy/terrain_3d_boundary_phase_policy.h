// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_BOUNDARY_PHASE_POLICY_H
#define TERRAIN3D_BOUNDARY_PHASE_POLICY_H

#include "core/config/terrain_3d_compiler.h"

#include <stdint.h>

EA_FORCE_INLINE bool t3dBoundaryShouldDefer(
	const bool	   bEnabled,
	const bool	   bSubsystemEnabled,
	const uint64_t nFramesSinceBoundary,
	const int	   nDelayFrames,
	const int	   nConsecutiveDeferrals ) {
	return bEnabled && bSubsystemEnabled && nDelayFrames > 0 && nFramesSinceBoundary < ( uint64_t ) ( nDelayFrames ) &&
		   nConsecutiveDeferrals < nDelayFrames;
}

#endif // TERRAIN3D_BOUNDARY_PHASE_POLICY_H
