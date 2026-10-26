// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_INSTANCE_RENDERER_H
#define TERRAIN3D_RD_INSTANCE_RENDERER_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_fixed_buffer.h"
#include "core/terrain_3d_sync.h"
#include "rendering/rd/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/terrain_3d_rd_instance_hiz.h"
#include "rendering/rd/terrain_3d_rd_forward_shader_bundle.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_pipeline_cache.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_descriptor_cache.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_render_target_pool.h"

#include "eastl_config.h"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <string.h>

namespace godot {
class RenderData;
}

class Terrain3DVirtualTexture;

typedef struct t3dRdInstanceDirtyRange_s {
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
} t3dRdInstanceDirtyRange_t;

typedef struct t3dRdInstanceDescriptorSpan_s {
	uint32_t m_nFirst = 0u;
	uint32_t m_nCount = 0u;
} t3dRdInstanceDescriptorSpan_t;

typedef struct t3dRdInstanceSceneSnapshot_s {
	uint64_t m_nGeneration = 0u;
	Terrain3DFixedBuffer<t3dRdInstanceRecord_t> m_instances;
	Terrain3DFixedBuffer<t3dRdInstanceCell_t> m_cells;
	Terrain3DFixedBuffer<t3dRdInstanceDirtyRange_t> m_dirtyRanges;
	Terrain3DFixedBuffer<uint32_t> m_dirtyCells;
	bool m_bInstanceLayoutChanged = true;
	bool m_bTopologyChanged = true;

	EA_FORCE_INLINE bool bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
		return m_instances.bindAllocatorZone( pZone, ePolicy ) && m_cells.bindAllocatorZone( pZone, ePolicy )
		        && m_dirtyRanges.bindAllocatorZone( pZone, ePolicy ) && m_dirtyCells.bindAllocatorZone( pZone, ePolicy );
	}
	EA_FORCE_INLINE bool reserveCold( const size_t nInstanceCapacity, const size_t nCellCapacity ) {
		return m_instances.reserveCold( nInstanceCapacity ) && m_cells.reserveCold( nCellCapacity )
		        && m_dirtyRanges.reserveCold( nCellCapacity ) && m_dirtyCells.reserveCold( nCellCapacity );
	}
	EA_FORCE_INLINE void reset( void ) {
		m_nGeneration = 0u;
		m_instances.reset();
		m_cells.reset();
		m_dirtyRanges.reset();
		m_dirtyCells.reset();
		m_bInstanceLayoutChanged = true;
		m_bTopologyChanged = true;
	}
	EA_FORCE_INLINE void shutdown( void ) {
		m_nGeneration = 0u;
		m_instances.shutdown();
		m_cells.shutdown();
		m_dirtyRanges.shutdown();
		m_dirtyCells.shutdown();
		m_bInstanceLayoutChanged = true;
		m_bTopologyChanged = true;
	}
	bool copyFrom( const t3dRdInstanceSceneSnapshot_s &rOther ) {
		if ( !m_instances.copyFrom( rOther.m_instances.data(), rOther.m_instances.size() )
		        || !m_cells.copyFrom( rOther.m_cells.data(), rOther.m_cells.size() )
		        || !m_dirtyRanges.copyFrom( rOther.m_dirtyRanges.data(), rOther.m_dirtyRanges.size() )
		        || !m_dirtyCells.copyFrom( rOther.m_dirtyCells.data(), rOther.m_dirtyCells.size() ) ) {
			return false;
		}
		m_nGeneration = rOther.m_nGeneration;
		m_bInstanceLayoutChanged = rOther.m_bInstanceLayoutChanged;
		m_bTopologyChanged = rOther.m_bTopologyChanged;
		return true;
	}
	bool copyIncrementalFrom( const t3dRdInstanceSceneSnapshot_s &rOther ) {
		bool layout_compatible = !rOther.m_bInstanceLayoutChanged && m_instances.size() == rOther.m_instances.size()
		        && m_cells.size() == rOther.m_cells.size();
		if ( layout_compatible ) {
			for ( size_t iCell = 0u; iCell < m_cells.size(); ++iCell ) {
				const t3dRdInstanceCell_t &current = m_cells[iCell];
				const t3dRdInstanceCell_t &incoming = rOther.m_cells[iCell];
				if ( current.m_nRegionX != incoming.m_nRegionX || current.m_nRegionZ != incoming.m_nRegionZ
				        || current.m_nCellX != incoming.m_nCellX || current.m_nCellZ != incoming.m_nCellZ
				        || current.m_nMeshAssetId != incoming.m_nMeshAssetId || current.m_nFirstInstance != incoming.m_nFirstInstance
				        || current.m_nInstanceCount != incoming.m_nInstanceCount ) {
					layout_compatible = false;
					break;
				}
			}
		}
		if ( !layout_compatible ) {
			return copyFrom( rOther );
		}
		if ( !m_instances.resizeUninitialized( rOther.m_instances.size() ) ) {
			return false;
		}
		for ( size_t iCell = 0u; iCell < m_cells.size(); ++iCell ) {
			const t3dRdInstanceCell_t &current = m_cells[iCell];
			const t3dRdInstanceCell_t &incoming = rOther.m_cells[iCell];
			if ( current.m_nRevision == incoming.m_nRevision || incoming.m_nInstanceCount == 0u ) {
				continue;
			}
			memcpy( m_instances.data() + incoming.m_nFirstInstance,
			        rOther.m_instances.data() + incoming.m_nFirstInstance,
			        ( size_t )( incoming.m_nInstanceCount ) * sizeof( t3dRdInstanceRecord_t ) );
		}
		if ( !m_cells.copyFrom( rOther.m_cells.data(), rOther.m_cells.size() )
		        || !m_dirtyRanges.copyFrom( rOther.m_dirtyRanges.data(), rOther.m_dirtyRanges.size() )
		        || !m_dirtyCells.copyFrom( rOther.m_dirtyCells.data(), rOther.m_dirtyCells.size() ) ) {
			return false;
		}
		m_nGeneration = rOther.m_nGeneration;
		m_bInstanceLayoutChanged = false;
		m_bTopologyChanged = rOther.m_bTopologyChanged;
		return true;
	}
} t3dRdInstanceSceneSnapshot_t;

