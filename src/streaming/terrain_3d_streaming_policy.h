// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_STREAMING_POLICY_H
#define TERRAIN3D_STREAMING_POLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "core/terrain_3d_compiler.h"

// Editor execution mode selects whether the authored repository is fully pinned
// or the shipping residency policy is exercised exactly.
typedef enum t3deditorExecutionMode_e {
	TERRAIN3D_EDITOR_EXECUTION_AUTHORING = 0,
	TERRAIN3D_EDITOR_EXECUTION_RUNTIME_PROFILE = 1,
} t3deditorExecutionMode_t;

// Every logical terrain region has exactly one authoritative runtime state.
// Numeric order is also residency priority.
typedef enum t3dregionStreamingState_e : uint8_t {
	TERRAIN3D_REGION_STREAMING_UNLOADED = 0,
	TERRAIN3D_REGION_STREAMING_COLD = 1,
	TERRAIN3D_REGION_STREAMING_HOT = 2,
	TERRAIN3D_REGION_STREAMING_ACTIVE = 3,
} t3dregionStreamingState_t;

typedef struct t3dstreamingCoord_s {
	int x = 0;
	int y = 0;
} t3dstreamingCoord_t;

typedef struct t3dregionStreamingConfig_s {
	int activeRadius = 1;
	int hotRadius = 8;
	int coldRadius = 12;
	int unloadGuard = 2;
	bool bPredictionEnabled = true;
	double fPredictionLookaheadSeconds = 0.75;
	int predictionMaximumRegions = 4;
} t3dregionStreamingConfig_t;

typedef struct t3dregionStreamingPrediction_s {
	double fVelocityXRegionsPerSecond = 0.0;
	double fVelocityYRegionsPerSecond = 0.0;
	t3dstreamingCoord_t predictedCenter;
} t3dregionStreamingPrediction_t;

EA_FORCE_INLINE constexpr bool terrain3dEditorExecutionModeIsValid( const int nMode ) noexcept { return nMode >= TERRAIN3D_EDITOR_EXECUTION_AUTHORING && nMode <= TERRAIN3D_EDITOR_EXECUTION_RUNTIME_PROFILE; }

EA_FORCE_INLINE constexpr bool terrain3dIsEditorAuthoringMode( const bool bEditorHint, const int nExecutionMode ) noexcept { return bEditorHint && nExecutionMode == TERRAIN3D_EDITOR_EXECUTION_AUTHORING; }

EA_FORCE_INLINE constexpr bool terrain3dIsRuntimeProfileMode( const bool bEditorHint, const int nExecutionMode ) noexcept { return bEditorHint && nExecutionMode == TERRAIN3D_EDITOR_EXECUTION_RUNTIME_PROFILE; }

EA_FORCE_INLINE constexpr bool terrain3dIsGpuPagingEffectivelyEnabled( const bool bConfiguredEnabled ) noexcept { return bConfiguredEnabled; }

EA_FORCE_INLINE constexpr bool terrain3dIsStreamingEffectivelyEnabled( const bool bConfiguredEnabled, const bool bEditorHint, const int nExecutionMode ) noexcept { return bConfiguredEnabled && !terrain3dIsEditorAuthoringMode( bEditorHint, nExecutionMode ); }

EA_FORCE_INLINE constexpr int terrain3dStreamingChebyshevDistance( const t3dstreamingCoord_t a, const t3dstreamingCoord_t b ) noexcept { const int dx = a.x >= b.x ? a.x - b.x : b.x - a.x; const int dy = a.y >= b.y ? a.y - b.y : b.y - a.y; return dx > dy ? dx : dy; }

constexpr t3dregionStreamingConfig_t terrain3dStreamingSanitizeConfig(
	const t3dregionStreamingConfig_t config
) noexcept {
	t3dregionStreamingConfig_t result = config;
	result.activeRadius = result.activeRadius < 0 ? 0 : result.activeRadius;
	result.hotRadius = result.hotRadius < result.activeRadius ? result.activeRadius : result.hotRadius;
	result.coldRadius = result.coldRadius < result.hotRadius ? result.hotRadius : result.coldRadius;
	result.unloadGuard = result.unloadGuard < 0 ? 0 : ( result.unloadGuard > 4 ? 4 : result.unloadGuard );
	result.fPredictionLookaheadSeconds = result.fPredictionLookaheadSeconds < 0.0 ? 0.0 :
		result.fPredictionLookaheadSeconds;
	result.predictionMaximumRegions = result.predictionMaximumRegions < 0 ? 0 :
		result.predictionMaximumRegions;
	return result;
}

EA_FORCE_INLINE constexpr bool terrain3dStreamingConfigIsValid( const t3dregionStreamingConfig_t config ) noexcept { return config.activeRadius >= 0 && config.hotRadius >= config.activeRadius && config.coldRadius >= config.hotRadius && config.unloadGuard >= 0 && config.unloadGuard <= 4 && config.fPredictionLookaheadSeconds >= 0.0 && config.predictionMaximumRegions >= 0; }

