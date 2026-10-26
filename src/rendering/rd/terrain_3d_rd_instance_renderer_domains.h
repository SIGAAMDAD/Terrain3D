// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Internal state owners for Terrain3DRdInstanceRenderer. This header is included
// after the renderer's frozen publication/packet POD declarations.
#ifndef TERRAIN3D_RD_INSTANCE_RENDERER_DOMAINS_H
#define TERRAIN3D_RD_INSTANCE_RENDERER_DOMAINS_H

class Terrain3DRdInstanceGeometryArena {
public:
	Terrain3DRdMeshCache m_meshCache;
	uint64_t m_nUploadedMeshGeneration = 0u;
	uint64_t m_nSceneCapacityMeshGeneration = 0u;
	bool m_bSharedMeshArenaSubmittedOnce = false;
};

class Terrain3DRdInstanceVisibility {
public:
	explicit Terrain3DRdInstanceVisibility( Terrain3DRdContext &rContext ) : m_instanceHiZ( rContext ) {
	}

	Terrain3DRdInstanceHiZ m_instanceHiZ;
	Ref<RDShaderSPIRV> m_hlodCullSourceShader;
	Ref<RDShaderSPIRV> m_instanceCullSourceShader;
	Ref<RDShaderSPIRV> m_instanceHiZReduceSourceShader;
	Terrain3DRdShader m_hHlodCullShader;
	RID m_hHlodCullPipeline;
	Terrain3DRdGpuBuffer m_hHlodCullDescriptorBuffer;
	RID m_hHlodCullUniformSet;
	Terrain3DRdShader m_hInstanceCullShader;
	RID m_hInstanceCullPipeline;
	Terrain3DRdGpuBuffer m_hInstanceCullDescriptorBuffer;
	Terrain3DRdGpuBuffer m_hInstanceHiZRegionDescriptorBuffer;
	Terrain3DRdGpuBuffer m_hInstanceHiZRegionVisibilityBuffer;
	RID m_hInstanceCullUniformSet;
	Terrain3DRdGpuBuffer m_hVisibleIndexBuffer;
	Terrain3DRdGpuBuffer m_hShadowVisibleIndexBuffer;
	Terrain3DRdGpuBuffer m_hVisibleDrawSlotBuffer;
	Terrain3DRdGpuBuffer m_hShadowVisibleDrawSlotBuffer;
	Terrain3DRdGpuBuffer m_hVisibleLayerBuffer;
	Terrain3DRdGpuBuffer m_hShadowVisibleLayerBuffer;
	uint32_t m_nVisibleIndexCapacity = 0u;
	uint32_t m_nVisibleIndexCount = 0u;
	uint32_t m_nShadowVisibleIndexCount = 0u;
	uint32_t m_nFixedVisibleCapacity = 0u;
	uint64_t m_szVisibilityLayerFilteredCells[3] = {};
	bool m_bCullingComparisonEnabled = false;
	bool m_bCullParityValidatedOnce = false;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_visibleIndices;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_visibleDrawSlots;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_shadowVisibleDrawSlots;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_visibleLayers;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_shadowVisibleLayers;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_shadowVisibleIndices;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_cullParityExpectedIndices;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_cullParityActualIndices;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_cullParityCommandCounts;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_cullParityCommandWriteOffsets;
	eastl::vector<t3dRdHlodCullDescriptor_t, Terrain3DEastlAllocator> m_hlodDescriptors;
	eastl::vector<t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator> m_instanceCullDescriptors;
	eastl::vector<t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator> m_shadowInstanceCullDescriptors;
	eastl::vector<t3dRdInstanceHiZRegionDescriptor_t, Terrain3DEastlAllocator> m_instanceHiZRegionDescriptors;
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_instanceHiZRegionKeys;
	eastl::vector<t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator> m_instanceCullCellSpans;
	eastl::vector<t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator> m_shadowInstanceCullCellSpans;
	uint64_t m_nHlodCullDispatches = 0u;
	uint64_t m_nHlodCullDescriptors = 0u;
	uint64_t m_nInstanceCullDispatches = 0u;
	uint64_t m_nHiZCellDescriptors = 0u;
	uint64_t m_nHiZInstanceDescriptors = 0u;
	uint64_t m_nHiZBuildRenderFrame = UINT64_MAX;
	uint64_t m_nHiZBuildViewportKey = 0u;
	bool m_bHiZReadyForFrame = false;
	uint64_t m_nGpuTopologyBuilds = 0u;
	uint64_t m_nGpuCullReuses = 0u;
	uint64_t m_nCullParityComparisons = 0u;
	uint64_t m_nCullParityMismatches = 0u;
	uint64_t m_nCullParityTopologyMismatches = 0u;
	uint64_t m_nCullParityReadbackBytes = 0u;
	uint64_t m_nCullParityMainReuses = 0u;
	uint64_t m_nCullParityVisibleCells = 0u;
	uint64_t m_nCullParityVisibleInstances = 0u;
	uint64_t m_nCullParityShadowVisibleCells = 0u;
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
	bool m_bHiZOcclusionSubmittedOnce = false;
	uint64_t m_nHiZStableSceneGeneration = 0u;
};

