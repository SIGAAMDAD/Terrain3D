// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_terrain_integration_gpu.h"

#include <string.h>

Terrain3DRenderWorldTerrainStageABridge::Terrain3DRenderWorldTerrainStageABridge() {
	m_pResources = NULL;
	m_pVTGpu = NULL;
	m_pResidency = NULL;
	m_pGenerationState = NULL;
	m_bInitialized = false;
}

bool Terrain3DRenderWorldTerrainStageABridge::initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
        const Terrain3DRenderWorldVTGpu *pVTGpu,
        const T3DVTResidencyState *pResidency,
        T3DTerrainRenderWorldStageAState *pGenerationState ) {
	if ( m_bInitialized || pResources == NULL || !pResources->is_initialized() || pVTGpu == NULL || !pVTGpu->is_initialized()
	        || pResidency == NULL || !pResidency->bInitialized || pGenerationState == NULL || !pGenerationState->bInitialized ) {
		return false;
	}
	m_pResources = pResources;
	m_pVTGpu = pVTGpu;
	m_pResidency = pResidency;
	m_pGenerationState = pGenerationState;
	m_bInitialized = true;
	return true;
}

void Terrain3DRenderWorldTerrainStageABridge::shutdown_cold() {
	m_pResources = NULL;
	m_pVTGpu = NULL;
	m_pResidency = NULL;
	m_pGenerationState = NULL;
	m_bInitialized = false;
}

bool Terrain3DRenderWorldTerrainStageABridge::refresh_shared_bindings_hot( T3DTerrainStageASharedVTBindings &rBindings ) {
	if ( !m_bInitialized || m_pResources == NULL || m_pResidency == NULL || m_pGenerationState == NULL ) {
		return false;
	}
	rBindings = T3DTerrainStageASharedVTBindings();
	rBindings.hAllocationTable = m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE );
	rBindings.hPageTable = m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_PAGE_TABLE );
	if ( !rBindings.hAllocationTable.is_valid() || !rBindings.hPageTable.is_valid() ) {
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		rBindings.nBankCount[c] = m_pResources->get_vt_bank_count( ( T3DRenderWorldVTChannel )c );
		for ( uint32_t b = 0u; b < rBindings.nBankCount[c]; ++b ) {
			rBindings.hPhysical[c][b] = m_pResources->get_vt_bank_rid( ( T3DRenderWorldVTChannel )c, b );
			rBindings.nBankLayers[c][b] = m_pResources->get_vt_bank_layer_count( ( T3DRenderWorldVTChannel )c, b );
			if ( !rBindings.hPhysical[c][b].is_valid() || rBindings.nBankLayers[c][b] == 0u ) {
				return false;
			}
		}
	}
	if ( !t3d_terrain_render_world_stage_a_publish_vt_hot( *m_pGenerationState, m_pResidency->nPageTableGeneration ) ) {
		return false;
	}
	if ( !t3d_terrain_render_world_stage_a_publish_resource_hot( *m_pGenerationState, m_pResources->get_stats().nResourceGeneration ) ) {
		return false;
	}
	rBindings.nVTPageTableGeneration = m_pGenerationState->Generations.nRenderWorldVTPageTableGeneration;
	rBindings.nResourceGeneration = m_pGenerationState->Generations.nRenderWorldResourceGeneration;
	rBindings.nBindingGeneration = m_pGenerationState->Generations.nSharedBindingGeneration;
	return true;
}

bool Terrain3DRenderWorldTerrainStageABridge::is_initialized() const {
	return m_bInitialized;
}
