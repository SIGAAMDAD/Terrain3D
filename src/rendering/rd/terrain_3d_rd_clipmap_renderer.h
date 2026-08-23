// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_CLIPMAP_RENDERER_H
#define TERRAIN3D_RD_CLIPMAP_RENDERER_H

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_culling.h"
#include "rendering/rd/terrain_3d_rd_forward_shader_bundle.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/terrain_3d_rd_topology.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <atomic>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <mutex>
#include <vector>

namespace godot {
class RenderData;
}

class Terrain3DRdClipmapRenderer {
private:
	static constexpr uint32_t s_nMaxViews = 4u;

	typedef struct t3dRdViewState_s {
		RID m_hFramebuffer;
		RID m_hszPipelines[3];
		RID m_hColor;
		RID m_hDepth;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
		bool m_bColorWrite = true;
	} t3dRdViewState_t;

	typedef struct t3dRdForwardPipelineKey_s {
		int64_t m_nFramebufferFormat = RenderingDevice::INVALID_ID;
		t3dRdForwardShaderVariant_t m_variant = T3D_RD_FORWARD_SHADER_COLOR;
		t3dRdForwardPassKind_t m_passKind = T3D_RD_FORWARD_PASS_INVALID;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
		uint32_t m_nViewCount = 1u;
		bool m_bReverseCull = false;
		bool m_bDepthPrepassActive = false;

		bool operator==( const t3dRdForwardPipelineKey_s &rOther ) const {
			return m_nFramebufferFormat == rOther.m_nFramebufferFormat && m_variant == rOther.m_variant &&
					m_passKind == rOther.m_passKind && m_samples == rOther.m_samples && m_nViewCount == rOther.m_nViewCount &&
					m_bReverseCull == rOther.m_bReverseCull && m_bDepthPrepassActive == rOther.m_bDepthPrepassActive;
		}
	} t3dRdForwardPipelineKey_t;

	typedef struct t3dRdForwardPipelineEntry_s {
		t3dRdForwardPipelineKey_t m_key;
		RID m_hszPipelines[3];
	} t3dRdForwardPipelineEntry_t;

