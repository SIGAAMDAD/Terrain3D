// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_RESOURCES_H
#define TERRAIN3D_VT_RESOURCES_H

#include "rendering/rd/vt/terrain_3d_vt_resource_plan.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_texture.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>

struct T3DVtGpuResourceStats {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nLiveBufferBytes;
	uint64_t nLiveTextureBytes;
	uint64_t nResourceGeneration;
	uint32_t nLiveResourceCount;
	uint32_t nHotSealViolationCount;
};

class Terrain3DVtResourceImage {
public:
	Terrain3DVtResourceImage();
	~Terrain3DVtResourceImage();

	bool initialize_cold( Terrain3DRdContext &rContext, const T3DWorldLimits &pLimits, const T3DVtGpuProfile &pProfile );
	bool begin_initialize_cold( Terrain3DRdContext &rContext, const T3DWorldLimits &pLimits, const T3DVtGpuProfile &pProfile );
	bool service_initialize_cold( uint32_t nMaxWorkUnits );
	bool is_initialize_pending_cold() const;
	void shutdown_cold();

	void seal_hot_runtime();
	void unseal_hot_runtime_cold();
	bool service_idle_frame_hot();
	bool validate_idle_plateau_reference_debug( uint32_t pFrameCount );

	bool is_initialized() const;
	bool is_hot_sealed() const;
	const T3DVtGpuResourcePlan &get_plan() const;
	const T3DWorldLimits &get_limits() const;
	const T3DVtGpuResourceStats &get_stats() const;
	const godot::RID &get_buffer_rid( T3DVtGpuBufferId pBuffer ) const;
	const Terrain3DRdGpuBuffer *get_buffer( T3DVtGpuBufferId pBuffer ) const;
	uint32_t get_vt_bank_count( T3DVtChannel pChannel ) const;
	uint32_t get_vt_bank_layer_count( T3DVtChannel pChannel, uint32_t pBank ) const;
	const godot::RID &get_vt_bank_rid( T3DVtChannel pChannel, uint32_t pBank ) const;
	const godot::RID &get_vt_tail_rid( T3DVtChannel pChannel ) const;
	bool update_vt_bank_layer( T3DVtChannel pChannel, uint32_t pBank, uint32_t pLayer, const godot::PackedByteArray &rData ) const;
	bool update_vt_tail_layer( T3DVtChannel pChannel, uint32_t pLayer, const godot::PackedByteArray &rData ) const;
	Terrain3DRdContext *get_rd_context() const;

private:
	bool initialize_internal_cold(
	        Terrain3DRdContext &rContext, const T3DWorldLimits &pLimits, const T3DVtGpuProfile &pProfile );
	bool validate_device_cold( const T3DWorldLimits &pLimits, const T3DVtGpuProfile &pProfile ) const;
	bool create_storage_buffer_cold( T3DVtGpuBufferId pBuffer, uint64_t pBytes );
	bool create_vt_channel_cold(
	        T3DVtChannel pChannel, uint32_t pPageCount, uint32_t pTileSize, godot::RenderingDevice::DataFormat pFormat );
	bool create_vt_tail_channel_cold( T3DVtChannel pChannel, godot::RenderingDevice::DataFormat pFormat );
	void rollback_failed_initialize_cold();
	bool hot_epoch_unchanged() const;

	Terrain3DRdContext *m_pContext;
	godot::RenderingDevice *m_pRenderingDevice;
	godot::RID m_hInvalid;
	Terrain3DRdGpuBuffer m_buffers[T3D_VT_GPU_BUFFER_COUNT];
	Terrain3DRdTexture m_vtPhysical[T3D_VT_CHANNEL_COUNT][T3D_VT_MAX_TEXTURE_BANKS];
	Terrain3DRdTexture m_vtTail[T3D_VT_CHANNEL_COUNT];
	uint32_t m_nVtBankCount[T3D_VT_CHANNEL_COUNT];
	uint32_t m_nVtBankLayers[T3D_VT_CHANNEL_COUNT][T3D_VT_MAX_TEXTURE_BANKS];
	T3DVtGpuResourcePlan m_Plan;
	T3DWorldLimits m_Limits;
	T3DVtGpuProfile m_InitializeProfile;
	T3DVtGpuResourceStats m_Stats;
	uint32_t m_nInitializeBufferCursor;
	uint32_t m_nInitializeChannelCursor;
	uint32_t m_nInitializeStage;
	bool m_bInitializePending;
	uint64_t m_nHotSealCreateEpoch;
	uint64_t m_nHotSealFreeEpoch;
	bool m_bInitialized;
	bool m_bHotSealed;
};

#endif // TERRAIN3D_VT_RESOURCES_H
