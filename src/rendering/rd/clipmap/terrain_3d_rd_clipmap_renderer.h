// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_CLIPMAP_RENDERER_H
#define TERRAIN3D_RD_CLIPMAP_RENDERER_H

#include "Terrain3DConstants.h"
#include "rendering/geometry/terrain_3d_mesher.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "rendering/rd/infrastructure/targets/terrain_3d_rd_framebuffer.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_sampler.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_cache.h"
#include "rendering/rd/instances/terrain_3d_rd_culling.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include "rendering/geometry/terrain_3d_clipmap_strip_contract.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"
#include "rendering/rd/vt/gpu/terrain_3d_vt_shared_bindings.h"
#include "rendering/material/terrain_3d_shader_specializer.h"

#include "eastl_config.h"
#include <EASTL/vector.h>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/classes/render_data.hpp>

// UWR-P10/P11: immutable descriptor snapshot consumed by the unified world
// raster during PREPARE. RIDs remain owned by Terrain3DRdClipmapRenderer.
typedef struct t3dRdWorldTerrainRasterBindings_s {

	RID m_hFrameBuffer;
	RID m_hRegionMapBuffer;
	RID m_hMaterialBuffer;
	RID m_hPatchBuffer;
	RID m_hVisibleBuffer;
	RID m_hMaterialConfigBuffer;
	RID m_hEditorOverlayBuffer;
	RID m_hTerrainSampler;
	RID m_hMaterialSampler;
	RID m_hMapSampler;
	RID m_hHeightTexture;
	RID m_hControlTexture;
	RID m_hColorTexture;
	RID m_hAlbedoTexture;
	RID m_hNormalTexture;
	RID m_hSurfaceTexture;
	RID m_hMaterialDisplacementTexture;
	RID m_hDisplacementTexture;
	RID m_hDerivedTexture;
	RID m_hMiddleCompositeTexture;
	RID m_hMiddleHeightTexture;
	RID m_hMiddleHeightRegionMapTexture;
	RID m_hFarHeightTexture;
	RID m_hFarColorTexture;
	RID m_hFarAlbedoTexture;
	RID m_hWorldNoiseTexture;
	RID m_hMacroNoiseTexture;
	RID m_hEditorBrushTexture;
	RID m_hTerrainShadowMaskTexture;
	bool m_bPersistentFrameBuffer = false;
	bool m_bPersistentCpuBuffers = false;
	uint64_t m_nGeneration = 0u;
} t3dRdWorldTerrainRasterBindings_t;


class Terrain3DRdClipmapRenderer {
private:
	typedef struct t3dRdViewState_s {

		Terrain3DRdFramebuffer m_hFramebuffer;
		RID m_hszPipelines[3];
		RID m_hColor;
		RID m_hDepth;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
		bool m_bColorWrite = true;
	} t3dRdViewState_t;