typedef struct t3dRdInstanceDrawPacket_s {
	uint32_t m_nSurfaceSlot = UINT32_MAX;
	uint32_t m_nDrawSlot = UINT32_MAX;
	uint32_t m_nFirstVisible = 0u;
	uint32_t m_nVisibleCount = 0u;
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nOverlayMaterial = UINT16_MAX;
	uint32_t m_nFlags = 0u;
	float m_fSortDistanceSq = 0.0f;
	AABB m_cullBounds;
	uint32_t m_nVisibilityLayers = 1u;
	float m_fLodBegin = 0.0f;
	float m_fLodEnd = 0.0f;
	float m_fFadeMargin = 0.0f;
	float m_fReserved0 = 0.0f;
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
		return m_nSurfaceSlot == rOther.m_nSurfaceSlot && m_nFlags == rOther.m_nFlags && m_nMaterial == rOther.m_nMaterial
		        && m_nOverlayMaterial == rOther.m_nOverlayMaterial && m_nVisibilityLayers == rOther.m_nVisibilityLayers
		        && m_nUnique == rOther.m_nUnique;
	}
} t3dRdPacketKey_t;

typedef struct t3dRdPacketKeyHash_s {
	EA_FORCE_INLINE size_t operator()( const t3dRdPacketKey_t &rKey ) const {
		uint64_t hash = 1469598103934665603ull;
		const uint32_t values[6] = {
			rKey.m_nSurfaceSlot, rKey.m_nFlags, rKey.m_nMaterial, rKey.m_nOverlayMaterial, rKey.m_nVisibilityLayers, rKey.m_nUnique
		};
		for ( size_t iValue = 0u; iValue < T3D_ARRAY_COUNT( values ); iValue++ ) {
			const uint32_t value = ( values )[iValue];
			hash ^= value;
			hash *= 1099511628211ull;
		}
		return ( size_t )( hash );
	}
} t3dRdPacketKeyHash_t;

