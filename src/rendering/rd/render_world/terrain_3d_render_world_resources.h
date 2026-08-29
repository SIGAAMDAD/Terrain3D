// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_RESOURCES_H
#define TERRAIN3D_RENDER_WORLD_RESOURCES_H

#include "rendering/rd/render_world/terrain_3d_render_world_resource_plan.h"
#include "rendering/rd/terrain_3d_rd_context.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>

struct T3DRenderWorldGpuResourceStats {
	uint64_t nGpuCreateCount;
	uint64_t nGpuFreeCount;
	uint64_t nLiveBufferBytes;
	uint64_t nLiveTextureBytes;
	uint64_t nResourceGeneration;
	uint32_t nLiveResourceCount;
	uint32_t nHotSealViolationCount;
};

class Terrain3DRenderWorldResourceImage {
public:
	Terrain3DRenderWorldResourceImage();
	~Terrain3DRenderWorldResourceImage();

	bool initialize_cold( Terrain3DRdContext &rContext, const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile );
	void shutdown_cold();

	void seal_hot_runtime();
	void unseal_hot_runtime_cold();
	bool service_idle_frame_hot();
	bool validate_idle_plateau_reference_debug( uint32_t pFrameCount );

	bool is_initialized() const;
	bool is_hot_sealed() const;
	const T3DRenderWorldGpuResourcePlan &get_plan() const;
	const T3DRenderWorldLimits &get_limits() const;
	const T3DRenderWorldGpuResourceStats &get_stats() const;
	const godot::RID &get_buffer_rid( T3DRenderWorldGpuBufferId pBuffer ) const;
	const godot::RID &get_hiz_rid() const;
	uint32_t get_vt_bank_count( T3DRenderWorldVTChannel pChannel ) const;
	uint32_t get_vt_bank_layer_count( T3DRenderWorldVTChannel pChannel, uint32_t pBank ) const;
	const godot::RID &get_vt_bank_rid( T3DRenderWorldVTChannel pChannel, uint32_t pBank ) const;
	Terrain3DRdContext *get_rd_context() const;

private:
	bool initialize_internal_cold(
	        Terrain3DRdContext &rContext, const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile );
	bool validate_device_cold( const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile ) const;
	bool create_buffers_cold();
	bool create_vt_textures_cold( const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile );
	bool create_hiz_texture_cold( const T3DRenderWorldGpuProfile &pProfile );
	bool create_storage_buffer_cold( T3DRenderWorldGpuBufferId pBuffer, uint64_t pBytes, bool pIndirect );
	bool create_vt_channel_cold(
	        T3DRenderWorldVTChannel pChannel, uint32_t pPageCount, uint32_t pTileSize, godot::RenderingDevice::DataFormat pFormat );
	bool create_texture_cold( godot::RID &rRid,
	        uint32_t pWidth,
	        uint32_t pHeight,
	        uint32_t pLayers,
	        uint32_t pMipCount,
	        godot::RenderingDevice::TextureType pType,
	        godot::RenderingDevice::DataFormat pFormat,
	        godot::BitField<godot::RenderingDevice::TextureUsageBits> pUsage );
	void free_rid_cold( godot::RID &rRid, uint64_t pBytes, bool pTexture );
	void rollback_failed_initialize_cold();
	bool hot_epoch_unchanged() const;

	Terrain3DRdContext *m_pContext;
	godot::RenderingDevice *m_pRenderingDevice;
	godot::RID m_hInvalid;
	godot::RID m_hBuffers[T3D_RW_GPU_BUFFER_COUNT];
	godot::RID m_hVtPhysical[T3D_RW_VT_CHANNEL_COUNT][T3D_RW_MAX_VT_TEXTURE_BANKS];
	godot::RID m_hHiZ;
	uint32_t m_nVtBankCount[T3D_RW_VT_CHANNEL_COUNT];
	uint32_t m_nVtBankLayers[T3D_RW_VT_CHANNEL_COUNT][T3D_RW_MAX_VT_TEXTURE_BANKS];
	uint64_t m_nVtBankBytes[T3D_RW_VT_CHANNEL_COUNT][T3D_RW_MAX_VT_TEXTURE_BANKS];
	T3DRenderWorldGpuResourcePlan m_Plan;
	T3DRenderWorldLimits m_Limits;
	T3DRenderWorldGpuResourceStats m_Stats;
	uint64_t m_nHotSealCreateEpoch;
	uint64_t m_nHotSealFreeEpoch;
	bool m_bInitialized;
	bool m_bHotSealed;
};

#endif // TERRAIN3D_RENDER_WORLD_RESOURCES_H
