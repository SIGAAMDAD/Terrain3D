// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_RD_INSTANCE_RENDERER_H
#define TERRAIN3D_RD_INSTANCE_RENDERER_H

#include "core/terrain_3d_flat_hash.h"
#include "rendering/rd/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/terrain_3d_rd_forward_shader_bundle.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <vector>

namespace godot { class RenderData; }

typedef struct t3dRdInstanceSceneSnapshot_s {
	uint64_t m_nGeneration = 0u;
	std::vector<t3dRdInstanceRecord_t> m_instances;
	std::vector<t3dRdInstanceCell_t> m_cells;
} t3dRdInstanceSceneSnapshot_t;


typedef struct t3dRdInstanceMaterialPairBinding_s {
	uint16_t m_nMaterial = UINT16_MAX;
	uint16_t m_nOverlayMaterial = UINT16_MAX;
	RID m_hUniformSet;
} t3dRdInstanceMaterialPairBinding_t;

typedef struct t3dRdInstanceDrawPacket_s {
	uint32_t m_nSurfaceSlot = UINT32_MAX;
	uint32_t m_nFirstVisible = 0u;
	uint32_t m_nVisibleCount = 0u;
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nOverlayMaterial = UINT16_MAX;
	uint32_t m_nFlags = 0u;
	float m_fSortDistanceSq = 0.0f;
	RenderingDevice::IndexBufferFormat m_indexFormat = RenderingDevice::INDEX_BUFFER_FORMAT_UINT16;
	AABB m_cullBounds;
	uint32_t m_nVisibilityLayers = 1u;
	t3dRdIndexedIndirectCommand_t m_command{};
} t3dRdInstanceDrawPacket_t;

typedef struct t3dRdPacketKey_s {
	uint32_t m_nSurfaceSlot = UINT32_MAX;
	uint32_t m_nFlags = 0u;
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nOverlayMaterial = UINT16_MAX;
	uint32_t m_nVisibilityLayers = 1u;
	uint32_t m_nUnique = 0u;

	EA_FORCE_INLINE bool operator==( const t3dRdPacketKey_s &rOther ) const {
		return m_nSurfaceSlot == rOther.m_nSurfaceSlot && m_nFlags == rOther.m_nFlags &&
				m_nMaterial == rOther.m_nMaterial && m_nOverlayMaterial == rOther.m_nOverlayMaterial &&
				m_nVisibilityLayers == rOther.m_nVisibilityLayers && m_nUnique == rOther.m_nUnique;
	}
} t3dRdPacketKey_t;

typedef struct t3dRdPacketKeyHash_s {
	EA_FORCE_INLINE size_t operator()( const t3dRdPacketKey_t &rKey ) const {
		uint64_t hash = 1469598103934665603ull;
		const uint32_t values[6] = { rKey.m_nSurfaceSlot, rKey.m_nFlags, rKey.m_nMaterial,
			rKey.m_nOverlayMaterial, rKey.m_nVisibilityLayers, rKey.m_nUnique };
		for ( const uint32_t value : values ) {
			hash ^= value;
			hash *= 1099511628211ull;
		}
		return (size_t)( hash );
	}
} t3dRdPacketKeyHash_t;

typedef struct t3dRdPacketBuildSlot_s {
	t3dRdInstanceDrawPacket_t m_packet;
	uint32_t m_nVisibleCount = 0u;
	uint32_t m_nWriteOffset = 0u;
} t3dRdPacketBuildSlot_t;

typedef struct t3dRdPacketBuildEntry_s {
	uint32_t m_nPacketIndex = 0u;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
} t3dRdPacketBuildEntry_t;