	Terrain3DRdContext &m_context;
	Terrain3DRdForwardLighting m_forwardLighting;
	Terrain3DRdTopology m_szTopology[MESH_TYPE_COUNT];
	RID m_hszShaders[3];
	RID m_hszForwardShaders[3][T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	RID m_hCullShader;
	RID m_hCullPipeline;
	RID m_hTerrainSampler;
	RID m_hszMaterialSamplers[4];
	RID m_hszMapSamplers[4];
	RID m_hFallbackDisplacementTexture;
	RID m_hFallbackArrayTexture;
	RID m_hFrameUniformBuffer;
	RID m_hRegionMapBuffer;
	RID m_hMaterialBuffer;
	RID m_hMaterialConfigBuffer;
	RID m_hEditorOverlayBuffer;
	RID m_hPatchBuffer;
	RID m_hVisibleBuffer;
	RID m_hIndirectBuffer;
	RID m_hszRasterUniformSets[3];
	RID m_hCullUniformSet;
	RID m_hBoundHeightTexture;
	RID m_hBoundDerivedTexture;
	RID m_hBoundControlTexture;
	RID m_hBoundColorTexture;
	RID m_hBoundMiddleCompositeTexture;
	RID m_hBoundTerrainShadowMaskTexture;
	RID m_hBoundMiddleHeightTexture;
	RID m_hBoundMiddleHeightRegionMapTexture;
	RID m_hBoundFarHeightTexture;
	RID m_hBoundFarColorTexture;
	RID m_hBoundAlbedoTexture;
	RID m_hBoundFarAlbedoTexture;
	RID m_hBoundNormalTexture;
	RID m_hBoundSurfaceTexture;
	RID m_hBoundDisplacementTexture;
	RID m_hBoundWorldNoiseTexture;
	RID m_hBoundMacroNoiseTexture;
	RID m_hBoundEditorBrushTexture;
	uint32_t m_nszBoundTierFiltering[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	t3dRdViewState_t m_szViewStates[s_nMaxViews]{};
	std::vector<t3dRdForwardPipelineEntry_t> m_forwardPipelines;
	int64_t m_nVertexFormat = RenderingDevice::INVALID_FORMAT_ID;
	uint32_t m_nRegionMapCapacityBytes = 0u;
	uint64_t m_nRegionMapGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nPatchGeneration = 0u;
	t3dRdPatch_t m_szUploadedPatches[s_nMaxClipmapLevels]{};
	uint32_t m_nUploadedPatchCount = 0u;
	uint64_t m_nForwardPreparedScenePass = 0u;
	uint64_t m_nForwardPreparedSnapshotGeneration = 0u;
	t3dRdForwardPassKind_t m_forwardPreparedCullPassKind = T3D_RD_FORWARD_PASS_INVALID;
	t3dRdForwardPipelineKey_t m_lastForwardPipelineKey{};
	bool m_bLastForwardPipelineKeyValid = false;
	bool m_bPatchUploadInitialized = false;
	std::atomic_bool m_bGpuCullingReady{ false };
	std::atomic_uint64_t m_nFramesRendered{ 0u };
	std::atomic_uint64_t m_nDrawsRendered{ 0u };
	std::atomic_uint64_t m_nCullDispatches{ 0u };
	std::atomic_uint64_t m_nIndirectDraws{ 0u };
	std::atomic_uint64_t m_nPatchUploads{ 0u };
	std::atomic_uint64_t m_nPatchDirtyRanges{ 0u };
	std::atomic_uint64_t m_nVisiblePatches{ 0u };
	std::atomic_uint64_t m_nCullComparisons{ 0u };
	std::atomic_uint64_t m_nCullMismatches{ 0u };
	std::atomic_uint64_t m_nIndirectValidationFailures{ 0u };
	std::atomic_uint64_t m_nDirectDebugDraws{ 0u };
	mutable std::mutex m_diagnosticsMutex;
	mutable std::mutex m_resourceStateMutex;
	String m_lastError;
	std::atomic_bool m_bHasError{ false };
	std::atomic_bool m_bCullingComparisonEnabled{ false };
	std::atomic_bool m_bDirectDrawDebugEnabled{ false };
	std::atomic_bool m_bMotionDebugEnabled{ false };
	t3dRdCpuCullResult_t m_cpuCullResult{};
	PackedByteArray m_frameUploadBytes;
	PackedByteArray m_materialUploadBytes;
	PackedByteArray m_materialConfigUploadBytes;
	PackedByteArray m_editorOverlayUploadBytes;
	PackedByteArray m_patchUploadBytes;
	PackedByteArray m_cullPushBytes;
	PackedByteArray m_cpuVisibleBytes;
	PackedByteArray m_cpuIndirectBytes;
	PackedByteArray m_drawPushBytes;

	void setError( const String &rError );
	void clearError( void );
	void freeViewState( t3dRdViewState_t &rState );
	void freeUniformSets( void );
	bool ensureDevice( void );
	bool ensureShaders( const Ref<RDShaderSPIRV> &rNear,
			const Ref<RDShaderSPIRV> &rMiddle,
			const Ref<RDShaderSPIRV> &rFar,
			const Ref<RDShaderSPIRV> &rCull );
	bool ensureForwardShaders( const t3dRdForwardShaderBundle_t &rShaders );
	void freeForwardPipelines( void );
	bool selectForwardVariant( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t &rVariant ) const;
	bool ensureForwardPipelines( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t variant );
	const t3dRdForwardPipelineEntry_t *findForwardPipeline( const t3dRdForwardPipelineKey_t &rKey ) const;
	bool ensureSamplers( void );
	bool ensureFallbackTextures( void );
	bool ensureBuffers( void );
	bool ensureRegionMapBuffer( uint32_t nRequiredBytes );
	bool ensureUniformSets( const RID &rHeight,
			const RID &rDerived,
			const RID &rControl,
			const RID &rColor,
			const RID &rMiddleComposite,
			const RID &rTerrainShadowMask,
			const RID &rMiddleHeight,
			const RID &rMiddleHeightRegionMap,
			const RID &rFarHeight,
			const RID &rFarColor,
			const RID &rAlbedo,
			const RID &rFarAlbedo,
			const RID &rNormal,
			const RID &rSurface,
			const RID &rDisplacement,
			const RID &rWorldNoise,
			const RID &rMacroNoise,
			const RID &rEditorBrush,
			const t3dRdMaterialConfig_t &rMaterialConfig );
	bool ensureViewState( uint32_t nView,
			const RID &rColor,
			const RID &rDepth,
			RenderingDevice::TextureSamples samples,
			bool bColorWrite );
	bool updateFrameUniforms( const t3dRdFrameSnapshot_t &rSnapshot, const Projection &rViewProjection, const Projection &rView, const Vector3 &rCamera,
			const Projection &rPreviousViewProjection = Projection(), const Vector2 &rTaaJitter = Vector2(),
			const Vector2 &rPreviousTaaJitter = Vector2(), bool bHistoryValid = false );
	bool uploadSnapshotBuffers( const t3dRdFrameSnapshot_t &rSnapshot,
			const PackedByteArray &rRegionMapBytes,
			uint64_t nRegionMapGeneration );
	bool runGpuCulling( const t3dRdFrameSnapshot_t &rSnapshot );
	bool uploadCpuIndirect( const t3dRdCpuCullResult_t &rResult );
	bool compareGpuCpuCulling( const t3dRdFrameSnapshot_t &rSnapshot,
			const Projection &rViewProjection,
			const t3dRdCpuCullResult_t &rExpected );
	bool drawView( uint32_t nView );
	bool drawViewDirect( uint32_t nView, const t3dRdCpuCullResult_t &rCullResult );
	bool drawForwardView( const t3dRdForwardPassContext_t &rContext );
	void copyProjection( const Projection &rProjection, float *pDestination ) const;

public:
	Terrain3DRdClipmapRenderer( void );
	~Terrain3DRdClipmapRenderer( void );

	bool prepareTopologies( const t3dClipmapGeometry_t &rLod0, const t3dClipmapGeometry_t &rOuter );
	bool initialize( const Ref<RDShaderSPIRV> &rNear,
			const Ref<RDShaderSPIRV> &rMiddle,
			const Ref<RDShaderSPIRV> &rFar,
			const Ref<RDShaderSPIRV> &rCull,
			const t3dRdForwardShaderBundle_t &rForwardShaders );
	void shutdown( void );
	bool render( RenderData *pRenderData,
			const t3dRdFrameSnapshot_t &rSnapshot,
			const PackedByteArray &rRegionMapBytes,
			uint64_t nRegionMapGeneration,
			const RID &rHeightTextureRd,
			const RID &rDerivedTextureRd,
			const RID &rControlTextureRd,
			const RID &rColorTextureRd,
			const RID &rMiddleCompositeTextureRd,
			const RID &rTerrainShadowMaskTextureRd,
			const RID &rMiddleHeightTextureRd,
			const RID &rMiddleHeightRegionMapTextureRd,
			const RID &rFarHeightTextureRd,
			const RID &rFarColorTextureRd,
			const RID &rAlbedoTextureRd,
			const RID &rFarAlbedoTextureRd,
			const RID &rNormalTextureRd,
			const RID &rSurfaceTextureRd,
			const RID &rDisplacementTextureRd,
			const RID &rWorldNoiseTextureRd,
			const RID &rMacroNoiseTextureRd,
			const RID &rEditorBrushTextureRd,
			bool bColorWrite );
	bool prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
			const t3dRdFrameSnapshot_t &rSnapshot,
			const PackedByteArray &rRegionMapBytes,
			uint64_t nRegionMapGeneration,
			const RID &rHeightTextureRd,
			const RID &rDerivedTextureRd,
			const RID &rControlTextureRd,
			const RID &rColorTextureRd,
			const RID &rMiddleCompositeTextureRd,
			const RID &rTerrainShadowMaskTextureRd,
			const RID &rMiddleHeightTextureRd,
			const RID &rMiddleHeightRegionMapTextureRd,
			const RID &rFarHeightTextureRd,
			const RID &rFarColorTextureRd,
			const RID &rAlbedoTextureRd,
			const RID &rFarAlbedoTextureRd,
			const RID &rNormalTextureRd,
			const RID &rSurfaceTextureRd,
			const RID &rDisplacementTextureRd,
			const RID &rWorldNoiseTextureRd,
			const RID &rMacroNoiseTextureRd,
			const RID &rEditorBrushTextureRd );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	Dictionary getDiagnostics( void ) const;
	EA_FORCE_INLINE void setCullingComparisonEnabled( const bool bEnabled ) {
		m_bCullingComparisonEnabled.store( bEnabled, std::memory_order_release );
	}
	EA_FORCE_INLINE bool isCullingComparisonEnabled( void ) const {
		return m_bCullingComparisonEnabled.load( std::memory_order_acquire );
	}
	EA_FORCE_INLINE void setDirectDrawDebugEnabled( const bool bEnabled ) {
		m_bDirectDrawDebugEnabled.store( bEnabled, std::memory_order_release );
	}
	EA_FORCE_INLINE bool isDirectDrawDebugEnabled( void ) const {
		return m_bDirectDrawDebugEnabled.load( std::memory_order_acquire );
	}
	EA_FORCE_INLINE void setMotionDebugEnabled( const bool bEnabled ) {
		m_bMotionDebugEnabled.store( bEnabled, std::memory_order_release );
	}

	EA_FORCE_INLINE bool isReady( void ) const {
		return m_context.isReady() && m_hszShaders[0].is_valid() && m_hszShaders[1].is_valid() && m_hszShaders[2].is_valid() &&
				m_szTopology[LOD0_MESH].isGpuReady() && m_szTopology[OUTER_RING_MESH].isGpuReady();
	}
	EA_FORCE_INLINE uint64_t getFramesRendered( void ) const { return m_nFramesRendered.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE uint64_t getDrawsRendered( void ) const { return m_nDrawsRendered.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE uint64_t getTopologyBytes( void ) const {
		return m_szTopology[LOD0_MESH].getGpuBytes() + m_szTopology[OUTER_RING_MESH].getGpuBytes();
	}
	String getLastError( void ) const;
};

#endif // TERRAIN3D_RD_CLIPMAP_RENDERER_H
