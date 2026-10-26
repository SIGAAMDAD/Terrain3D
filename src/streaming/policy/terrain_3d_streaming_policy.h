// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_STREAMING_POLICY_H
#define TERRAIN3D_STREAMING_POLICY_H

#include "Terrain3DTypes.h"

#include "core/config/terrain_3d_compiler.h"

#include <math.h>
#include <stdint.h>
#include <godot_cpp/core/defs.hpp>

// Editor execution mode selects whether the authored repository is fully pinned
// or the shipping residency policy is exercised exactly.
// t3dEditorExecutionMode_e centralized in Terrain3DTypes.h.


// Every logical terrain region has exactly one authoritative runtime state.
// Numeric order is also residency priority.
// t3dRegionStreamingState_e centralized in Terrain3DTypes.h.


// t3dStreamingCoord_s centralized in Terrain3DTypes.h.


// t3dRegionStreamingConfig_s centralized in Terrain3DTypes.h.


// t3dRuntimeStreamingConfig_s centralized in Terrain3DTypes.h.


// t3dRegionStreamingPrediction_s centralized in Terrain3DTypes.h.


EA_FORCE_INLINE bool t3dEditorExecutionModeIsValid( const int nMode ) noexcept
{
	return nMode >= EDITOR_EXECUTION_AUTHORING && nMode <= EDITOR_EXECUTION_RUNTIME_PROFILE;
}

EA_FORCE_INLINE bool t3dIsEditorAuthoringMode( const bool bEditorHint, const int nExecutionMode ) noexcept
{
	return bEditorHint && nExecutionMode == EDITOR_EXECUTION_AUTHORING;
}

EA_FORCE_INLINE bool t3dIsRuntimeProfileMode( const bool bEditorHint, const int nExecutionMode ) noexcept
{
	return bEditorHint && nExecutionMode == EDITOR_EXECUTION_RUNTIME_PROFILE;
}

EA_FORCE_INLINE bool t3dIsGpuPagingEffectivelyEnabled( const bool bConfiguredEnabled ) noexcept
{
	return bConfiguredEnabled;
}

EA_FORCE_INLINE bool t3dIsStreamingEffectivelyEnabled(
    const bool bConfiguredEnabled, const bool bEditorHint, const int nExecutionMode ) noexcept
{
	return bConfiguredEnabled && !t3dIsEditorAuthoringMode( bEditorHint, nExecutionMode );
}

EA_FORCE_INLINE int t3dStreamingChebyshevDistance( const t3dStreamingCoord_t a, const t3dStreamingCoord_t b ) noexcept
{
	const int dx = a.m_nX >= b.m_nX ? a.m_nX - b.m_nX : b.m_nX - a.m_nX;
	const int dy = a.m_nY >= b.m_nY ? a.m_nY - b.m_nY : b.m_nY - a.m_nY;
	return dx > dy ? dx : dy;
}

EA_FORCE_INLINE t3dRuntimeStreamingConfig_t t3dStreamingCompileConfig( const t3dRegionStreamingConfig_t rawConfig ) noexcept
{
	t3dRuntimeStreamingConfig_t config;
	config.m_nActiveRadius = rawConfig.m_nActiveRadius < 0 ? 0 : rawConfig.m_nActiveRadius;
	config.m_nHotRadius = rawConfig.m_nHotRadius < config.m_nActiveRadius ? config.m_nActiveRadius : rawConfig.m_nHotRadius;
	config.m_nColdRadius = rawConfig.m_nColdRadius < config.m_nHotRadius ? config.m_nHotRadius : rawConfig.m_nColdRadius;
	config.m_nUnloadGuard = rawConfig.m_nUnloadGuard < 0 ? 0 : ( rawConfig.m_nUnloadGuard > 4 ? 4 : rawConfig.m_nUnloadGuard );
	config.m_nUnloadRadius = config.m_nColdRadius + config.m_nUnloadGuard;
	config.m_nPredictionMaximumRegions = rawConfig.m_nPredictionMaximumRegions < 0 ? 0 : rawConfig.m_nPredictionMaximumRegions;
	config.m_fPredictionLookaheadSeconds = rawConfig.m_fPredictionLookaheadSeconds < 0.0 ? 0.0 : rawConfig.m_fPredictionLookaheadSeconds;
	config.m_bPredictionEnabled =
	    rawConfig.m_bPredictionEnabled && config.m_nPredictionMaximumRegions > 0 && config.m_fPredictionLookaheadSeconds > 0.0;
	return config;
}

EA_FORCE_INLINE bool t3dStreamingConfigIsValid( const t3dRegionStreamingConfig_t config ) noexcept
{
	return config.m_nActiveRadius >= 0 && config.m_nHotRadius >= config.m_nActiveRadius && config.m_nColdRadius >= config.m_nHotRadius && config.m_nUnloadGuard >= 0 && config.m_nUnloadGuard <= 4 && config.m_fPredictionLookaheadSeconds >= 0.0 && config.m_nPredictionMaximumRegions >= 0;
}