class Terrain3DRdInstanceRenderer {
private:
	static constexpr uint32_t s_nMaxViews = 4u;
	typedef struct t3dRdInstanceViewState_s {
		RID m_hFramebuffer;
		RID m_szPipeline[9];
		RID m_hColor;
		RID m_hDepth;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
	} t3dRdInstanceViewState_t;
	typedef struct t3dRdInstanceForwardPipelineKey_s {
		int64_t m_nFramebufferFormat = RenderingDevice::INVALID_ID;
		t3dRdForwardShaderVariant_t m_variant = T3D_RD_FORWARD_SHADER_COLOR;
		t3dRdForwardPassKind_t m_passKind = T3D_RD_FORWARD_PASS_INVALID;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
		uint32_t m_nViewCount = 1u;
		bool m_bReverseCull = false;
		bool m_bDepthPrepassActive = false;
		bool operator==( const t3dRdInstanceForwardPipelineKey_s &rOther ) const {
			return m_nFramebufferFormat == rOther.m_nFramebufferFormat && m_variant == rOther.m_variant &&
					m_passKind == rOther.m_passKind && m_samples == rOther.m_samples && m_nViewCount == rOther.m_nViewCount &&
					m_bReverseCull == rOther.m_bReverseCull && m_bDepthPrepassActive == rOther.m_bDepthPrepassActive;
		}
	} t3dRdInstanceForwardPipelineKey_t;
	typedef struct t3dRdInstanceForwardPipelineEntry_s {
		t3dRdInstanceForwardPipelineKey_t m_key;
		RID m_hszPipelines[3];
	} t3dRdInstanceForwardPipelineEntry_t;

