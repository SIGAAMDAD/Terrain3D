// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_INSTANCE_RENDERER_H
#define TERRAIN3D_RD_INSTANCE_RENDERER_H
#include <EASTL/vector_map.h>
#include <EASTL/vector.h>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "Terrain3DConstants.h"
#include "core/containers/terrain_3d_fixed_buffer.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer_contracts.h"
#include "rendering/rd/instances/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_lease_table.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/targets/terrain_3d_rd_framebuffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_sampler.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/targets/terrain_3d_rd_render_target_pool.h"

#include "eastl_config.h"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <string.h>
#include "rendering/rd/vt/runtime/terrain_3d_virtual_texture.h"

#include <godot_cpp/classes/render_data.hpp>

class Terrain3DRdInstanceRenderer
{
private:
	// Private state records; all behavior and ownership stay with this renderer.
	struct Terrain3DRdInstanceGeometryArena
	{
		Terrain3DRdMeshCache m_meshCache;
		uint64_t			 m_nUploadedMeshGeneration = 0u;
		uint64_t			 m_nSceneCapacityMeshGeneration = 0u;
		bool				 m_bSharedMeshArenaSubmittedOnce = false;
	};

	struct Terrain3DRdInstanceVisibility
	{
		Terrain3DRdShader								   m_hInstanceCullShader;
		RID												   m_hInstanceCullPipeline;
		Terrain3DRdGpuBuffer							   m_hInstanceCullDescriptorBuffer;
		Terrain3DRdGpuBuffer m_hInstanceCullCommandOwnerBuffer;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_instanceCullCommandOwnerWords;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_instanceCullCommandOwnerCursors;
		RID												   m_hInstanceCullUniformSet;
		Terrain3DRdGpuBuffer							   m_hVisibleIndexBuffer;
		Terrain3DRdGpuBuffer							   m_hShadowVisibleIndexBuffer;
		Terrain3DRdGpuBuffer							   m_hVisibleDrawSlotBuffer;
		Terrain3DRdGpuBuffer							   m_hShadowVisibleDrawSlotBuffer;
		Terrain3DRdGpuBuffer							   m_hVisibleLayerBuffer;
		Terrain3DRdGpuBuffer							   m_hShadowVisibleLayerBuffer;
		Terrain3DRdGpuBuffer							   m_hVisibleFlagBuffer;
		Terrain3DRdGpuBuffer							   m_hShadowVisibleFlagBuffer;
		uint32_t										   m_nVisibleIndexCapacity = 0u;
		uint32_t										   m_nVisibleIndexCount = 0u;
		uint32_t										   m_nShadowVisibleIndexCount = 0u;
		uint32_t										   m_nFixedVisibleCapacity = 0u;
		uint64_t										   m_szVisibilityLayerFilteredCells[3] = {};
		bool											   m_bCullingComparisonEnabled = false;
		bool											   m_bCullParityValidatedOnce = false;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_visibleIndices;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_visibleDrawSlots;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_shadowVisibleDrawSlots;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_visibleLayers;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_shadowVisibleLayers;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_shadowVisibleIndices;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_cullParityExpectedIndices;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_cullParityActualIndices;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_cullParityCommandCounts;
		eastl::vector< uint32_t, Terrain3DEastlAllocator > m_cullParityCommandWriteOffsets;
		eastl::vector< t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator > m_instanceCullDescriptors;
		eastl::vector< t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator > m_shadowInstanceCullDescriptors;
		bool m_bMainCommandOwnershipExclusive = false;
		bool m_bShadowCommandOwnershipExclusive = false;
		eastl::vector< t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator > m_instanceCullCellSpans;
		eastl::vector< t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator > m_shadowInstanceCullCellSpans;
		uint64_t																m_nInstanceCullDispatches = 0u;
		uint64_t																m_nGpuTopologyBuilds = 0u;
		uint64_t																m_nGpuCullReuses = 0u;
		uint64_t																m_nCullParityComparisons = 0u;
		uint64_t																m_nCullParityMismatches = 0u;
		uint64_t																m_nCullParityTopologyMismatches = 0u;
		uint64_t																m_nCullParityReadbackBytes = 0u;
		uint64_t																m_nCullParityMainReuses = 0u;
		uint64_t																m_nCullParityVisibleCells = 0u;
		uint64_t																m_nCullParityVisibleInstances = 0u;
		uint64_t																m_nCullParityShadowVisibleCells = 0u;
		uint64_t m_nCullParityShadowVisibleInstances = 0u;
		uint64_t m_nCullParityMainRenderFrame = UINT64_MAX;
		uint64_t m_nCullParityMainViewportKey = 0u;
		uint64_t m_nCullParityMainViewKey = 0u;
		uint64_t m_nCullParityMainSceneGeneration = 0u;
		uint32_t m_nCullParityMainVisibilityMask = UINT32_MAX;
		uint32_t m_nCullParityMainViewCount = 0u;
		uint64_t m_nGpuTopologySceneGeneration = 0u;
		uint64_t m_nGpuTopologyMeshGeneration = 0u;
		uint64_t m_nGpuCullMainRenderFrame = UINT64_MAX;
		uint64_t m_nGpuCullMainViewportKey = 0u;
		uint64_t m_nGpuCullMainViewKey = 0u;
		uint64_t m_nGpuCullMainSceneGeneration = 0u;
		uint32_t m_nGpuCullMainVisibilityMask = UINT32_MAX;
		uint32_t m_nGpuCullMainViewCount = 0u;
	};

