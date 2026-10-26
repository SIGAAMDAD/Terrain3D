// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_INSTANCE_RENDERER_H
#define TERRAIN3D_RD_INSTANCE_RENDERER_H

#include "Terrain3DConstants.h"
#include "core/containers/terrain_3d_fixed_buffer.h"
#include "core/utility/terrain_3d_sync.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer_contracts.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer_domains.h"
#include "rendering/rd/instances/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_pipeline_base.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_descriptor_cache.h"
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


class Terrain3DRdInstanceRenderer {
private:
	typedef struct t3dRdInstanceViewState_s {

		Terrain3DRdFramebuffer m_hFramebuffer;
		RID m_szPipeline[9];
		RID m_hColor;
		RID m_hDepth;
		RenderingDevice::TextureSamples m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
	} t3dRdInstanceViewState_t;

	typedef struct t3dRdInstanceForwardPipelineKey_s : public t3dRdForwardPipelineBaseKey_t {

		bool operator==( const t3dRdInstanceForwardPipelineKey_s &rOther ) const {
			return ( const t3dRdForwardPipelineBaseKey_t & )( *this  ) == ( const t3dRdForwardPipelineBaseKey_t & )( rOther  );
		}
	} t3dRdInstanceForwardPipelineKey_t;

	typedef struct t3dRdInstanceForwardPipelineEntry_s {

		t3dRdInstanceForwardPipelineKey_t m_key;
		RID m_hszPipelines[3];
	} t3dRdInstanceForwardPipelineEntry_t;

	typedef struct t3dRdInstanceCullPipelineKey_s {

		t3dRdInstanceForwardPipelineKey_t forward;
		uint32_t cull = 0u;
		bool operator==( const t3dRdInstanceCullPipelineKey_s &rOther ) const {
			return cull == rOther.cull && forward == rOther.forward;
		}
	} t3dRdInstanceCullPipelineKey_t;

	typedef struct t3dForwardSetKey_s {

		RID shader;
		bool shadow = false;
		bool operator==( const t3dForwardSetKey_s &rOther ) const {
			return shader == rOther.shader && shadow == rOther.shadow;
		}
	} t3dForwardSetKey_t;

	typedef struct t3dVTSetKey_s {

		RID shader;
		uint64_t signature = 0u;
		bool operator==( const t3dVTSetKey_s &rOther ) const {
			return shader == rOther.shader && signature == rOther.signature;
		}
	} t3dVTSetKey_t;


