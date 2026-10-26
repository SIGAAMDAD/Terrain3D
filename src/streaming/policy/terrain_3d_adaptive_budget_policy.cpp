#include "streaming/policy/terrain_3d_adaptive_budget_policy.h"

void t3dAdaptiveSanitizeLimits(
	t3dAdaptiveBudgetLimits_t &rLimits ) {
	rLimits.m_maximum.m_nGpuPages = MAX( 0, rLimits.m_maximum.m_nGpuPages );
	rLimits.m_maximum.m_nCpuPages = MAX( 0, rLimits.m_maximum.m_nCpuPages );
	rLimits.m_maximum.m_nInstancePages = MAX( 0, rLimits.m_maximum.m_nInstancePages );
	rLimits.m_maximum.m_nInstancerCells = MAX( 0, rLimits.m_maximum.m_nInstancerCells );
	rLimits.m_maximum.m_nNodeLoadRegions = MAX( 0, rLimits.m_maximum.m_nNodeLoadRegions );
	rLimits.m_maximum.m_nNodeUnloadRegions = MAX( 0, rLimits.m_maximum.m_nNodeUnloadRegions );
	rLimits.m_maximum.m_nWorldUploadKb = MAX( 1, rLimits.m_maximum.m_nWorldUploadKb );
	rLimits.m_maximum.m_nWorldClusters = MAX( 1, rLimits.m_maximum.m_nWorldClusters );
	rLimits.m_maximum.m_nVtRequests = MAX( 1, rLimits.m_maximum.m_nVtRequests );
	rLimits.m_maximum.m_nVtAdmissions = MAX( 1, rLimits.m_maximum.m_nVtAdmissions );
	rLimits.m_minimum.m_nGpuPages =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nGpuPages, 0, rLimits.m_maximum.m_nGpuPages );
	rLimits.m_minimum.m_nCpuPages =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nCpuPages, 0, rLimits.m_maximum.m_nCpuPages );
	rLimits.m_minimum.m_nInstancePages =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nInstancePages, 0, rLimits.m_maximum.m_nInstancePages );
	rLimits.m_minimum.m_nInstancerCells =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nInstancerCells, 0, rLimits.m_maximum.m_nInstancerCells );
	rLimits.m_minimum.m_nNodeLoadRegions =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nNodeLoadRegions, 0, rLimits.m_maximum.m_nNodeLoadRegions );
	rLimits.m_minimum.m_nNodeUnloadRegions =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nNodeUnloadRegions, 0, rLimits.m_maximum.m_nNodeUnloadRegions );
	rLimits.m_minimum.m_nWorldUploadKb =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nWorldUploadKb, 1, rLimits.m_maximum.m_nWorldUploadKb );
	rLimits.m_minimum.m_nWorldClusters =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nWorldClusters, 1, rLimits.m_maximum.m_nWorldClusters );
	rLimits.m_minimum.m_nVtRequests =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nVtRequests, 1, rLimits.m_maximum.m_nVtRequests );
	rLimits.m_minimum.m_nVtAdmissions =
		t3dAdaptiveClampValue( rLimits.m_minimum.m_nVtAdmissions, 1, rLimits.m_maximum.m_nVtAdmissions );
	rLimits.m_nTargetUsec = MAX( 1u, rLimits.m_nTargetUsec );
	rLimits.m_nHardMaxUsec = MAX( rLimits.m_nTargetUsec, rLimits.m_nHardMaxUsec );
	rLimits.m_nAdjustmentWindowFrames = MAX( 1, rLimits.m_nAdjustmentWindowFrames );
}