	struct Terrain3DRdInstanceCommandTable
	{
		Terrain3DRdGpuBuffer												m_hDrawMetadataBuffer;
		Terrain3DRdGpuBuffer												m_hIndirectBuffer;
		t3dRdGpuBufferSlice_t												m_mainIndirectSlice;
		t3dRdGpuBufferSlice_t												m_shadowIndirectSlice;
		uint64_t															m_nDrawMetadataMeshGeneration = 0u;
		uint32_t															m_nPacketCapacity = 0u;
		uint32_t															m_nMainCompatibleCommandCount = 0u;
		uint32_t															m_nShadowCompatibleCommandCount = 0u;
		uint32_t															m_szMainGroupedCommands[3] = {};
		uint32_t															m_szShadowGroupedCommands[3] = {};
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_groupedCommandRemap;
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_fixedDrawVisibleBases;
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_fixedDrawVisibleCapacities;
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_shadowFixedDrawVisibleBases;
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_shadowFixedDrawVisibleCapacities;
		eastl::vector< t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator > m_drawPackets;
		eastl::vector< t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator > m_depthPackets;
		eastl::vector< t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator > m_shadowPackets;
		eastl::vector< t3dRdPacketBuildSlot_t, Terrain3DEastlAllocator >	m_packetBuildSlots;
		eastl::vector< t3dRdPacketBuildEntry_t, Terrain3DEastlAllocator >	m_packetBuildEntries;
		eastl::vector< t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator > m_alphaPackets;
		eastl::vector< t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator > m_alphaSortScratch;
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_transparentPacketOrder;
		eastl::vector< uint8_t, Terrain3DEastlAllocator >					m_gpuPacketBoundValid;
		eastl::vector_map< t3dRdPacketKey_t, uint32_t, t3dRdPacketKeyLess_t, Terrain3DEastlAllocator > m_packetIndex;
		uint64_t																					   m_nDraws = 0u;
		uint64_t m_nDepthSubmittedDraws = 0u;
		uint64_t m_nOpaqueSubmittedDraws = 0u;
		uint64_t m_nShadowSubmittedDraws = 0u;
		uint64_t m_nMotionSubmittedDraws = 0u;
		uint64_t m_nTransparentSubmittedDraws = 0u;
		uint64_t m_nOneSubmitOpaque = 0u;
		uint64_t m_nOneSubmitDepth = 0u;
		uint64_t m_nOneSubmitMotion = 0u;
		uint64_t m_nOneSubmitShadow = 0u;
		uint64_t m_nOneSubmitFallbackDraws = 0u;
		uint64_t m_szSubmittedCommands[T3D_RD_FORWARD_PASS_MAX] = {};
		uint64_t m_szGroupedCommands[T3D_RD_FORWARD_PASS_MAX] = {};
		uint64_t m_szFallbackCommands[T3D_RD_FORWARD_PASS_MAX] = {};
		uint64_t m_nCpuSurfacePacketsBuilt = 0u;
		uint64_t m_nPacketBuildInputCells = 0u;
		uint64_t m_nResolvedMeshCells = 0u;
		uint64_t m_nUnresolvedMeshCells = 0u;
		uint64_t m_nVisibleInstances = 0u;
		uint64_t m_nStaticInstances = 0u;
		uint64_t m_nGpuProductionPasses = 0u;
		uint64_t m_nGpuProductionIndirectSubmissions = 0u;
		uint64_t m_nGpuTransparentOrderBuilds = 0u;
		uint64_t m_nCpuReferencePasses = 0u;
		uint64_t m_nCpuReferenceDirectSubmissions = 0u;
		uint32_t m_nTransparentPacketOrderCount = 0u;
		bool	 m_bGpuProductionSubmittedOnce = false;
	};