EA_FORCE_INLINE t3dRegionStreamingState_t t3dStreamingClassify(
    const t3dStreamingCoord_t location, const t3dStreamingCoord_t center, const t3dRuntimeStreamingConfig_t& rConfig ) noexcept
{
	const int distance = t3dStreamingChebyshevDistance( location, center );
	return distance <= rConfig.m_nActiveRadius
	           ? TERRAIN3D_REGION_STREAMING_ACTIVE
	           : ( distance <= rConfig.m_nHotRadius ? TERRAIN3D_REGION_STREAMING_HOT
	                                                : ( distance <= rConfig.m_nUnloadRadius ? TERRAIN3D_REGION_STREAMING_COLD
	                                                                                        : TERRAIN3D_REGION_STREAMING_UNLOADED ) );
}

EA_FORCE_INLINE bool t3dStreamingInsideUnloadGuard(
    const t3dStreamingCoord_t location, const t3dStreamingCoord_t center, const t3dRuntimeStreamingConfig_t& rConfig ) noexcept
{
	return t3dStreamingChebyshevDistance( location, center ) <= rConfig.m_nUnloadRadius;
}

inline t3dStreamingCoord_t t3dStreamingClampCenter( const t3dStreamingCoord_t center, const int nMapSize ) noexcept
{
	if ( nMapSize <= 0 ) {
		return {};
	}
	const int minimum = -( nMapSize / 2 );
	const int maximum = minimum + nMapSize - 1;
	return {
		godot::CLAMP( center.m_nX, minimum, maximum ),
		godot::CLAMP( center.m_nY, minimum, maximum ),
	};
}

inline t3dRegionStreamingPrediction_t t3dStreamingPredict( const t3dStreamingCoord_t currentCenter,
                                                           const int nMapSize,
                                                           const double fInstantVelocityXRegionsPerSecond,
                                                           const double fInstantVelocityYRegionsPerSecond,
                                                           const double fPreviousVelocityXRegionsPerSecond,
                                                           const double fPreviousVelocityYRegionsPerSecond,
                                                           const double fDeltaSeconds,
                                                           const t3dRuntimeStreamingConfig_t& rConfig ) noexcept
{
	t3dRegionStreamingPrediction_t result;
	const double delta = godot::MAX( 0.0, fDeltaSeconds );
	// Eight-per-second response is deliberate: prediction follows sustained motion
	// quickly while decaying within a fraction of a second after the target stops.
	const double blend = godot::CLAMP( delta * 8.0, 0.0, 1.0 );
	result.m_fVelocityXRegionsPerSecond =
	    fPreviousVelocityXRegionsPerSecond + ( fInstantVelocityXRegionsPerSecond - fPreviousVelocityXRegionsPerSecond ) * blend;
	result.m_fVelocityYRegionsPerSecond =
	    fPreviousVelocityYRegionsPerSecond + ( fInstantVelocityYRegionsPerSecond - fPreviousVelocityYRegionsPerSecond ) * blend;
	if ( abs( fInstantVelocityXRegionsPerSecond ) < 0.01 ) {
		result.m_fVelocityXRegionsPerSecond *= godot::MAX( 0.0, 1.0 - delta * 10.0 );
	}
	if ( abs( fInstantVelocityYRegionsPerSecond ) < 0.01 ) {
		result.m_fVelocityYRegionsPerSecond *= godot::MAX( 0.0, 1.0 - delta * 10.0 );
	}
	result.m_predictedCenter = t3dStreamingClampCenter( currentCenter, nMapSize );
	if ( !rConfig.m_bPredictionEnabled ) {
		return result;
	}
	const double predictedX = result.m_fVelocityXRegionsPerSecond * rConfig.m_fPredictionLookaheadSeconds;
	const double predictedY = result.m_fVelocityYRegionsPerSecond * rConfig.m_fPredictionLookaheadSeconds;
	const double dominant = godot::MAX( abs( predictedX ), abs( predictedY ) );
	const double scale = dominant > (double)( rConfig.m_nPredictionMaximumRegions ) && dominant > 0.0
	                         ? (double)( rConfig.m_nPredictionMaximumRegions ) / dominant
	                         : 1.0;
	const t3dStreamingCoord_t predicted = {
		currentCenter.m_nX + (int)( lround( predictedX * scale ) ),
		currentCenter.m_nY + (int)( lround( predictedY * scale ) ),
	};
	result.m_predictedCenter = t3dStreamingClampCenter( predicted, nMapSize );
	return result;
}

inline const char *t3dStreamingStateName( const t3dRegionStreamingState_t state ) noexcept
{
	switch ( state ) {
		case TERRAIN3D_REGION_STREAMING_ACTIVE:
			return "Active";
		case TERRAIN3D_REGION_STREAMING_HOT:
			return "Hot";
		case TERRAIN3D_REGION_STREAMING_COLD:
			return "Cold";
		case TERRAIN3D_REGION_STREAMING_UNLOADED:
		default:
			return "Unloaded";
	}
}

#endif // TERRAIN3D_STREAMING_POLICY_H
