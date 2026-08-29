// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_terrain_integration.h"

#include <string.h>

bool t3d_terrain_render_world_stage_a_initialize_cold(
        const T3DTerrainRenderWorldGenerations &pInitial, T3DTerrainRenderWorldStageAState &rState ) {
	memset( &rState, 0, sizeof( rState ) );
	rState.Generations = pInitial;
	rState.bInitialized = true;
	return true;
}

bool t3d_terrain_render_world_stage_a_publish_terrain_hot( T3DTerrainRenderWorldStageAState &rState,
        uint64_t pLogicalTopologyGeneration,
        uint64_t pPhysicalPageTableGeneration,
        uint64_t pClipmapPlacementGeneration,
        uint64_t pMaterialGeneration,
        uint64_t pLightingGeneration ) {
	if ( !rState.bInitialized ) {
		return false;
	}
	T3DTerrainRenderWorldGenerations &G = rState.Generations;
	const bool bChanged = G.nLogicalRegionTopologyGeneration != pLogicalTopologyGeneration
	        || G.nTerrainPhysicalPageTableGeneration != pPhysicalPageTableGeneration
	        || G.nClipmapPlacementGeneration != pClipmapPlacementGeneration || G.nMaterialGeneration != pMaterialGeneration
	        || G.nLightingGeneration != pLightingGeneration;
	G.nLogicalRegionTopologyGeneration = pLogicalTopologyGeneration;
	G.nTerrainPhysicalPageTableGeneration = pPhysicalPageTableGeneration;
	G.nClipmapPlacementGeneration = pClipmapPlacementGeneration;
	G.nMaterialGeneration = pMaterialGeneration;
	G.nLightingGeneration = pLightingGeneration;
	if ( bChanged ) {
		++G.nSharedBindingGeneration;
	}
	return true;
}

bool t3d_terrain_render_world_stage_a_publish_vt_hot( T3DTerrainRenderWorldStageAState &rState, uint64_t pVTPageTableGeneration ) {
	if ( !rState.bInitialized ) {
		return false;
	}
	if ( rState.Generations.nRenderWorldVTPageTableGeneration != pVTPageTableGeneration ) {
		rState.Generations.nRenderWorldVTPageTableGeneration = pVTPageTableGeneration;
		++rState.Generations.nSharedBindingGeneration;
	}
	return true;
}

bool t3d_terrain_render_world_stage_a_publish_resource_hot( T3DTerrainRenderWorldStageAState &rState, uint64_t pResourceGeneration ) {
	if ( !rState.bInitialized ) {
		return false;
	}
	if ( rState.Generations.nRenderWorldResourceGeneration != pResourceGeneration ) {
		rState.Generations.nRenderWorldResourceGeneration = pResourceGeneration;
		++rState.Generations.nSharedBindingGeneration;
	}
	return true;
}

bool t3d_terrain_render_world_stage_a_validate_independent_generations_reference_debug( const T3DTerrainRenderWorldGenerations &pBefore,
        const T3DTerrainRenderWorldGenerations &pAfter,
        bool pExpectTerrainPhysicalChange,
        bool pExpectVTChange ) {
	const bool bTerrainChanged = pBefore.nTerrainPhysicalPageTableGeneration != pAfter.nTerrainPhysicalPageTableGeneration;
	const bool bVTChanged = pBefore.nRenderWorldVTPageTableGeneration != pAfter.nRenderWorldVTPageTableGeneration;
	if ( bTerrainChanged != pExpectTerrainPhysicalChange || bVTChanged != pExpectVTChange ) {
		return false;
	}
	if ( !pExpectTerrainPhysicalChange && pBefore.nTerrainPhysicalPageTableGeneration != pAfter.nTerrainPhysicalPageTableGeneration ) {
		return false;
	}
	if ( !pExpectVTChange && pBefore.nRenderWorldVTPageTableGeneration != pAfter.nRenderWorldVTPageTableGeneration ) {
		return false;
	}
	return true;
}