	typedef struct t3dRdForwardPipelineKey_s : public t3dRdForwardPipelineBaseKey_t {

		bool m_bOneSunLighting = false;
		t3dTerrainShaderPath_t m_shaderPath = T3D_TERRAIN_SHADER_PATH_COMPATIBILITY_MIXED;
		uint8_t m_nShippingFeatureClass = T3D_TERRAIN_SHIPPING_FEATURE_RUNTIME;
		uint8_t m_nszLightingModelClass[3] = { T3D_TERRAIN_LIGHTING_MODEL_RUNTIME, T3D_TERRAIN_LIGHTING_MODEL_RUNTIME,
			T3D_TERRAIN_LIGHTING_MODEL_RUNTIME };
		uint8_t m_nSceneIntegrationClass = T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME;
		uint8_t m_nszDnrOutputClass[3] = { T3D_TERRAIN_DNR_OUTPUT_RUNTIME, T3D_TERRAIN_DNR_OUTPUT_RUNTIME,
			T3D_TERRAIN_DNR_OUTPUT_RUNTIME };
		uint8_t m_nMiddleSurfaceClass = T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME;

		bool operator==( const t3dRdForwardPipelineKey_s &rOther ) const {
			return ( const t3dRdForwardPipelineBaseKey_t & )( *this  ) == ( const t3dRdForwardPipelineBaseKey_t & )( rOther  )
			        && m_bOneSunLighting == rOther.m_bOneSunLighting
			        && m_shaderPath == rOther.m_shaderPath && m_nShippingFeatureClass == rOther.m_nShippingFeatureClass
			        && m_nszLightingModelClass[0] == rOther.m_nszLightingModelClass[0]
			        && m_nszLightingModelClass[1] == rOther.m_nszLightingModelClass[1]
			        && m_nszLightingModelClass[2] == rOther.m_nszLightingModelClass[2]
			        && m_nSceneIntegrationClass == rOther.m_nSceneIntegrationClass
			        && m_nszDnrOutputClass[0] == rOther.m_nszDnrOutputClass[0]
			        && m_nszDnrOutputClass[1] == rOther.m_nszDnrOutputClass[1]
			        && m_nszDnrOutputClass[2] == rOther.m_nszDnrOutputClass[2]
			        && m_nMiddleSurfaceClass == rOther.m_nMiddleSurfaceClass;
		}
	} t3dRdForwardPipelineKey_t;


	typedef struct t3dRdForwardPipelineEntry_s {

		t3dRdForwardPipelineKey_t m_key;
		RID m_hszPipelines[3];
	} t3dRdForwardPipelineEntry_t;

	typedef struct t3dRdTierPipelineKey_s {

		t3dRdForwardPipelineKey_t forward;
		uint32_t tier = 0u;
		bool operator==( const t3dRdTierPipelineKey_s &rOther ) const {
			return tier == rOther.tier && forward == rOther.forward;
		}
	} t3dRdTierPipelineKey_t;