typedef struct t3dRdPacketKeyLess_s {
	EA_FORCE_INLINE bool operator()( const t3dRdPacketKey_t &a, const t3dRdPacketKey_t &b ) const {
		if ( a.m_nSurfaceSlot != b.m_nSurfaceSlot ) {
			return a.m_nSurfaceSlot < b.m_nSurfaceSlot;
		}
		if ( a.m_nFlags != b.m_nFlags ) {
			return a.m_nFlags < b.m_nFlags;
		}
		if ( a.m_nMaterial != b.m_nMaterial ) {
			return a.m_nMaterial < b.m_nMaterial;
		}
		if ( a.m_nOverlayMaterial != b.m_nOverlayMaterial ) {
			return a.m_nOverlayMaterial < b.m_nOverlayMaterial;
		}
		if ( a.m_nVisibilityLayers != b.m_nVisibilityLayers ) {
			return a.m_nVisibilityLayers < b.m_nVisibilityLayers;
		}
		return a.m_nUnique < b.m_nUnique;
	}
} t3dRdPacketKeyLess_t;

typedef struct t3dRdPacketBuildSlot_s {
	t3dRdInstanceDrawPacket_t m_packet;
	uint32_t m_nVisibleCount = 0u;
	uint32_t m_nWriteOffset = 0u;
	uint32_t m_nCommandIndex = UINT32_MAX;
	bool m_bOneSubmitCompatible = false;
} t3dRdPacketBuildSlot_t;

typedef struct t3dRdPacketBuildEntry_s {
	uint32_t m_nPacketIndex = 0u;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nCellIndex = 0u;
} t3dRdPacketBuildEntry_t;

#include "rendering/rd/terrain_3d_rd_instance_renderer_domains.h"

