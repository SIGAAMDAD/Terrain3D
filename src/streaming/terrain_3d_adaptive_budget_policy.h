// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H
#define TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H

#include <algorithm>
#include <cstdint>
#include "core/terrain_3d_compiler.h"

typedef struct t3dAdaptiveBudgetValues_s {
	int m_nGpuPages = 4;
	int m_nCpuPages = 4;
	int m_nInstancePages = 4;
	int m_nInstancerCells = 32;
	int m_nNodeLoadRegions = 2;
	int m_nNodeUnloadRegions = 4;
} t3dAdaptiveBudgetValues_t;

typedef struct t3dAdaptiveBudgetLimits_s {
	t3dAdaptiveBudgetValues_t m_minimum;
	t3dAdaptiveBudgetValues_t m_maximum;
	uint64_t m_nTargetUsec = 1000u;
	uint64_t m_nHardMaxUsec = 2000u;
	int m_nAdjustmentWindowFrames = 4;
} t3dAdaptiveBudgetLimits_t;

typedef struct t3dAdaptiveBudgetPressure_s {
	bool m_bGpuLimited = false;
	bool m_bCpuLimited = false;
	bool m_bInstanceLimited = false;
	bool m_bInstancerLimited = false;
	bool m_bNodeLoadLimited = false;
	bool m_bNodeUnloadLimited = false;
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
	t3dAdaptiveBudgetValues_t m_effective;
	int m_nOverBudgetStreak = 0;
	int m_nUnderBudgetStreak = 0;
	uint64_t m_nAdjustmentCount = 0u;
	t3dAdaptiveBudgetReason_t m_reason = TERRAIN3D_ADAPTIVE_REASON_DISABLED;
	bool m_bInitialized = false;
} t3dAdaptiveBudgetState_t;

EA_FORCE_INLINE int t3dAdaptiveClampValue( const int nValue, const int nMinimum, const int nMaximum ) { const int maximum = std::max( nMinimum, nMaximum ); return std::max( nMinimum, std::min( nValue, maximum ) ); }

inline void t3dAdaptiveSanitizeLimits( t3dAdaptiveBudgetLimits_t &rLimits ) {
	rLimits.m_maximum.m_nGpuPages = std::max( 0, rLimits.m_maximum.m_nGpuPages );
	rLimits.m_maximum.m_nCpuPages = std::max( 0, rLimits.m_maximum.m_nCpuPages );
	rLimits.m_maximum.m_nInstancePages = std::max( 0, rLimits.m_maximum.m_nInstancePages );
	rLimits.m_maximum.m_nInstancerCells = std::max( 0, rLimits.m_maximum.m_nInstancerCells );
	rLimits.m_maximum.m_nNodeLoadRegions = std::max( 0, rLimits.m_maximum.m_nNodeLoadRegions );
	rLimits.m_maximum.m_nNodeUnloadRegions = std::max( 0, rLimits.m_maximum.m_nNodeUnloadRegions );
	rLimits.m_minimum.m_nGpuPages = t3dAdaptiveClampValue( rLimits.m_minimum.m_nGpuPages, 0,
		rLimits.m_maximum.m_nGpuPages );
	rLimits.m_minimum.m_nCpuPages = t3dAdaptiveClampValue( rLimits.m_minimum.m_nCpuPages, 0,
		rLimits.m_maximum.m_nCpuPages );
	rLimits.m_minimum.m_nInstancePages = t3dAdaptiveClampValue( rLimits.m_minimum.m_nInstancePages, 0,
		rLimits.m_maximum.m_nInstancePages );
	rLimits.m_minimum.m_nInstancerCells = t3dAdaptiveClampValue( rLimits.m_minimum.m_nInstancerCells, 0,
		rLimits.m_maximum.m_nInstancerCells );
	rLimits.m_minimum.m_nNodeLoadRegions = t3dAdaptiveClampValue( rLimits.m_minimum.m_nNodeLoadRegions, 0,
		rLimits.m_maximum.m_nNodeLoadRegions );
	rLimits.m_minimum.m_nNodeUnloadRegions = t3dAdaptiveClampValue( rLimits.m_minimum.m_nNodeUnloadRegions, 0,
		rLimits.m_maximum.m_nNodeUnloadRegions );
	rLimits.m_nTargetUsec = std::max<uint64_t>( 1u, rLimits.m_nTargetUsec );
	rLimits.m_nHardMaxUsec = std::max( rLimits.m_nTargetUsec, rLimits.m_nHardMaxUsec );
	rLimits.m_nAdjustmentWindowFrames = std::max( 1, rLimits.m_nAdjustmentWindowFrames );
}