	Terrain3DRdContext &m_context;
	Terrain3DRdInstanceGeometryArena m_geometryArena;
	Terrain3DRdInstanceVisibility m_visibility;
	Terrain3DRdPipelineCache<RID, 1u> m_hlodCullPipelines;
	Terrain3DRdPipelineCache<RID, 1u> m_instanceCullPipelines;
	Terrain3DRdDescriptorCache<RID, 1u> m_hlodCullDescriptors;
	Terrain3DRdDescriptorCache<RID, 1u> m_instanceCullDescriptors;
	Terrain3DRdDescriptorCache<RID, 1u> m_occluderDescriptors;
	Terrain3DRdDescriptorCache<RID, 1u> m_instanceSet0Descriptors;
	Terrain3DRdPipelineCache<RID, 1u> m_occluderPipelines;
	Terrain3DRdPipelineCache<uint32_t, T3D_MAX_VIEWS * 9u> m_viewPipelines;
	Terrain3DRdRenderTargetPool<4u> m_targetPool;
	Terrain3DRdDescriptorCache<t3dForwardSetKey_t, T3D_RD_FORWARD_SHADER_VARIANT_MAX * 2u> m_forwardDescriptors;
	Terrain3DRdDescriptorCache<t3dVTSetKey_t, T3D_RD_FORWARD_SHADER_VARIANT_MAX + 1u> m_vtDescriptors;
	Terrain3DRdInstanceCommandTable m_commandTable;
	Terrain3DRdInstanceOccluderDepth m_occluderDepth;
	Terrain3DRdInstanceDescriptorState m_descriptorState;
	Terrain3DRdInstanceUploadState m_uploadState;
	Terrain3DVirtualTexture *m_pVirtualTexture = NULL;
	Terrain3DRdForwardLighting m_forwardLighting;
	t3dRdInstanceViewState_t m_szViewStates[T3D_MAX_VIEWS]{};
	Terrain3DRdShader m_hShader;
	Terrain3DRdShader m_hszForwardShaders[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	RID m_hPreviewColorTexture;
	RID m_hPreviewDepthTexture;
	Vector2i m_previewSize;
	bool m_bPreviewClear = false;
	bool m_bDirectDrawDebug = false;
	bool m_bMotionDebug = false;
	String m_lastError;
	mutable Terrain3DMutex m_stylizedSunMutex;
	Vector4 m_stylizedSunShape;
	Vector4 m_stylizedSunDiffuse;
	Vector4 m_stylizedSunSpecular;
	Vector4 m_stylizedSunRim;

	Terrain3DRdPipelineCache<t3dRdInstanceCullPipelineKey_t, T3D_FORWARD_PIPELINE_CAPACITY * 3u> m_forwardPipelines;
	mutable t3dRdInstanceForwardPipelineEntry_t m_currentForwardPipelineEntry{};
	uint64_t m_nForwardPreparedScenePass = 0u;
	uint64_t m_nForwardPreparedViewportKey = 0u;
	uint64_t m_nForwardPreparedSceneGeneration = 0u;
	t3dRdForwardPassKind_t m_forwardPreparedPacketPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint32_t m_nForwardPreparedVisibilityMask = UINT32_MAX;
	t3dRdInstanceForwardPipelineKey_t m_lastForwardPipelineKey{};
	bool m_bLastForwardPipelineKeyValid = false;
	bool m_bForwardPreparedHasDraws = false;
	bool m_bForwardPreparedGpuVisibility = false;
	uint32_t m_nUploadedVtDatabaseGeneration = 0u;
	uint64_t m_nVtStableKeyMissCount = 0u;
	uint64_t m_nVtInvalidAllocationReferenceCount = 0u;
	uint64_t m_nVtOutOfRangePageReferenceCount = 0u;
	uint64_t m_nLastVtResidencyRequestFrame = UINT64_MAX;
	uint64_t m_nVtResidencyRequestAttempts = 0u;
	uint64_t m_nVtResidencyRequestsAccepted = 0u;

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
	bool buildOccluderDepthPackets(
	        const t3dRdForwardPassContext_t &rContext, const t3dRdInstanceSceneSnapshot_t &rScene, uint32_t nVisibilityMask );
	bool uploadOccluderDepthVisibility( void );
	bool rasterOccluderDepth( const t3dRdForwardPassContext_t &rContext, const t3dRdInstanceSceneSnapshot_t &rScene );
	bool ensureInstanceCullShader( void );
	void freeForwardPipelines( void );
	bool ensureForwardPipelines( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t variant );
	const t3dRdInstanceForwardPipelineEntry_t *findForwardPipeline( const t3dRdInstanceForwardPipelineKey_t &rKey ) const;
	bool ensureHlodCullShader( void );
	bool ensureOwnedBuffer( Terrain3DRdGpuBuffer &rBuffer,
	        uint32_t nRequired,
	        const char *pName,
	        t3dRdResourceType_t type,
	        t3dRdMemoryCategory_t memory,
	        t3dRdResourceOwner_t owner = T3D_RD_CONTEXT_OWNER_INSTANCES );
	bool ensureBuffers( uint32_t nInstanceBytes, uint32_t nVisibleBytes, uint32_t nMaterialBytes, uint32_t nIndirectBytes );
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
	        uint32_t &rInstanceBytes,
	        uint32_t &rVisibleBytes,
	        uint32_t &rMaterialBytes,
	        uint32_t &rIndirectBytes,
	        bool &rFullInstanceUpload );
	bool hasRequiredSceneVisibilityCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene ) const;
	bool hasRequiredSceneMotionCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene ) const;
	bool hasRequiredScenePacketCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene ) const;
	bool hasRequiredSceneCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene ) const;
	bool ensureSceneCpuStorage( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool ensureSceneGpuStorage( uint32_t nInstanceBytes, uint32_t nVisibleBytes, uint32_t nMaterialBytes, uint32_t nIndirectBytes );
	void ensureSceneUploadByteStorage( void );
	bool uploadSceneInstanceRecords(
	        const t3dRdInstanceSceneSnapshot_t &rScene, uint32_t nInstanceBytes, bool bFullInstanceUpload );
	bool uploadScene( const t3dRdInstanceSceneSnapshot_t *pScene );
	bool uploadPreviousMotionRanges(
	        const t3dRdInstanceRecord_t *pRecords, const eastl::vector<t3dRdInstanceDirtyRange_t, Terrain3DEastlAllocator> &rRanges );
	bool prepareMotionHistory( const t3dRdInstanceSceneSnapshot_t &rScene, uint64_t nRenderFrame );
	bool prepareForwardResources( const t3dRdForwardPassContext_t &rContext,
	        const t3dRdMeshSourceSnapshot_t *pMeshes,
	        const t3dRdInstanceSceneSnapshot_t *pScene,
	        t3dRdForwardShaderVariant_t variant,
	        bool bShadowPass );
	bool prepareForwardVisibility( const t3dRdForwardPassContext_t &rContext,
	        const t3dRdInstanceSceneSnapshot_t &rScene,
	        uint32_t nVisibilityMask,
	        bool bTemporalHistoryValid,
	        bool bShadowPass,
	        bool &rGpuVisibility );
	bool updateForwardFrameBuffer( const t3dRdForwardPassContext_t &rContext, bool bTemporalHistoryValid );
	bool runForwardCullParity( const t3dRdForwardPassContext_t &rContext,
	        const t3dRdInstanceSceneSnapshot_t &rScene,
	        uint32_t nVisibilityMask,
	        bool bShadowPass,
	        bool bGpuVisibility,
	        bool &rCullDispatchedForParity );
	bool dispatchForwardGpuVisibility( const t3dRdForwardPassContext_t &rContext,
	        const t3dRdInstanceSceneSnapshot_t &rScene,
	        uint32_t nVisibilityMask,
	        bool bShadowPass,
	        bool bGpuVisibility,
	        bool bCullDispatchedForParity );
	bool prepareForwardLighting( const t3dRdForwardPassContext_t &rContext,
	        uint32_t nVisibilityMask,
	        t3dRdForwardShaderVariant_t variant,
	        bool bViewportReconfigured );
	void commitForwardPreparedState( const t3dRdForwardPassContext_t &rContext,
	        const t3dRdInstanceSceneSnapshot_t &rScene,
	        uint32_t nVisibilityMask,
	        bool bShadowPass,
	        bool bGpuVisibility );
	bool uploadMaterials( void );
	void requestPublishedMaterialResidencyHot( uint32_t nFrameId );
	template <bool t_bShadowPass>
	bool beginPacketBuildForPass( const t3dRdInstanceSceneSnapshot_t &rScene, bool bSkipFrustum );
	template <bool t_bShadowPass>
	bool collectPacketEntriesForPass( const t3dRdInstanceSceneSnapshot_t &rScene,
	        const Projection *pViewProjections,
	        uint32_t nViewCount,
	        const Vector3 &rCamera,
	        uint32_t nVisibilityMask,
	        bool bSkipFrustum );
	template <bool t_bShadowPass>
	bool buildVisibleOutputForPass( bool bSkipFrustum, bool &rDeferVisibilityOutput );
	template <bool t_bShadowPass>
	bool finalizePacketCommandsForPass( bool bSkipFrustum, bool bDeferVisibilityOutput );
	template <bool t_bShadowPass>
	bool buildCullDescriptorsForPass( const t3dRdInstanceSceneSnapshot_t &rScene, uint32_t nViewCount, bool bSkipFrustum );
	template <bool t_bShadowPass>
	bool buildPacketsForPass( const t3dRdInstanceSceneSnapshot_t &rScene,
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
	template <bool t_bShadowPass>
	bool buildGpuCommandTableForPass( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool buildFixedDrawTable( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool buildFixedVisibleLayout( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool uploadFixedDrawMetadata( void );
	bool buildGpuPacketTopology( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool updateGpuCullDescriptorSpanDirty( const t3dRdInstanceSceneSnapshot_t &rScene,
	        uint32_t nCellIndex,
	        eastl::vector<t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator> &rDescriptors,
	        const eastl::vector<t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator> &rSpans,
	        uint32_t nDescriptorBase,
	        bool bShadow,
	        uint32_t &rUploadedBytes );
	bool refreshGpuCullDescriptorsDirty( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool refreshGpuPacketDerivedBounds( void );
	bool uploadGpuCullDescriptors( void );
	bool dispatchInstanceCull( uint32_t nVisibilityMask, uint32_t nViewCount, bool bShadowPass );
	bool buildGpuTransparentOrder( const Vector3 &rCamera );
	bool compareGpuCpuInstanceCulling( const t3dRdInstanceSceneSnapshot_t &rScene,
	        const Projection *pViewProjections,
	        uint32_t nViewCount,
	        const Vector3 &rCamera,
	        uint32_t nVisibilityMask,
	        bool bShadowPass );
	void invalidateGpuCullTopologyAfterReferenceBuild( void );
	bool ensureViewState( uint32_t nView, const RID &rColor, const RID &rDepth, RenderingDevice::TextureSamples samples );
	bool ensurePreviewTargets( const Vector2i &rSize );
	void freePreviewTargets( void );
	void freeViewState( t3dRdInstanceViewState_t &rState );
	bool drawPacket( int64_t nDrawList, t3dRdInstanceViewState_t &rState, uint32_t &nBoundPipeline, uint32_t nPacket, bool bDepthPrepass );
	bool drawView( uint32_t nView, const Projection &rViewProjection, const Vector3 &rCamera, uint32_t nVisibilityMask );
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
	bool renderPreviewToTarget( const t3dRdMeshSourceSnapshot_t *pMeshes, uint16_t nAssetId, const Vector2i &rSize );
	bool schedulePreviewReadback( const Callable &rCallback );
	bool renderPreview( const t3dRdMeshSourceSnapshot_t *pMeshes, uint16_t nAssetId, const Vector2i &rSize, Ref<Image> &rImage );
	Dictionary getDiagnostics( void ) const;
	EA_FORCE_INLINE RID getOccluderDepthTexture( void ) const {
		return m_occluderDepth.m_bDepthValid ? m_occluderDepth.m_hDepthTexture : RID();
	}
	EA_FORCE_INLINE bool hasValidOccluderDepth( void ) const {
		return m_occluderDepth.m_bDepthValid && m_occluderDepth.m_hDepthTexture.is_valid();
	}
	size_t getRetainedCpuBytes( void ) const;
	EA_FORCE_INLINE void setDirectDrawDebugEnabled( bool bEnabled ) {
		m_bDirectDrawDebug = bEnabled;
	}
	EA_FORCE_INLINE void setMotionDebugEnabled( bool bEnabled ) {
		m_bMotionDebug = bEnabled;
	}
	EA_FORCE_INLINE void setCullingComparisonEnabled( bool bEnabled ) {
		m_visibility.m_bCullingComparisonEnabled = bEnabled;
	}
	void setStylizedSunGlobals( const Vector4 &rShape, const Vector4 &rDiffuse, const Vector4 &rSpecular, const Vector4 &rRim );
	EA_FORCE_INLINE void recordVtResidencyAcceptedHot( const uint32_t nAccepted ) {
		m_nVtResidencyRequestsAccepted += nAccepted;
	}
	EA_FORCE_INLINE bool isCullingComparisonEnabled( void ) const {
		return m_visibility.m_bCullingComparisonEnabled;
	}
	EA_FORCE_INLINE const eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &getDepthPackets( void ) const {
		return m_commandTable.m_depthPackets;
	}
	EA_FORCE_INLINE const eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &getShadowPackets( void ) const {
		return m_commandTable.m_shadowPackets;
	}
	EA_FORCE_INLINE bool hasForwardPreparedDraws( void ) const {
		return m_bForwardPreparedHasDraws;
	}
	EA_FORCE_INLINE uint64_t getPacketBuildInputCellCount( void ) const {
		return m_commandTable.m_nPacketBuildInputCells;
	}
	EA_FORCE_INLINE uint64_t getResolvedMeshCellCount( void ) const {
		return m_commandTable.m_nResolvedMeshCells;
	}
	EA_FORCE_INLINE uint64_t getUnresolvedMeshCellCount( void ) const {
		return m_commandTable.m_nUnresolvedMeshCells;
	}
	EA_FORCE_INLINE uint64_t getDrawPacketCount( void ) const {
		return ( uint64_t )( m_commandTable.m_drawPackets.size() );
	}
	EA_FORCE_INLINE uint64_t getMeshUploadFailureCount( void ) const {
		return m_geometryArena.m_meshCache.getUploadFailureCount();
	}
	EA_FORCE_INLINE uint64_t getMeshTextureFallbackCount( void ) const {
		return 0u;
	}
	EA_FORCE_INLINE uint64_t getMeshCacheLodCount( void ) const {
		return ( uint64_t )( m_geometryArena.m_meshCache.getLodCount() );
	}
	EA_FORCE_INLINE uint64_t getMeshCacheSurfaceCount( void ) const {
		return ( uint64_t )( m_geometryArena.m_meshCache.getSurfaceCount() );
	}
	EA_FORCE_INLINE String getMeshCacheLastError( void ) const {
		return m_geometryArena.m_meshCache.getLastError();
	}
	EA_FORCE_INLINE uint64_t getVtDescriptorBindsInstancerPass( void ) const {
		return m_descriptorState.m_nVtDescriptorBindsInstancerPass;
	}
	EA_FORCE_INLINE uint64_t getVtDescriptorRebuildsRuntime( void ) const {
		return m_descriptorState.m_nVtDescriptorRebuildsRuntime;
	}
	uint64_t getForwardSubmittedDrawCount( const t3dRdForwardPassKind_t ePassKind ) const;
	EA_FORCE_INLINE String getLastError( void ) const {
		return m_lastError;
	}
};

#endif // TERRAIN3D_RD_INSTANCE_RENDERER_H