void t3dAdaptiveClampState(
	const t3dAdaptiveBudgetLimits_t &rLimits,
	t3dAdaptiveBudgetState_t		&rState ) {
	rState.m_effective.m_nGpuPages = t3dAdaptiveClampValue( rState.m_effective.m_nGpuPages,
															rLimits.m_minimum.m_nGpuPages,
															rLimits.m_maximum.m_nGpuPages );
	rState.m_effective.m_nCpuPages = t3dAdaptiveClampValue( rState.m_effective.m_nCpuPages,
															rLimits.m_minimum.m_nCpuPages,
															rLimits.m_maximum.m_nCpuPages );
	rState.m_effective.m_nInstancePages = t3dAdaptiveClampValue( rState.m_effective.m_nInstancePages,
																 rLimits.m_minimum.m_nInstancePages,
																 rLimits.m_maximum.m_nInstancePages );
	rState.m_effective.m_nInstancerCells = t3dAdaptiveClampValue( rState.m_effective.m_nInstancerCells,
																  rLimits.m_minimum.m_nInstancerCells,
																  rLimits.m_maximum.m_nInstancerCells );
	rState.m_effective.m_nNodeLoadRegions = t3dAdaptiveClampValue( rState.m_effective.m_nNodeLoadRegions,
																   rLimits.m_minimum.m_nNodeLoadRegions,
																   rLimits.m_maximum.m_nNodeLoadRegions );
	rState.m_effective.m_nNodeUnloadRegions = t3dAdaptiveClampValue( rState.m_effective.m_nNodeUnloadRegions,
																	 rLimits.m_minimum.m_nNodeUnloadRegions,
																	 rLimits.m_maximum.m_nNodeUnloadRegions );
	rState.m_effective.m_nWorldUploadKb = t3dAdaptiveClampValue( rState.m_effective.m_nWorldUploadKb,
																 rLimits.m_minimum.m_nWorldUploadKb,
																 rLimits.m_maximum.m_nWorldUploadKb );
	rState.m_effective.m_nWorldClusters = t3dAdaptiveClampValue( rState.m_effective.m_nWorldClusters,
																 rLimits.m_minimum.m_nWorldClusters,
																 rLimits.m_maximum.m_nWorldClusters );
	rState.m_effective.m_nVtRequests = t3dAdaptiveClampValue( rState.m_effective.m_nVtRequests,
															  rLimits.m_minimum.m_nVtRequests,
															  rLimits.m_maximum.m_nVtRequests );
	rState.m_effective.m_nVtAdmissions = t3dAdaptiveClampValue( rState.m_effective.m_nVtAdmissions,
																rLimits.m_minimum.m_nVtAdmissions,
																rLimits.m_maximum.m_nVtAdmissions );
}

