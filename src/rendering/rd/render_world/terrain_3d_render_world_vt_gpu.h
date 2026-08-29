// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_VT_GPU_H
#define TERRAIN3D_RENDER_WORLD_VT_GPU_H

#include "rendering/rd/render_world/terrain_3d_render_world_resources.h"
#include "rendering/rd/render_world/terrain_3d_render_world_streaming.h"
#include "rendering/rd/terrain_3d_rd_render_world_shader_bundle.h"

#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

struct T3DVTGpuStats {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nDispatchCount;
	uint64_t nBarrierCount;
	uint64_t nAsyncReadbackCount;
	uint64_t nUploadBytes;
	uint64_t nReadbackConsumeCount;
	uint64_t nAcceptedRequestCount;
	uint64_t nRetireRetryCount;
	uint32_t nReadbackRingBusyCount;
	uint32_t nReadbackRejectCount;
	uint32_t nHotViolationCount;
	uint32_t nReserved;
};

class Terrain3DRenderWorldVTGpu {
public:
	Terrain3DRenderWorldVTGpu();
	~Terrain3DRenderWorldVTGpu();

	bool initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
	        const T3DRenderWorldLimits &pLimits,
	        const godot::Callable pReadbackCallbacks[T3D_RW_GPU_FRAME_RING_COUNT],
	        const t3dRdRenderWorldShaderBundle_t &rShaders );
	void shutdown_cold();
	void seal_hot_runtime();
	bool upload_database_warm( const T3DVTDatabaseView &pDatabase, const T3DVTResidencyState &pResidency );
	bool upload_neutral_backing_warm( T3DVTResidencyState &rResidency,
	        uint32_t pChannel,
	        uint32_t pPhysicalSlot,
	        const uint8_t *pDecodedBytes,
	        uint32_t pDecodedByteCount );
	bool dispatch_prediction_hot( const T3DRenderWorldLimits &pLimits );
	bool schedule_request_readback_hot( uint32_t pFrameId );
	bool consume_request_readback_owner_hot( uint32_t pSlot,
	        uint32_t pFrameId,
	        const uint8_t *pReadbackBytes,
	        uint32_t pReadbackByteCount,
	        T3DVTStreamingState &rStreaming,
	        uint32_t &rAcceptedRequestCount );
	bool service_retired_slots_hot( T3DVTResidencyState &rResidency );
	bool upload_commit_hot( T3DVTResidencyState &rResidency,
	        const T3DVTUploadReservation &pReservation,
	        const uint8_t *pDecodedBytes,
	        uint32_t pDecodedByteCount,
	        uint32_t pFrameId );
	static bool upload_commit_callback( T3DVTResidencyState &rResidency,
	        const T3DVTUploadReservation &pReservation,
	        const uint8_t *pDecodedBytes,
	        uint32_t pDecodedByteCount,
	        uint32_t pFrameId,
	        void *pUser );

	bool is_initialized() const;
	bool is_readback_enabled() const;
	const T3DVTGpuStats &get_stats() const;

private:
	enum PipelineId : uint32_t {
		PIPE_RESET = 0,
		PIPE_PREDICT,
		PIPE_COMPACT,
		PIPE_COUNT
	};
	bool create_pipeline_cold( PipelineId pId, const godot::Ref<godot::RDShaderSPIRV> &rSpirv );
	bool create_uniform_sets_cold();
	bool create_buffer_uniform_set_cold( PipelineId pId, const T3DRenderWorldGpuBufferId *pBuffers, uint32_t pCount, godot::RID &rSet );
	bool create_predict_uniform_set_cold( T3DRenderWorldGpuBufferId pVisibleBuffer, godot::RID &rSet );
	bool update_page_entry_hot( uint32_t pVirtualPage, uint32_t pChannel, const T3DVTPageTableEntry &pEntry );
	bool make_gpu_page_entry( uint32_t pChannel, const T3DVTPageTableEntry &pCpuEntry, T3DVTPageTableEntry &rGpuEntry ) const;
	bool retire_victim_gpu_hot( T3DVTResidencyState &rResidency, uint32_t pChannel, uint32_t pPhysicalSlot, uint32_t pVictimPage );
	bool map_physical_slot_hot( uint32_t pChannel, uint32_t pPhysicalSlot, uint32_t &rBank, uint32_t &rLayer ) const;
	uint32_t physical_layer_bytes( uint32_t pChannel ) const;
	void free_rid_cold( godot::RID &rRid );
	bool hot_epoch_unchanged() const;

	godot::RenderingDevice *m_pRenderingDevice;
	const Terrain3DRenderWorldResourceImage *m_pResources;
	T3DRenderWorldLimits m_Limits;
	godot::RID m_hShader[PIPE_COUNT];
	godot::RID m_hPipeline[PIPE_COUNT];
	godot::RID m_hUniformReset;
	godot::RID m_hUniformPredict[T3D_RW_TIER_COUNT];
	godot::RID m_hUniformCompact;
	godot::PackedByteArray m_TierPush[T3D_RW_TIER_COUNT];
	godot::PackedByteArray m_PageEntryUpload;
	godot::PackedByteArray m_PhysicalUpload[T3D_RW_VT_CHANNEL_COUNT];
	godot::Callable m_ReadbackCallback[T3D_RW_GPU_FRAME_RING_COUNT];
	bool m_bReadbackInFlight[T3D_RW_GPU_FRAME_RING_COUNT];
	uint32_t m_nReadbackFrameId[T3D_RW_GPU_FRAME_RING_COUNT];
	T3DVTGpuStats m_Stats;
	uint64_t m_nHotCreateEpoch;
	uint64_t m_nHotFreeEpoch;
	bool m_bInitialized;
	bool m_bDatabaseUploadedWarm;
	bool m_bReadbackEnabled;
	bool m_bHotSealed;
};

#endif // TERRAIN3D_RENDER_WORLD_VT_GPU_H