	struct Terrain3DRdInstanceOccluderDepth
	{
		Terrain3DRdShader m_hShader;
		RID				  m_hPipeline;
		RID				  m_hDepthTexture;
		// Borrowed aliases into Terrain3DRdRenderTargetPool-owned texture/framebuffer resources.
		RID																	m_hFramebuffer;
		RID																	m_hszDepthTextures[2];
		RID																	m_hszFramebuffers[2];
		Terrain3DRdGpuBuffer												m_hVisibleIndexBuffer;
		RID																	m_hUniformSet;
		Vector2i															m_targetSize;
		uint32_t															m_nPacketCapacity = 0u;
		uint32_t															m_nVisibleIndexCapacity = 0u;
		uint32_t															m_nVisibleIndexCount = 0u;
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_visibleIndices;
		eastl::vector< t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator > m_packets;
		PackedByteArray														m_visibleBytes;
		uint64_t															m_nRasterFrames = 0u;
		uint64_t															m_nRasterDrawPackets = 0u;
		uint64_t															m_nRasterInstances = 0u;
		uint64_t															m_nLastRasterRenderFrame = UINT64_MAX;
		uint64_t															m_nLastRasterViewportKey = 0u;
		uint64_t m_szRasterRenderFrames[2] = { UINT64_MAX, UINT64_MAX };
		uint64_t m_szRasterViewportKeys[2] = {};
		uint64_t m_szRasterSceneGenerations[2] = {};
		uint64_t m_szRasterMeshGenerations[2] = {};
		bool	 m_szDepthValid[2] = {};
		uint32_t m_nWriteSlot = 0u;
		uint64_t m_nHistorySamples = 0u;
		uint64_t m_nHistoryInvalidations = 0u;
		uint64_t m_nHistoryValidFrames = 0u;
		uint64_t m_nHistoryInvalidFrames = 0u;
		uint64_t m_nTargetRebuilds = 0u;
		uint64_t m_nCapacityGrowths = 0u;
		uint64_t m_nLastHistoryDecisionRenderFrame = UINT64_MAX;
		bool	 m_bDepthValid = false;
	};

	struct Terrain3DRdInstanceDescriptorState
	{
		RID				   m_hUniformSet;
		RID				   m_hszForwardUniformSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
		RID				   m_hszForwardShadowUniformSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
		RID				   m_hMaterialVtUniformSet;
		RID				   m_hszForwardMaterialVtUniformSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
		Terrain3DRdSampler m_hVtSampler;
		uint64_t		   m_nMaterialVtResourceSignature = 0u;
		bool			   m_bMaterialVtSetBuiltOnce = false;
		uint64_t		   m_nVtDescriptorBindsInstancerPass = 0u;
		uint64_t		   m_nVtDescriptorRebuildsRuntime = 0u;
	};

	struct Terrain3DRdInstanceUploadState
	{
		Terrain3DRdGpuBuffer												m_hFrameBuffer;
		Terrain3DRdGpuBuffer												m_hInstanceBuffer;
		Terrain3DRdGpuBuffer												m_hPreviousInstanceBuffer;
		Terrain3DRdGpuBuffer												m_hMaterialBuffer;
		uint64_t															m_nUploadedSceneGeneration = 0u;
		uint32_t															m_nMotionHistoryRecordCount = 0u;
		uint32_t															m_nMotionHistoryCellCount = 0u;
		uint32_t															m_nMotionPreviousRecordCount = 0u;
		uint32_t															m_nMotionCurrentOrderCount = 0u;
		uint32_t															m_nMotionPreviousOrderCount = 0u;
		eastl::vector< t3dRdInstanceRecord_t, Terrain3DEastlAllocator >		m_motionHistoryRecords;
		eastl::vector< t3dRdInstanceCell_t, Terrain3DEastlAllocator >		m_motionHistoryCells;
		eastl::vector< t3dRdInstanceRecord_t, Terrain3DEastlAllocator >		m_motionPreviousRecords;
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_motionCurrentOrder;
		eastl::vector< uint32_t, Terrain3DEastlAllocator >					m_motionPreviousOrder;
		eastl::vector< t3dRdInstanceDirtyRange_t, Terrain3DEastlAllocator > m_motionDirtyRanges;
		PackedByteArray														m_frameBytes;
		PackedByteArray														m_instanceBytes;
		PackedByteArray														m_previousInstanceBytes;
		PackedByteArray														m_dirtyInstanceBytes;
		PackedByteArray														m_visibleBytes;
		PackedByteArray														m_shadowVisibleBytes;
		PackedByteArray														m_visibleDrawSlotBytes;
		PackedByteArray														m_shadowVisibleDrawSlotBytes;
		PackedByteArray														m_visibleLayerBytes;
		PackedByteArray														m_shadowVisibleLayerBytes;
		PackedByteArray														m_drawMetadataBytes;
		PackedByteArray														m_materialBytes;
		PackedByteArray														m_indirectBytes;
		PackedByteArray														m_shadowIndirectBytes;
		PackedByteArray														m_pushBytes;
		PackedByteArray														m_instanceCullDescriptorBytes;
        PackedByteArray m_instanceCullCommandOwnerBytes;
		PackedByteArray														m_instanceCullPushBytes;
		uint64_t															m_nFrames = 0u;
		uint64_t															m_nPreviewSceneGeneration = 0u;
		uint64_t															m_nSceneUploads = 0u;
		uint64_t															m_nMotionHistoryRenderFrame = UINT64_MAX;
		uint64_t															m_nMotionHistorySceneGeneration = 0u;
		uint64_t															m_nMotionHistoryResets = 0u;
		uint64_t															m_nMotionHistoryMatches = 0u;
		uint64_t															m_nBufferCapacityMissCount = 0u;
		uint64_t															m_nBufferRebuildFailureCount = 0u;
		uint64_t															m_nCpuCapacityRebuildCount = 0u;
		uint64_t															m_nCpuCapacityRebuildFailureCount = 0u;
		bool																m_bMotionHistoryNeedsAdvance = false;
	};