	Terrain3DRdContext &m_context;
	Terrain3DRdForwardLighting m_forwardLighting;
	
	
	
	
	static_assert( T3D_CLIPMAP_INDIRECT_ARENA_BYTES <= t3d_rd_context_contract::fixedIndirectCapacity( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP ),
	        "Clipmap indirect commands exceed the shared fixed slice" );
	static_assert( t3d_rd_context_contract::fixedIndirectOffset( T3D_RD_CONTEXT_FIXED_INDIRECT_CLIPMAP )
	                       % sizeof( t3dRdIndexedIndirectCommand_t ) == 0u,
	        "Clipmap indexed command base must be an integral command index" );
	static_assert( T3D_CLIPMAP_STRIP_INDIRECT_OFFSET_BYTES % sizeof( t3dRdDrawIndirectCommand_t ) == 0u,
	        "Clipmap strip commands must start at a whole-command offset" );
	t3dClipmapStripPart_t m_szStripParts[MESH_TYPE_COUNT][T3D_CLIPMAP_STRIP_PART_CAPACITY]{};
	uint16_t m_szStripPartCounts[MESH_TYPE_COUNT]{};
	Terrain3DRdGpuBuffer m_hStripPartBuffer;
	Terrain3DRdShader m_hszShaders[3];
	Terrain3DRdShader m_hszForwardShaders[T3D_TERRAIN_SHADER_PATH_COUNT][3][T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	Terrain3DRdShader m_hszForwardOneSunShaders[T3D_TERRAIN_SHADER_PATH_COUNT][3][T3D_RD_FORWARD_COLOR_VARIANT_COUNT];
	Terrain3DRdShader m_hCullShader;
	RID m_hCullPipeline;
	Terrain3DRdPipelineCache<RID, 1u> m_cullPipelines;
	Terrain3DRdPipelineCache<uint32_t, T3D_MAX_VIEWS * 3u> m_viewPipelines;
	Terrain3DRdSampler m_hTerrainSampler;
	Terrain3DRdSampler m_hszMaterialSamplers[4];
	Terrain3DRdSampler m_hszMapSamplers[4];
	Terrain3DRdSampler m_hszVtSamplers[4];
	Terrain3DRdTexture m_hFallbackDisplacementTexture;
	Terrain3DRdTexture m_hFallbackArrayTexture;
	Terrain3DRdGpuBuffer m_hFrameUniformBuffer;
	Terrain3DRdGpuBuffer m_hRegionMapBuffer;
	Terrain3DRdGpuBuffer m_hMaterialBuffer;
	Terrain3DRdGpuBuffer m_hMaterialConfigBuffer;
	Terrain3DRdGpuBuffer m_hEditorOverlayBuffer;
	Terrain3DRdGpuBuffer m_hPatchBuffer;
	Terrain3DRdGpuBuffer m_hVisibleBuffer;
	Terrain3DRdGpuBuffer *m_pIndirectBuffer = NULL;
	t3dRdGpuBufferSlice_t m_indexedIndirectSlice;
	t3dRdGpuBufferSlice_t m_stripIndirectSlice;
	RID m_hszRasterUniformSets[3];
	RID m_hCullUniformSet;

	typedef struct t3dRdClipmapDescriptorKey_s {

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
		RID m_hBoundMaterialDisplacementTexture;
		RID m_hBoundDisplacementTexture;
		RID m_hBoundWorldNoiseTexture;
		RID m_hBoundMacroNoiseTexture;
		RID m_hBoundEditorBrushTexture;
		uint64_t m_nBoundSharedVTResourceGeneration = 0u;
		uint32_t m_nszBoundTierFiltering[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };

		bool operator==( const t3dRdClipmapDescriptorKey_s &rOther ) const {
			return m_hBoundHeightTexture == rOther.m_hBoundHeightTexture && m_hBoundDerivedTexture == rOther.m_hBoundDerivedTexture
			        && m_hBoundControlTexture == rOther.m_hBoundControlTexture && m_hBoundColorTexture == rOther.m_hBoundColorTexture
			        && m_hBoundMiddleCompositeTexture == rOther.m_hBoundMiddleCompositeTexture
			        && m_hBoundTerrainShadowMaskTexture == rOther.m_hBoundTerrainShadowMaskTexture
			        && m_hBoundMiddleHeightTexture == rOther.m_hBoundMiddleHeightTexture
			        && m_hBoundMiddleHeightRegionMapTexture == rOther.m_hBoundMiddleHeightRegionMapTexture
			        && m_hBoundFarHeightTexture == rOther.m_hBoundFarHeightTexture
			        && m_hBoundFarColorTexture == rOther.m_hBoundFarColorTexture && m_hBoundAlbedoTexture == rOther.m_hBoundAlbedoTexture
			        && m_hBoundFarAlbedoTexture == rOther.m_hBoundFarAlbedoTexture && m_hBoundNormalTexture == rOther.m_hBoundNormalTexture
			        && m_hBoundSurfaceTexture == rOther.m_hBoundSurfaceTexture
			        && m_hBoundMaterialDisplacementTexture == rOther.m_hBoundMaterialDisplacementTexture
			        && m_hBoundDisplacementTexture == rOther.m_hBoundDisplacementTexture
			        && m_hBoundWorldNoiseTexture == rOther.m_hBoundWorldNoiseTexture
			        && m_hBoundMacroNoiseTexture == rOther.m_hBoundMacroNoiseTexture
			        && m_hBoundEditorBrushTexture == rOther.m_hBoundEditorBrushTexture
			        && m_nBoundSharedVTResourceGeneration == rOther.m_nBoundSharedVTResourceGeneration
			        && m_nszBoundTierFiltering[0] == rOther.m_nszBoundTierFiltering[0]
			        && m_nszBoundTierFiltering[1] == rOther.m_nszBoundTierFiltering[1]
			        && m_nszBoundTierFiltering[2] == rOther.m_nszBoundTierFiltering[2];
		}
	} t3dRdClipmapDescriptorKey_t;

	typedef struct t3dSetCacheKey_s {

		RID shader;
		t3dRdClipmapDescriptorKey_t bindings;
		bool operator==( const t3dSetCacheKey_s &rOther ) const {
			return shader == rOther.shader && bindings == rOther.bindings;
		}
	} t3dSetCacheKey_t;

	Terrain3DRdDescriptorCache<t3dSetCacheKey_t, 4u> m_descriptorCache;

	t3dRdClipmapDescriptorKey_t m_boundDescriptorKey{};
	bool m_bBoundDescriptorKeyValid = false;
	t3dRdViewState_t m_szViewStates[T3D_MAX_VIEWS]{};

	Terrain3DRdPipelineCache<t3dRdTierPipelineKey_t, T3D_FORWARD_PIPELINE_CAPACITY * 3u> m_forwardPipelines;
	mutable t3dRdForwardPipelineEntry_t m_currentForwardPipelineEntry{};
	uint64_t m_nRegionMapGeneration = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nPatchGeneration = 0u;
	bool m_bRegionMapUploadInitialized = false;
	bool m_bMaterialUploadInitialized = false;
	bool m_bPersistentFrameUniformBuffer = false;
	bool m_bPersistentCpuBuffers = false;
	bool m_bEditorOverlayUploadInitialized = false;
	t3dRdEditorOverlayConfig_t m_uploadedEditorOverlay{};
	t3dRdPatch_t m_szUploadedPatches[T3D_MAX_CLIPMAP_LEVELS]{};
	uint32_t m_nUploadedPatchCount = 0u;
	uint64_t m_nForwardPreparedScenePass = 0u;
	uint64_t m_nForwardPreparedViewportKey = 0u;
	Vector3 m_szForwardEyePositions[T3D_RD_FORWARD_MAX_VIEWS]{};
	bool m_bForwardHistoryValid = false;
	uint64_t m_nForwardPreparedSnapshotGeneration = 0u;
	t3dRdForwardPassKind_t m_forwardPreparedCullPassKind = T3D_RD_FORWARD_PASS_INVALID;
	t3dRdForwardPipelineKey_t m_lastForwardPipelineKey{};
	bool m_bLastForwardPipelineKeyValid = false;
	bool m_bPatchUploadInitialized = false;
	t3dAtomicBool_t m_bGpuCullingReady{ false };
	t3dAtomicU64_t m_nFramesRendered{ 0u };
	t3dAtomicU64_t m_nDrawsRendered{ 0u };
	t3dAtomicU64_t m_nCullDispatches{ 0u };
	t3dAtomicU64_t m_nIndirectDraws{ 0u };
	t3dAtomicU64_t m_nPatchUploads{ 0u };
	t3dAtomicU64_t m_nPatchDirtyRanges{ 0u };
	t3dAtomicU64_t m_nEditorOverlayUploads{ 0u };
	t3dAtomicU64_t m_nVisiblePatches{ 0u };
	t3dAtomicU64_t m_nCullComparisons{ 0u };
	t3dAtomicU64_t m_nCullMismatches{ 0u };
	t3dAtomicU64_t m_nIndirectValidationFailures{ 0u };
	t3dAtomicU64_t m_nDirectDebugDraws{ 0u };
	mutable Terrain3DMutex m_diagnosticsMutex;
	mutable Terrain3DMutex m_resourceStateMutex;
	String m_lastError;
	uint64_t m_nRepeatedErrorCount = 0u;
	t3dAtomicBool_t m_bHasError{ false };
	t3dAtomicBool_t m_bCullingComparisonEnabled{ false };
	t3dAtomicBool_t m_bDirectDrawDebugEnabled{ false };
	t3dAtomicBool_t m_bMotionDebugEnabled{ false };
	t3dRdCpuCullResult_t m_cpuCullResult{};
	PackedByteArray m_frameUploadBytes;
	PackedByteArray m_materialUploadBytes;
	PackedByteArray m_materialConfigUploadBytes;
	PackedByteArray m_editorOverlayUploadBytes;
	PackedByteArray m_patchUploadBytes;
	PackedByteArray m_cullPushBytes;
	PackedByteArray m_cpuVisibleBytes;
	PackedByteArray m_cpuIndirectBytes;
	PackedByteArray m_cpuStripIndirectBytes;
	PackedByteArray m_stripPartBytes;
	PackedByteArray m_drawPushBytes;

	void setError( const String &rError );
	void clearError( void );
	void freeViewState( t3dRdViewState_t &rState );
	void freeUniformSets( void );
	bool ensureDevice( void );
	bool ensureShaders( void );
	bool ensureForwardShaders( void );
	void freeForwardPipelines( void );
	bool ensureForwardPipelines( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t variant, bool bOneSunLighting,
	        t3dTerrainShaderPath_t shaderPath, uint8_t nShippingFeatureClass, const uint8_t *pLightingModelClasses,
	        uint8_t nSceneIntegrationClass, const uint8_t *pDnrOutputClasses, uint8_t nMiddleSurfaceClass );
	t3dTerrainShaderPath_t selectTerrainShaderPath( const t3dRdFrameSnapshot_t &rSnapshot ) const;
	RID getForwardShader( uint32_t nTier, t3dRdForwardShaderVariant_t variant, bool bOneSunLighting,
	        t3dTerrainShaderPath_t shaderPath ) const;
	const t3dRdForwardPipelineEntry_t *findForwardPipeline( const t3dRdForwardPipelineKey_t &rKey ) const;
	bool ensureSamplers( void );
	bool ensureFallbackTextures( void );
	bool ensureBuffers( void );
	bool ensureRegionMapBuffer( uint32_t nRequiredBytes );
	t3dRdTerrainBindings_t resolveTerrainBindings( const t3dRdTerrainBindings_t &rBindings ) const;
	t3dRdClipmapDescriptorKey_t buildUniformSetDescriptorKey( const t3dRdTerrainBindings_t &rBindings,
	        const t3dRdMaterialConfig_t &rMaterialConfig,
	        const t3dSharedVTBindings_t *pSharedVTBindings ) const;
	bool hasCompatibleUniformSetDescriptorKey( const t3dRdClipmapDescriptorKey_t &rKey ) const;
	bool ensureUniformSets( const t3dRdTerrainBindings_t &rBindings,
	        const t3dRdMaterialConfig_t &rMaterialConfig,
	        const t3dSharedVTBindings_t *pSharedVTBindings );
	bool ensureViewState( uint32_t nView, const RID &rColor, const RID &rDepth, RenderingDevice::TextureSamples samples, bool bColorWrite );
	bool updateFrameUniforms( const t3dRdFrameSnapshot_t &rSnapshot,
	        const Projection *pViewProjections,
	        uint32_t nViewCount,
	        const Projection &rView,
	        const Vector3 *pCameras,
	        const Projection *pPreviousViewProjections,
	        const Vector2 &rTaaJitter,
	        const Vector2 &rPreviousTaaJitter,
	        bool bHistoryValid,
	        const t3dSharedVTBindings_t *pSharedVTBindings );
	bool uploadSnapshotBuffers(
	        const t3dRdFrameSnapshot_t &rSnapshot, const PackedByteArray &rRegionMapBytes, uint64_t nRegionMapGeneration );
	bool runGpuCulling( const t3dRdFrameSnapshot_t &rSnapshot );
	bool uploadCpuIndirect( const t3dRdCpuCullResult_t &rResult );
	bool compareGpuCpuCulling(
	        const t3dRdFrameSnapshot_t &rSnapshot, const Projection &rViewProjection, const t3dRdCpuCullResult_t &rExpected );
	bool drawView( uint32_t nView );
	bool drawViewDirect( uint32_t nView, const t3dRdCpuCullResult_t &rCullResult );
	bool drawForwardView( const t3dRdForwardPassContext_t &rContext );
	void copyProjection( const Projection &rProjection, float *pDestination ) const;
	uint32_t getStripPartCountForGroup( uint32_t nGroup ) const;
	uint32_t getStripDescriptorBaseForGroup( uint32_t nGroup ) const;
	bool buildStripIndirectCommands( const t3dRdCpuCullResult_t &rResult );

public:
	Terrain3DRdClipmapRenderer( void );
	~Terrain3DRdClipmapRenderer( void );

	bool prepareStripParts( const t3dClipmapStripPart_t *pLod0Parts,
	        uint16_t nLod0PartCount,
	        const t3dClipmapStripPart_t *pOuterParts,
	        uint16_t nOuterPartCount );
	bool initialize( void );
	void shutdown( void );
	bool render( RenderData *pRenderData,
	        const t3dRdFrameSnapshot_t &rSnapshot,
	        const PackedByteArray &rRegionMapBytes,
	        uint64_t nRegionMapGeneration,
	        const t3dRdTerrainBindings_t &rBindings,
	        bool bColorWrite );
	bool prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
	        const t3dRdFrameSnapshot_t &rSnapshot,
	        const PackedByteArray &rRegionMapBytes,
	        uint64_t nRegionMapGeneration,
	        const t3dRdTerrainBindings_t &rBindings,
	        const t3dSharedVTBindings_t *pSharedVTBindings,
	        bool bTemporalHistoryValid,
	        bool bViewportReconfigured,
	        bool bOneSunLighting );
	bool renderForwardPass( const t3dRdForwardPassContext_t &rContext );
	bool snapshotWorldRasterBindingsHot( t3dRdWorldTerrainRasterBindings_t &rBindings ) const;
	Dictionary getDiagnostics( void ) const;
	EA_FORCE_INLINE void setCullingComparisonEnabled( const bool bEnabled ) {
		m_bCullingComparisonEnabled.store( bEnabled, T3D_MEMORY_ORDER_RELEASE );
	}
	EA_FORCE_INLINE bool isCullingComparisonEnabled( void ) const {
		return m_bCullingComparisonEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE void setDirectDrawDebugEnabled( const bool bEnabled ) {
		m_bDirectDrawDebugEnabled.store( bEnabled, T3D_MEMORY_ORDER_RELEASE );
	}
	EA_FORCE_INLINE bool isDirectDrawDebugEnabled( void ) const {
		return m_bDirectDrawDebugEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE void setMotionDebugEnabled( const bool bEnabled ) {
		m_bMotionDebugEnabled.store( bEnabled, T3D_MEMORY_ORDER_RELEASE );
	}

	EA_FORCE_INLINE bool isReady( void ) const {
		return m_context.isReady() && m_hszShaders[0].isValid() && m_hszShaders[1].isValid() && m_hszShaders[2].isValid()
		        && m_hStripPartBuffer.is_valid() && m_pIndirectBuffer
		        && m_pIndirectBuffer->ownsSlice( m_indexedIndirectSlice )
		        && m_pIndirectBuffer->ownsSlice( m_stripIndirectSlice );
	}
	EA_FORCE_INLINE uint64_t getFramesRendered( void ) const {
		return m_nFramesRendered.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getDrawsRendered( void ) const {
		return m_nDrawsRendered.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	EA_FORCE_INLINE uint64_t getTopologyBytes( void ) const {
		return 0u;
	}
	EA_FORCE_INLINE uint64_t getStripDescriptorBytes( void ) const {
		return sizeof( m_szStripParts );
	}
	EA_FORCE_INLINE bool usesPersistentFrameBuffer( void ) const {
		return m_bPersistentFrameUniformBuffer;
	}
	EA_FORCE_INLINE bool usesPersistentCpuBuffers( void ) const {
		return m_bPersistentCpuBuffers;
	}
	String getLastError( void ) const;
};

#endif // TERRAIN3D_RD_CLIPMAP_RENDERER_H
