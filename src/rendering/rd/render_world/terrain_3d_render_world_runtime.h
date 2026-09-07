// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_RUNTIME_H
#define TERRAIN3D_RENDER_WORLD_RUNTIME_H

#include "rendering/rd/render_world/terrain_3d_render_world.h"
#include "core/terrain_3d_sync.h"
#include "memory/terrain_3d_memory_zone.h"
#include "rendering/rd/render_world/terrain_3d_render_world_terrain_integration_gpu.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/rd/terrain_3d_rd_render_world_shader_bundle.h"

#include <stdint.h>

struct t3dRenderWorldPreparedViewKey_t {
	uint64_t m_nRenderFrame = 0u;
	uint64_t m_nViewportKey = 0u;
	uint64_t m_nViewStateHash = 0u;
	uint64_t m_nSceneGeneration = 0u;
	uint64_t m_nTerrainGeneration = 0u;
	uint64_t m_nCullingGeneration = 0u;
	uint32_t m_nViewCount = 0u;
	bool m_bValid = false;

	EA_FORCE_INLINE bool matches( const t3dRenderWorldPreparedViewKey_t &rOther ) const {
		return m_bValid && rOther.m_bValid && m_nRenderFrame == rOther.m_nRenderFrame && m_nViewportKey == rOther.m_nViewportKey
		        && m_nViewStateHash == rOther.m_nViewStateHash && m_nSceneGeneration == rOther.m_nSceneGeneration
		        && m_nTerrainGeneration == rOther.m_nTerrainGeneration && m_nCullingGeneration == rOther.m_nCullingGeneration
		        && m_nViewCount == rOther.m_nViewCount;
	}
};

class Terrain3DRenderWorldRuntime {
public:
	Terrain3DRenderWorldRuntime();
	~Terrain3DRenderWorldRuntime();

	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone );
	bool initialize_cold( Terrain3DRdContext &rContext, const t3dRdRenderWorldShaderBundle_t &rShaders, int64_t pFramebufferFormat );
	void shutdown_cold();
	bool rebuild_raster_cold( int64_t pFramebufferFormat );
	bool prepare_forward_cold( const t3dRdForwardPassContext_t &pContext );
	bool publish_terrain_generations_hot( uint64_t pLogicalTopologyGeneration,
	        uint64_t pPhysicalPageTableGeneration,
	        uint64_t pClipmapPlacementGeneration,
	        uint64_t pMaterialGeneration,
	        uint64_t pLightingGeneration );
	bool upload_scene_database_warm( const T3DGeometryDatabase &pGeometry,
	        const T3DSceneCell *pCells,
	        uint32_t pCellCount,
	        const T3DInstanceRecord *pInstances,
	        uint32_t pInstanceCount,
	        const T3DMaterialRecord *pMaterials,
	        uint32_t pMaterialCount );
	void release_scene_cold();
	bool render_main_camera_hot( const t3dRdForwardPassContext_t &pContext,
	        uint32_t pCellCount,
	        uint32_t pInstanceCount,
	        uint32_t pMeshCount,
	        uint32_t pLodCount,
	        uint32_t pClusterCount,
	        T3DRenderWorldFrameResult &rResult );

	bool is_initialized() const;
	bool is_hiz_enabled() const;
	bool has_scene() const;
	uint64_t get_cpu_image_bytes() const;
	uint32_t get_operation_event_count() const;
	uint64_t get_compute_prepare_count() const;
	uint64_t get_compute_reuse_count() const;
	uint64_t get_raster_submit_count() const;
	uint32_t copy_operation_events_reference_debug( T3DOperationEvent *pOutEvents, uint32_t pCapacity ) const;
	const T3DRenderWorldLimits &get_limits() const;
	const Terrain3DRenderWorldResourceImage &get_resources() const;
	Terrain3DRenderWorld &get_render_world();
	T3DTerrainRenderWorldStageAState &get_stage_a_state();

private:
	bool allocate_cpu_image_cold();
	void release_cpu_image_cold();
	bool initialize_neutral_vt_cold();
	void record_operation(
	        uint64_t pFrameId, uint32_t pType, uint32_t pClass, uint64_t pBytes, uint32_t pCount, uint32_t pResult, uint32_t pSourceTag );
	uint64_t hash_visibility_frame_hot( const T3DVisibilityFrame &rFrame ) const;
	uint64_t build_culling_generation_hot( void ) const;
	void invalidate_prepared_view( void );
	void build_visibility_frame_hot( const t3dRdForwardPassContext_t &pContext,
	        uint32_t pCellCount,
	        uint32_t pInstanceCount,
	        uint32_t pMeshCount,
	        uint32_t pLodCount,
	        uint32_t pClusterCount,
	        T3DVisibilityFrame &rFrame ) const;

	Terrain3DRenderWorldResourceImage m_Resources;
	Terrain3DRenderWorldGpuVisibility m_Visibility;
	Terrain3DRenderWorldHiZGpu m_HiZ;
	Terrain3DRenderWorldVTGpu m_VT;
	Terrain3DRdForwardLighting m_ForwardLighting;
	Terrain3DRenderWorldThreeTierRaster m_Raster;
	Terrain3DRenderWorld m_RenderWorld;
	Terrain3DRenderWorldTerrainStageABridge m_TerrainBridge;
	T3DTerrainRenderWorldStageAState m_TerrainStageA;
	T3DTerrainStageASharedVTBindings m_SharedVTBindings;

	T3DRenderWorldLimits m_Limits;
	T3DRenderWorldGpuProfile m_Profile;
	T3DVTDatabaseView m_NeutralDatabase;
	T3DVTFileHeader m_NeutralHeader;
	T3DVTAllocationRecord m_NeutralAllocations[T3D_RW_VT_CHANNELS];
	T3DVTPageFileRecord m_NeutralPages[T3D_RW_VT_CHANNELS];
	T3DVTPayloadRecord m_NeutralPayloads[T3D_RW_VT_CHANNELS];
	uint8_t m_NeutralPayloadBytes[T3D_RW_VT_CHANNELS];
	T3DVTResidencyState m_Residency;
	T3DVTStreamingState m_Streaming;

	Terrain3DMemoryZone *m_pRuntimeAllocatorZone;
	void *m_pCpuImage;
	uint64_t m_nCpuImageBytes;
	T3DVTPageTableEntry *m_pPageTable;
	T3DVTPhysicalSlot *m_pPhysicalSlots[T3D_RW_VT_CHANNELS];
	T3DVTStreamJob *m_pStreamJobs;
	uint8_t *m_pStreamScratch;
	uint32_t *m_pScratchLaneOwner;
	t3dAtomic_t<uint32_t> m_ScratchLaneCompletion[T3D_RW_MAX_STREAM_SCRATCH_LANES];
	T3DOperationEvent *m_pOperationEvents;
	uint32_t m_nOperationWriteIndex;
	int64_t m_nFramebufferFormat;
	bool m_bInitialized;
	bool m_bHiZEnabled;
	bool m_bScenePublished;
	uint32_t m_nPublishedCellCount;
	uint32_t m_nPublishedInstanceCount;
	uint32_t m_nPublishedMeshCount;
	uint32_t m_nPublishedLodCount;
	uint32_t m_nPublishedClusterCount;
	uint32_t m_nPublishedMaterialCount;
	uint64_t m_nScenePublicationGeneration;
	uint64_t m_nTerrainPublicationGeneration;
	t3dRenderWorldPreparedViewKey_t m_preparedViewKey;
	uint32_t m_nPreparedStatusBits;
};

#endif // TERRAIN3D_RENDER_WORLD_RUNTIME_H
