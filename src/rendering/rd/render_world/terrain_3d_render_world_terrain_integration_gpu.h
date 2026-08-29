// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_TERRAIN_INTEGRATION_GPU_H
#define TERRAIN3D_RENDER_WORLD_TERRAIN_INTEGRATION_GPU_H

#include "rendering/rd/render_world/terrain_3d_render_world_terrain_integration.h"
#include "rendering/rd/render_world/terrain_3d_render_world_vt_gpu.h"

#include <godot_cpp/variant/rid.hpp>

struct T3DTerrainStageASharedVTBindings {
	godot::RID hAllocationTable;
	godot::RID hPageTable;
	godot::RID hPhysical[T3D_RW_VT_CHANNEL_COUNT][T3D_RW_MAX_VT_TEXTURE_BANKS];
	uint32_t nBankCount[T3D_RW_VT_CHANNEL_COUNT];
	uint32_t nBankLayers[T3D_RW_VT_CHANNEL_COUNT][T3D_RW_MAX_VT_TEXTURE_BANKS];
	uint64_t nVTPageTableGeneration;
	uint64_t nResourceGeneration;
	uint64_t nBindingGeneration;
};

class Terrain3DRenderWorldTerrainStageABridge {
public:
	Terrain3DRenderWorldTerrainStageABridge();
	bool initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
	        const Terrain3DRenderWorldVTGpu *pVTGpu,
	        const T3DVTResidencyState *pResidency,
	        T3DTerrainRenderWorldStageAState *pGenerationState );
	void shutdown_cold();
	bool refresh_shared_bindings_hot( T3DTerrainStageASharedVTBindings &rBindings );
	bool is_initialized() const;

private:
	const Terrain3DRenderWorldResourceImage *m_pResources;
	const Terrain3DRenderWorldVTGpu *m_pVTGpu;
	const T3DVTResidencyState *m_pResidency;
	T3DTerrainRenderWorldStageAState *m_pGenerationState;
	bool m_bInitialized;
};

#endif // TERRAIN3D_RENDER_WORLD_TERRAIN_INTEGRATION_GPU_H
