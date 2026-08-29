// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_AUX_H
#define TERRAIN3D_RENDER_WORLD_AUX_H

#include "rendering/rd/render_world/terrain_3d_render_world_submission.h"
#include "rendering/rd/terrain_3d_rd_render_world_shader_bundle.h"

#include <godot_cpp/variant/rid.hpp>
#include <stdint.h>

enum T3DRenderWorldAuxPass : uint32_t {
	T3D_RW_AUX_PASS_SHADOW = 0u,
	T3D_RW_AUX_PASS_MOTION,
	T3D_RW_AUX_PASS_AUX,
	T3D_RW_AUX_PASS_COUNT
};

struct T3DRenderWorldAuxFormats {
	int64_t nShadowFramebufferFormat;
	int64_t nMotionFramebufferFormat;
	int64_t nAuxFramebufferFormat;
};

struct T3DRenderWorldAuxTelemetry {
	uint64_t nShadowVisibleClusters;
	uint64_t nShadowDrawCount;
	uint64_t nMotionDrawCount;
	uint64_t nAuxDrawCount;
	uint64_t nSubmissionFailureCount;
	uint32_t nLastShadowViewCount;
	uint32_t nReserved;
};

struct T3DRenderWorldAuxBindings {
	godot::RID hPipeline[T3D_RW_AUX_PASS_COUNT][T3D_RW_TIER_COUNT];
	godot::RID hUniformSet[T3D_RW_AUX_PASS_COUNT][T3D_RW_TIER_COUNT];
	uint32_t nUniformSetIndex;
};

class Terrain3DRenderWorldAuxRaster {
public:
	Terrain3DRenderWorldAuxRaster();
	~Terrain3DRenderWorldAuxRaster();

	bool initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
	        const T3DRenderWorldAuxFormats &pFormats,
	        const t3dRdRenderWorldShaderBundle_t &rShaders );
	void shutdown_cold();
	void seal_hot_runtime();
	bool get_bindings_hot( T3DRenderWorldAuxBindings &rBindings );
	bool record_shadow_visibility_hot( uint32_t pShadowViewCount, uint32_t pVisibleClusterCount );
	bool record_pass_draws_hot( T3DRenderWorldAuxPass pPass, uint32_t pDrawCount );

	bool is_initialized() const;
	const T3DRenderWorldAuxTelemetry &get_telemetry() const;

private:
	bool create_shaders_cold( const t3dRdRenderWorldShaderBundle_t &rShaders );
	bool create_pipelines_cold( const T3DRenderWorldAuxFormats &pFormats );
	bool create_uniform_sets_cold();
	void free_rid_cold( godot::RID &rRid );
	bool hot_epoch_unchanged() const;

	godot::RenderingDevice *m_pRenderingDevice;
	const Terrain3DRenderWorldResourceImage *m_pResources;
	godot::RID m_hShader[T3D_RW_AUX_PASS_COUNT];
	godot::RID m_hPipeline[T3D_RW_AUX_PASS_COUNT][T3D_RW_TIER_COUNT];
	godot::RID m_hUniformSet[T3D_RW_AUX_PASS_COUNT][T3D_RW_TIER_COUNT];
	T3DRenderWorldAuxTelemetry m_Telemetry;
	uint64_t m_nGpuCreateCount;
	uint64_t m_nGpuFreeCount;
	uint64_t m_nHotCreateEpoch;
	uint64_t m_nHotFreeEpoch;
	bool m_bInitialized;
	bool m_bHotSealed;
};

bool t3d_render_world_submit_aux_indirect_hot( int64_t pDrawList,
        T3DRenderWorldAuxPass pPass,
        const Terrain3DRenderWorldResourceImage &pResources,
        const T3DRenderWorldAuxBindings &pBindings,
        uint32_t &rDrawCount );

#endif // TERRAIN3D_RENDER_WORLD_AUX_H
