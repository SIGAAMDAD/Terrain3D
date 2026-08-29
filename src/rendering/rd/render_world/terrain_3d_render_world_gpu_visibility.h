// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_GPU_VISIBILITY_H
#define TERRAIN3D_RENDER_WORLD_GPU_VISIBILITY_H

#include "rendering/rd/render_world/terrain_3d_render_world_resources.h"
#include "rendering/rd/terrain_3d_rd_render_world_shader_bundle.h"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

struct T3DGpuVisibilityStats {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nDispatchCount;
	uint64_t nBarrierCount;
	uint64_t nFrameUploadBytes;
	uint32_t nFrameUploadCount;
	uint32_t nHotViolationCount;
};

class Terrain3DRenderWorldGpuVisibility {
public:
	Terrain3DRenderWorldGpuVisibility();
	~Terrain3DRenderWorldGpuVisibility();

	bool initialize_cold( const Terrain3DRenderWorldResourceImage *pResources, const t3dRdRenderWorldShaderBundle_t &rShaders );
	void shutdown_cold();
	void seal_hot_runtime();

	bool upload_frame_reference_debug( const T3DVisibilityFrame &pFrame );
	bool publish_frame_hot( const T3DVisibilityFrame &pFrame );
	bool dispatch_m04_hot( const T3DRenderWorldLimits &pLimits );
	bool dispatch_with_cluster_override_hot(
	        const T3DRenderWorldLimits &pLimits, const godot::RID &pClusterPipeline, const godot::RID &pClusterUniformSet );

	bool is_initialized() const;
	const T3DGpuVisibilityStats &get_stats() const;
	const godot::RID &get_cluster_pipeline_rid() const;
	const godot::RID &get_cluster_uniform_set_rid() const;

private:
	enum PipelineId : uint32_t {
		PIPELINE_RESET = 0,
		PIPELINE_CELL,
		PIPELINE_INSTANCE,
		PIPELINE_CLUSTER,
		PIPELINE_ARGS,
		PIPELINE_COUNT
	};

	bool upload_frame_internal( const T3DVisibilityFrame &pFrame );
	bool dispatch_internal_hot(
	        const T3DRenderWorldLimits &pLimits, const godot::RID &pClusterPipeline, const godot::RID &pClusterUniformSet );
	bool create_pipeline_cold( PipelineId pId, const godot::Ref<godot::RDShaderSPIRV> &rSpirv );
	bool create_uniform_sets_cold();
	bool create_uniform_set_cold( PipelineId pId, const T3DRenderWorldGpuBufferId *pBuffers, uint32_t pBufferCount );
	void free_rid_cold( godot::RID &rRid );
	bool hot_epoch_unchanged() const;

	godot::RenderingDevice *m_pRenderingDevice;
	const Terrain3DRenderWorldResourceImage *m_pResources;
	godot::RID m_hShader[PIPELINE_COUNT];
	godot::RID m_hPipeline[PIPELINE_COUNT];
	godot::RID m_hUniformSet[PIPELINE_COUNT];
	godot::PackedByteArray m_FrameUpload;
	T3DGpuVisibilityStats m_Stats;
	uint64_t m_nHotCreateEpoch;
	uint64_t m_nHotFreeEpoch;
	bool m_bInitialized;
	bool m_bHotSealed;
};

#endif // TERRAIN3D_RENDER_WORLD_GPU_VISIBILITY_H