bool t3dAdaptiveStep(
	const t3dAdaptiveBudgetLimits_t	  &rLimits,
	const t3dAdaptiveBudgetPressure_t &rPressure,
	const uint64_t					   nMeasuredUsec,
	t3dAdaptiveBudgetState_t		  &rState ) {
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
		rState.m_effective.m_nGpuPages =
			t3dAdaptiveDecrease( rState.m_effective.m_nGpuPages, rLimits.m_minimum.m_nGpuPages );
		rState.m_effective.m_nCpuPages =
			t3dAdaptiveDecrease( rState.m_effective.m_nCpuPages, rLimits.m_minimum.m_nCpuPages );
		rState.m_effective.m_nInstancePages =
			t3dAdaptiveDecrease( rState.m_effective.m_nInstancePages, rLimits.m_minimum.m_nInstancePages );
		rState.m_effective.m_nInstancerCells =
			t3dAdaptiveDecrease( rState.m_effective.m_nInstancerCells, rLimits.m_minimum.m_nInstancerCells );
		rState.m_effective.m_nNodeLoadRegions =
			t3dAdaptiveDecrease( rState.m_effective.m_nNodeLoadRegions, rLimits.m_minimum.m_nNodeLoadRegions );
		rState.m_effective.m_nNodeUnloadRegions =
			t3dAdaptiveDecrease( rState.m_effective.m_nNodeUnloadRegions, rLimits.m_minimum.m_nNodeUnloadRegions );
		rState.m_effective.m_nWorldUploadKb =
			t3dAdaptiveDecrease( rState.m_effective.m_nWorldUploadKb, rLimits.m_minimum.m_nWorldUploadKb );
		rState.m_effective.m_nWorldClusters =
			t3dAdaptiveDecrease( rState.m_effective.m_nWorldClusters, rLimits.m_minimum.m_nWorldClusters );
		rState.m_effective.m_nVtRequests =
			t3dAdaptiveDecrease( rState.m_effective.m_nVtRequests, rLimits.m_minimum.m_nVtRequests );
		rState.m_effective.m_nVtAdmissions =
			t3dAdaptiveDecrease( rState.m_effective.m_nVtAdmissions, rLimits.m_minimum.m_nVtAdmissions );
		rState.m_nOverBudgetStreak = 0;
		rState.m_reason = TERRAIN3D_ADAPTIVE_REASON_HARD_LIMIT;
		const bool bChanged = previous.m_nGpuPages != rState.m_effective.m_nGpuPages ||
							  previous.m_nCpuPages != rState.m_effective.m_nCpuPages ||
							  previous.m_nInstancePages != rState.m_effective.m_nInstancePages ||
							  previous.m_nInstancerCells != rState.m_effective.m_nInstancerCells ||
							  previous.m_nNodeLoadRegions != rState.m_effective.m_nNodeLoadRegions ||
							  previous.m_nNodeUnloadRegions != rState.m_effective.m_nNodeUnloadRegions ||
							  previous.m_nWorldUploadKb != rState.m_effective.m_nWorldUploadKb ||
							  previous.m_nWorldClusters != rState.m_effective.m_nWorldClusters ||
							  previous.m_nVtRequests != rState.m_effective.m_nVtRequests ||
							  previous.m_nVtAdmissions != rState.m_effective.m_nVtAdmissions;
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
			rState.m_effective.m_nGpuPages =
				t3dAdaptiveIncrease( rState.m_effective.m_nGpuPages, rLimits.m_maximum.m_nGpuPages );
		}
		if ( rPressure.m_bCpuLimited ) {
			rState.m_effective.m_nCpuPages =
				t3dAdaptiveIncrease( rState.m_effective.m_nCpuPages, rLimits.m_maximum.m_nCpuPages );
		}
		if ( rPressure.m_bInstanceLimited ) {
			rState.m_effective.m_nInstancePages =
				t3dAdaptiveIncrease( rState.m_effective.m_nInstancePages, rLimits.m_maximum.m_nInstancePages );
		}
		if ( rPressure.m_bInstancerLimited ) {
			rState.m_effective.m_nInstancerCells =
				t3dAdaptiveIncrease( rState.m_effective.m_nInstancerCells, rLimits.m_maximum.m_nInstancerCells );
		}
		if ( rPressure.m_bNodeLoadLimited ) {
			rState.m_effective.m_nNodeLoadRegions =
				t3dAdaptiveIncrease( rState.m_effective.m_nNodeLoadRegions, rLimits.m_maximum.m_nNodeLoadRegions );
		}
		if ( rPressure.m_bNodeUnloadLimited ) {
			rState.m_effective.m_nNodeUnloadRegions =
				t3dAdaptiveIncrease( rState.m_effective.m_nNodeUnloadRegions, rLimits.m_maximum.m_nNodeUnloadRegions );
		}
		if ( rPressure.m_bWorldLimited ) {
			rState.m_effective.m_nWorldUploadKb =
				t3dAdaptiveIncrease( rState.m_effective.m_nWorldUploadKb, rLimits.m_maximum.m_nWorldUploadKb );
			rState.m_effective.m_nWorldClusters =
				t3dAdaptiveIncrease( rState.m_effective.m_nWorldClusters, rLimits.m_maximum.m_nWorldClusters );
		}
		if ( rPressure.m_bVtLimited ) {
			rState.m_effective.m_nVtRequests =
				t3dAdaptiveIncrease( rState.m_effective.m_nVtRequests, rLimits.m_maximum.m_nVtRequests );
			rState.m_effective.m_nVtAdmissions =
				t3dAdaptiveIncrease( rState.m_effective.m_nVtAdmissions, rLimits.m_maximum.m_nVtAdmissions );
		}
		rState.m_nUnderBudgetStreak = 0;
		const bool bChanged = previous.m_nGpuPages != rState.m_effective.m_nGpuPages ||
							  previous.m_nCpuPages != rState.m_effective.m_nCpuPages ||
							  previous.m_nInstancePages != rState.m_effective.m_nInstancePages ||
							  previous.m_nInstancerCells != rState.m_effective.m_nInstancerCells ||
							  previous.m_nNodeLoadRegions != rState.m_effective.m_nNodeLoadRegions ||
							  previous.m_nNodeUnloadRegions != rState.m_effective.m_nNodeUnloadRegions ||
							  previous.m_nWorldUploadKb != rState.m_effective.m_nWorldUploadKb ||
							  previous.m_nWorldClusters != rState.m_effective.m_nWorldClusters ||
							  previous.m_nVtRequests != rState.m_effective.m_nVtRequests ||
							  previous.m_nVtAdmissions != rState.m_effective.m_nVtAdmissions;
		rState.m_reason =
			bChanged ? TERRAIN3D_ADAPTIVE_REASON_BACKLOG_RECOVERY : TERRAIN3D_ADAPTIVE_REASON_UNDER_TARGET_NO_BACKLOG;
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