EA_FORCE_INLINE void t3dAdaptiveReset( const t3dAdaptiveBudgetLimits_t &rLimits, t3dAdaptiveBudgetState_t &rState, const t3dAdaptiveBudgetReason_t reason ) { rState.m_effective = rLimits.m_maximum; rState.m_nOverBudgetStreak = 0; rState.m_nUnderBudgetStreak = 0; rState.m_reason = reason; rState.m_bInitialized = true; }

inline int t3dAdaptiveDecrease( const int nValue, const int nMinimum ) {
	if ( nValue <= nMinimum ) {
		return nMinimum;
	}
	const int step = std::max( 1, nValue / 4 );
	return std::max( nMinimum, nValue - step );
}

inline int t3dAdaptiveIncrease( const int nValue, const int nMaximum ) {
	if ( nValue >= nMaximum ) {
		return nMaximum;
	}
	const int step = std::max( 1, nMaximum / 8 );
	return std::min( nMaximum, nValue + step );
}

inline void t3dAdaptiveClampState(
	const t3dAdaptiveBudgetLimits_t &rLimits,
	t3dAdaptiveBudgetState_t &rState
) {
	rState.m_effective.m_nGpuPages = t3dAdaptiveClampValue( rState.m_effective.m_nGpuPages,
		rLimits.m_minimum.m_nGpuPages, rLimits.m_maximum.m_nGpuPages );
	rState.m_effective.m_nCpuPages = t3dAdaptiveClampValue( rState.m_effective.m_nCpuPages,
		rLimits.m_minimum.m_nCpuPages, rLimits.m_maximum.m_nCpuPages );
	rState.m_effective.m_nInstancePages = t3dAdaptiveClampValue( rState.m_effective.m_nInstancePages,
		rLimits.m_minimum.m_nInstancePages, rLimits.m_maximum.m_nInstancePages );
	rState.m_effective.m_nInstancerCells = t3dAdaptiveClampValue( rState.m_effective.m_nInstancerCells,
		rLimits.m_minimum.m_nInstancerCells, rLimits.m_maximum.m_nInstancerCells );
	rState.m_effective.m_nNodeLoadRegions = t3dAdaptiveClampValue( rState.m_effective.m_nNodeLoadRegions,
		rLimits.m_minimum.m_nNodeLoadRegions, rLimits.m_maximum.m_nNodeLoadRegions );
	rState.m_effective.m_nNodeUnloadRegions = t3dAdaptiveClampValue( rState.m_effective.m_nNodeUnloadRegions,
		rLimits.m_minimum.m_nNodeUnloadRegions, rLimits.m_maximum.m_nNodeUnloadRegions );
}

