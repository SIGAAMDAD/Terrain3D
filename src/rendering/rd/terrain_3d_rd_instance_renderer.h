// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_INSTANCE_RENDERER_H
#define TERRAIN3D_RD_INSTANCE_RENDERER_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_fixed_buffer.h"
#include "rendering/rd/terrain_3d_rd_mesh_cache.h"
#include "rendering/rd/terrain_3d_rd_forward_shader_bundle.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include "eastl_config.h"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {
class RenderData;
}

typedef struct t3dRdInstanceSceneSnapshot_s {
	uint64_t m_nGeneration = 0u;
	Terrain3DFixedBuffer<t3dRdInstanceRecord_t> m_instances;
	Terrain3DFixedBuffer<t3dRdInstanceCell_t> m_cells;

	EA_FORCE_INLINE bool bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
		return m_instances.bindAllocatorZone( pZone, ePolicy ) && m_cells.bindAllocatorZone( pZone, ePolicy );
	}
	EA_FORCE_INLINE bool reserveCold( const size_t nInstanceCapacity, const size_t nCellCapacity ) {
		return m_instances.reserveCold( nInstanceCapacity ) && m_cells.reserveCold( nCellCapacity );
	}
	EA_FORCE_INLINE void reset( void ) {
		m_nGeneration = 0u;
		m_instances.reset();
		m_cells.reset();
	}
	EA_FORCE_INLINE void shutdown( void ) {
		m_nGeneration = 0u;
		m_instances.shutdown();
		m_cells.shutdown();
	}
	bool copyFrom( const t3dRdInstanceSceneSnapshot_s &rOther ) {
		if ( !m_instances.copyFrom( rOther.m_instances.data(), rOther.m_instances.size() )
		        || !m_cells.copyFrom( rOther.m_cells.data(), rOther.m_cells.size() ) ) {
			return false;
		}
		m_nGeneration = rOther.m_nGeneration;
		return true;
	}
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
} t3dRdPacketBuildSlot_t;

