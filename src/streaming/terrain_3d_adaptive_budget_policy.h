// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H
#define TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H

#include <algorithm>
#include <cstdint>
#include "core/terrain_3d_compiler.h"

typedef struct t3dAdaptiveBudgetValues_s {
	int nGpuPages = 4;
	int nCpuPages = 4;
	int nInstancePages = 4;
	int instancerCells = 32;
	int nNodeLoadRegions = 2;
	int nNodeUnloadRegions = 4;
} t3dAdaptiveBudgetValues_t;

typedef struct t3dAdaptiveBudgetLimits_s {
	t3dAdaptiveBudgetValues_t minimum;
	t3dAdaptiveBudgetValues_t maximum;
	uint64_t nTargetUsec = 1000u;
	uint64_t nHardMaxUsec = 2000u;
	int nAdjustmentWindowFrames = 4;
} t3dAdaptiveBudgetLimits_t;

typedef struct t3dAdaptiveBudgetPressure_s {
	bool bGpuLimited = false;
	bool bCpuLimited = false;
	bool bInstanceLimited = false;
	bool bInstancerLimited = false;
	bool bNodeLoadLimited = false;
	bool bNodeUnloadLimited = false;
} t3dAdaptiveBudgetPressure_t;

typedef enum t3dAdaptiveBudgetReason_e {
	TERRAIN3D_ADAPTIVE_REASON_DISABLED = 0,
	TERRAIN3D_ADAPTIVE_REASON_INITIALIZED,
	TERRAIN3D_ADAPTIVE_REASON_HARD_LIMIT,
	TERRAIN3D_ADAPTIVE_REASON_BACKLOG_RECOVERY,
	TERRAIN3D_ADAPTIVE_REASON_UNDER_TARGET_NO_BACKLOG,
	TERRAIN3D_ADAPTIVE_REASON_STABLE,
} t3dAdaptiveBudgetReason_t;

typedef struct t3dAdaptiveBudgetState_s {
	t3dAdaptiveBudgetValues_t effective;
	int overBudgetStreak = 0;
	int underBudgetStreak = 0;
	uint64_t nAdjustmentCount = 0u;
	t3dAdaptiveBudgetReason_t reason = TERRAIN3D_ADAPTIVE_REASON_DISABLED;
	bool bInitialized = false;
} t3dAdaptiveBudgetState_t;

EA_FORCE_INLINE int terrain3dAdaptiveClampValue( const int nValue, const int nMinimum, const int nMaximum ) { const int maximum = std::max( nMinimum, nMaximum ); return std::max( nMinimum, std::min( nValue, maximum ) ); }

inline void terrain3dAdaptiveSanitizeLimits( t3dAdaptiveBudgetLimits_t &rLimits ) {
	rLimits.maximum.nGpuPages = std::max( 0, rLimits.maximum.nGpuPages );
	rLimits.maximum.nCpuPages = std::max( 0, rLimits.maximum.nCpuPages );
	rLimits.maximum.nInstancePages = std::max( 0, rLimits.maximum.nInstancePages );
	rLimits.maximum.instancerCells = std::max( 0, rLimits.maximum.instancerCells );
	rLimits.maximum.nNodeLoadRegions = std::max( 0, rLimits.maximum.nNodeLoadRegions );
	rLimits.maximum.nNodeUnloadRegions = std::max( 0, rLimits.maximum.nNodeUnloadRegions );
	rLimits.minimum.nGpuPages = terrain3dAdaptiveClampValue( rLimits.minimum.nGpuPages, 0,
		rLimits.maximum.nGpuPages );
	rLimits.minimum.nCpuPages = terrain3dAdaptiveClampValue( rLimits.minimum.nCpuPages, 0,
		rLimits.maximum.nCpuPages );
	rLimits.minimum.nInstancePages = terrain3dAdaptiveClampValue( rLimits.minimum.nInstancePages, 0,
		rLimits.maximum.nInstancePages );
	rLimits.minimum.instancerCells = terrain3dAdaptiveClampValue( rLimits.minimum.instancerCells, 0,
		rLimits.maximum.instancerCells );
	rLimits.minimum.nNodeLoadRegions = terrain3dAdaptiveClampValue( rLimits.minimum.nNodeLoadRegions, 0,
		rLimits.maximum.nNodeLoadRegions );
	rLimits.minimum.nNodeUnloadRegions = terrain3dAdaptiveClampValue( rLimits.minimum.nNodeUnloadRegions, 0,
		rLimits.maximum.nNodeUnloadRegions );
	rLimits.nTargetUsec = std::max<uint64_t>( 1u, rLimits.nTargetUsec );
	rLimits.nHardMaxUsec = std::max( rLimits.nTargetUsec, rLimits.nHardMaxUsec );
	rLimits.nAdjustmentWindowFrames = std::max( 1, rLimits.nAdjustmentWindowFrames );
}