	typedef struct t3dRdInstanceViewState_s
	{
		Terrain3DRdFramebuffer			m_hFramebuffer;
		RID								m_szPipeline[9];
		RID								m_hColor;
		RID								m_hDepth;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
	} t3dRdInstanceViewState_t;

	typedef struct t3dRdInstanceForwardPipelineKey_s : public t3dRdForwardPipelineBaseKey_t
	{
		bool operator==(
			const t3dRdInstanceForwardPipelineKey_s &rOther ) const {
			return ( const t3dRdForwardPipelineBaseKey_t & ) ( *this ) ==
				   ( const t3dRdForwardPipelineBaseKey_t & ) ( rOther );
		}
	} t3dRdInstanceForwardPipelineKey_t;

	typedef struct t3dRdInstanceForwardPipelineEntry_s
	{
		t3dRdInstanceForwardPipelineKey_t m_key;
		RID								  m_hszPipelines[3];
	} t3dRdInstanceForwardPipelineEntry_t;

	typedef struct t3dRdInstanceCullPipelineKey_s
	{
		t3dRdInstanceForwardPipelineKey_t forward;
		uint32_t						  cull = 0u;
		bool							  operator==(
			 const t3dRdInstanceCullPipelineKey_s &rOther ) const {
			return cull == rOther.cull && forward == rOther.forward;
		}
	} t3dRdInstanceCullPipelineKey_t;

	typedef struct t3dForwardSetKey_s
	{
		RID	 shader;
		bool shadow = false;
		bool operator==(
			const t3dForwardSetKey_s &rOther ) const {
			return shader == rOther.shader && shadow == rOther.shadow;
		}
	} t3dForwardSetKey_t;

	typedef struct t3dVTSetKey_s
	{
		RID		 shader;
		uint64_t signature = 0u;
		bool	 operator==(
			const t3dVTSetKey_s &rOther ) const {
			return shader == rOther.shader && signature == rOther.signature;
		}
	} t3dVTSetKey_t;

