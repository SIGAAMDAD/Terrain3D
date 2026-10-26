// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H
#define TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H

#include "Terrain3DTypes.h"

#include "core/config/terrain_3d_compiler.h"

#include <stdint.h>
#include <godot_cpp/core/defs.hpp>

// t3dAdaptiveBudgetValues_s centralized in Terrain3DTypes.h.


// t3dAdaptiveBudgetLimits_s centralized in Terrain3DTypes.h.


// t3dAdaptiveBudgetPressure_s centralized in Terrain3DTypes.h.


// t3dAdaptiveBudgetReason_e centralized in Terrain3DTypes.h.


// t3dAdaptiveBudgetState_s centralized in Terrain3DTypes.h.


EA_FORCE_INLINE int t3dAdaptiveClampValue( const int nValue, const int nMinimum, const int nMaximum ) {
	const int maximum = godot::MAX( nMinimum, nMaximum );
	return godot::MAX( nMinimum, godot::MIN( nValue, maximum ) );
}

void t3dAdaptiveSanitizeLimits( t3dAdaptiveBudgetLimits_t &rLimits );

EA_FORCE_INLINE void t3dAdaptiveReset(
        const t3dAdaptiveBudgetLimits_t &rLimits, t3dAdaptiveBudgetState_t &rState, const t3dAdaptiveBudgetReason_t reason ) {
	rState.m_effective = rLimits.m_maximum;
	rState.m_nOverBudgetStreak = 0;
	rState.m_nUnderBudgetStreak = 0;
	rState.m_reason = reason;
	rState.m_bInitialized = true;
}

inline int t3dAdaptiveDecrease( const int nValue, const int nMinimum ) {
	if ( nValue <= nMinimum ) {
		return nMinimum;
	}
	const int step = godot::MAX( 1, nValue / 4 );
	return godot::MAX( nMinimum, nValue - step );
}

inline int t3dAdaptiveIncrease( const int nValue, const int nMaximum ) {
	if ( nValue >= nMaximum ) {
		return nMaximum;
	}
	const int step = godot::MAX( 1, nMaximum / 8 );
	return godot::MIN( nMaximum, nValue + step );
}

void t3dAdaptiveClampState( const t3dAdaptiveBudgetLimits_t &rLimits, t3dAdaptiveBudgetState_t &rState );

bool t3dAdaptiveStep( const t3dAdaptiveBudgetLimits_t &rLimits,
        const t3dAdaptiveBudgetPressure_t &rPressure,
        const uint64_t nMeasuredUsec,
        t3dAdaptiveBudgetState_t &rState );

#endif // TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H