class Terrain3DRdInstanceRenderer {
private:
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
			return m_nFramebufferFormat == rOther.m_nFramebufferFormat && m_variant == rOther.m_variant && m_passKind == rOther.m_passKind
			        && m_samples == rOther.m_samples && m_nViewCount == rOther.m_nViewCount && m_bReverseCull == rOther.m_bReverseCull
			        && m_bDepthPrepassActive == rOther.m_bDepthPrepassActive;
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
	typedef struct t3dVtSetKey_s {
		RID shader;
		uint64_t signature = 0u;
		bool operator==( const t3dVtSetKey_s &rOther ) const {
			return shader == rOther.shader && signature == rOther.signature;
		}
	} t3dVtSetKey_t;

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
	Terrain3DRdPipelineCache<uint32_t, s_nMaxViews * 9u> m_viewPipelines;
	Terrain3DRdRenderTargetPool<4u> m_targetPool;
	Terrain3DRdDescriptorCache<t3dForwardSetKey_t, T3D_RD_FORWARD_SHADER_VARIANT_MAX * 2u> m_forwardDescriptors;
	Terrain3DRdDescriptorCache<t3dVtSetKey_t, T3D_RD_FORWARD_SHADER_VARIANT_MAX + 1u> m_vtDescriptors;
	Terrain3DRdInstanceCommandTable m_commandTable;
	Terrain3DRdInstanceOccluderDepth m_occluderDepth;
	Terrain3DRdInstanceDescriptorState m_descriptorState;
	Terrain3DRdInstanceUploadState m_uploadState;
	Terrain3DVirtualTexture *m_pVirtualTexture = NULL;
	Terrain3DRdForwardLighting m_forwardLighting;
	t3dRdForwardShaderBundle_t m_forwardShaderBundle{};
	t3dRdInstanceViewState_t m_szViewStates[s_nMaxViews]{};
	RID m_hShader;
	RID m_hszForwardShaders[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	RID m_hPreviewColorTexture;
	RID m_hPreviewDepthTexture;
	Vector2i m_previewSize;
	bool m_bPreviewClear = false;
	bool m_bDirectDrawDebug = false;
	bool m_bMotionDebug = false;
	String m_lastError;
	mutable t3dMutex_t m_stylizedSunMutex;
	Vector4 m_stylizedSunShape;
	Vector4 m_stylizedSunDiffuse;
	Vector4 m_stylizedSunSpecular;
	Vector4 m_stylizedSunRim;

	Terrain3DRdPipelineCache<t3dRdInstanceCullPipelineKey_t, s_nForwardPipelineCapacity * 3u> m_forwardPipelines;
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
	bool ensureShader( const Ref<RDShaderSPIRV> &rShader );
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
	bool ensureInstanceCullShader( const Ref<RDShaderSPIRV> &rShader );
	void freeForwardPipelines( void );
	bool selectForwardVariant( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t &rVariant ) const;
	bool ensureForwardPipelines( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t variant );
	const t3dRdInstanceForwardPipelineEntry_t *findForwardPipeline( const t3dRdInstanceForwardPipelineKey_t &rKey ) const;
	bool ensureHlodCullShader( const Ref<RDShaderSPIRV> &rShader );
	bool ensureBuffer( RID &rRid,
	        uint32_t &rCapacity,
	        uint32_t nRequired,
	        const String &rName,
	        Terrain3DRdContext::ResourceType type,
	        Terrain3DRdContext::MemoryCategory memory,
	        Terrain3DRdContext::ResourceOwner owner = Terrain3DRdContext::OWNER_INSTANCES );
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
	        bool bShadowPass,
	        bool &rHiZSourceReady );
	bool prepareForwardVisibility( const t3dRdForwardPassContext_t &rContext,
	        const t3dRdInstanceSceneSnapshot_t &rScene,
	        uint32_t nVisibilityMask,
	        bool bTemporalHistoryValid,
	        bool bShadowPass,
	        bool bHiZSourceReady,
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
	bool uploadHlodDescriptors( void );
	bool dispatchHlodCull( uint32_t nVisibilityMask );
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
	bool rebuildHiZRegionTopology( const t3dRdInstanceSceneSnapshot_t &rScene );
	bool uploadHiZRegionDescriptors( void );
	bool refreshGpuPacketDerivedBounds( void );
	bool uploadGpuCullDescriptors( void );
	bool dispatchInstanceCull( uint32_t nVisibilityMask, uint32_t nViewCount, bool bShadowPass, bool bAllowHiZ = true );
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

	bool initialize( const Ref<RDShaderSPIRV> &rShader,
	        const Ref<RDShaderSPIRV> &rHlodCullShader,
	        const Ref<RDShaderSPIRV> &rInstanceCullShader,
	        const Ref<RDShaderSPIRV> &rInstanceHiZReduceShader,
	        const Ref<RDShaderSPIRV> &rOccluderDepthShader,
	        const t3dRdForwardShaderBundle_t &rForwardShaders );
	bool initializePreview( const Ref<RDShaderSPIRV> &rShader );
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
	EA_FORCE_INLINE uint64_t getForwardSubmittedDrawCount( const t3dRdForwardPassKind_t ePassKind ) const {
		switch ( ePassKind ) {
			case T3D_RD_FORWARD_PASS_DEPTH:
				return m_commandTable.m_nDepthSubmittedDraws;
			case T3D_RD_FORWARD_PASS_OPAQUE:
				return m_commandTable.m_nOpaqueSubmittedDraws;
			case T3D_RD_FORWARD_PASS_SHADOW:
				return m_commandTable.m_nShadowSubmittedDraws;
			case T3D_RD_FORWARD_PASS_MOTION:
				return m_commandTable.m_nMotionSubmittedDraws;
			case T3D_RD_FORWARD_PASS_TRANSPARENT:
				return m_commandTable.m_nTransparentSubmittedDraws;
			default:
				return 0u;
		}
	}
	EA_FORCE_INLINE String getLastError( void ) const {
		return m_lastError;
	}
};

#endif // TERRAIN3D_RD_INSTANCE_RENDERER_H