class Terrain3DRdInstanceCommandTable {
public:
	Terrain3DRdGpuBuffer m_hDrawMetadataBuffer;
	Terrain3DRdGpuBuffer m_hIndirectBuffer;
	t3dRdGpuBufferSlice_t m_mainIndirectSlice;
	t3dRdGpuBufferSlice_t m_shadowIndirectSlice;
	uint64_t m_nDrawMetadataMeshGeneration = 0u;
	uint32_t m_nPacketCapacity = 0u;
	uint32_t m_nMainCompatibleCommandCount = 0u;
	uint32_t m_nShadowCompatibleCommandCount = 0u;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_fixedDrawVisibleBases;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_fixedDrawVisibleCapacities;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_shadowFixedDrawVisibleBases;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_shadowFixedDrawVisibleCapacities;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_drawPackets;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_depthPackets;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_shadowPackets;
	eastl::vector<t3dRdPacketBuildSlot_t, Terrain3DEastlAllocator> m_packetBuildSlots;
	eastl::vector<t3dRdPacketBuildEntry_t, Terrain3DEastlAllocator> m_packetBuildEntries;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_alphaPackets;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_alphaSortScratch;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_transparentPacketOrder;
	eastl::vector<uint8_t, Terrain3DEastlAllocator> m_gpuPacketBoundValid;
	eastl::vector_map<t3dRdPacketKey_t, uint32_t, t3dRdPacketKeyLess_t, Terrain3DEastlAllocator> m_packetIndex;
	uint64_t m_nDraws = 0u;
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
	bool m_bGpuProductionSubmittedOnce = false;
};

class Terrain3DRdInstanceOccluderDepth {
public:
	Ref<RDShaderSPIRV> m_sourceShader;
	Terrain3DRdShader m_hShader;
	RID m_hPipeline;
	RID m_hDepthTexture;
	// Borrowed aliases into Terrain3DRdRenderTargetPool-owned texture/framebuffer resources.
	RID m_hFramebuffer;
	RID m_hszDepthTextures[2];
	RID m_hszFramebuffers[2];
	Terrain3DRdGpuBuffer m_hVisibleIndexBuffer;
	RID m_hUniformSet;
	Vector2i m_targetSize;
	uint32_t m_nPacketCapacity = 0u;
	uint32_t m_nVisibleIndexCapacity = 0u;
	uint32_t m_nVisibleIndexCount = 0u;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_visibleIndices;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_packets;
	PackedByteArray m_visibleBytes;
	uint64_t m_nRasterFrames = 0u;
	uint64_t m_nRasterDrawPackets = 0u;
	uint64_t m_nRasterInstances = 0u;
	uint64_t m_nLastRasterRenderFrame = UINT64_MAX;
	uint64_t m_nLastRasterViewportKey = 0u;
	uint64_t m_szRasterRenderFrames[2] = { UINT64_MAX, UINT64_MAX };
	uint64_t m_szRasterViewportKeys[2] = {};
	uint64_t m_szRasterSceneGenerations[2] = {};
	uint64_t m_szRasterMeshGenerations[2] = {};
	bool m_szDepthValid[2] = {};
	uint32_t m_nWriteSlot = 0u;
	uint64_t m_nHistorySamples = 0u;
	uint64_t m_nHistoryInvalidations = 0u;
	uint64_t m_nHistoryValidFrames = 0u;
	uint64_t m_nHistoryInvalidFrames = 0u;
	uint64_t m_nTargetRebuilds = 0u;
	uint64_t m_nCapacityGrowths = 0u;
	uint64_t m_nLastHistoryDecisionRenderFrame = UINT64_MAX;
	bool m_bDepthValid = false;
};