inline bool t3dAdaptiveStep(
	const t3dAdaptiveBudgetLimits_t &rLimits,
	const t3dAdaptiveBudgetPressure_t &rPressure, const uint64_t nMeasuredUsec,
	t3dAdaptiveBudgetState_t &rState
) {
	if ( !rState.m_bInitialized ) {
		t3dAdaptiveReset( rLimits, rState, TERRAIN3D_ADAPTIVE_REASON_INITIALIZED );
		return true;
	}
	t3dAdaptiveClampState( rLimits, rState );
	if ( nMeasuredUsec > rLimits.m_nHardMaxUsec ) {
		++rState.m_nOverBudgetStreak;
		rState.m_nUnderBudgetStreak = 0;
		if ( rState.m_nOverBudgetStreak < rLimits.m_nAdjustmentWindowFrames ) {
			rState.m_reason = TERRAIN3D_ADAPTIVE_REASON_STABLE;
			return false;
		}
		const t3dAdaptiveBudgetValues_t previous = rState.m_effective;
		rState.m_effective.m_nGpuPages = t3dAdaptiveDecrease( rState.m_effective.m_nGpuPages,
			rLimits.m_minimum.m_nGpuPages );
		rState.m_effective.m_nCpuPages = t3dAdaptiveDecrease( rState.m_effective.m_nCpuPages,
			rLimits.m_minimum.m_nCpuPages );
		rState.m_effective.m_nInstancePages = t3dAdaptiveDecrease( rState.m_effective.m_nInstancePages,
			rLimits.m_minimum.m_nInstancePages );
		rState.m_effective.m_nInstancerCells = t3dAdaptiveDecrease( rState.m_effective.m_nInstancerCells,
			rLimits.m_minimum.m_nInstancerCells );
		rState.m_effective.m_nNodeLoadRegions = t3dAdaptiveDecrease( rState.m_effective.m_nNodeLoadRegions,
			rLimits.m_minimum.m_nNodeLoadRegions );
		rState.m_effective.m_nNodeUnloadRegions = t3dAdaptiveDecrease( rState.m_effective.m_nNodeUnloadRegions,
			rLimits.m_minimum.m_nNodeUnloadRegions );
		rState.m_nOverBudgetStreak = 0;
		rState.m_reason = TERRAIN3D_ADAPTIVE_REASON_HARD_LIMIT;
		const bool bChanged = previous.m_nGpuPages != rState.m_effective.m_nGpuPages ||
			previous.m_nCpuPages != rState.m_effective.m_nCpuPages ||
			previous.m_nInstancePages != rState.m_effective.m_nInstancePages ||
			previous.m_nInstancerCells != rState.m_effective.m_nInstancerCells ||
			previous.m_nNodeLoadRegions != rState.m_effective.m_nNodeLoadRegions ||
			previous.m_nNodeUnloadRegions != rState.m_effective.m_nNodeUnloadRegions;
		if ( bChanged ) {
			++rState.m_nAdjustmentCount;
		}
		return bChanged;
	}
	if ( nMeasuredUsec < rLimits.m_nTargetUsec ) {
		++rState.m_nUnderBudgetStreak;
		rState.m_nOverBudgetStreak = 0;
		if ( rState.m_nUnderBudgetStreak < rLimits.m_nAdjustmentWindowFrames ) {
			rState.m_reason = TERRAIN3D_ADAPTIVE_REASON_STABLE;
			return false;
		}
		const t3dAdaptiveBudgetValues_t previous = rState.m_effective;
		if ( rPressure.m_bGpuLimited ) {
			rState.m_effective.m_nGpuPages = t3dAdaptiveIncrease( rState.m_effective.m_nGpuPages,
				rLimits.m_maximum.m_nGpuPages );
		}
		if ( rPressure.m_bCpuLimited ) {
			rState.m_effective.m_nCpuPages = t3dAdaptiveIncrease( rState.m_effective.m_nCpuPages,
				rLimits.m_maximum.m_nCpuPages );
		}
		if ( rPressure.m_bInstanceLimited ) {
			rState.m_effective.m_nInstancePages = t3dAdaptiveIncrease( rState.m_effective.m_nInstancePages,
				rLimits.m_maximum.m_nInstancePages );
		}
		if ( rPressure.m_bInstancerLimited ) {
			rState.m_effective.m_nInstancerCells = t3dAdaptiveIncrease( rState.m_effective.m_nInstancerCells,
				rLimits.m_maximum.m_nInstancerCells );
		}
		if ( rPressure.m_bNodeLoadLimited ) {
			rState.m_effective.m_nNodeLoadRegions = t3dAdaptiveIncrease( rState.m_effective.m_nNodeLoadRegions,
				rLimits.m_maximum.m_nNodeLoadRegions );
		}
		if ( rPressure.m_bNodeUnloadLimited ) {
			rState.m_effective.m_nNodeUnloadRegions = t3dAdaptiveIncrease( rState.m_effective.m_nNodeUnloadRegions,
				rLimits.m_maximum.m_nNodeUnloadRegions );
		}
		rState.m_nUnderBudgetStreak = 0;
		const bool bChanged = previous.m_nGpuPages != rState.m_effective.m_nGpuPages ||
			previous.m_nCpuPages != rState.m_effective.m_nCpuPages ||
			previous.m_nInstancePages != rState.m_effective.m_nInstancePages ||
			previous.m_nInstancerCells != rState.m_effective.m_nInstancerCells ||
			previous.m_nNodeLoadRegions != rState.m_effective.m_nNodeLoadRegions ||
			previous.m_nNodeUnloadRegions != rState.m_effective.m_nNodeUnloadRegions;
		rState.m_reason = bChanged ? TERRAIN3D_ADAPTIVE_REASON_BACKLOG_RECOVERY :
			TERRAIN3D_ADAPTIVE_REASON_UNDER_TARGET_NO_BACKLOG;
		if ( bChanged ) {
			++rState.m_nAdjustmentCount;
		}
		return bChanged;
	}
	rState.m_nOverBudgetStreak = 0;
	rState.m_nUnderBudgetStreak = 0;
	rState.m_reason = TERRAIN3D_ADAPTIVE_REASON_STABLE;
	return false;
}

#endif // TERRAIN3D_ADAPTIVE_BUDGET_POLICY_H
