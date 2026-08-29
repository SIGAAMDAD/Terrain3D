// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_RASTER_H
#define TERRAIN3D_RENDER_WORLD_RASTER_H

#include "rendering/rd/render_world/terrain_3d_render_world_submission.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/rd/terrain_3d_rd_render_world_shader_bundle.h"

#include <godot_cpp/variant/rid.hpp>

struct T3DRasterResourceStats {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nPipelineCacheHitCount;
	uint64_t nPipelineCacheMissCount;
	uint32_t nHotViolationCount;
	uint32_t nReserved;
};

class Terrain3DRenderWorldThreeTierRaster {
public:
	Terrain3DRenderWorldThreeTierRaster();
	~Terrain3DRenderWorldThreeTierRaster();

	bool initialize_cold( const Terrain3DRenderWorldResourceImage *pResources, const t3dRdRenderWorldShaderBundle_t &rShaders );
	bool prepare_forward_cold( const t3dRdForwardPassContext_t &pContext, Terrain3DRdForwardLighting &rLighting );
	void shutdown_cold();
	void seal_hot_runtime();
	void reseal_hot_runtime_cold();

	bool get_bindings_hot( T3DThreeTierRasterBindings &rBindings );
	bool is_initialized() const;
	const T3DRasterResourceStats &get_stats() const;

private:
	struct T3DPipelineKey {
		int64_t nFramebufferFormat;
		t3dRdForwardShaderVariant_t eVariant;
		godot::RenderingDevice::TextureSamples eSamples;
		uint32_t nViewCount;
		bool bReverseCull;
		bool bDepthPrepassActive;

		bool operator==( const T3DPipelineKey &rOther ) const {
			return nFramebufferFormat == rOther.nFramebufferFormat && eVariant == rOther.eVariant && eSamples == rOther.eSamples
			        && nViewCount == rOther.nViewCount && bReverseCull == rOther.bReverseCull
			        && bDepthPrepassActive == rOther.bDepthPrepassActive;
		}
	};

	struct T3DPipelineEntry {
		T3DPipelineKey Key;
		godot::RID hPipeline[T3D_RW_TIER_COUNT];
	};

	static const uint32_t T3D_RW_FORWARD_COLOR_VARIANT_COUNT = T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT;
	static const uint32_t T3D_RW_PIPELINE_CACHE_CAPACITY = 16u;

	bool create_shaders_cold( const t3dRdRenderWorldShaderBundle_t &rShaders );
	bool create_sampler_cold();
	bool create_uniform_sets_cold();
	bool select_forward_variant( const t3dRdForwardPassContext_t &pContext, t3dRdForwardShaderVariant_t &rVariant ) const;
	bool ensure_forward_pipelines_cold( const t3dRdForwardPassContext_t &pContext, t3dRdForwardShaderVariant_t pVariant );
	T3DPipelineEntry *find_pipeline_entry_cold( const T3DPipelineKey &pKey );
	const T3DPipelineEntry *find_pipeline_entry_hot( const T3DPipelineKey &pKey ) const;
	void free_pipeline_entry_cold( T3DPipelineEntry &rEntry );
	void free_rid_cold( godot::RID &rRid );
	bool hot_epoch_unchanged() const;

	godot::RenderingDevice *m_pRenderingDevice;
	const Terrain3DRenderWorldResourceImage *m_pResources;
	godot::RID m_hShader[T3D_RW_FORWARD_COLOR_VARIANT_COUNT];
	godot::RID m_hVtSampler;
	godot::RID m_hUniformSet[T3D_RW_FORWARD_COLOR_VARIANT_COUNT][T3D_RW_TIER_COUNT];
	T3DPipelineEntry m_PipelineCache[T3D_RW_PIPELINE_CACHE_CAPACITY];
	uint32_t m_nPipelineCount;
	uint32_t m_nPipelineEvictCursor;
	T3DPipelineKey m_CurrentPipelineKey;
	godot::RID m_hCurrentLightingSet;
	T3DRasterResourceStats m_Stats;
	uint64_t m_nHotCreateEpoch;
	uint64_t m_nHotFreeEpoch;
	bool m_bCurrentPipelineValid;
	bool m_bInitialized;
	bool m_bHotSealed;
};

#endif // TERRAIN3D_RENDER_WORLD_RASTER_H