class Terrain3DRdInstanceDescriptorState {
public:
	RID m_hUniformSet;
	RID m_hszForwardUniformSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	RID m_hszForwardShadowUniformSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	RID m_hMaterialVtUniformSet;
	RID m_hszForwardMaterialVtUniformSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	Terrain3DRdSampler m_hVtSampler;
	uint64_t m_nMaterialVtResourceSignature = 0u;
	bool m_bMaterialVtSetBuiltOnce = false;
	uint64_t m_nVtDescriptorBindsInstancerPass = 0u;
	uint64_t m_nVtDescriptorRebuildsRuntime = 0u;
};

class Terrain3DRdInstanceUploadState {
public:
	Terrain3DRdGpuBuffer m_hFrameBuffer;
	Terrain3DRdGpuBuffer m_hInstanceBuffer;
	Terrain3DRdGpuBuffer m_hPreviousInstanceBuffer;
	Terrain3DRdGpuBuffer m_hMaterialBuffer;
	uint64_t m_nUploadedSceneGeneration = 0u;
	uint32_t m_nMotionHistoryRecordCount = 0u;
	uint32_t m_nMotionHistoryCellCount = 0u;
	uint32_t m_nMotionPreviousRecordCount = 0u;
	uint32_t m_nMotionCurrentOrderCount = 0u;
	uint32_t m_nMotionPreviousOrderCount = 0u;
	eastl::vector<t3dRdInstanceRecord_t, Terrain3DEastlAllocator> m_motionHistoryRecords;
	eastl::vector<t3dRdInstanceCell_t, Terrain3DEastlAllocator> m_motionHistoryCells;
	eastl::vector<t3dRdInstanceRecord_t, Terrain3DEastlAllocator> m_motionPreviousRecords;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_motionCurrentOrder;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_motionPreviousOrder;
	eastl::vector<t3dRdInstanceDirtyRange_t, Terrain3DEastlAllocator> m_motionDirtyRanges;
	PackedByteArray m_frameBytes;
	PackedByteArray m_instanceBytes;
	PackedByteArray m_previousInstanceBytes;
	PackedByteArray m_dirtyInstanceBytes;
	PackedByteArray m_visibleBytes;
	PackedByteArray m_shadowVisibleBytes;
	PackedByteArray m_visibleDrawSlotBytes;
	PackedByteArray m_shadowVisibleDrawSlotBytes;
	PackedByteArray m_visibleLayerBytes;
	PackedByteArray m_shadowVisibleLayerBytes;
	PackedByteArray m_drawMetadataBytes;
	PackedByteArray m_materialBytes;
	PackedByteArray m_indirectBytes;
	PackedByteArray m_shadowIndirectBytes;
	PackedByteArray m_pushBytes;
	PackedByteArray m_hlodDescriptorBytes;
	PackedByteArray m_hlodCullPushBytes;
	PackedByteArray m_instanceCullDescriptorBytes;
	PackedByteArray m_instanceHiZRegionDescriptorBytes;
	PackedByteArray m_instanceCullPushBytes;
	uint64_t m_nFrames = 0u;
	uint64_t m_nPreviewSceneGeneration = 0u;
	uint64_t m_nSceneUploads = 0u;
	uint64_t m_nMotionHistoryRenderFrame = UINT64_MAX;
	uint64_t m_nMotionHistorySceneGeneration = 0u;
	uint64_t m_nMotionHistoryResets = 0u;
	uint64_t m_nMotionHistoryMatches = 0u;
	uint64_t m_nBufferCapacityMissCount = 0u;
	uint64_t m_nBufferRebuildFailureCount = 0u;
	uint64_t m_nCpuCapacityRebuildCount = 0u;
	uint64_t m_nCpuCapacityRebuildFailureCount = 0u;
	bool m_bMotionHistoryNeedsAdvance = false;
};

#endif // TERRAIN3D_RD_INSTANCE_RENDERER_DOMAINS_H