	Terrain3DRdContext &m_context;
	Terrain3DRdForwardLighting m_forwardLighting;
	Terrain3DRdMeshCache m_meshCache;
	t3dRdInstanceViewState_t m_szViewStates[s_nMaxViews]{};
	RID m_hShader;
	RID m_hszForwardShaders[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	RID m_hHlodCullShader;
	RID m_hHlodCullPipeline;
	RID m_hHlodCullDescriptorBuffer;
	RID m_hHlodCullUniformSet;
	RID m_hFrameBuffer;
	RID m_hInstanceBuffer;
	RID m_hPreviousInstanceBuffer;
	RID m_hVisibleIndexBuffer;
	RID m_hShadowVisibleIndexBuffer;
	RID m_hMaterialBuffer;
	RID m_hIndirectBuffer;
	RID m_hShadowIndirectBuffer;
	RID m_hUniformSet;
	RID m_hShadowUniformSet;
	RID m_hMaterialSamplers[12];
	RID m_hFallbackAlbedoTexture;
	RID m_hFallbackNormalTexture;
	RID m_hFallbackOrmTexture;
	uint64_t m_nMaterialBindingGeneration = 0u;
	uint32_t m_nInstanceCapacityBytes = 0u;
	uint32_t m_nPreviousInstanceCapacityBytes = 0u;
	uint32_t m_nVisibleCapacityBytes = 0u;
	uint32_t m_nShadowVisibleCapacityBytes = 0u;
	uint32_t m_nMaterialCapacityBytes = 0u;
	uint32_t m_nIndirectCapacityBytes = 0u;
	uint32_t m_nShadowIndirectCapacityBytes = 0u;
	uint32_t m_nHlodDescriptorCapacityBytes = 0u;
	uint64_t m_nUploadedSceneGeneration = 0u;
	uint64_t m_nUploadedMeshGeneration = 0u;
	uint64_t m_nSceneCapacityMeshGeneration = 0u;
	uint32_t m_nInstanceUsedBytes = 0u;
	uint32_t m_nPacketCapacity = 0u;
	uint32_t m_nVisibleIndexCapacity = 0u;
	RID m_hPreviewColorTexture;
	RID m_hPreviewDepthTexture;
	Vector2i m_previewSize;
	bool m_bPreviewClear = false;
	bool m_bDirectDrawDebug = false;
	bool m_bMotionDebug = false;
	String m_lastError;
	std::vector<uint32_t> m_visibleIndices;
	std::vector<t3dRdInstanceRecord_t> m_motionHistoryRecords;
	std::vector<t3dRdInstanceCell_t> m_motionHistoryCells;
	std::vector<t3dRdInstanceRecord_t> m_motionPreviousRecords;
	std::vector<uint32_t> m_shadowVisibleIndices;
	std::vector<t3dRdInstanceDrawPacket_t> m_drawPackets;
	std::vector<t3dRdInstanceDrawPacket_t> m_depthPackets;
	std::vector<t3dRdInstanceDrawPacket_t> m_shadowPackets;
	std::vector<t3dRdInstanceMaterialPairBinding_t> m_materialBindings;
	std::vector<t3dRdHlodCullDescriptor_t> m_hlodDescriptors;
	std::vector<t3dRdPacketBuildSlot_t> m_packetBuildSlots;
	std::vector<t3dRdPacketBuildEntry_t> m_packetBuildEntries;
	std::vector<t3dRdInstanceDrawPacket_t> m_alphaPackets;
	std::vector<t3dRdInstanceDrawPacket_t> m_alphaSortScratch;
	Terrain3DFlatHashMap<t3dRdPacketKey_t, uint32_t, t3dRdPacketKeyHash_t> m_packetIndex;
	std::vector<uint32_t> m_motionCurrentOrder;
	std::vector<uint32_t> m_motionPreviousOrder;
	std::vector<uint8_t> m_dirtyInstanceChunks;
	std::vector<t3dRdInstanceForwardPipelineEntry_t> m_forwardPipelines;
	PackedByteArray m_frameBytes;
	PackedByteArray m_instanceBytes;
	PackedByteArray m_previousInstanceBytes;
	PackedByteArray m_dirtyInstanceBytes;
	PackedByteArray m_visibleBytes;
	PackedByteArray m_shadowVisibleBytes;
	PackedByteArray m_materialBytes;
	PackedByteArray m_indirectBytes;
	PackedByteArray m_shadowIndirectBytes;
	PackedByteArray m_pushBytes;
	PackedByteArray m_hlodDescriptorBytes;
	PackedByteArray m_hlodCullPushBytes;
	uint64_t m_nFrames = 0u;
	uint64_t m_nPreviewSceneGeneration = 0u;
	uint64_t m_nDraws = 0u;
	uint64_t m_nVisibleInstances = 0u;
	uint64_t m_nStaticInstances = 0u;
	uint64_t m_nSceneUploads = 0u;
	uint64_t m_nDirtyUploadBytes = 0u;
	uint64_t m_nDirtyUploadRanges = 0u;
	uint64_t m_nHlodCullDispatches = 0u;
	uint64_t m_nHlodCullDescriptors = 0u;
	uint64_t m_nForwardPreparedScenePass = 0u;
	uint64_t m_nForwardPreparedViewportKey = 0u;
	uint64_t m_nMotionHistoryRenderFrame = UINT64_MAX;
	uint64_t m_nMotionHistorySceneGeneration = 0u;
	uint64_t m_nMotionHistoryResets = 0u;
	uint64_t m_nMotionHistoryMatches = 0u;
	bool m_bMotionHistoryNeedsAdvance = false;
	uint64_t m_nForwardPreparedSceneGeneration = 0u;
	t3dRdForwardPassKind_t m_forwardPreparedPacketPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t m_nForwardPreparedVisibilityMask = UINT32_MAX;
	t3dRdInstanceForwardPipelineKey_t m_lastForwardPipelineKey{};
	bool m_bLastForwardPipelineKeyValid = false;
	bool m_bForwardPreparedHasDraws = false;

	bool ensureDevice( void );
	bool ensureShader( const Ref<RDShaderSPIRV> &rShader );
	bool ensureForwardShaders( const t3dRdForwardShaderBundle_t &rShaders );
	void freeForwardPipelines( void );
	bool selectForwardVariant( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t &rVariant ) const;
	bool ensureForwardPipelines( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t variant );
	const t3dRdInstanceForwardPipelineEntry_t *findForwardPipeline( const t3dRdInstanceForwardPipelineKey_t &rKey ) const;
	bool ensureHlodCullShader( const Ref<RDShaderSPIRV> &rShader );
	bool ensureBuffer( RID &rRid, uint32_t &rCapacity, uint32_t nRequired, const String &rName,
			Terrain3DRdContext::ResourceType type, Terrain3DRdContext::MemoryCategory memory );
	bool ensureBuffers( uint32_t nInstanceBytes, uint32_t nVisibleBytes, uint32_t nMaterialBytes, uint32_t nIndirectBytes );
	bool ensureShadowBuffers( uint32_t nVisibleBytes, uint32_t nIndirectBytes );
	bool ensureUniformSet( void );
	bool ensureShadowUniformSet( void );
	bool ensureMaterialSamplers( void );
	bool ensureFallbackMaterialTextures( void );
	bool rebuildMaterialBindings( void );
	void freeMaterialBindings( void );
	RID getMaterialBinding( uint32_t nMaterial, uint32_t nOverlayMaterial ) const;
	bool uploadScene( const t3dRdInstanceSceneSnapshot_t *pScene );
	bool prepareMotionHistory( const t3dRdInstanceSceneSnapshot_t &rScene, uint64_t nRenderFrame );
	bool uploadMaterials( void );
	template <bool t_bShadowPass> bool buildPacketsForPass( const t3dRdInstanceSceneSnapshot_t &rScene,
			const Projection *pViewProjections,
			uint32_t nViewCount,
			const Vector3 &rCamera,
			uint32_t nVisibilityMask,
			bool bSkipFrustum );
	bool buildPackets( const t3dRdInstanceSceneSnapshot_t &rScene,
			const Projection *pViewProjections,
			uint32_t nViewCount,
			const Vector3 &rCamera,
			uint32_t nVisibilityMask,
			bool bShadowPass,
			bool bSkipFrustum = false );
	bool uploadVisibility( void );
	bool uploadShadowVisibility( void );
	bool uploadHlodDescriptors( void );
	bool dispatchHlodCull( uint32_t nVisibilityMask );
	bool ensureViewState( uint32_t nView, const RID &rColor, const RID &rDepth, RenderingDevice::TextureSamples samples );
	bool ensurePreviewTargets( const Vector2i &rSize );
	void freePreviewTargets( void );
	void freeViewState( t3dRdInstanceViewState_t &rState );
	bool drawView( uint32_t nView, const Projection &rViewProjection, const Vector3 &rCamera, uint32_t nVisibilityMask );
	bool drawForwardView( const t3dRdForwardPassContext_t &rContext );
	void setError( const String &rError );

public:
	Terrain3DRdInstanceRenderer( void );
	~Terrain3DRdInstanceRenderer( void );

	bool initialize( const Ref<RDShaderSPIRV> &rShader, const Ref<RDShaderSPIRV> &rHlodCullShader,
			const t3dRdForwardShaderBundle_t &rForwardShaders );
	void shutdown( void );
	bool render( RenderData *pRenderData,
			const t3dRdMeshSourceSnapshot_t *pMeshes,
			const t3dRdInstanceSceneSnapshot_t *pScene,
			uint32_t nVisibilityMask );
	bool prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
			const t3dRdMeshSourceSnapshot_t *pMeshes,
			const t3dRdInstanceSceneSnapshot_t *pScene,
			uint32_t nVisibilityMask,
			bool bTemporalHistoryValid,
			bool bViewportReconfigured );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	bool renderPreview( const t3dRdMeshSourceSnapshot_t *pMeshes, uint16_t nAssetId, const Vector2i &rSize, Ref<Image> &rImage );
	Dictionary getDiagnostics( void ) const;
	EA_FORCE_INLINE void setDirectDrawDebugEnabled( bool bEnabled ) { m_bDirectDrawDebug = bEnabled; }
	EA_FORCE_INLINE void setMotionDebugEnabled( bool bEnabled ) { m_bMotionDebug = bEnabled; }
	EA_FORCE_INLINE const std::vector<t3dRdInstanceDrawPacket_t> &getDepthPackets( void ) const { return m_depthPackets; }
	EA_FORCE_INLINE const std::vector<t3dRdInstanceDrawPacket_t> &getShadowPackets( void ) const { return m_shadowPackets; }
	EA_FORCE_INLINE String getLastError( void ) const { return m_lastError; }
};

#endif // TERRAIN3D_RD_INSTANCE_RENDERER_H
