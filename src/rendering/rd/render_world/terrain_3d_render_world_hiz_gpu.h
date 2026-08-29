// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_HIZ_GPU_H
#define TERRAIN3D_RENDER_WORLD_HIZ_GPU_H

#include "rendering/rd/render_world/terrain_3d_render_world_gpu_visibility.h"
#include "rendering/rd/render_world/terrain_3d_render_world_hiz.h"
#include "rendering/rd/terrain_3d_rd_render_world_shader_bundle.h"

#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/typed_array.hpp>

struct T3DHiZGpuStats {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nDispatchCount;
	uint64_t nBarrierCount;
	uint32_t nHistoryClearCount;
	uint32_t nHotViolationCount;
};

class Terrain3DRenderWorldHiZGpu {
public:
	Terrain3DRenderWorldHiZGpu();
	~Terrain3DRenderWorldHiZGpu();

	bool initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
	        const T3DRenderWorldGpuProfile &pProfile,
	        const godot::RID &pBorrowedPreviousDepth,
	        const t3dRdRenderWorldShaderBundle_t &rShaders );
	void shutdown_cold();
	void seal_hot_runtime();

	bool dispatch_hiz_hot( uint32_t pActiveWidth, uint32_t pActiveHeight, uint32_t pActiveMipCount );
	bool dispatch_visibility_m05_hot( Terrain3DRenderWorldGpuVisibility &rVisibility, const T3DRenderWorldLimits &pLimits );

	bool is_initialized() const;
	const godot::RID &get_borrowed_previous_depth_rid() const;
	const godot::RID &get_cluster_pipeline_rid() const;
	const godot::RID &get_cluster_uniform_set_rid() const;
	const T3DHiZGpuStats &get_stats() const;

private:
	bool create_sampler_cold();
	bool create_mip_views_cold();
	bool create_reduce_shader_and_pipelines_cold( const godot::Ref<godot::RDShaderSPIRV> &rSpirv );
	bool create_cluster_shader_and_pipeline_cold( const godot::Ref<godot::RDShaderSPIRV> &rSpirv );
	bool create_reduce_uniform_sets_cold();
	bool create_cluster_uniform_set_cold();
	bool add_storage_uniform_cold( godot::TypedArray<godot::Ref<godot::RDUniform>> &rUniforms, int32_t pBinding, const godot::RID &pRid );
	bool add_sampler_texture_uniform_cold(
	        godot::TypedArray<godot::Ref<godot::RDUniform>> &rUniforms, int32_t pBinding, const godot::RID &pTexture );
	bool add_image_uniform_cold( godot::TypedArray<godot::Ref<godot::RDUniform>> &rUniforms, int32_t pBinding, const godot::RID &pTexture );
	void free_rid_cold( godot::RID &rRid );
	bool hot_epoch_unchanged() const;

	godot::RenderingDevice *m_pRenderingDevice;
	const Terrain3DRenderWorldResourceImage *m_pResources;
	godot::RID m_hBorrowedPreviousDepth;
	godot::RID m_hSampler;
	godot::RID m_hMipView[T3D_RW_HIZ_MAX_MIPS];
	godot::RID m_hReduceShader;
	godot::RID m_hReducePipeline[T3D_RW_HIZ_MAX_MIPS];
	godot::RID m_hReduceUniformSet[T3D_RW_HIZ_MAX_MIPS];
	godot::RID m_hClusterShader;
	godot::RID m_hClusterPipeline;
	godot::RID m_hClusterUniformSet;
	T3DHiZGpuStats m_Stats;
	uint64_t m_nHotCreateEpoch;
	uint64_t m_nHotFreeEpoch;
	uint32_t m_nMaximumWidth;
	uint32_t m_nMaximumHeight;
	uint32_t m_nMaximumMipCount;
	uint32_t m_nPreviousDepthWidth;
	uint32_t m_nPreviousDepthHeight;
	bool m_bInitialized;
	bool m_bHotSealed;
};

#endif // TERRAIN3D_RENDER_WORLD_HIZ_GPU_H
