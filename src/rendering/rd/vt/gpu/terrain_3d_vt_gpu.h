// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_GPU_H
#define TERRAIN3D_VT_GPU_H

#include "Terrain3DTypes.h"

#include "rendering/rd/vt/gpu/terrain_3d_vt_resources.h"
#include "rendering/rd/vt/streaming/terrain_3d_vt_streaming.h"

#include <godot_cpp/variant/callable.hpp>
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_uniform_set.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rid.hpp>

// t3dVTGpuStats_s centralized in Terrain3DTypes.h.


class Terrain3DVTGpu {
public:
	Terrain3DVTGpu();
	~Terrain3DVTGpu();

	bool prepare_cpu_cold( const t3dVTGpuResourcePlan_t &pPlan, const t3dWorldLimits_t &pLimits );
	bool prepare_storage_cold( const Terrain3DVTResourceImage *pResources,
	        const t3dWorldLimits_t &pLimits );
	bool bind_executable_cold();
	bool service_bind_executable_cold( uint32_t nMaxWorkUnits );
	bool initialize_cold( const Terrain3DVTResourceImage *pResources,
	        const t3dWorldLimits_t &pLimits );
	void shutdown_cold( bool bPreserveTailStaging = false );
	void seal_hot_runtime();
	bool begin_package_publication_warm();
	void set_database_view_warm( const t3dVTDatabaseView_t *pDatabase ) { m_pDatabaseView = pDatabase; }
	bool upload_database_warm( const t3dVTDatabaseView_t &pDatabase, const t3dVTResidencyState_t &pResidency );
	bool service_database_upload_warm( const t3dVTDatabaseView_t &pDatabase, const t3dVTResidencyState_t &pResidency,
	        uint64_t nMaxBytes, uint32_t nMaxWorkUnits );
	bool is_database_upload_pending_warm() const;
	bool update_allocation_quality_hot( const t3dVTResidencyState_t &rResidency );
	bool upload_neutral_page_set_warm( t3dVTResidencyState_t &rResidency, uint32_t pPhysicalSlot );
	bool service_retired_slots_hot( t3dVTResidencyState_t &rResidency, uint32_t pFrameId );
	bool begin_upload_batch_hot();
	bool end_upload_batch_hot( t3dVTResidencyState_t &rResidency, uint32_t pFrameId );
	bool upload_page_set_commit_hot( t3dVTResidencyState_t &rResidency,
	        const t3dVTUploadReservation_t &pReservation,
	        const t3dVTPageSetPayloadView_t &pPayloads,
	        uint32_t pFrameId );
	bool upload_commit_hot( t3dVTResidencyState_t &rResidency,
	        const t3dVTUploadReservation_t &pReservation,
	        const uint8_t *pDecodedBytes,
	        uint32_t pDecodedByteCount,
	        uint32_t pFrameId );
	static bool upload_batch_begin_callback( void *pUser );
	static bool upload_batch_end_callback( t3dVTResidencyState_t &rResidency, uint32_t pFrameId, void *pUser );
	static bool upload_commit_callback( t3dVTResidencyState_t &rResidency,
	        const t3dVTUploadReservation_t &pReservation,
	        const uint8_t *pDecodedBytes,
	        uint32_t pDecodedByteCount,
	        uint32_t pFrameId,
	        void *pUser );