EA_FORCE_INLINE void terrain3dAdaptiveReset( const t3dAdaptiveBudgetLimits_t &rLimits, t3dAdaptiveBudgetState_t &rState, const t3dAdaptiveBudgetReason_t reason ) { rState.effective = rLimits.maximum; rState.overBudgetStreak = 0; rState.underBudgetStreak = 0; rState.reason = reason; rState.bInitialized = true; }

inline int terrain3dAdaptiveDecrease( const int nValue, const int nMinimum ) {
	if ( nValue <= nMinimum ) {
		return nMinimum;
	}
	const int step = std::max( 1, nValue / 4 );
	return std::max( nMinimum, nValue - step );
}

inline int terrain3dAdaptiveIncrease( const int nValue, const int nMaximum ) {
	if ( nValue >= nMaximum ) {
		return nMaximum;
	}
	const int step = std::max( 1, nMaximum / 8 );
	return std::min( nMaximum, nValue + step );
}

inline void terrain3dAdaptiveClampState(
	const t3dAdaptiveBudgetLimits_t &rLimits,
	t3dAdaptiveBudgetState_t &rState
) {
	rState.effective.nGpuPages = terrain3dAdaptiveClampValue( rState.effective.nGpuPages,
		rLimits.minimum.nGpuPages, rLimits.maximum.nGpuPages );
	rState.effective.nCpuPages = terrain3dAdaptiveClampValue( rState.effective.nCpuPages,
		rLimits.minimum.nCpuPages, rLimits.maximum.nCpuPages );
	rState.effective.nInstancePages = terrain3dAdaptiveClampValue( rState.effective.nInstancePages,
		rLimits.minimum.nInstancePages, rLimits.maximum.nInstancePages );
	rState.effective.instancerCells = terrain3dAdaptiveClampValue( rState.effective.instancerCells,
		rLimits.minimum.instancerCells, rLimits.maximum.instancerCells );
	rState.effective.nNodeLoadRegions = terrain3dAdaptiveClampValue( rState.effective.nNodeLoadRegions,
		rLimits.minimum.nNodeLoadRegions, rLimits.maximum.nNodeLoadRegions );
	rState.effective.nNodeUnloadRegions = terrain3dAdaptiveClampValue( rState.effective.nNodeUnloadRegions,
		rLimits.minimum.nNodeUnloadRegions, rLimits.maximum.nNodeUnloadRegions );
}