typedef struct t3dRdPacketBuildEntry_s {
	uint32_t m_nPacketIndex = 0u;
	uint32_t m_nFirstInstance = 0u;
	uint32_t m_nInstanceCount = 0u;
} t3dRdPacketBuildEntry_t;

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
	uint32_t m_nVisibleIndexCount = 0u;
	uint32_t m_nShadowVisibleIndexCount = 0u;
	uint32_t m_nMotionHistoryRecordCount = 0u;
	uint32_t m_nMotionHistoryCellCount = 0u;
	uint32_t m_nMotionPreviousRecordCount = 0u;
	uint32_t m_nMotionCurrentOrderCount = 0u;
	uint32_t m_nMotionPreviousOrderCount = 0u;
	RID m_hPreviewColorTexture;
	RID m_hPreviewDepthTexture;
	Vector2i m_previewSize;
	bool m_bPreviewClear = false;
	bool m_bDirectDrawDebug = false;
	bool m_bMotionDebug = false;
	String m_lastError;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_visibleIndices;
	eastl::vector<t3dRdInstanceRecord_t, Terrain3DEastlAllocator> m_motionHistoryRecords;
	eastl::vector<t3dRdInstanceCell_t, Terrain3DEastlAllocator> m_motionHistoryCells;
	eastl::vector<t3dRdInstanceRecord_t, Terrain3DEastlAllocator> m_motionPreviousRecords;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_shadowVisibleIndices;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_drawPackets;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_depthPackets;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_shadowPackets;
	eastl::vector<t3dRdInstanceMaterialPairBinding_t, Terrain3DEastlAllocator> m_materialBindings;
	eastl::vector<t3dRdHlodCullDescriptor_t, Terrain3DEastlAllocator> m_hlodDescriptors;
	eastl::vector<t3dRdPacketBuildSlot_t, Terrain3DEastlAllocator> m_packetBuildSlots;
	eastl::vector<t3dRdPacketBuildEntry_t, Terrain3DEastlAllocator> m_packetBuildEntries;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_alphaPackets;
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> m_alphaSortScratch;
	eastl::vector_map<t3dRdPacketKey_t, uint32_t, t3dRdPacketKeyLess_t, Terrain3DEastlAllocator> m_packetIndex;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_motionCurrentOrder;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_motionPreviousOrder;
	eastl::vector<uint8_t, Terrain3DEastlAllocator> m_dirtyInstanceChunks;

	t3dRdInstanceForwardPipelineEntry_t m_szForwardPipelines[s_nForwardPipelineCapacity]{};
	uint32_t m_nForwardPipelineCount = 0u;
	uint32_t m_nPipelineEvictCursor = 0u;
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
	uint64_t m_nBufferCapacityMissCount = 0u;
	uint64_t m_nBufferRebuildCount = 0u;
	uint64_t m_nBufferRebuildFailureCount = 0u;
	uint64_t m_nCpuCapacityRebuildCount = 0u;
	uint64_t m_nCpuCapacityRebuildFailureCount = 0u;
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
	bool ensureBuffer( RID &rRid,
	        uint32_t &rCapacity,
	        uint32_t nRequired,
	        const String &rName,
	        Terrain3DRdContext::ResourceType type,
	        Terrain3DRdContext::MemoryCategory memory,
	        Terrain3DRdContext::ResourceOwner owner = Terrain3DRdContext::OWNER_INSTANCES );
	bool ensureBuffers( uint32_t nInstanceBytes, uint32_t nVisibleBytes, uint32_t nMaterialBytes, uint32_t nIndirectBytes );
	bool ensureShadowBuffers( uint32_t nVisibleBytes, uint32_t nIndirectBytes );
	bool ensureUniformSet( void );
	bool ensureShadowUniformSet( void );
	bool ensureMaterialSamplers( void );
	bool createFallbackMaterialTexture( RID &rTexture, uint32_t nRgba, const String &rName );
	const t3dRdInstanceMaterial_t *getMaterialPtr( uint32_t nIndex ) const;
	RID getMaterialTexture( const t3dRdInstanceMaterial_t *pMaterial, uint32_t nSlot, const RID &rFallback ) const;
	RID getMaterialSampler( const t3dRdInstanceMaterial_t *pMaterial ) const;
	bool addMaterialBinding( uint32_t nMaterial, uint32_t nOverlay );
	bool ensureFallbackMaterialTextures( void );
	bool rebuildMaterialBindings( void );
	void freeMaterialBindings( void );
	RID getMaterialBinding( uint32_t nMaterial, uint32_t nOverlayMaterial ) const;
	bool uploadScene( const t3dRdInstanceSceneSnapshot_t *pScene );
	bool prepareMotionHistory( const t3dRdInstanceSceneSnapshot_t &rScene, uint64_t nRenderFrame );
	bool uploadMaterials( void );
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
	bool ensureViewState( uint32_t nView, const RID &rColor, const RID &rDepth, RenderingDevice::TextureSamples samples );
	bool ensurePreviewTargets( const Vector2i &rSize );
	void freePreviewTargets( void );
	void freeViewState( t3dRdInstanceViewState_t &rState );
	bool drawPacket( int64_t nDrawList, t3dRdInstanceViewState_t &rState, uint32_t &nBoundPipeline, uint32_t nPacket, bool bDepthPrepass );
	bool drawView( uint32_t nView, const Projection &rViewProjection, const Vector3 &rCamera, uint32_t nVisibilityMask );
	bool drawForwardView( const t3dRdForwardPassContext_t &rContext );
	void setError( const String &rError );

public:
	bool bindRuntimeAllocator( Terrain3DMemoryZone *pZone, t3dAllocatorOverflowPolicy_t ePolicy );
	Terrain3DRdInstanceRenderer( void );
	~Terrain3DRdInstanceRenderer( void );

	bool initialize( const Ref<RDShaderSPIRV> &rShader,
	        const Ref<RDShaderSPIRV> &rHlodCullShader,
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
	size_t getRetainedCpuBytes( void ) const;
	EA_FORCE_INLINE void setDirectDrawDebugEnabled( bool bEnabled ) {
		m_bDirectDrawDebug = bEnabled;
	}
	EA_FORCE_INLINE void setMotionDebugEnabled( bool bEnabled ) {
		m_bMotionDebug = bEnabled;
	}
	EA_FORCE_INLINE const eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &getDepthPackets( void ) const {
		return m_depthPackets;
	}
	EA_FORCE_INLINE const eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &getShadowPackets( void ) const {
		return m_shadowPackets;
	}
	EA_FORCE_INLINE String getLastError( void ) const {
		return m_lastError;
	}
};

#endif // TERRAIN3D_RD_INSTANCE_RENDERER_H
