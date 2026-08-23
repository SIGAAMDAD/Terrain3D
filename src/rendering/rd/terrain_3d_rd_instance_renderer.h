// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_RD_INSTANCE_RENDERER_H
#define TERRAIN3D_RD_INSTANCE_RENDERER_H

#include "rendering/rd/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/terrain_3d_rd_forward_shader_bundle.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <memory>
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
	bool uploadScene( const std::shared_ptr<const t3dRdInstanceSceneSnapshot_t> &pScene );
	bool prepareMotionHistory( const t3dRdInstanceSceneSnapshot_t &rScene, uint64_t nRenderFrame );
	bool uploadMaterials( void );
	bool buildPackets( const t3dRdInstanceSceneSnapshot_t &rScene, const Projection *pViewProjections, uint32_t nViewCount, const Vector3 &rCamera, uint32_t nVisibilityMask, bool bShadowPass );
	bool uploadVisibility( void );
	bool uploadShadowVisibility( void );
	bool uploadHlodDescriptors( void );
	bool dispatchHlodCull( uint32_t nVisibilityMask );
	bool ensureViewState( uint32_t nView, const RID &rColor, const RID &rDepth, RenderingDevice::TextureSamples samples );
	void freeViewState( t3dRdInstanceViewState_t &rState );
	bool drawView( uint32_t nView, const Projection &rViewProjection, const Vector3 &rCamera, uint32_t nVisibilityMask );
	bool drawForwardView( const t3dRdForwardPassContext_t &rContext );
	bool isCellVisible( const t3dRdInstanceCell_t &rCell, const Projection &rViewProjection, const Vector3 &rCamera, bool bShadowPass = false ) const;
	void setError( const String &rError );

public:
	Terrain3DRdInstanceRenderer( void );
	~Terrain3DRdInstanceRenderer( void );

	bool initialize( const Ref<RDShaderSPIRV> &rShader, const Ref<RDShaderSPIRV> &rHlodCullShader,
			const t3dRdForwardShaderBundle_t &rForwardShaders );
	void shutdown( void );
	bool render( RenderData *pRenderData,
			const std::shared_ptr<const t3dRdMeshSourceSnapshot_t> &pMeshes,
			const std::shared_ptr<const t3dRdInstanceSceneSnapshot_t> &pScene,
			uint32_t nVisibilityMask );
	bool prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
			const std::shared_ptr<const t3dRdMeshSourceSnapshot_t> &pMeshes,
			const std::shared_ptr<const t3dRdInstanceSceneSnapshot_t> &pScene,
			uint32_t nVisibilityMask );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	Dictionary getDiagnostics( void ) const;
	EA_FORCE_INLINE void setDirectDrawDebugEnabled( bool bEnabled ) { m_bDirectDrawDebug = bEnabled; }
	EA_FORCE_INLINE void setMotionDebugEnabled( bool bEnabled ) { m_bMotionDebug = bEnabled; }
	EA_FORCE_INLINE const std::vector<t3dRdInstanceDrawPacket_t> &getDepthPackets( void ) const { return m_depthPackets; }
	EA_FORCE_INLINE const std::vector<t3dRdInstanceDrawPacket_t> &getShadowPackets( void ) const { return m_shadowPackets; }
	EA_FORCE_INLINE String getLastError( void ) const { return m_lastError; }
};

#endif // TERRAIN3D_RD_INSTANCE_RENDERER_H