	Terrain3DRdContext										&m_context;
	Terrain3DRdInstanceGeometryArena						 m_geometryArena;
	Terrain3DRdInstanceVisibility							 m_visibility;
	Terrain3DRdPipelineCache< RID, 1u >						 m_instanceCullPipelines;
	Terrain3DRdDescriptorLeaseTable< RID, 1u >				 m_instanceCullDescriptors;
	Terrain3DRdDescriptorLeaseTable< RID, 1u >				 m_occluderDescriptors;
	Terrain3DRdDescriptorLeaseTable< RID, 1u >				 m_instanceSet0Descriptors;
	Terrain3DRdPipelineCache< RID, 1u >						 m_occluderPipelines;
	Terrain3DRdPipelineCache< uint32_t, T3D_MAX_VIEWS * 9u > m_viewPipelines;
	Terrain3DRdRenderTargetPool< 4u >						 m_targetPool;
	Terrain3DRdDescriptorLeaseTable< t3dForwardSetKey_t, T3D_RD_FORWARD_SHADER_VARIANT_MAX * 2u > m_forwardDescriptors;
	Terrain3DRdDescriptorLeaseTable< t3dVTSetKey_t, T3D_RD_FORWARD_SHADER_VARIANT_MAX + 1u >	  m_vtDescriptors;
	Terrain3DRdInstanceCommandTable																  m_commandTable;
	Terrain3DRdInstanceOccluderDepth															  m_occluderDepth;
	Terrain3DRdInstanceDescriptorState															  m_descriptorState;
	Terrain3DRdInstanceUploadState																  m_uploadState;
	Terrain3DVirtualTexture	  *m_pVirtualTexture = NULL;
	Terrain3DRdForwardLighting m_forwardLighting;
	t3dRdInstanceViewState_t   m_szViewStates[T3D_MAX_VIEWS]{};
	Terrain3DRdShader		   m_hShader;
	Terrain3DRdShader		   m_hszForwardShaders[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	bool					   m_bForwardFeedback[T3D_RD_FORWARD_SHADER_VARIANT_MAX]{};
	bool m_bForwardCompact[T3D_RD_FORWARD_SHADER_VARIANT_MAX]{};
	RID m_hCurrentCompactSet;
	RID						   m_hszForwardFeedbackSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	RID						   m_hPreviewColorTexture;
	RID						   m_hPreviewDepthTexture;
	Vector2i				   m_previewSize;
	bool					   m_bPreviewClear = false;
	bool					   m_bDirectDrawDebug = false;
	bool					   m_bMotionDebug = false;
	String					   m_lastError;
	mutable Terrain3DMutex	   m_stylizedSunMutex;
	Vector4					   m_stylizedSunShape;
	Vector4					   m_stylizedSunDiffuse;
	Vector4					   m_stylizedSunSpecular;
	Vector4					   m_stylizedSunRim;

	Terrain3DRdPipelineCache< t3dRdInstanceCullPipelineKey_t, T3D_FORWARD_PIPELINE_CAPACITY * 3u > m_forwardPipelines;
	mutable t3dRdInstanceForwardPipelineEntry_t m_currentForwardPipelineEntry{};
	uint64_t									m_nForwardPreparedScenePass = 0u;
	uint64_t									m_nForwardPreparedViewportKey = 0u;
	uint64_t									m_nForwardPreparedSceneGeneration = 0u;
	t3dRdForwardPassKind_t						m_forwardPreparedPacketPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t									m_nForwardPreparedVisibilityMask = UINT32_MAX;
	t3dRdInstanceForwardPipelineKey_t			m_lastForwardPipelineKey{};
	bool										m_bLastForwardPipelineKeyValid = false;
	bool										m_bForwardPreparedHasDraws = false;
	bool										m_bForwardPreparedGpuVisibility = false;
	uint32_t									m_nUploadedVtDatabaseGeneration = 0u;
	uint64_t									m_nVtStableKeyMissCount = 0u;
	uint64_t									m_nVtInvalidAllocationReferenceCount = 0u;
	uint64_t									m_nVtOutOfRangePageReferenceCount = 0u;
	float										m_szTextureMipBias[4] = {};
	uint64_t									m_nLastVtResidencyRequestFrame = UINT64_MAX;
	uint32_t									m_nVtFootprintSweep = 0u;
	uint64_t									m_nVtResidencyRequestAttempts = 0u;
	uint64_t									m_szVtPredictedMipRequests[T3D_VT_MAX_MIPS]{};
	uint64_t									m_nVtResidencyRequestsAccepted = 0u;

	bool ensureDevice( void );
	bool ensureShader( void );
	bool ensureForwardShaderVariant( t3dRdForwardShaderVariant_t variant );
	bool ensureOccluderDepthShader( void );
	void freeOccluderDepthUniformSet( void );
	void freeOccluderDepthTarget( void );
	void freeOccluderDepthResources( void );
	bool ensureOccluderDepthTarget( const Vector2i &rRenderSize );
	bool ensureOccluderDepthStorage( const t3dRdInstanceSceneSnapshot_t &rScene, const Vector2i &rRenderSize );
	bool ensureOccluderDepthUniformSet( void );
	bool buildOccluderDepthPackets( const t3dRdForwardPassContext_t	   &rContext,
									const t3dRdInstanceSceneSnapshot_t &rScene,
									uint32_t							nVisibilityMask );
	bool uploadOccluderDepthVisibility( void );
	bool rasterOccluderDepth( const t3dRdForwardPassContext_t &rContext, const t3dRdInstanceSceneSnapshot_t &rScene );
	bool ensureInstanceCullShader( void );
	void freeForwardPipelines( void );
	bool ensureForwardPipelines( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t variant );
	const t3dRdInstanceForwardPipelineEntry_t	   *
	 findForwardPipeline( const t3dRdInstanceForwardPipelineKey_t &rKey ) const;
	bool ensureOwnedBuffer( Terrain3DRdGpuBuffer &rBuffer,
							uint32_t			  nRequired,
							const char			 *pName,
							t3dRdResourceType_t	  type,
							t3dRdMemoryCategory_t memory,
							t3dRdResourceOwner_t  owner = T3D_RD_CONTEXT_OWNER_INSTANCES );
	bool
	ensureBuffers( uint32_t nInstanceBytes, uint32_t nVisibleBytes, uint32_t nMaterialBytes, uint32_t nIndirectBytes );
	bool ensureIndirectArena( uint32_t nRequiredSliceBytes );
	bool ensureShadowBuffers( uint32_t nVisibleBytes, uint32_t nIndirectBytes );
	bool ensureUniformSet( void );
	void freeForwardUniformSets( void );
	bool ensureForwardUniformSet( t3dRdForwardShaderVariant_t variant, bool bShadow );
	bool ensureVtSampler( void );
	void freeMaterialVtUniformSets( bool bRuntimeRebuild );
	bool buildMaterialVtUniformSet( const RID &rShader, RID &rUniformSet, const String &rName );
	bool ensureMaterialVtUniformSet( void );
	bool ensureForwardMaterialVtUniformSet( t3dRdForwardShaderVariant_t variant );
	bool prepareSceneUploadPlan( const t3dRdInstanceSceneSnapshot_t &rScene,
								 uint32_t							&rInstanceBytes,
								 uint32_t							&rVisibleBytes,
								 uint32_t							&rMaterialBytes,
								 uint32_t							&rIndirectBytes,
								 bool								&rFullInstanceUpload );
	bool hasRequiredSceneVisibilityCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene ) const;
	bool hasRequiredSceneMotionCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene ) const;
	bool hasRequiredScenePacketCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene ) const;
	bool hasRequiredSceneCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene ) const;
	bool ensureSceneCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool ensureSceneGpuStorage( uint32_t nInstanceBytes,
								uint32_t nVisibleBytes,
								uint32_t nMaterialBytes,
								uint32_t nIndirectBytes );
	void ensureSceneUploadByteStorage( void );
	bool uploadSceneInstanceRecords( const t3dRdInstanceSceneSnapshot_t &rScene,
									 uint32_t							 nInstanceBytes,
									 bool								 bFullInstanceUpload );
	bool uploadScene( const t3dRdInstanceSceneSnapshot_t *pScene );
	bool
		 uploadPreviousMotionRanges( const t3dRdInstanceRecord_t											   *pRecords,
									 const eastl::vector< t3dRdInstanceDirtyRange_t, Terrain3DEastlAllocator > &rRanges );
	bool prepareMotionHistory( const t3dRdInstanceSceneSnapshot_t &rScene, uint64_t nRenderFrame );
	bool prepareForwardResources( const t3dRdForwardPassContext_t	 &rContext,
								  const t3dRdMeshSourceSnapshot_t	 *pMeshes,
								  const t3dRdInstanceSceneSnapshot_t *pScene,
								  t3dRdForwardShaderVariant_t		  variant,
								  bool								  bShadowPass );
	bool prepareForwardVisibility( const t3dRdForwardPassContext_t	  &rContext,
								   const t3dRdInstanceSceneSnapshot_t &rScene,
								   uint32_t							   nVisibilityMask,
								   bool								   bTemporalHistoryValid,
								   bool								   bShadowPass,
								   bool								  &rGpuVisibility );
	bool updateForwardFrameBuffer( const t3dRdForwardPassContext_t &rContext, bool bTemporalHistoryValid );
	bool runForwardCullParity( const t3dRdForwardPassContext_t	  &rContext,
							   const t3dRdInstanceSceneSnapshot_t &rScene,
							   uint32_t							   nVisibilityMask,
							   bool								   bShadowPass,
							   bool								   bGpuVisibility,
							   bool								  &rCullDispatchedForParity );
	bool dispatchForwardGpuVisibility( const t3dRdForwardPassContext_t	  &rContext,
									   const t3dRdInstanceSceneSnapshot_t &rScene,
									   uint32_t							   nVisibilityMask,
									   bool								   bShadowPass,
									   bool								   bGpuVisibility,
									   bool								   bCullDispatchedForParity );
	bool prepareForwardLighting( const t3dRdForwardPassContext_t &rContext,
								 uint32_t						  nVisibilityMask,
								 t3dRdForwardShaderVariant_t	  variant,
								 bool							  bViewportReconfigured );
	void commitForwardPreparedState( const t3dRdForwardPassContext_t	&rContext,
									 const t3dRdInstanceSceneSnapshot_t &rScene,
									 uint32_t							 nVisibilityMask,
									 bool								 bShadowPass,
									 bool								 bGpuVisibility );
	bool uploadMaterials( void );
	void requestPublishedMaterialResidencyHot( const t3dRdForwardPassContext_t	  &rContext,
											   const t3dRdInstanceSceneSnapshot_t &rScene,
											   uint32_t							   nVisibilityMask );
	template< bool t_bShadowPass >
	bool beginPacketBuildForPass( const t3dRdInstanceSceneSnapshot_t &rScene, bool bSkipFrustum );
	template< bool t_bShadowPass >
	bool								collectPacketEntriesForPass( const t3dRdInstanceSceneSnapshot_t &rScene,
																	 const Projection					*pViewProjections,
																	 uint32_t							 nViewCount,
																	 const Vector3						&rCamera,
																	 uint32_t							 nVisibilityMask,
																	 bool								 bSkipFrustum );
	template< bool t_bShadowPass > bool buildVisibleOutputForPass( bool bSkipFrustum, bool &rDeferVisibilityOutput );
	template< bool t_bShadowPass > bool finalizePacketCommandsForPass( bool bSkipFrustum, bool bDeferVisibilityOutput );
	template< bool t_bShadowPass >
	bool
	buildCullDescriptorsForPass( const t3dRdInstanceSceneSnapshot_t &rScene, uint32_t nViewCount, bool bSkipFrustum );
	template< bool t_bShadowPass >
	bool								buildPacketsForPass( const t3dRdInstanceSceneSnapshot_t &rScene,
															 const Projection					*pViewProjections,
															 uint32_t							 nViewCount,
															 const Vector3						&rCamera,
															 uint32_t							 nVisibilityMask,
															 bool								 bSkipFrustum );
	bool								buildPackets( const t3dRdInstanceSceneSnapshot_t &rScene,
													  const Projection					 *pViewProjections,
													  uint32_t							  nViewCount,
													  const Vector3						 &rCamera,
													  uint32_t							  nVisibilityMask,
													  bool								  bShadowPass,
													  bool								  bSkipFrustum = false );
	bool								uploadVisibility( void );
	bool								uploadShadowVisibility( void );
	template< bool t_bShadowPass > bool buildGpuCommandTableForPass( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool								buildFixedDrawTable( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool								buildFixedVisibleLayout( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool								uploadFixedDrawMetadata( void );
	bool								buildGpuPacketTopology( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool								updateGpuCullDescriptorSpanDirty(
									   const t3dRdInstanceSceneSnapshot_t											 &rScene,
									   uint32_t																		  nCellIndex,
									   eastl::vector< t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator >		 &rDescriptors,
									   const eastl::vector< t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator > &rSpans,
									   uint32_t																		  nDescriptorBase,
									   bool																			  bShadow,
									   uint32_t																		 &rUploadedBytes );
	bool refreshGpuCullDescriptorsDirty( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool refreshGpuPacketDerivedBounds( void );
	bool uploadGpuCullDescriptors( void );
	bool uploadInstanceCommandOwnerTable( void );
	bool dispatchInstanceCull( uint32_t nVisibilityMask, uint32_t nViewCount, bool bShadowPass );
	bool buildGpuTransparentOrder( const Vector3 &rCamera );
	bool compareGpuCpuInstanceCulling( const t3dRdInstanceSceneSnapshot_t &rScene,
									   const Projection					  *pViewProjections,
									   uint32_t							   nViewCount,
									   const Vector3					  &rCamera,
									   uint32_t							   nVisibilityMask,
									   bool								   bShadowPass );
	void invalidateGpuCullTopologyAfterReferenceBuild( void );
	bool
	ensureViewState( uint32_t nView, const RID &rColor, const RID &rDepth, RenderingDevice::TextureSamples samples );
	bool ensurePreviewTargets( const Vector2i &rSize );
	void freePreviewTargets( void );
	void freeViewState( t3dRdInstanceViewState_t &rState );
	bool drawPacket( int64_t				   nDrawList,
					 t3dRdInstanceViewState_t &rState,
					 uint32_t				  &nBoundPipeline,
					 uint32_t				   nPacket,
					 bool					   bDepthPrepass );
	bool
	drawView( uint32_t nView, const Projection &rViewProjection, const Vector3 &rCamera, uint32_t nVisibilityMask );
	bool drawForwardView( const t3dRdForwardPassContext_t &rContext );
	void setError( const String &rError );
	void copyStylizedSunGlobals( float *pShape, float *pDiffuse, float *pSpecular, float *pRim ) const;

public:
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	Terrain3DRdInstanceRenderer( void );
	bool attachVirtualTextureCold( Terrain3DVirtualTexture *pVirtualTexture );
	~Terrain3DRdInstanceRenderer( void );

	bool initialize( void );
	bool initializePreview( void );
	void shutdown( void );
	bool render( RenderData							*pRenderData,
				 const t3dRdMeshSourceSnapshot_t	*pMeshes,
				 const t3dRdInstanceSceneSnapshot_t *pScene,
				 uint32_t							 nVisibilityMask );
	bool prepareForwardPass( const t3dRdForwardPassContext_t	&rContext,
							 const t3dRdMeshSourceSnapshot_t	*pMeshes,
							 const t3dRdInstanceSceneSnapshot_t *pScene,
							 uint32_t							 nVisibilityMask,
							 bool								 bTemporalHistoryValid,
							 bool								 bViewportReconfigured );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	bool renderPreviewToTarget( const t3dRdMeshSourceSnapshot_t *pMeshes, uint16_t nAssetId, const Vector2i &rSize );
	bool schedulePreviewReadback( const Callable &rCallback );
	bool renderPreview( const t3dRdMeshSourceSnapshot_t *pMeshes,
						uint16_t						 nAssetId,
						const Vector2i					&rSize,
						Ref< Image >					&rImage );
	Dictionary			getDiagnostics( void ) const;
	EA_FORCE_INLINE RID getOccluderDepthTexture(
		void ) const {
		return m_occluderDepth.m_bDepthValid ? m_occluderDepth.m_hDepthTexture : RID();
	}
	EA_FORCE_INLINE bool hasValidOccluderDepth(
		void ) const {
		return m_occluderDepth.m_bDepthValid && m_occluderDepth.m_hDepthTexture.is_valid();
	}
	size_t				 getRetainedCpuBytes( void ) const;
	EA_FORCE_INLINE void setDirectDrawDebugEnabled(
		bool bEnabled ) {
		m_bDirectDrawDebug = bEnabled;
	}
	EA_FORCE_INLINE void setMotionDebugEnabled(
		bool bEnabled ) {
		m_bMotionDebug = bEnabled;
	}
	EA_FORCE_INLINE void setCullingComparisonEnabled(
		bool bEnabled ) {
		m_visibility.m_bCullingComparisonEnabled = bEnabled;
	}
	void				 setStylizedSunGlobals( const Vector4 &rShape,
												const Vector4 &rDiffuse,
												const Vector4 &rSpecular,
												const Vector4 &rRim );
	EA_FORCE_INLINE void setTextureMipBiasHot(
		const float *pBias ) {
		memcpy( m_szTextureMipBias, pBias, sizeof( m_szTextureMipBias ) );
	}
	EA_FORCE_INLINE void recordVtResidencyAcceptedHot(
		const uint32_t nAccepted ) {
		m_nVtResidencyRequestsAccepted += nAccepted;
	}
	EA_FORCE_INLINE bool isCullingComparisonEnabled(
		void ) const {
		return m_visibility.m_bCullingComparisonEnabled;
	}
	EA_FORCE_INLINE const eastl::vector< t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator > &getDepthPackets(
		void ) const {
		return m_commandTable.m_depthPackets;
	}
	EA_FORCE_INLINE const eastl::vector< t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator > &getShadowPackets(
		void ) const {
		return m_commandTable.m_shadowPackets;
	}
	EA_FORCE_INLINE bool hasForwardPreparedDraws(
		void ) const {
		return m_bForwardPreparedHasDraws;
	}
	EA_FORCE_INLINE uint64_t getPacketBuildInputCellCount(
		void ) const {
		return m_commandTable.m_nPacketBuildInputCells;
	}
	EA_FORCE_INLINE uint64_t getResolvedMeshCellCount(
		void ) const {
		return m_commandTable.m_nResolvedMeshCells;
	}
	EA_FORCE_INLINE uint64_t getUnresolvedMeshCellCount(
		void ) const {
		return m_commandTable.m_nUnresolvedMeshCells;
	}
	EA_FORCE_INLINE uint64_t getDrawPacketCount(
		void ) const {
		return ( uint64_t ) ( m_commandTable.m_drawPackets.size() );
	}
	EA_FORCE_INLINE uint64_t getMeshUploadFailureCount(
		void ) const {
		return m_geometryArena.m_meshCache.getUploadFailureCount();
	}
	EA_FORCE_INLINE uint64_t getMeshTextureFallbackCount(
		void ) const {
		return 0u;
	}
	EA_FORCE_INLINE uint64_t getMeshCacheLodCount(
		void ) const {
		return ( uint64_t ) ( m_geometryArena.m_meshCache.getLodCount() );
	}
	EA_FORCE_INLINE uint64_t getMeshCacheSurfaceCount(
		void ) const {
		return ( uint64_t ) ( m_geometryArena.m_meshCache.getSurfaceCount() );
	}
	EA_FORCE_INLINE String getMeshCacheLastError(
		void ) const {
		return m_geometryArena.m_meshCache.getLastError();
	}
	EA_FORCE_INLINE uint64_t getVtDescriptorBindsInstancerPass(
		void ) const {
		return m_descriptorState.m_nVtDescriptorBindsInstancerPass;
	}
	EA_FORCE_INLINE uint64_t getVtDescriptorRebuildsRuntime(
		void ) const {
		return m_descriptorState.m_nVtDescriptorRebuildsRuntime;
	}
	uint64_t			   getForwardSubmittedDrawCount( const t3dRdForwardPassKind_t ePassKind ) const;
	EA_FORCE_INLINE String getLastError(
		void ) const {
		return m_lastError;
	}
};

#endif // TERRAIN3D_RD_INSTANCE_RENDERER_H