constexpr t3dregionStreamingState_t terrain3dStreamingClassify(
	const t3dstreamingCoord_t location,
	const t3dstreamingCoord_t center,
	const t3dregionStreamingConfig_t rawConfig
) noexcept {
	const t3dregionStreamingConfig_t config = terrain3dStreamingSanitizeConfig( rawConfig );
	const int distance = terrain3dStreamingChebyshevDistance( location, center );
	return distance <= config.activeRadius ? TERRAIN3D_REGION_STREAMING_ACTIVE :
		( distance <= config.hotRadius ? TERRAIN3D_REGION_STREAMING_HOT :
			( distance <= config.coldRadius + config.unloadGuard ? TERRAIN3D_REGION_STREAMING_COLD :
				TERRAIN3D_REGION_STREAMING_UNLOADED ) );
}

EA_FORCE_INLINE constexpr bool terrain3dStreamingInsideUnloadGuard( const t3dstreamingCoord_t location, const t3dstreamingCoord_t center, const t3dregionStreamingConfig_t rawConfig ) noexcept { const t3dregionStreamingConfig_t config = terrain3dStreamingSanitizeConfig( rawConfig ); return terrain3dStreamingChebyshevDistance( location, center ) <= config.coldRadius + config.unloadGuard; }

inline t3dstreamingCoord_t terrain3dStreamingClampCenter(
	const t3dstreamingCoord_t center,
	const int nMapSize
) noexcept {
	if ( nMapSize <= 0 ) {
		return {};
	}
	const int minimum = -( nMapSize / 2 );
	const int maximum = minimum + nMapSize - 1;
	return {
		std::clamp( center.x, minimum, maximum ),
		std::clamp( center.y, minimum, maximum ),
	};
}

inline t3dregionStreamingPrediction_t terrain3dStreamingPredict(
	const t3dstreamingCoord_t currentCenter,
	const int nMapSize,
	const double fInstantVelocityXRegionsPerSecond,
	const double fInstantVelocityYRegionsPerSecond,
	const double fPreviousVelocityXRegionsPerSecond,
	const double fPreviousVelocityYRegionsPerSecond,
	const double fDeltaSeconds,
	const t3dregionStreamingConfig_t rawConfig
) noexcept {
	const t3dregionStreamingConfig_t config = terrain3dStreamingSanitizeConfig( rawConfig );
	t3dregionStreamingPrediction_t result;
	const double delta = std::max( 0.0, fDeltaSeconds );
	// Eight-per-second response is deliberate: prediction follows sustained motion
	// quickly while decaying within a fraction of a second after the target stops.
	const double blend = std::clamp( delta * 8.0, 0.0, 1.0 );
	result.fVelocityXRegionsPerSecond = fPreviousVelocityXRegionsPerSecond +
		( fInstantVelocityXRegionsPerSecond - fPreviousVelocityXRegionsPerSecond ) * blend;
	result.fVelocityYRegionsPerSecond = fPreviousVelocityYRegionsPerSecond +
		( fInstantVelocityYRegionsPerSecond - fPreviousVelocityYRegionsPerSecond ) * blend;
	if ( std::abs( fInstantVelocityXRegionsPerSecond ) < 0.01 ) {
		result.fVelocityXRegionsPerSecond *= std::max( 0.0, 1.0 - delta * 10.0 );
	}
	if ( std::abs( fInstantVelocityYRegionsPerSecond ) < 0.01 ) {
		result.fVelocityYRegionsPerSecond *= std::max( 0.0, 1.0 - delta * 10.0 );
	}
	result.predictedCenter = terrain3dStreamingClampCenter( currentCenter, nMapSize );
	if ( !config.bPredictionEnabled || config.predictionMaximumRegions <= 0 ||
		config.fPredictionLookaheadSeconds <= 0.0 ) {
		return result;
	}
	const double predictedX = result.fVelocityXRegionsPerSecond * config.fPredictionLookaheadSeconds;
	const double predictedY = result.fVelocityYRegionsPerSecond * config.fPredictionLookaheadSeconds;
	const double dominant = std::max( std::abs( predictedX ), std::abs( predictedY ) );
	const double scale = dominant > ( double ) ( config.predictionMaximumRegions ) && dominant > 0.0 ?
		( double ) ( config.predictionMaximumRegions ) / dominant : 1.0;
	const t3dstreamingCoord_t predicted = {
		currentCenter.x + ( int ) ( std::lround( predictedX * scale ) ),
		currentCenter.y + ( int ) ( std::lround( predictedY * scale ) ),
	};
	result.predictedCenter = terrain3dStreamingClampCenter( predicted, nMapSize );
	return result;
}

inline const char *terrain3dStreamingStateName( const t3dregionStreamingState_t state ) noexcept {
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
