// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_H
#define TERRAIN3D_RENDER_WORLD_H

#include "rendering/rd/render_world/terrain_3d_render_world_aux.h"
#include "rendering/rd/render_world/terrain_3d_render_world_forward_bindings.h"
#include "rendering/rd/render_world/terrain_3d_render_world_hiz_gpu.h"
#include "rendering/rd/render_world/terrain_3d_render_world_plateau.h"
#include "rendering/rd/render_world/terrain_3d_render_world_raster.h"
#include "rendering/rd/render_world/terrain_3d_render_world_streaming.h"
#include "rendering/rd/render_world/terrain_3d_render_world_vt_gpu.h"

#include <stdint.h>

enum T3DRenderWorldFrameStatusBits : uint32_t {
	T3D_RW_FRAME_OK = 0u,
	T3D_RW_FRAME_VT_READBACK_DEFERRED = 1u << 0u
};

struct T3DRenderWorldFrameResult {
	uint64_t nVTUploadBytes;
	uint32_t nDrawCount;
	uint32_t nStatusBits;
};

struct T3DRenderWorldProductionStats {
	uint64_t nFrameCount;
	uint64_t nDrawCount;
	uint64_t nVTUploadBytes;
	uint64_t nVTReadbackDeferredCount;
	uint64_t nVTReadbackConsumedCount;
	uint64_t nVTAcceptedRequestCount;
	uint64_t nFailureCount;
	uint64_t nSyncWaitCount;
	uint64_t nSyncReadbackCount;
};

class Terrain3DRenderWorld {
public:
	Terrain3DRenderWorld();

	bool initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
	        Terrain3DRenderWorldGpuVisibility *pVisibility,
	        Terrain3DRenderWorldHiZGpu *pHiZ,
	        Terrain3DRenderWorldVTGpu *pVT,
	        Terrain3DRenderWorldThreeTierRaster *pRaster,
	        T3DVTStreamingState *pStreaming,
	        T3DVTResidencyState *pResidency );
	void seal_hot_runtime();
	void shutdown_cold();
	void set_submission_enabled_cold( bool pEnabled );
	bool is_submission_enabled() const;
	bool attach_forward_bindings_cold( T3DForwardBorrowedBindingState *pForwardBindings );
	bool attach_aux_raster_cold( Terrain3DRenderWorldAuxRaster *pAuxRaster );
	bool submit_aux_pass_hot( T3DRenderWorldAuxPass pPass,
	        int64_t pDrawList,
	        uint32_t pShadowViewCount,
	        uint32_t pShadowVisibleClusterCount,
	        uint32_t &rDrawCount );
	bool capture_plateau_snapshot_reference_debug( T3DRenderWorldPlateauSnapshot &rSnapshot ) const;
	bool validate_plateau_reference_debug(
	        const T3DRenderWorldPlateauSnapshot &pBaseline, const T3DRenderWorldPlateauSnapshot &pCurrent ) const;
	bool validate_frame_contract_hot( const T3DRenderWorldLimits &pLimits, const T3DVisibilityFrame &pFrame ) const;
	bool consume_vt_readback_owner_hot( uint32_t pSlot,
	        uint32_t pFrameId,
	        const uint8_t *pReadbackBytes,
	        uint32_t pReadbackByteCount,
	        uint32_t &rAcceptedRequestCount );
	bool claim_vt_stream_job_owner_hot( uint32_t &rJobIndex );
	bool execute_vt_stream_job_async( uint32_t pJobIndex, T3DVTStreamReadDecodeFn pReadDecode, void *pUser );
	bool render_main_camera_hot(
	        const T3DRenderWorldLimits &pLimits, const T3DVisibilityFrame &pFrame, int64_t pDrawList, T3DRenderWorldFrameResult &rResult );

	bool is_initialized() const;
	const T3DRenderWorldProductionStats &get_stats() const;

private:
	godot::RenderingDevice *m_pRenderingDevice;
	const Terrain3DRenderWorldResourceImage *m_pResources;
	Terrain3DRenderWorldGpuVisibility *m_pVisibility;
	Terrain3DRenderWorldHiZGpu *m_pHiZ;
	Terrain3DRenderWorldVTGpu *m_pVT;
	Terrain3DRenderWorldThreeTierRaster *m_pRaster;
	T3DVTStreamingState *m_pStreaming;
	T3DVTResidencyState *m_pResidency;
	T3DForwardBorrowedBindingState *m_pForwardBindings;
	Terrain3DRenderWorldAuxRaster *m_pAuxRaster;
	T3DRenderWorldProductionStats m_Stats;
	T3DRenderWorldLimits m_SealedLimits;
	uint64_t m_nLimitsSignature;
	bool m_bInitialized;
	bool m_bHotSealed;
	bool m_bSubmissionEnabled;
};

#endif // TERRAIN3D_RENDER_WORLD_H
