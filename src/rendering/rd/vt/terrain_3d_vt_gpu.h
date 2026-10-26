// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_GPU_H
#define TERRAIN3D_VT_GPU_H

#include "rendering/rd/vt/terrain_3d_vt_resources.h"
#include "rendering/rd/vt/terrain_3d_vt_streaming.h"

#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rid.hpp>

struct T3DVTGpuStats {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nDispatchCount;
	uint64_t nComputeListBarrierCount;
	uint64_t nAsyncReadbackCount;
	uint64_t nUploadBytes;
	uint64_t nReadbackConsumeCount;
	uint64_t nAcceptedRequestCount;
	uint64_t nReadbackBytes;
	uint64_t nVariableTierDispatchCount;
	uint64_t nRetireRetryCount;
	uint64_t nPageTableUpdateBytes;
	uint64_t nPageTableUpdateCalls;
	uint64_t nPageTableBatchCount;
	uint64_t nPageTableBatchEntries;
	uint64_t nRequestAttempts;
	uint64_t nRequestUnique;
	uint64_t nRequestSameKey;
	uint64_t nRequestHashOverflow;
	uint64_t nRequestListOverflow;
	uint64_t nFragmentFaultCount;
	uint32_t nReadbackRingBusyCount;
	uint32_t nReadbackRejectCount;
	uint32_t nReadbackStaleGenerationCount;
	uint32_t nReadbackInvalidPayloadCount;
	uint32_t nReadbackCadenceSkipCount;
	uint32_t nHotViolationCount;
	uint32_t nReserved;
};

class Terrain3DVTGpu {
public:
	Terrain3DVTGpu();
	~Terrain3DVTGpu();

	bool prepare_cpu_cold( const T3DVtGpuResourcePlan &pPlan, const T3DRenderWorldLimits &pLimits );
	bool prepare_storage_cold( const Terrain3DVtResourceImage *pResources,
	        const T3DRenderWorldLimits &pLimits );
	bool bind_executable_cold();
	bool service_bind_executable_cold( uint32_t nMaxWorkUnits );
	bool initialize_cold( const Terrain3DVtResourceImage *pResources,
	        const T3DRenderWorldLimits &pLimits );
	void shutdown_cold();
	void seal_hot_runtime();
	bool upload_database_warm( const T3DVTDatabaseView &pDatabase, const T3DVTResidencyState &pResidency );
	bool service_database_upload_warm( const T3DVTDatabaseView &pDatabase, const T3DVTResidencyState &pResidency,
	        uint64_t nMaxBytes, uint32_t nMaxWorkUnits );
	bool is_database_upload_pending_warm() const;
	bool update_allocation_quality_hot( const T3DVTResidencyState &rResidency );
	bool upload_neutral_page_set_warm( T3DVTResidencyState &rResidency, uint32_t pPhysicalSlot );
	bool service_retired_slots_hot( T3DVTResidencyState &rResidency, uint32_t pFrameId );
	bool begin_upload_batch_hot();
	bool end_upload_batch_hot( T3DVTResidencyState &rResidency, uint32_t pFrameId );
	bool upload_page_set_commit_hot( T3DVTResidencyState &rResidency,
	        const T3DVTUploadReservation &pReservation,
	        const T3DVTPageSetPayloadView &pPayloads,
	        uint32_t pFrameId );
	bool upload_commit_hot( T3DVTResidencyState &rResidency,
	        const T3DVTUploadReservation &pReservation,
	        const uint8_t *pDecodedBytes,
	        uint32_t pDecodedByteCount,
	        uint32_t pFrameId );
	static bool upload_batch_begin_callback( void *pUser );
	static bool upload_batch_end_callback( T3DVTResidencyState &rResidency, uint32_t pFrameId, void *pUser );
	static bool upload_commit_callback( T3DVTResidencyState &rResidency,
	        const T3DVTUploadReservation &pReservation,
	        const uint8_t *pDecodedBytes,
	        uint32_t pDecodedByteCount,
	        uint32_t pFrameId,
	        void *pUser );

	bool is_initialized() const;
	bool is_storage_ready() const;
	bool is_executable_ready() const;
	bool is_readback_enabled() const;
	bool get_physical_family_binding_v3( uint32_t pFamily, godot::RID &rRid, uint32_t &rLayerCount ) const;
	bool validate_physical_format_families_v3_cold() const;
	uint64_t get_reserved_gpu_bytes() const;
	const T3DVTGpuStats &get_stats() const;

private:
	bool update_page_entry_hot( uint32_t pVirtualPage, const T3DVTPageTableEntry &pEntry );
	bool publish_upload_batch_page_table_hot( const T3DVTResidencyState &rResidency );
	void abort_upload_batch_hot( T3DVTResidencyState &rResidency );
	bool make_gpu_page_entry( uint32_t nVirtualPage, const T3DVTPageTableEntry &pCpuEntry, T3DVTPageTableEntry &rGpuEntry ) const;
	bool retire_victim_gpu_hot( T3DVTResidencyState &rResidency, uint32_t pPhysicalSlot, uint32_t pVictimPage, uint32_t pFrameId );
	bool map_physical_slot_hot( uint32_t pPhysicalSlot, uint32_t &rBank, uint32_t &rLayer ) const;
	bool validate_physical_bank_layout_cold() const;
	bool initialize_neutral_payloads_cold();
	uint32_t physical_layer_bytes( uint32_t pChannel ) const;
	bool hot_epoch_unchanged() const;

	godot::RenderingDevice *m_pRenderingDevice;
	const Terrain3DVtResourceImage *m_pResources;
	const T3DVTDatabaseView *m_pDatabaseView;
	T3DRenderWorldLimits m_Limits;
	T3DVtGpuResourcePlan m_Plan;
	godot::PackedByteArray m_AllocationUpload;
	godot::PackedByteArray m_PageEntryUpload;
	godot::PackedInt32Array m_RetiredPageScratch;
	godot::PackedInt32Array m_RetiredSlotScratch;
	godot::PackedInt32Array m_UploadBatchOrder;
	godot::PackedByteArray m_UploadBatchReservations;
	godot::PackedByteArray m_PhysicalUpload[T3D_RW_VT_CHANNEL_COUNT];
	godot::PackedByteArray m_NeutralPhysicalUpload[T3D_RW_VT_CHANNEL_COUNT];
	godot::PackedByteArray m_NeutralTailUpload[T3D_RW_VT_CHANNEL_COUNT];
	T3DVTGpuStats m_Stats;
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
