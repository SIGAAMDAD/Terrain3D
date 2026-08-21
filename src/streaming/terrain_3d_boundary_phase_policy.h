// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN_3D_BOUNDARY_PHASE_POLICY_H
#define TERRAIN_3D_BOUNDARY_PHASE_POLICY_H

#include "core/terrain_3d_compiler.h"

#include <cstdint>

EA_FORCE_INLINE bool t3dBoundaryShouldDefer( const bool bEnabled,
		const bool bSubsystemEnabled,
		const uint64_t nFramesSinceBoundary,
		const int nDelayFrames,
		const int nConsecutiveDeferrals ) {
	return bEnabled && bSubsystemEnabled && nDelayFrames > 0 && nFramesSinceBoundary < ( uint64_t )( nDelayFrames ) &&
			nConsecutiveDeferrals < nDelayFrames;
}

#endif // TERRAIN_3D_BOUNDARY_PHASE_POLICY_H