inline bool terrain3dAdaptiveStep(
	const t3dAdaptiveBudgetLimits_t &rLimits,
	const t3dAdaptiveBudgetPressure_t &rPressure, const uint64_t nMeasuredUsec,
	t3dAdaptiveBudgetState_t &rState
) {
	if ( !rState.bInitialized ) {
		terrain3dAdaptiveReset( rLimits, rState, TERRAIN3D_ADAPTIVE_REASON_INITIALIZED );
		return true;
	}
	terrain3dAdaptiveClampState( rLimits, rState );
	if ( nMeasuredUsec > rLimits.nHardMaxUsec ) {
		++rState.overBudgetStreak;
		rState.underBudgetStreak = 0;
		if ( rState.overBudgetStreak < rLimits.nAdjustmentWindowFrames ) {
			rState.reason = TERRAIN3D_ADAPTIVE_REASON_STABLE;
			return false;
		}
		const t3dAdaptiveBudgetValues_t previous = rState.effective;
		rState.effective.nGpuPages = terrain3dAdaptiveDecrease( rState.effective.nGpuPages,
			rLimits.minimum.nGpuPages );
		rState.effective.nCpuPages = terrain3dAdaptiveDecrease( rState.effective.nCpuPages,
			rLimits.minimum.nCpuPages );
		rState.effective.nInstancePages = terrain3dAdaptiveDecrease( rState.effective.nInstancePages,
			rLimits.minimum.nInstancePages );
		rState.effective.instancerCells = terrain3dAdaptiveDecrease( rState.effective.instancerCells,
			rLimits.minimum.instancerCells );
		rState.effective.nNodeLoadRegions = terrain3dAdaptiveDecrease( rState.effective.nNodeLoadRegions,
			rLimits.minimum.nNodeLoadRegions );
		rState.effective.nNodeUnloadRegions = terrain3dAdaptiveDecrease( rState.effective.nNodeUnloadRegions,
			rLimits.minimum.nNodeUnloadRegions );
		rState.overBudgetStreak = 0;
		rState.reason = TERRAIN3D_ADAPTIVE_REASON_HARD_LIMIT;
		const bool bChanged = previous.nGpuPages != rState.effective.nGpuPages ||
			previous.nCpuPages != rState.effective.nCpuPages ||
			previous.nInstancePages != rState.effective.nInstancePages ||
			previous.instancerCells != rState.effective.instancerCells ||
			previous.nNodeLoadRegions != rState.effective.nNodeLoadRegions ||
			previous.nNodeUnloadRegions != rState.effective.nNodeUnloadRegions;
		if ( bChanged ) {
			++rState.nAdjustmentCount;
		}
		return bChanged;
	}
	if ( nMeasuredUsec < rLimits.nTargetUsec ) {
		++rState.underBudgetStreak;
		rState.overBudgetStreak = 0;
		if ( rState.underBudgetStreak < rLimits.nAdjustmentWindowFrames ) {
			rState.reason = TERRAIN3D_ADAPTIVE_REASON_STABLE;
			return false;
		}
		const t3dAdaptiveBudgetValues_t previous = rState.effective;
		if ( rPressure.bGpuLimited ) {
			rState.effective.nGpuPages = terrain3dAdaptiveIncrease( rState.effective.nGpuPages,
				rLimits.maximum.nGpuPages );
		}
		if ( rPressure.bCpuLimited ) {
			rState.effective.nCpuPages = terrain3dAdaptiveIncrease( rState.effective.nCpuPages,
				rLimits.maximum.nCpuPages );
		}
		if ( rPressure.bInstanceLimited ) {
			rState.effective.nInstancePages = terrain3dAdaptiveIncrease( rState.effective.nInstancePages,
				rLimits.maximum.nInstancePages );
		}
		if ( rPressure.bInstancerLimited ) {
			rState.effective.instancerCells = terrain3dAdaptiveIncrease( rState.effective.instancerCells,
				rLimits.maximum.instancerCells );
		}
		if ( rPressure.bNodeLoadLimited ) {
			rState.effective.nNodeLoadRegions = terrain3dAdaptiveIncrease( rState.effective.nNodeLoadRegions,
				rLimits.maximum.nNodeLoadRegions );
		}
		if ( rPressure.bNodeUnloadLimited ) {
			rState.effective.nNodeUnloadRegions = terrain3dAdaptiveIncrease( rState.effective.nNodeUnloadRegions,
				rLimits.maximum.nNodeUnloadRegions );
		}
		rState.underBudgetStreak = 0;
		const bool bChanged = previous.nGpuPages != rState.effective.nGpuPages ||
			previous.nCpuPages != rState.effective.nCpuPages ||
			previous.nInstancePages != rState.effective.nInstancePages ||
			previous.instancerCells != rState.effective.instancerCells ||
			previous.nNodeLoadRegions != rState.effective.nNodeLoadRegions ||
			previous.nNodeUnloadRegions != rState.effective.nNodeUnloadRegions;
		rState.reason = bChanged ? TERRAIN3D_ADAPTIVE_REASON_BACKLOG_RECOVERY :
			TERRAIN3D_ADAPTIVE_REASON_UNDER_TARGET_NO_BACKLOG;
		if ( bChanged ) {
			++rState.nAdjustmentCount;
		}
		return bChanged;
	}
	rState.overBudgetStreak = 0;
	rState.underBudgetStreak = 0;
	rState.reason = TERRAIN3D_ADAPTIVE_REASON_STABLE;
	return false;
}

#endif // TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H
