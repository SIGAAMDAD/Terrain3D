// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_TERRAIN_INTEGRATION_H
#define TERRAIN3D_RENDER_WORLD_TERRAIN_INTEGRATION_H

#include <stdint.h>

struct T3DTerrainRenderWorldGenerations {
	uint64_t nLogicalRegionTopologyGeneration;
	uint64_t nTerrainPhysicalPageTableGeneration;
	uint64_t nClipmapPlacementGeneration;
	uint64_t nMaterialGeneration;
	uint64_t nLightingGeneration;
	uint64_t nRenderWorldVTPageTableGeneration;
	uint64_t nRenderWorldResourceGeneration;
	uint64_t nSharedBindingGeneration;
};

struct T3DTerrainRenderWorldStageAState {
	T3DTerrainRenderWorldGenerations Generations;
	bool bInitialized;
};

bool t3d_terrain_render_world_stage_a_initialize_cold(
        const T3DTerrainRenderWorldGenerations &pInitial, T3DTerrainRenderWorldStageAState &rState );
bool t3d_terrain_render_world_stage_a_publish_terrain_hot( T3DTerrainRenderWorldStageAState &rState,
        uint64_t pLogicalTopologyGeneration,
        uint64_t pPhysicalPageTableGeneration,
        uint64_t pClipmapPlacementGeneration,
        uint64_t pMaterialGeneration,
        uint64_t pLightingGeneration );
bool t3d_terrain_render_world_stage_a_publish_vt_hot( T3DTerrainRenderWorldStageAState &rState, uint64_t pVTPageTableGeneration );
bool t3d_terrain_render_world_stage_a_publish_resource_hot( T3DTerrainRenderWorldStageAState &rState, uint64_t pResourceGeneration );
bool t3d_terrain_render_world_stage_a_validate_independent_generations_reference_debug( const T3DTerrainRenderWorldGenerations &pBefore,
        const T3DTerrainRenderWorldGenerations &pAfter,
        bool pExpectTerrainPhysicalChange,
        bool pExpectVTChange );

#endif // TERRAIN3D_RENDER_WORLD_TERRAIN_INTEGRATION_H