	bool is_initialized() const;
	bool is_storage_ready() const;
	bool is_executable_ready() const;
	bool is_readback_enabled() const;
	bool has_fragment_feedback_callback() const { return m_feedbackCallback.is_valid(); }
	void set_fragment_feedback_callback_cold( const godot::Callable &rCallback ) { m_feedbackCallback = rCallback; }
	void consume_fragment_feedback_readback_hot( const godot::PackedByteArray &rBytes );
	uint32_t service_fragment_feedback_hot( t3dVTResidencyState_t &rResidency, t3dVTStreamingState_t &rStreaming, uint32_t nFrameId, uint32_t nRequestBudget );
	godot::RID get_fragment_feedback_set_cold( const godot::RID &rShader, uint32_t nVariant );
	uint64_t get_fragment_feedback_consumed() const { return m_nFeedbackConsumed; }
	uint64_t get_fragment_feedback_misses() const { return m_nFeedbackMisses; }
	uint64_t get_fragment_feedback_overflow() const { return m_nFeedbackOverflow; }
	uint64_t get_fragment_feedback_dropped() const { return m_nFeedbackDropped; }
	bool get_physical_family_binding_v3( uint32_t pFamily, godot::RID &rRid, uint32_t &rLayerCount ) const;
	bool validate_physical_format_families_v3_cold() const;
	uint64_t get_reserved_gpu_bytes() const;
	const t3dVTGpuStats_t &get_stats() const;
	const t3dVTGpuResourcePlan_t &get_cpu_resource_plan() const { return m_Plan; }

private:
	bool update_page_entry_hot( uint32_t pVirtualPage, const t3dVTPageTableEntry_t &pEntry );
	bool publish_upload_batch_page_table_hot( const t3dVTResidencyState_t &rResidency );
	void abort_upload_batch_hot( t3dVTResidencyState_t &rResidency );
	bool make_gpu_page_entry( uint32_t nVirtualPage, const t3dVTPageTableEntry_t &pCpuEntry, t3dVTPageTableEntry_t &rGpuEntry ) const;
	bool retire_victim_gpu_hot( t3dVTResidencyState_t &rResidency, uint32_t pPhysicalSlot, uint32_t pVictimPage, uint32_t pFrameId );
	bool map_physical_slot_hot( uint32_t pPhysicalSlot, uint32_t &rBank, uint32_t &rLayer ) const;
	bool validate_physical_bank_layout_cold() const;
	bool initialize_neutral_payloads_cold();
	uint32_t physical_layer_bytes( uint32_t pChannel ) const;
	bool hot_epoch_unchanged() const;

	godot::Callable m_feedbackCallback;
	godot::PackedByteArray m_feedbackReset;
	godot::PackedByteArray m_feedbackCounterReset;
	godot::PackedByteArray m_feedbackParams;
	godot::PackedByteArray m_feedbackReadback;
	Terrain3DRdUniformSet m_feedbackSets[2u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT];
	godot::RID m_feedbackSetShaders[2u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT];
	uint32_t m_nFeedbackEpoch = 1u;
	uint32_t m_nFeedbackFrame = UINT32_MAX;
	uint32_t m_nFeedbackDatabase = 0u;
	bool m_bFeedbackPending = false;
	bool m_bFeedbackCountersPending = false;
	bool m_bFeedbackWritten = false;
	uint64_t m_nFeedbackConsumed = 0u;
	uint64_t m_nFeedbackDropped = 0u;
	uint64_t m_nFeedbackMisses = 0u;
	uint64_t m_nFeedbackOverflow = 0u;
	godot::RenderingDevice *m_pRenderingDevice;
	const Terrain3DVTResourceImage *m_pResources;
	const t3dVTDatabaseView_t *m_pDatabaseView;
	t3dWorldLimits_t m_Limits;
	t3dVTGpuResourcePlan_t m_Plan;
	Terrain3DMemoryReservation m_stagingMemory;
	godot::PackedByteArray m_AllocationUpload;
	godot::PackedByteArray m_DatabaseUpload;
	godot::PackedByteArray m_PageEntryUpload;
	godot::PackedInt32Array m_RetiredPageScratch;
	godot::PackedInt32Array m_RetiredSlotScratch;
	godot::PackedInt32Array m_UploadBatchOrder;
	godot::PackedByteArray m_UploadBatchReservations;
	godot::PackedByteArray m_PhysicalUpload[T3D_VT_CHANNEL_COUNT];
	godot::PackedByteArray m_NeutralPhysicalUpload[T3D_VT_CHANNEL_COUNT];
	godot::PackedByteArray m_NeutralTailUpload[T3D_VT_CHANNEL_COUNT];
	t3dVTGpuStats_t m_Stats;
	uint64_t m_nHotCreateEpoch;
	uint64_t m_nHotFreeEpoch;
	uint32_t m_nUploadBatchCount;
	uint32_t m_nDatabaseUploadStage;
	uint32_t m_nDatabaseTailChannel;
	bool m_bUploadBatchActive;
	bool m_bDatabaseUploadBegun;
	bool m_bCpuPrepared;
	bool m_bStorageReady;
	bool m_bExecutableReady;
	bool m_bInitialized;
	bool m_bDatabaseUploadedWarm;
	bool m_bHotSealed;
};

#endif // TERRAIN3D_VT_GPU_H
