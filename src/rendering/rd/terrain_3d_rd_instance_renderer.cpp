// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/infrastructure/terrain_3d_rd_binding_builder.h"
#include "Terrain3DConstants.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/terrain_3d_rd_instance_record.h"
#include "rendering/rd/terrain_3d_rd_forward_variant.h"
#include "rendering/rd/terrain_3d_rd_frustum.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"
#include "rendering/rd/vt/terrain_3d_virtual_texture.h"
#include "instances/terrain_3d_instancer_bringup.h"

#include "memory/terrain_3d_memory.h"

#include <EASTL/vector.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

typedef struct alignas( 16 ) t3dRdInstanceFrame_s {
	float m_szViewProjection[s_nRdForwardMaxViews][16];
	float m_szView[16];
	float m_szPreviousViewProjection[s_nRdForwardMaxViews][16];
	float m_szCamera[s_nRdForwardMaxViews][4];
	float m_szMotionJitter[4];
	float m_szMotionParams[4];
	float m_szNomadSunShape[4];
	float m_szNomadSunDiffuse[4];
	float m_szNomadSunSpecular[4];
	float m_szNomadSunRim[4];
} t3dRdInstanceFrame_t;
static_assert( sizeof( t3dRdInstanceFrame_t ) == 448u, "Instance frame ABI changed" );

typedef struct alignas( 16 ) t3dRdInstancePush_s {
	uint32_t m_nMaterial = UINT16_MAX;
	uint32_t m_nOverlayMaterial = UINT16_MAX;
	uint32_t m_nVisibleBase = 0u;
	uint32_t m_nFlags = 0u;
	uint32_t m_nVisibilityLayers = UINT32_MAX;
	uint32_t m_nReserved0 = 0u;
	uint32_t m_nReserved1 = 0u;
	uint32_t m_nReserved2 = 0u;
} t3dRdInstancePush_t;
static_assert( sizeof( t3dRdInstancePush_t ) == 32u, "Instance push ABI changed" );

typedef struct t3dRdHlodCullPush_s {
	uint32_t m_nDescriptorCount = 0u;
	uint32_t m_nVisibilityMask = UINT32_MAX;
	uint32_t m_nCommandCount = 0u;
	uint32_t m_nMainCommandBase = 0u;
} t3dRdHlodCullPush_t;
static_assert( sizeof( t3dRdHlodCullPush_t ) == 16u, "HLOD cull push ABI changed" );

typedef struct t3dRdInstanceCullPush_s {
	uint32_t m_nDescriptorBase = 0u;
	uint32_t m_nDescriptorCount = 0u;
	uint32_t m_nCommandCount = 0u;
	uint32_t m_nVisibilityMask = UINT32_MAX;
	uint32_t m_nViewCount = 1u;
	uint32_t m_nShadowPass = 0u;
	uint32_t m_nMode = 0u;
	uint32_t m_nHiZEnabled = 0u;
	uint32_t m_nHiZWidth = 0u;
	uint32_t m_nHiZHeight = 0u;
	uint32_t m_nHiZMipCount = 0u;
	uint32_t m_nHiZRegionCount = 0u;
	uint32_t m_nMainCommandBase = 0u;
	uint32_t m_nShadowCommandBase = 0u;
	uint32_t m_nReserved2 = 0u;
	uint32_t m_nReserved3 = 0u;
} t3dRdInstanceCullPush_t;
static_assert( sizeof( t3dRdInstanceCullPush_t ) == 64u, "Instance cull push ABI changed" );

static bool t3dRdInstanceOneSubmitCompatible( const t3dRdInstanceDrawPacket_t &rPacket ) {
	const uint32_t incompatible_flags = T3D_RD_INSTANCE_ALPHA_BLEND | T3D_RD_INSTANCE_FADE_ACTIVE | T3D_RD_INSTANCE_DOUBLE_SIDED
	        | T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED | T3D_RD_INSTANCE_CULL_FRONT;
	return ( rPacket.m_nFlags & incompatible_flags ) == 0u;
}

static bool t3dRdInstanceMeshIdentityMatchesCell( const t3dRdInstanceCell_t &rCell, const t3dRdMeshSourceLod_t *const pLod ) {
	if ( !pLod ) {
		return false;
	}
	const uint64_t expected_key =
	        rCell.m_nMeshSourceKey != 0u ? rCell.m_nMeshSourceKey : t3dRdRegularMeshSourceKey( rCell.m_nMeshAssetId, rCell.m_nLod );
	if ( pLod->m_nSourceKey != expected_key ) {
		return false;
	}
	if ( ( rCell.m_nFlags & T3D_RD_INSTANCE_HLOD ) == 0u && ( pLod->m_nAssetId != rCell.m_nMeshAssetId || pLod->m_nLod != rCell.m_nLod ) ) {
		return false;
	}
	return true;
}

static uint64_t t3dRdInstanceHashU32( uint64_t nHash, const uint32_t nValue ) {
	nHash ^= nValue;
	return nHash * 1099511628211ull;
}

static uint64_t t3dRdInstanceHashF32( uint64_t nHash, const float fValue ) {
	uint32_t bits = 0u;
	memcpy( &bits, &fValue, sizeof( bits ) );
	return t3dRdInstanceHashU32( nHash, bits );
}

static uint64_t t3dRdInstanceHashU64( uint64_t nHash, const uint64_t nValue ) {
	nHash = t3dRdInstanceHashU32( nHash, ( uint32_t )( nValue ) );
	return t3dRdInstanceHashU32( nHash, ( uint32_t )( nValue >> 32u ) );
}

static uint64_t t3dRdInstanceViewKey( const t3dRdForwardPassContext_t &rContext ) {
	uint64_t hash = 1469598103934665603ull;
	hash = t3dRdInstanceHashU32( hash, rContext.m_nViewCount );
	for ( uint32_t view = 0u; view < rContext.m_nViewCount; ++view ) {
		for ( uint32_t column = 0u; column < 4u; ++column ) {
			const Vector4 p = rContext.m_szViewProjection[view][column];
			hash = t3dRdInstanceHashF32( hash, ( float )p.x );
			hash = t3dRdInstanceHashF32( hash, ( float )p.y );
			hash = t3dRdInstanceHashF32( hash, ( float )p.z );
			hash = t3dRdInstanceHashF32( hash, ( float )p.w );
		}
	}
	return hash;
}

typedef t3dRdFrustum_t t3dRdInstanceFrustum_t;

static bool t3dRdInstanceAabbVisible( const AABB &rBounds, const t3dRdInstanceFrustum_t &rFrustum ) {
	const Vector3 center = rBounds.position + rBounds.size * 0.5f;
	const Vector3 extent = rBounds.size * 0.5f;
	for ( size_t iPlane = 0u; iPlane < T3D_ARRAY_COUNT( rFrustum.m_szPlanes ); iPlane++ ) {
		const Vector4 &plane = ( rFrustum.m_szPlanes )[iPlane];
		const double distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
		const double radius = abs( plane.x ) * extent.x + abs( plane.y ) * extent.y + abs( plane.z ) * extent.z;
		if ( distance + radius < 0.0 ) {
			return false;
		}
	}
	return true;
}

EA_FORCE_INLINE static void t3dRdFillInstanceCullDescriptorCellState(
        t3dRdInstanceCullDescriptor_t &rDescriptor, const t3dRdInstanceCell_t &rCell, const bool bShadowPass ) {
	const Vector3 center = rCell.m_bounds.get_center();
	const Vector3 extent = rCell.m_bounds.size * 0.5f;
	rDescriptor.m_szBoundsCenter[0] = ( float )center.x;
	rDescriptor.m_szBoundsCenter[1] = ( float )center.y;
	rDescriptor.m_szBoundsCenter[2] = ( float )center.z;
	rDescriptor.m_szBoundsExtent[0] = ( float )extent.x;
	rDescriptor.m_szBoundsExtent[1] = ( float )extent.y;
	rDescriptor.m_szBoundsExtent[2] = ( float )extent.z;
	const Vector3 occlusion_center = rCell.m_occlusionBounds.get_center();
	const Vector3 occlusion_extent = rCell.m_occlusionBounds.size * 0.5f;
	rDescriptor.m_szOcclusionCenter[0] = ( float )occlusion_center.x;
	rDescriptor.m_szOcclusionCenter[1] = ( float )occlusion_center.y;
	rDescriptor.m_szOcclusionCenter[2] = ( float )occlusion_center.z;
	rDescriptor.m_szOcclusionExtent[0] = ( float )occlusion_extent.x;
	rDescriptor.m_szOcclusionExtent[1] = ( float )occlusion_extent.y;
	rDescriptor.m_szOcclusionExtent[2] = ( float )occlusion_extent.z;
	rDescriptor.m_nVisibilityLayers = rCell.m_nVisibilityLayers;
	rDescriptor.m_nFlags = rCell.m_nFlags;
	rDescriptor.m_fLodBegin = bShadowPass ? 0.0f : rCell.m_fLodBegin;
	rDescriptor.m_fLodEnd = bShadowPass ? 0.0f : rCell.m_fLodEnd;
}

static bool t3dRdMotionCellsMatch( const t3dRdInstanceCell_t &rCurrent, const t3dRdInstanceCell_t &rPrevious ) {
	const bool current_hlod = ( rCurrent.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u;
	const bool previous_hlod = ( rPrevious.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u;
	if ( current_hlod != previous_hlod || rCurrent.m_nRegionX != rPrevious.m_nRegionX || rCurrent.m_nRegionZ != rPrevious.m_nRegionZ
	        || rCurrent.m_nCellX != rPrevious.m_nCellX || rCurrent.m_nCellZ != rPrevious.m_nCellZ
	        || rCurrent.m_nInstanceCount != rPrevious.m_nInstanceCount ) {
		return false;
	}
	if ( current_hlod ) {
		return rCurrent.m_nMeshSourceKey == rPrevious.m_nMeshSourceKey && rCurrent.m_nRevision == rPrevious.m_nRevision;
	}
	return rCurrent.m_nMeshAssetId == rPrevious.m_nMeshAssetId;
}

static int t3dRdMotionCellCompare( const t3dRdInstanceCell_t &rA, const t3dRdInstanceCell_t &rB ) {
	const uint32_t a_hlod = ( rA.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u ? 1u : 0u;
	const uint32_t b_hlod = ( rB.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u ? 1u : 0u;
	if ( a_hlod != b_hlod ) {
		return a_hlod < b_hlod ? -1 : 1;
	}
	if ( rA.m_nRegionX != rB.m_nRegionX ) {
		return rA.m_nRegionX < rB.m_nRegionX ? -1 : 1;
	}
	if ( rA.m_nRegionZ != rB.m_nRegionZ ) {
		return rA.m_nRegionZ < rB.m_nRegionZ ? -1 : 1;
	}
	if ( rA.m_nCellX != rB.m_nCellX ) {
		return rA.m_nCellX < rB.m_nCellX ? -1 : 1;
	}
	if ( rA.m_nCellZ != rB.m_nCellZ ) {
		return rA.m_nCellZ < rB.m_nCellZ ? -1 : 1;
	}
	if ( a_hlod != 0u ) {
		if ( rA.m_nMeshSourceKey != rB.m_nMeshSourceKey ) {
			return rA.m_nMeshSourceKey < rB.m_nMeshSourceKey ? -1 : 1;
		}
		if ( rA.m_nRevision != rB.m_nRevision ) {
			return rA.m_nRevision < rB.m_nRevision ? -1 : 1;
		}
	} else if ( rA.m_nMeshAssetId != rB.m_nMeshAssetId ) {
		return rA.m_nMeshAssetId < rB.m_nMeshAssetId ? -1 : 1;
	}
	if ( rA.m_nInstanceCount != rB.m_nInstanceCount ) {
		return rA.m_nInstanceCount < rB.m_nInstanceCount ? -1 : 1;
	}
	return 0;
}

typedef struct t3dRdMotionCellIndexLess_s {
	const t3dRdInstanceCell_t *m_pCells;

	bool operator()( const uint32_t nA, const uint32_t nB ) const {
		return t3dRdMotionCellCompare( m_pCells[nA], m_pCells[nB] ) < 0;
	}
} t3dRdMotionCellIndexLess_t;

static bool t3dRdInstanceCellVisible(
        const t3dRdInstanceCell_t &rCell, const t3dRdInstanceFrustum_t &rFrustum, const Vector3 &rCamera, const bool bShadowPass ) {
	if ( rCell.m_nInstanceCount == 0u || ( !bShadowPass && ( rCell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) != 0u ) ) {
		return false;
	}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	( void )rFrustum;
	( void )rCamera;
	return true;
#else
	if ( !bShadowPass && ( rCell.m_nFlags & T3D_RD_INSTANCE_LOD_PRESELECTED ) == 0u ) {
		const float distance_sq = ( float )( rCell.m_bounds.get_center().distance_squared_to( rCamera ) );
		if ( rCell.m_fLodBegin > 0.001f && distance_sq < rCell.m_fLodBegin * rCell.m_fLodBegin ) {
			return false;
		}
		if ( rCell.m_fLodEnd > 0.001f && distance_sq >= rCell.m_fLodEnd * rCell.m_fLodEnd ) {
			return false;
		}
	}
	return t3dRdInstanceAabbVisible( rCell.m_bounds, rFrustum );
#endif
}

typedef struct t3dRdTransparentPacketIndexLess_s {
	const t3dRdInstanceDrawPacket_t *m_pPackets = NULL;
	Vector3 m_camera;

	bool operator()( const uint32_t nA, const uint32_t nB ) const {
		const float a_distance = ( float )m_pPackets[nA].m_cullBounds.get_center().distance_squared_to( m_camera );
		const float b_distance = ( float )m_pPackets[nB].m_cullBounds.get_center().distance_squared_to( m_camera );
		if ( a_distance != b_distance ) {
			return a_distance > b_distance;
		}
		return nA < nB;
	}
} t3dRdTransparentPacketIndexLess_t;

static void t3dRdStableSortAlphaPackets( eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &rPackets,
        eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &rScratch ) {
	const size_t count = rPackets.size();
	if ( count < 2u ) {
		return;
	}
	if ( rScratch.size() < count ) {
		return;
	}
	for ( size_t width = 1u; width < count; width *= 2u ) {
		for ( size_t first = 0u; first < count; first += width * 2u ) {
			const size_t middle = MIN( first + width, count );
			const size_t last = MIN( first + width * 2u, count );
			size_t left = first;
			size_t right = middle;
			for ( size_t out = first; out < last; ++out ) {
				const bool take_left =
				        right >= last || ( left < middle && rPackets[left].m_fSortDistanceSq >= rPackets[right].m_fSortDistanceSq );
				rScratch[out] = take_left ? rPackets[left++] : rPackets[right++];
			}
		}
		for ( size_t i = 0u; i < count; ++i ) {
			rPackets[i] = rScratch[i];
		}
		if ( width > count / 2u ) {
			break;
		}
	}
}


bool Terrain3DRdInstanceRenderer::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone || !m_geometryArena.m_meshCache.bindRuntimeAllocator( pZone, ePolicy ) ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_visibility.m_visibleIndices.set_allocator( allocator );
	m_visibility.m_visibleDrawSlots.set_allocator( allocator );
	m_visibility.m_shadowVisibleDrawSlots.set_allocator( allocator );
	m_visibility.m_visibleLayers.set_allocator( allocator );
	m_visibility.m_shadowVisibleLayers.set_allocator( allocator );
	m_commandTable.m_fixedDrawVisibleBases.set_allocator( allocator );
	m_commandTable.m_fixedDrawVisibleCapacities.set_allocator( allocator );
	m_commandTable.m_shadowFixedDrawVisibleBases.set_allocator( allocator );
	m_commandTable.m_shadowFixedDrawVisibleCapacities.set_allocator( allocator );
	m_uploadState.m_motionHistoryRecords.set_allocator( allocator );
	m_uploadState.m_motionHistoryCells.set_allocator( allocator );
	m_uploadState.m_motionPreviousRecords.set_allocator( allocator );
	m_visibility.m_shadowVisibleIndices.set_allocator( allocator );
	m_visibility.m_cullParityExpectedIndices.set_allocator( allocator );
	m_visibility.m_cullParityActualIndices.set_allocator( allocator );
	m_visibility.m_cullParityCommandCounts.set_allocator( allocator );
	m_visibility.m_cullParityCommandWriteOffsets.set_allocator( allocator );
	m_commandTable.m_drawPackets.set_allocator( allocator );
	m_commandTable.m_depthPackets.set_allocator( allocator );
	m_commandTable.m_shadowPackets.set_allocator( allocator );
	m_visibility.m_hlodDescriptors.set_allocator( allocator );
	m_visibility.m_instanceCullDescriptors.set_allocator( allocator );
	m_visibility.m_shadowInstanceCullDescriptors.set_allocator( allocator );
	m_visibility.m_instanceHiZRegionDescriptors.set_allocator( allocator );
	m_visibility.m_instanceHiZRegionKeys.set_allocator( allocator );
	m_visibility.m_instanceCullCellSpans.set_allocator( allocator );
	m_visibility.m_shadowInstanceCullCellSpans.set_allocator( allocator );
	m_commandTable.m_packetBuildSlots.set_allocator( allocator );
	m_commandTable.m_packetBuildEntries.set_allocator( allocator );
	m_commandTable.m_alphaPackets.set_allocator( allocator );
	m_commandTable.m_alphaSortScratch.set_allocator( allocator );
	m_commandTable.m_transparentPacketOrder.set_allocator( allocator );
	m_commandTable.m_gpuPacketBoundValid.set_allocator( allocator );
	m_commandTable.m_packetIndex.set_allocator( allocator );
	m_occluderDepth.m_visibleIndices.set_allocator( allocator );
	m_occluderDepth.m_packets.set_allocator( allocator );
	m_uploadState.m_motionCurrentOrder.set_allocator( allocator );
	m_uploadState.m_motionPreviousOrder.set_allocator( allocator );
	m_uploadState.m_motionDirtyRanges.set_allocator( allocator );
	return true;
}

void Terrain3DRdInstanceRenderer::setStylizedSunGlobals(
        const Vector4 &rShape, const Vector4 &rDiffuse, const Vector4 &rSpecular, const Vector4 &rRim ) {
	t3dScopedLock_t lock( m_stylizedSunMutex );
	m_stylizedSunShape = rShape;
	m_stylizedSunDiffuse = rDiffuse;
	m_stylizedSunSpecular = rSpecular;
	m_stylizedSunRim = rRim;
}

void Terrain3DRdInstanceRenderer::copyStylizedSunGlobals(
        float *const pShape, float *const pDiffuse, float *const pSpecular, float *const pRim ) const {
	t3dScopedLock_t lock( m_stylizedSunMutex );
	const Vector4 values[4] = { m_stylizedSunShape, m_stylizedSunDiffuse, m_stylizedSunSpecular, m_stylizedSunRim };
	float *outputs[4] = { pShape, pDiffuse, pSpecular, pRim };
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		outputs[i][0] = ( float )( values[i].x );
		outputs[i][1] = ( float )( values[i].y );
		outputs[i][2] = ( float )( values[i].z );
		outputs[i][3] = ( float )( values[i].w );
	}
}

Terrain3DRdInstanceRenderer::Terrain3DRdInstanceRenderer( void ) :
        m_context( Terrain3DRdContext::getShared() ), m_visibility( m_context ),
        m_hlodCullPipelines( m_context, Terrain3DRdContext::OWNER_HLOD ),
        m_instanceCullPipelines( m_context, Terrain3DRdContext::OWNER_INSTANCES ),
        m_hlodCullDescriptors( m_context, Terrain3DRdContext::OWNER_HLOD ),
        m_instanceCullDescriptors( m_context, Terrain3DRdContext::OWNER_INSTANCES ),
        m_occluderDescriptors( m_context, Terrain3DRdContext::OWNER_INSTANCES ),
        m_instanceSet0Descriptors( m_context, Terrain3DRdContext::OWNER_INSTANCES ),
        m_occluderPipelines( m_context, Terrain3DRdContext::OWNER_INSTANCES ),
        m_viewPipelines( m_context, Terrain3DRdContext::OWNER_INSTANCES ),
        m_targetPool( m_context, Terrain3DRdContext::OWNER_INSTANCES, Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE ),
        m_forwardDescriptors( m_context, Terrain3DRdContext::OWNER_INSTANCES ),
        m_vtDescriptors( m_context, Terrain3DRdContext::OWNER_INSTANCES ),
        m_forwardLighting( m_context ), m_forwardPipelines( m_context, Terrain3DRdContext::OWNER_INSTANCES ) {
}
Terrain3DRdInstanceRenderer::~Terrain3DRdInstanceRenderer( void ) {
	shutdown();
}

bool Terrain3DRdInstanceRenderer::attachVirtualTextureCold( Terrain3DVirtualTexture *const pVirtualTexture ) {
	if ( pVirtualTexture == NULL || ( m_pVirtualTexture != NULL && m_pVirtualTexture != pVirtualTexture ) ) {
		return m_pVirtualTexture == pVirtualTexture;
	}
	m_pVirtualTexture = pVirtualTexture;
	return true;
}

void Terrain3DRdInstanceRenderer::setError( const String &rError ) {
	m_lastError = rError;
}
bool Terrain3DRdInstanceRenderer::ensureDevice( void ) {
	return m_context.initialize() && m_context.validateDevice() && m_context.getDevice();
}

bool Terrain3DRdInstanceRenderer::ensureShader( const Ref<RDShaderSPIRV> &rShader ) {
	if ( m_hShader.isValid() ) {
		return true;
	}
	if ( rShader.is_null() ) {
		setError( "Terrain3D instance shader source is null" );
		return false;
	}
	if ( !m_context.getDevice() ) {
		setError( "Terrain3D instance shader creation lost the RenderingDevice" );
		return false;
	}
	if ( !m_hShader.createTracked( m_context, Terrain3DRdContext::OWNER_INSTANCES, "T3D.InstanceShader", rShader, "T3D.Instance.Shader" ) ) {
		setError( "Failed to create/register Terrain3D instance shader RID" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureOccluderDepthShader( void ) {
	if ( m_occluderDepth.m_hShader.isValid() ) {
		return true;
	}
	if ( m_occluderDepth.m_sourceShader.is_null() ) {
		setError( "Terrain3D occluder-depth shader source is null" );
		return false;
	}
	if ( !m_context.getDevice() ) {
		setError( "Terrain3D occluder-depth shader creation lost the RenderingDevice" );
		return false;
	}
	if ( !m_occluderDepth.m_hShader.createTracked( m_context, Terrain3DRdContext::OWNER_INSTANCES,
	             "T3D.InstanceOccluderDepth", m_occluderDepth.m_sourceShader, "T3D.Instance.OccluderDepth.Shader" ) ) {
		setError( "Failed to create/register Terrain3D occluder-depth shader RID" );
		return false;
	}
	return true;
}

void Terrain3DRdInstanceRenderer::freeOccluderDepthUniformSet( void ) {
	m_occluderDescriptors.erase( m_occluderDepth.m_hUniformSet );
	m_occluderDepth.m_hUniformSet = RID();
}

void Terrain3DRdInstanceRenderer::freeOccluderDepthTarget( void ) {
	m_occluderPipelines.clear();
	m_occluderDepth.m_hPipeline = RID();
	for ( uint32_t iSlot = 0u; iSlot < 2u; ++iSlot ) {
		m_targetPool.release( m_occluderDepth.m_hszDepthTextures[iSlot], m_occluderDepth.m_hszFramebuffers[iSlot] );
		m_occluderDepth.m_szRasterRenderFrames[iSlot] = UINT64_MAX;
		m_occluderDepth.m_szRasterViewportKeys[iSlot] = 0u;
		m_occluderDepth.m_szRasterSceneGenerations[iSlot] = 0u;
		m_occluderDepth.m_szRasterMeshGenerations[iSlot] = 0u;
		m_occluderDepth.m_szDepthValid[iSlot] = false;
	}
	m_occluderDepth.m_hDepthTexture = RID();
	m_occluderDepth.m_hFramebuffer = RID();
	m_occluderDepth.m_targetSize = Vector2i();
	m_occluderDepth.m_nWriteSlot = 0u;
	m_occluderDepth.m_bDepthValid = false;
}

void Terrain3DRdInstanceRenderer::freeOccluderDepthResources( void ) {
	freeOccluderDepthUniformSet();
	freeOccluderDepthTarget();
	m_occluderDepth.m_hVisibleIndexBuffer.releaseTracked( m_context );
	m_occluderDepth.m_hShader.releaseTracked( m_context );
	m_occluderDepth.m_sourceShader.unref();
	m_occluderDepth.m_nPacketCapacity = 0u;
	m_occluderDepth.m_nVisibleIndexCapacity = 0u;
	m_occluderDepth.m_nVisibleIndexCount = 0u;
	m_occluderDepth.m_packets.clear();
	m_occluderDepth.m_visibleIndices.clear();
	m_occluderDepth.m_visibleBytes.clear();
}

bool Terrain3DRdInstanceRenderer::ensureOccluderDepthTarget( const Vector2i &rRenderSize ) {
	const Vector2i target_size( MIN( MAX( rRenderSize.x, 1 ), 2048 ), MIN( MAX( rRenderSize.y, 1 ), 2048 ) );
	if ( m_occluderDepth.m_hszDepthTextures[0].is_valid() && m_occluderDepth.m_hszDepthTextures[1].is_valid()
	        && m_occluderDepth.m_hszFramebuffers[0].is_valid() && m_occluderDepth.m_hszFramebuffers[1].is_valid()
	        && m_occluderDepth.m_hPipeline.is_valid() && m_occluderDepth.m_targetSize == target_size ) {
		return true;
	}
	if ( !ensureOccluderDepthShader() || m_geometryArena.m_meshCache.getVertexFormat() == RenderingDevice::INVALID_FORMAT_ID ) {
		setError( "Terrain3D occluder-depth target has no shader or mesh vertex format" );
		return false;
	}
	freeOccluderDepthTarget();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "Terrain3D occluder-depth target creation lost the RenderingDevice" );
		return false;
	}
	m_context.beginResourceRebuild();
	const Terrain3DRdRenderTargetPool<4u>::Key depth_key {
		( uint32_t )target_size.x, ( uint32_t )target_size.y, RenderingDevice::DATA_FORMAT_D32_SFLOAT,
		( uint32_t )( RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT ),
		4u, 1u, true
	};
	for ( uint32_t iSlot = 0u; iSlot < 2u; ++iSlot ) {
		if ( !m_targetPool.acquire( depth_key,
		                String( "T3D.Instance.OccluderDepth." ) + String::num_uint64( iSlot ),
		                m_occluderDepth.m_hszDepthTextures[iSlot], m_occluderDepth.m_hszFramebuffers[iSlot] ) ) {
			m_context.endResourceRebuild();
			freeOccluderDepthTarget();
			setError( "Failed to acquire Terrain3D private occluder-depth history target" );
			return false;
		}
	}
	m_occluderDepth.m_hDepthTexture = m_occluderDepth.m_hszDepthTextures[0];
	m_occluderDepth.m_hFramebuffer = m_occluderDepth.m_hszFramebuffers[0];
	m_occluderDepth.m_targetSize = target_size;
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_cull_mode( RenderingDevice::POLYGON_CULL_DISABLED );
	raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE );
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( RenderingDevice::TEXTURE_SAMPLES_1 );
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test( true );
	depth->set_enable_depth_write( true );
	depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	m_occluderDepth.m_hPipeline = m_occluderPipelines.getOrCreateRaster( m_occluderDepth.m_hShader.rid(),
	        "T3D.Instance.OccluderDepth.Pipeline", m_occluderDepth.m_hShader.rid(),
	        m_targetPool.framebufferFormat( m_occluderDepth.m_hszFramebuffers[0] ),
	        m_geometryArena.m_meshCache.getVertexFormat(),
	        RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
	        raster,
	        multisample,
	        depth,
	        blend,
	        0u );
	if ( !m_occluderDepth.m_hPipeline.is_valid() ) {
		m_context.endResourceRebuild();
		freeOccluderDepthTarget();
		setError( "Failed to create Terrain3D private occluder-depth pipeline" );
		return false;
	}
	m_context.endResourceRebuild();
	++m_occluderDepth.m_nTargetRebuilds;
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureOccluderDepthUniformSet( void ) {
	if ( m_occluderDepth.m_hUniformSet.is_valid() ) {
		return true;
	}
	if ( !m_occluderDepth.m_hShader.isValid() || !m_uploadState.m_hFrameBuffer.is_valid() || !m_uploadState.m_hInstanceBuffer.is_valid()
	        || !m_occluderDepth.m_hVisibleIndexBuffer.is_valid() ) {
		setError( "Terrain3D occluder-depth set-0 prerequisites are unavailable" );
		return false;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_uploadState.m_hFrameBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_uploadState.m_hInstanceBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_occluderDepth.m_hVisibleIndexBuffer ) );
	m_occluderDepth.m_hUniformSet = m_occluderDescriptors.getOrCreate( m_occluderDepth.m_hShader.rid(),
	        "T3D.Instance.OccluderDepth.UniformSet", uniforms, m_occluderDepth.m_hShader.rid(), 0u );
	if ( !m_occluderDepth.m_hUniformSet.is_valid() ) {
		setError( "Failed to create Terrain3D private occluder-depth uniform set" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureOccluderDepthStorage( const t3dRdInstanceSceneSnapshot_t &rScene, const Vector2i &rRenderSize ) {
	uint64_t packet_capacity64 = 0u;
	uint64_t visible_capacity64 = 0u;
	for ( size_t iCell = 0u; iCell < rScene.m_cells.size(); ++iCell ) {
		const t3dRdInstanceCell_t &cell = rScene.m_cells[iCell];
		if ( ( cell.m_nFlags & T3D_RD_INSTANCE_CATEGORY_MASK ) != T3D_RD_INSTANCE_CATEGORY_OCCLUDER || cell.m_nInstanceCount == 0u ) {
			continue;
		}
		const t3dRdMeshHandle_t mesh = m_geometryArena.m_meshCache.getOcclusionProxyMeshHandle( cell.m_nMeshAssetId );
		const t3dRdMeshSourceLod_t *const pLod = m_geometryArena.m_meshCache.resolveMesh( mesh );
		if ( !pLod || pLod->m_nSourceKey != t3dRdOcclusionProxySourceKey( cell.m_nMeshAssetId )
		        || pLod->m_nAssetId != cell.m_nMeshAssetId ) {
			continue;
		}
		packet_capacity64 += pLod->m_nSurfaceCount;
		visible_capacity64 += ( uint64_t )cell.m_nInstanceCount * ( uint64_t )pLod->m_nSurfaceCount;
	}
	if ( packet_capacity64 > UINT32_MAX || visible_capacity64 > UINT32_MAX || visible_capacity64 > UINT32_MAX / sizeof( uint32_t ) ) {
		setError( "Terrain3D occluder-depth packet capacity exceeds 32-bit limits" );
		return false;
	}
	m_occluderDepth.m_nPacketCapacity = ( uint32_t )packet_capacity64;
	m_occluderDepth.m_nVisibleIndexCapacity = ( uint32_t )visible_capacity64;
	const size_t packet_capacity_before = m_occluderDepth.m_packets.capacity();
	const size_t visible_capacity_before = m_occluderDepth.m_visibleIndices.capacity();
	if ( !t3dContainerReserveCold( m_occluderDepth.m_packets, m_occluderDepth.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_occluderDepth.m_visibleIndices, m_occluderDepth.m_nVisibleIndexCapacity ) ) {
		setError( "Terrain3D occluder-depth CPU packet capacity reservation failed" );
		return false;
	}
	const bool cpu_capacity_grew = m_occluderDepth.m_packets.capacity() > packet_capacity_before
	        || m_occluderDepth.m_visibleIndices.capacity() > visible_capacity_before;
	if ( m_occluderDepth.m_nPacketCapacity == 0u ) {
		m_occluderDepth.m_packets.clear();
		m_occluderDepth.m_visibleIndices.clear();
		m_occluderDepth.m_nVisibleIndexCount = 0u;
		m_occluderDepth.m_bDepthValid = false;
		return true;
	}
	const uint32_t visible_bytes = MAX( m_occluderDepth.m_nVisibleIndexCapacity * ( uint32_t )sizeof( uint32_t ), 4u );
	const bool replace_visible = m_occluderDepth.m_hVisibleIndexBuffer.needsCapacity( visible_bytes );
	if ( cpu_capacity_grew || replace_visible ) {
		++m_occluderDepth.m_nCapacityGrowths;
	}
	if ( replace_visible ) {
		freeOccluderDepthUniformSet();
	}
	if ( !ensureOwnedBuffer( m_occluderDepth.m_hVisibleIndexBuffer,
	             visible_bytes,
	             "T3D.Instance.OccluderDepth.VisibleIndices",
	             Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	             Terrain3DRdContext::MEMORY_INSTANCE ) ) {
		return false;
	}
	if ( m_occluderDepth.m_visibleBytes.size() < ( int64_t )m_occluderDepth.m_hVisibleIndexBuffer.capacityBytes() ) {
		m_occluderDepth.m_visibleBytes.resize( m_occluderDepth.m_hVisibleIndexBuffer.capacityBytes() );
	}
	return ensureOccluderDepthTarget( rRenderSize ) && ensureOccluderDepthUniformSet();
}

bool Terrain3DRdInstanceRenderer::buildOccluderDepthPackets(
        const t3dRdForwardPassContext_t &rContext, const t3dRdInstanceSceneSnapshot_t &rScene, const uint32_t nVisibilityMask ) {
	m_occluderDepth.m_packets.clear();
	m_occluderDepth.m_visibleIndices.clear();
	m_occluderDepth.m_nVisibleIndexCount = 0u;
	m_occluderDepth.m_bDepthValid = false;
	if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_nViewCount != 1u || m_occluderDepth.m_nPacketCapacity == 0u ) {
		return true;
	}
	const t3dRdInstanceFrustum_t frustum = t3dRdExtractFrustum( rContext.m_szViewProjection[0] );
	const Vector3 camera = rContext.m_cameraTransform.origin;
	for ( size_t iCell = 0u; iCell < rScene.m_cells.size(); ++iCell ) {
		const t3dRdInstanceCell_t &cell = rScene.m_cells[iCell];
		if ( ( cell.m_nFlags & T3D_RD_INSTANCE_CATEGORY_MASK ) != T3D_RD_INSTANCE_CATEGORY_OCCLUDER || cell.m_nInstanceCount == 0u
		        || cell.m_nOcclusionProxyGeneration == 0u || cell.m_nOcclusionProxyPolicy == 0u
		        || ( cell.m_nVisibilityLayers & nVisibilityMask ) == 0u || !t3dRdInstanceCellVisible( cell, frustum, camera, false ) ) {
			continue;
		}
		const uint64_t instance_end = ( uint64_t )cell.m_nFirstInstance + ( uint64_t )cell.m_nInstanceCount;
		if ( instance_end > rScene.m_instances.size() ) {
			setError( "Terrain3D occluder-depth cell instance range exceeds publication bounds" );
			return false;
		}
		const t3dRdMeshHandle_t mesh = m_geometryArena.m_meshCache.getOcclusionProxyMeshHandle( cell.m_nMeshAssetId );
		const t3dRdMeshSourceLod_t *const pLod = m_geometryArena.m_meshCache.resolveMesh( mesh );
		if ( !pLod || pLod->m_nSourceKey != t3dRdOcclusionProxySourceKey( cell.m_nMeshAssetId )
		        || pLod->m_nAssetId != cell.m_nMeshAssetId ) {
			continue;
		}
		for ( uint32_t iSurface = 0u; iSurface < pLod->m_nSurfaceCount; ++iSurface ) {
			const t3dRdSurfaceHandle_t surface_handle = m_geometryArena.m_meshCache.getSurfaceHandle( mesh, iSurface );
			const t3dRdMeshSourceSurface_t *const pSurface = m_geometryArena.m_meshCache.resolveSurface( surface_handle );
			if ( !pSurface || pSurface->m_nIndexCount == 0u
			        || pSurface->m_nSourceKey != t3dRdOcclusionProxySourceKey( cell.m_nMeshAssetId ) ) {
				continue;
			}
			if ( m_occluderDepth.m_packets.size() >= m_occluderDepth.m_packets.capacity()
			        || m_occluderDepth.m_visibleIndices.size() + cell.m_nInstanceCount > m_occluderDepth.m_visibleIndices.capacity() ) {
				setError( "Terrain3D occluder-depth hot packet build exceeded cold capacity" );
				return false;
			}
			t3dRdInstanceDrawPacket_t packet;
			packet.m_nSurfaceSlot = surface_handle.m_nSlot;
			packet.m_nFirstVisible = ( uint32_t )m_occluderDepth.m_visibleIndices.size();
			packet.m_nVisibleCount = cell.m_nInstanceCount;
			packet.m_nMaterial = UINT16_MAX;
			packet.m_nOverlayMaterial = UINT16_MAX;
			packet.m_nFlags = 0u;
			packet.m_cullBounds = cell.m_occlusionBounds;
			packet.m_nVisibilityLayers = cell.m_nVisibilityLayers;
			m_occluderDepth.m_packets.push_back( packet );
			for ( uint32_t iInstance = 0u; iInstance < cell.m_nInstanceCount; ++iInstance ) {
				m_occluderDepth.m_visibleIndices.push_back( cell.m_nFirstInstance + iInstance );
			}
		}
	}
	m_occluderDepth.m_nVisibleIndexCount = ( uint32_t )m_occluderDepth.m_visibleIndices.size();
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadOccluderDepthVisibility( void ) {
	const uint32_t bytes = m_occluderDepth.m_nVisibleIndexCount * ( uint32_t )sizeof( uint32_t );
	if ( bytes == 0u ) {
		return true;
	}
	if ( bytes > m_occluderDepth.m_hVisibleIndexBuffer.capacityBytes() || m_occluderDepth.m_visibleBytes.size() < bytes ) {
		setError( "Terrain3D occluder-depth visibility upload exceeded fixed backing" );
		return false;
	}
	memcpy( m_occluderDepth.m_visibleBytes.ptrw(), m_occluderDepth.m_visibleIndices.data(), bytes );
	if ( !m_occluderDepth.m_hVisibleIndexBuffer.writePackedTracked( m_context, 0u, m_occluderDepth.m_visibleBytes, bytes, T3D_GPU_SCRATCH ) ) {
		setError( "Terrain3D occluder-depth visibility upload failed" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::rasterOccluderDepth(
        const t3dRdForwardPassContext_t &rContext, const t3dRdInstanceSceneSnapshot_t &rScene ) {
	if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_nViewCount != 1u || m_occluderDepth.m_nPacketCapacity == 0u ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const uint32_t write_slot = ( uint32_t )( rContext.m_nRenderFrame & 1u );
	m_occluderDepth.m_nWriteSlot = write_slot;
	m_occluderDepth.m_hDepthTexture = m_occluderDepth.m_hszDepthTextures[write_slot];
	m_occluderDepth.m_hFramebuffer = m_occluderDepth.m_hszFramebuffers[write_slot];
	if ( !rd || !m_occluderDepth.m_hFramebuffer.is_valid() || !m_occluderDepth.m_hDepthTexture.is_valid()
	        || !m_occluderDepth.m_hPipeline.is_valid() || !m_occluderDepth.m_hUniformSet.is_valid() ) {
		setError( "Terrain3D occluder-depth raster prerequisites are unavailable" );
		return false;
	}
	PackedColorArray clear_colors;
	const int64_t list = rd->draw_list_begin( m_occluderDepth.m_hFramebuffer, RenderingDevice::DRAW_DEFAULT_ALL, clear_colors, 0.0f );
	if ( list == RenderingDevice::INVALID_ID ) {
		setError( "Terrain3D occluder-depth draw-list creation failed" );
		return false;
	}
	rd->draw_list_bind_render_pipeline( list, m_occluderDepth.m_hPipeline );
	rd->draw_list_bind_uniform_set( list, m_occluderDepth.m_hUniformSet, 0u );
	uint64_t submitted_instances = 0u;
	for ( size_t iPacket = 0u; iPacket < m_occluderDepth.m_packets.size(); ++iPacket ) {
		const t3dRdInstanceDrawPacket_t &packet = m_occluderDepth.m_packets[iPacket];
		const RID vertex_array = m_geometryArena.m_meshCache.getSurfaceVertexArray( packet.m_nSurfaceSlot );
		const RID index_array = m_geometryArena.m_meshCache.getSurfaceIndexArray( packet.m_nSurfaceSlot );
		if ( !vertex_array.is_valid() || !index_array.is_valid() ) {
			rd->draw_list_end();
			setError( "Terrain3D occluder-depth proxy surface view is unavailable" );
			return false;
		}
		rd->draw_list_bind_vertex_array( list, vertex_array );
		rd->draw_list_bind_index_array( list, index_array );
		t3dRdInstancePush_t push;
		push.m_nVisibleBase = packet.m_nFirstVisible;
		push.m_nVisibilityLayers = packet.m_nVisibilityLayers;
		memcpy( m_uploadState.m_pushBytes.ptrw(), &push, sizeof( push ) );
		rd->draw_list_set_push_constant( list, m_uploadState.m_pushBytes, sizeof( push ) );
		rd->draw_list_draw( list, true, packet.m_nVisibleCount, 0u );
		submitted_instances += packet.m_nVisibleCount;
	}
	rd->draw_list_end();
	m_occluderDepth.m_bDepthValid = true;
	m_occluderDepth.m_szDepthValid[write_slot] = true;
	m_occluderDepth.m_szRasterRenderFrames[write_slot] = rContext.m_nRenderFrame;
	m_occluderDepth.m_szRasterViewportKeys[write_slot] = rContext.m_nViewportKey;
	m_occluderDepth.m_szRasterSceneGenerations[write_slot] = rScene.m_nGeneration;
	m_occluderDepth.m_szRasterMeshGenerations[write_slot] = m_geometryArena.m_meshCache.getSourceGeneration();
	m_occluderDepth.m_nLastRasterRenderFrame = rContext.m_nRenderFrame;
	m_occluderDepth.m_nLastRasterViewportKey = rContext.m_nViewportKey;
	++m_occluderDepth.m_nRasterFrames;
	m_occluderDepth.m_nRasterDrawPackets += m_occluderDepth.m_packets.size();
	m_occluderDepth.m_nRasterInstances += submitted_instances;
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureForwardShaderVariant( const t3dRdForwardShaderVariant_t variant ) {
	if ( variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ) {
		setError( "Invalid Terrain3D Forward+ instance shader variant" );
		return false;
	}
	Terrain3DRdShader &shader = m_hszForwardShaders[variant];
	if ( shader.isValid() ) {
		return true;
	}
	const Ref<RDShaderSPIRV> spirv = m_forwardShaderBundle.m_szInstances[variant];
	if ( spirv.is_null() ) {
		setError( String( "Missing Terrain3D Forward+ instance shader variant " ) + String::num_int64( variant ) );
		return false;
	}
	if ( !m_context.getDevice() ) {
		setError(
		        String( "Terrain3D Forward+ instance shader creation lost the RenderingDevice; variant=" ) + String::num_int64( variant ) );
		return false;
	}
	m_context.beginResourceRebuild();
	const String name = String( "T3D.Forward.Shader.Instance." ) + String::num_int64( variant );
	const bool created = shader.createTracked( m_context, Terrain3DRdContext::OWNER_INSTANCES, name, spirv );
	m_context.endResourceRebuild();
	if ( !created ) {
		setError( String( "Failed to create Terrain3D Forward+ instance shader variant " ) + String::num_int64( variant ) );
		return false;
	}
	return true;
}

void Terrain3DRdInstanceRenderer::freeForwardPipelines( void ) {
	m_forwardPipelines.clear();
	m_currentForwardPipelineEntry = t3dRdInstanceForwardPipelineEntry_t{};
	m_bLastForwardPipelineKeyValid = false;
}



const Terrain3DRdInstanceRenderer::t3dRdInstanceForwardPipelineEntry_t *Terrain3DRdInstanceRenderer::findForwardPipeline(
        const t3dRdInstanceForwardPipelineKey_t &rKey ) const {
	for ( uint32_t cull = 0u; cull < 3u; ++cull ) {
		const auto *const entry = m_forwardPipelines.find( { rKey, cull } );
		if ( !entry ) {
			return nullptr;
		}
		m_currentForwardPipelineEntry.m_hszPipelines[cull] = entry->m_hPipeline;
	}
	m_currentForwardPipelineEntry.m_key = rKey;
	return &m_currentForwardPipelineEntry;
}

bool Terrain3DRdInstanceRenderer::ensureForwardPipelines(
        const t3dRdForwardPassContext_t &rContext, const t3dRdForwardShaderVariant_t variant ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 || m_geometryArena.m_meshCache.getVertexFormat() == RenderingDevice::INVALID_FORMAT_ID ) {
		return false;
	}
	t3dRdInstanceForwardPipelineKey_t key;
	if ( !t3dRdCompileForwardPipelineBaseKey( rd, rContext, variant, true, key ) ) {
		setError( "Terrain3D Forward+ sample-count metadata does not match native framebuffer format" );
		return false;
	}
	if ( findForwardPipeline( key ) ) {
		m_lastForwardPipelineKey = key;
		m_bLastForwardPipelineKeyValid = true;
		return true;
	}
	const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
	t3dRdForwardPipelineStateOptions_t state_options;
	state_options.m_frontFace = RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE;
	state_options.m_nBlendAttachmentCount = t3dRdForwardBlendAttachmentCount( variant, transparent_pass );
	state_options.m_bBlendFirstAttachment = transparent_pass;
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	state_options.m_bEnableDepthTest = false;
	state_options.m_bEnableDepthWrite = false;
	state_options.m_depthCompare = RenderingDevice::COMPARE_OP_ALWAYS;
#else
	state_options.m_bEnableDepthWrite = !transparent_pass && !key.m_bDepthPrepassActive;
	state_options.m_depthCompare =
	        key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
#endif
	Ref<RDPipelineMultisampleState> multisample;
	Ref<RDPipelineDepthStencilState> depth;
	Ref<RDPipelineColorBlendState> blend;
	if ( !t3dRdBuildForwardSharedPipelineStates( key, state_options, multisample, depth, blend ) ) {
		setError( "Failed to allocate Terrain3D Forward+ instance pipeline state" );
		return false;
	}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	const RenderingDevice::PolygonCullMode cull_modes[3] = {
		RenderingDevice::POLYGON_CULL_DISABLED,
		RenderingDevice::POLYGON_CULL_DISABLED,
		RenderingDevice::POLYGON_CULL_DISABLED,
	};
#else
	const RenderingDevice::PolygonCullMode cull_modes[3] = {
		key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK,
		key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_BACK : RenderingDevice::POLYGON_CULL_FRONT,
		RenderingDevice::POLYGON_CULL_DISABLED,
	};
#endif
	m_context.beginResourceRebuild();
	for ( uint32_t cull = 0u; cull < 3u; ++cull ) {
		Ref<RDPipelineRasterizationState> raster;
		state_options.m_cullMode = cull_modes[cull];
		if ( !t3dRdBuildForwardRasterState( state_options, raster ) ) {
			m_forwardPipelines.clear();
			m_context.endResourceRebuild();
			setError( "Failed to allocate Terrain3D Forward+ instance raster state" );
			return false;
		}
		const String name = String( "T3D.Forward.Pipeline.Instance." ) + String::num_int64( variant ) + "." + String::num_int64( cull );
		const RID pipeline = m_forwardPipelines.getOrCreateRaster( { key, cull }, name, m_hszForwardShaders[variant].rid(),
		        rContext.m_nFramebufferFormat,
		        m_geometryArena.m_meshCache.getVertexFormat(),
		        RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
		        raster,
		        multisample,
		        depth,
		        blend,
		        0u );
		if ( !pipeline.is_valid() ) {
			m_forwardPipelines.clear();
			m_context.endResourceRebuild();
			setError( String( "Failed to create framebuffer-compatible Terrain3D Forward+ instance pipeline RID; variant=" )
			        + String::num_int64( variant ) + String( ", cull=" ) + String::num_int64( cull ) );
			return false;
		}
	}
	m_context.endResourceRebuild();
	m_lastForwardPipelineKey = key;
	m_bLastForwardPipelineKeyValid = true;
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureHlodCullShader( const Ref<RDShaderSPIRV> &rShader ) {
	if ( m_visibility.m_hHlodCullShader.isValid() && m_visibility.m_hHlodCullPipeline.is_valid() ) {
		return true;
	}
	if ( rShader.is_null() ) {
		setError( "Missing Terrain3D HLOD cull shader" );
		return false;
	}
	if ( !m_context.getDevice() ) {
		setError( "Terrain3D HLOD cull resource creation lost the RenderingDevice" );
		return false;
	}
	if ( !m_visibility.m_hHlodCullShader.isValid() ) {
		if ( !m_visibility.m_hHlodCullShader.createTracked(
		             m_context, Terrain3DRdContext::OWNER_HLOD, "T3D.HlodCullShader", rShader, "T3D.HLOD.CullShader" ) ) {
			setError( "Failed to create/register Terrain3D HLOD cull shader" );
			return false;
		}
	}
	if ( !m_visibility.m_hHlodCullPipeline.is_valid() ) {
		m_visibility.m_hHlodCullPipeline = m_hlodCullPipelines.getOrCreateCompute(
		        m_visibility.m_hHlodCullShader.rid(), "T3D.HLOD.CullPipeline", m_visibility.m_hHlodCullShader.rid() );
		if ( !m_visibility.m_hHlodCullPipeline.is_valid() ) {
			setError( "Failed to create Terrain3D HLOD cull pipeline" );
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureInstanceCullShader( const Ref<RDShaderSPIRV> &rShader ) {
	if ( m_visibility.m_hInstanceCullShader.isValid() && m_visibility.m_hInstanceCullPipeline.is_valid() ) {
		return true;
	}
	if ( rShader.is_null() ) {
		setError( "Missing Terrain3D instance cull shader" );
		return false;
	}
	if ( !m_context.getDevice() ) {
		setError( "Terrain3D instance cull resource creation lost the RenderingDevice" );
		return false;
	}
	if ( !m_visibility.m_hInstanceCullShader.isValid() ) {
		if ( !m_visibility.m_hInstanceCullShader.createTracked(
		             m_context, Terrain3DRdContext::OWNER_INSTANCES, "T3D.InstanceCullShader", rShader, "T3D.Instance.CullShader" ) ) {
			setError( "Failed to create/register Terrain3D instance cull shader" );
			return false;
		}
	}
	if ( !m_visibility.m_hInstanceCullPipeline.is_valid() ) {
		m_visibility.m_hInstanceCullPipeline = m_instanceCullPipelines.getOrCreateCompute(
		        m_visibility.m_hInstanceCullShader.rid(), "T3D.Instance.CullPipeline", m_visibility.m_hInstanceCullShader.rid() );
		if ( !m_visibility.m_hInstanceCullPipeline.is_valid() ) {
			setError( "Failed to create Terrain3D instance cull pipeline" );
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureOwnedBuffer( Terrain3DRdGpuBuffer &rBuffer,
        const uint32_t nRequired,
        const char *const pName,
        const Terrain3DRdContext::ResourceType type,
        const Terrain3DRdContext::MemoryCategory memory,
        const Terrain3DRdContext::ResourceOwner owner ) {
	if ( !rBuffer.isConfigured() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = pName;
		spec.m_owner = owner;
		spec.m_type = type;
		spec.m_memory = memory;
		spec.m_growth = T3D_GPU_POWER_OF_TWO;
		spec.m_nMinimumCapacityBytes = type == Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER ? 256u : 4096u;
		if ( !rBuffer.configure( spec ) ) {
			return false;
		}
	}
	if ( !rBuffer.needsCapacity( nRequired ) ) {
		return true;
	}
	++m_uploadState.m_nBufferCapacityMissCount;
	if ( !rBuffer.ensureTracked( m_context, nRequired ) ) {
		++m_uploadState.m_nBufferRebuildFailureCount;
		setError( String( "RD instance buffer growth failed: " ) + String::utf8( pName ) + String( ", required bytes=" )
		        + String::num_int64( nRequired ) );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureIndirectArena( const uint32_t nRequiredSliceBytes ) {
	if ( m_commandTable.m_hIndirectBuffer.isValid()
	        && m_commandTable.m_mainIndirectSlice.m_nCapacityBytes >= nRequiredSliceBytes
	        && m_commandTable.m_shadowIndirectSlice.m_nCapacityBytes >= nRequiredSliceBytes ) {
		return true;
	}
	++m_uploadState.m_nBufferCapacityMissCount;
	uint32_t grown = 0u;
	const uint64_t stride = sizeof( t3dRdIndexedIndirectCommand_t );
	if ( !t3dRdGpuBufferGrownCapacity( 4096u, nRequiredSliceBytes, T3D_GPU_POWER_OF_TWO, grown ) ) {
		++m_uploadState.m_nBufferRebuildFailureCount;
		setError( "RD instance indirect arena capacity overflow" );
		return false;
	}
	const uint64_t aligned = ( ( uint64_t )grown + stride - 1u ) / stride * stride;
	if ( aligned * 2u > UINT32_MAX ) {
		++m_uploadState.m_nBufferRebuildFailureCount;
		setError( "RD instance indirect arena exceeds 32-bit buffer limits" );
		return false;
	}
	const uint32_t total = ( uint32_t )( aligned * 2u );
	t3dRdGpuBufferSpec_t spec;
	spec.m_pName = "T3D.Instance.IndirectArena";
	spec.m_owner = Terrain3DRdContext::OWNER_INSTANCES;
	spec.m_type = Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER;
	spec.m_memory = Terrain3DRdContext::MEMORY_INDIRECT;
	spec.m_usage = RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT;
	if ( ( !m_commandTable.m_hIndirectBuffer.isConfigured() && !m_commandTable.m_hIndirectBuffer.configure( spec ) )
	        || !m_commandTable.m_hIndirectBuffer.replaceTracked( m_context, total ) ) {
		++m_uploadState.m_nBufferRebuildFailureCount;
		setError( "RD instance indirect arena creation or registration failed" );
		return false;
	}
	// Indexed indirect commands are 20-byte records. The arena split stays command-stride aligned,
	// while the RD byte-address alignment carried by the slice remains the 4-byte uint32_t boundary.
	if ( !m_commandTable.m_hIndirectBuffer.slice( 0u, ( uint32_t )aligned, sizeof( uint32_t ), m_commandTable.m_mainIndirectSlice )
	        || !m_commandTable.m_hIndirectBuffer.slice( ( uint32_t )aligned, ( uint32_t )aligned, sizeof( uint32_t ),
	                m_commandTable.m_shadowIndirectSlice ) ) {
		++m_uploadState.m_nBufferRebuildFailureCount;
		setError( "RD instance indirect arena slice layout failed" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureBuffers(
        const uint32_t nInstanceBytes, const uint32_t nVisibleBytes, const uint32_t nMaterialBytes, const uint32_t nIndirectBytes ) {
	const bool replace_instance = m_uploadState.m_hInstanceBuffer.needsCapacity( nInstanceBytes );
	const bool replace_visible = m_visibility.m_hVisibleIndexBuffer.needsCapacity( nVisibleBytes );
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	const bool replace_previous_instance = false;
	const bool replace_material = false;
	const bool replace_indirect = false;
	( void )nMaterialBytes;
	( void )nIndirectBytes;
#else
	const uint64_t draw_metadata_bytes64 =
	        MAX( ( uint64_t )( m_geometryArena.m_meshCache.getSurfaceCount() ), 1ull ) * sizeof( t3dInstanceDrawMeta_t );
	if ( draw_metadata_bytes64 > UINT32_MAX ) {
		setError( "RD fixed draw metadata capacity exceeds 32-bit buffer limits" );
		return false;
	}
	const uint32_t draw_metadata_bytes = ( uint32_t )( draw_metadata_bytes64 );
	const bool replace_previous_instance = m_uploadState.m_hPreviousInstanceBuffer.needsCapacity( nInstanceBytes );
	const bool replace_visible_draw_slot = m_visibility.m_hVisibleDrawSlotBuffer.needsCapacity( nVisibleBytes );
	const bool replace_visible_layer = m_visibility.m_hVisibleLayerBuffer.needsCapacity( nVisibleBytes );
	const bool replace_draw_metadata = m_commandTable.m_hDrawMetadataBuffer.needsCapacity( draw_metadata_bytes );
	const bool replace_material = m_uploadState.m_hMaterialBuffer.needsCapacity( nMaterialBytes );
	const bool replace_indirect = !m_commandTable.m_hIndirectBuffer.isValid()
	        || m_commandTable.m_mainIndirectSlice.m_nCapacityBytes < nIndirectBytes;
#endif
	if ( ( replace_instance || replace_previous_instance || replace_visible ) && m_descriptorState.m_hUniformSet.is_valid() ) {
		m_instanceSet0Descriptors.erase( m_descriptorState.m_hUniformSet );
		m_descriptorState.m_hUniformSet = RID();
	}
	if ( replace_instance ) {
		freeOccluderDepthUniformSet();
	}
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( replace_draw_metadata ) {
		m_commandTable.m_nDrawMetadataMeshGeneration = 0u;
	}
	if ( ( replace_visible_draw_slot || replace_visible_layer || replace_draw_metadata ) && m_descriptorState.m_hUniformSet.is_valid() ) {
		m_instanceSet0Descriptors.erase( m_descriptorState.m_hUniformSet );
		m_descriptorState.m_hUniformSet = RID();
	}
#endif
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( replace_instance || replace_previous_instance || replace_visible ) {
		freeForwardUniformSets();
	}
#else
	if ( replace_instance || replace_previous_instance || replace_visible || replace_visible_draw_slot || replace_visible_layer
	        || replace_draw_metadata ) {
		freeForwardUniformSets();
	}
#endif
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( replace_material ) {
		freeMaterialVtUniformSets( m_descriptorState.m_bMaterialVtSetBuiltOnce );
	}
	if ( replace_indirect && m_visibility.m_hHlodCullUniformSet.is_valid() ) {
		m_hlodCullDescriptors.erase( m_visibility.m_hHlodCullUniformSet );
		m_visibility.m_hHlodCullUniformSet = RID();
	}
	if ( ( replace_visible || replace_visible_draw_slot || replace_visible_layer || replace_indirect || replace_instance )
	        && m_visibility.m_hInstanceCullUniformSet.is_valid() ) {
		m_instanceCullDescriptors.erase( m_visibility.m_hInstanceCullUniformSet );
		m_visibility.m_hInstanceCullUniformSet = RID();
	}
#endif
	if ( !m_uploadState.m_hFrameBuffer.isValid() ) {
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Instance.FrameUBO";
		spec.m_owner = Terrain3DRdContext::OWNER_INSTANCES;
		spec.m_type = Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER;
		spec.m_memory = Terrain3DRdContext::MEMORY_UNIFORM;
		if ( !m_uploadState.m_hFrameBuffer.configure( spec ) || !m_uploadState.m_hFrameBuffer.createTracked( m_context, sizeof( t3dRdInstanceFrame_t ) ) ) {
			setError( "RD instance frame buffer creation or registration failed" );
			return false;
		}
	}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	return ensureOwnedBuffer( m_uploadState.m_hInstanceBuffer,
	               MAX( nInstanceBytes, ( uint32_t )( sizeof( t3dRdInstanceRecord_t ) ) ),
	               "T3D.Instance.Records", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureOwnedBuffer( m_visibility.m_hVisibleIndexBuffer,
	                MAX( nVisibleBytes, 4u ), "T3D.Instance.VisibleIndices",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE );
#else
	const bool base_buffers_ok = ensureOwnedBuffer( m_uploadState.m_hInstanceBuffer,
	                                     MAX( nInstanceBytes, ( uint32_t )( sizeof( t3dRdInstanceRecord_t ) ) ),
	                                     "T3D.Instance.Records", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                                     Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureOwnedBuffer( m_uploadState.m_hPreviousInstanceBuffer,
	                MAX( nInstanceBytes, ( uint32_t )( sizeof( t3dRdInstanceRecord_t ) ) ),
	                "T3D.Instance.PreviousRecords", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                Terrain3DRdContext::MEMORY_HISTORY )
	        && ensureOwnedBuffer( m_visibility.m_hVisibleIndexBuffer,
	                MAX( nVisibleBytes, 4u ), "T3D.Instance.VisibleIndices",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureOwnedBuffer( m_visibility.m_hVisibleDrawSlotBuffer,
	                MAX( nVisibleBytes, 4u ), "T3D.Instance.VisibleDrawSlots",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureOwnedBuffer( m_visibility.m_hVisibleLayerBuffer,
	                MAX( nVisibleBytes, 4u ), "T3D.Instance.VisibleLayers",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureOwnedBuffer( m_commandTable.m_hDrawMetadataBuffer,
	                draw_metadata_bytes, "T3D.Instance.DrawMetadata",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureOwnedBuffer( m_uploadState.m_hMaterialBuffer,
	                MAX( nMaterialBytes, ( uint32_t )( sizeof( t3dRenderingMaterialGpu_t ) ) ),
	                "T3D.Instance.VtMaterialTable", Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                Terrain3DRdContext::MEMORY_MATERIAL_TABLE );
	return base_buffers_ok && ensureIndirectArena( MAX( nIndirectBytes, ( uint32_t )( sizeof( t3dRdIndexedIndirectCommand_t ) ) ) );
#endif
}

bool Terrain3DRdInstanceRenderer::ensureShadowBuffers( const uint32_t nVisibleBytes, const uint32_t nIndirectBytes ) {
	const bool replace_visible = m_visibility.m_hShadowVisibleIndexBuffer.needsCapacity( nVisibleBytes );
	if ( replace_visible ) {
		freeForwardUniformSets();
	}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	( void )nIndirectBytes;
	return ensureOwnedBuffer( m_visibility.m_hShadowVisibleIndexBuffer,
	        MAX( nVisibleBytes, 4u ), "T3D.Instance.ShadowVisibleIndices",
	        Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE );
#else
	const bool replace_visible_draw_slot = m_visibility.m_hShadowVisibleDrawSlotBuffer.needsCapacity( nVisibleBytes );
	const bool replace_visible_layer = m_visibility.m_hShadowVisibleLayerBuffer.needsCapacity( nVisibleBytes );
	const bool replace_indirect = !m_commandTable.m_hIndirectBuffer.isValid()
	        || m_commandTable.m_shadowIndirectSlice.m_nCapacityBytes < nIndirectBytes;
	if ( replace_visible_draw_slot || replace_visible_layer ) {
		freeForwardUniformSets();
	}
	if ( ( replace_visible || replace_visible_draw_slot || replace_visible_layer || replace_indirect )
	        && m_visibility.m_hInstanceCullUniformSet.is_valid() ) {
		m_instanceCullDescriptors.erase( m_visibility.m_hInstanceCullUniformSet );
		m_visibility.m_hInstanceCullUniformSet = RID();
	}
	return ensureOwnedBuffer( m_visibility.m_hShadowVisibleIndexBuffer,
	               MAX( nVisibleBytes, 4u ), "T3D.Instance.ShadowVisibleIndices",
	               Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureOwnedBuffer( m_visibility.m_hShadowVisibleDrawSlotBuffer,
	                MAX( nVisibleBytes, 4u ), "T3D.Instance.ShadowVisibleDrawSlots",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureOwnedBuffer( m_visibility.m_hShadowVisibleLayerBuffer,
	                MAX( nVisibleBytes, 4u ), "T3D.Instance.ShadowVisibleLayers",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_INSTANCE )
	        && m_commandTable.m_hIndirectBuffer.isValid()
	        && m_commandTable.m_shadowIndirectSlice.m_nCapacityBytes >= nIndirectBytes;
#endif
}

bool Terrain3DRdInstanceRenderer::ensureUniformSet( void ) {
	if ( m_descriptorState.m_hUniformSet.is_valid() ) {
		return true;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_uploadState.m_hFrameBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_uploadState.m_hInstanceBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hVisibleIndexBuffer ) );
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_commandTable.m_hDrawMetadataBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_uploadState.m_hPreviousInstanceBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 5, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hVisibleDrawSlotBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 6, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hVisibleLayerBuffer ) );
#endif
	m_descriptorState.m_hUniformSet = m_instanceSet0Descriptors.getOrCreate(
	        m_hShader.rid(), "T3D.Instance.UniformSet", uniforms, m_hShader.rid(), 0u );
	if ( !m_descriptorState.m_hUniformSet.is_valid() ) {
		setError( "RD instance set-0 uniform creation failed" );
		return false;
	}
	return true;
}

void Terrain3DRdInstanceRenderer::freeForwardUniformSets( void ) {
	m_forwardDescriptors.clear();
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_descriptorState.m_hszForwardUniformSets[variant] = RID();
		m_descriptorState.m_hszForwardShadowUniformSets[variant] = RID();
	}
}

bool Terrain3DRdInstanceRenderer::ensureForwardUniformSet( const t3dRdForwardShaderVariant_t variant, const bool bShadow ) {
	if ( variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX || !m_hszForwardShaders[variant].isValid() ) {
		setError( String( "RD Forward+ instance set-0 requested an invalid shader variant; variant=" ) + String::num_int64( variant ) );
		return false;
	}
	RID &uniform_set =
	        bShadow ? m_descriptorState.m_hszForwardShadowUniformSets[variant] : m_descriptorState.m_hszForwardUniformSets[variant];
	if ( uniform_set.is_valid() ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( String( "RD Forward+ instance set-0 lost the RenderingDevice; variant=" ) + String::num_int64( variant )
		        + String( ", shadow=" ) + String::num_int64( bShadow ? 1 : 0 ) );
		return false;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_uploadState.m_hFrameBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_uploadState.m_hInstanceBuffer ) );
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	( void )bShadow;
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hVisibleIndexBuffer ) );
#else
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 2,
	        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
	        bShadow ? m_visibility.m_hShadowVisibleIndexBuffer : m_visibility.m_hVisibleIndexBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_commandTable.m_hDrawMetadataBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_uploadState.m_hPreviousInstanceBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 5,
	        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
	        bShadow ? m_visibility.m_hShadowVisibleDrawSlotBuffer : m_visibility.m_hVisibleDrawSlotBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 6,
	        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
	        bShadow ? m_visibility.m_hShadowVisibleLayerBuffer : m_visibility.m_hVisibleLayerBuffer ) );
#endif
	const String name = String( bShadow ? "T3D.Forward.Instance.ShadowUniformSet." : "T3D.Forward.Instance.UniformSet." )
	        + String::num_int64( variant );
	uniform_set = m_forwardDescriptors.getOrCreate( { m_hszForwardShaders[variant].rid(), bShadow }, name,
	        uniforms, m_hszForwardShaders[variant].rid(), 0u );
	if ( !uniform_set.is_valid() ) {
		setError( String( "RD Forward+ instance set-0 creation failed; variant=" ) + String::num_int64( variant ) + String( ", shadow=" )
		        + String::num_int64( bShadow ? 1 : 0 ) );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureVtSampler( void ) {
	if ( m_descriptorState.m_hVtSampler.isValid() ) return true;
	if ( !m_context.getDevice() ) {
		setError( "RD instance VT sampler creation lost the RenderingDevice" );
		return false;
	}
	Ref<RDSamplerState> state;
	state.instantiate();
	if ( state.is_null() ) {
		setError( "RD instance VT sampler state allocation failed" );
		return false;
	}
	state->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
	state->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
	state->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
	state->set_min_lod( 0.0f );
	state->set_max_lod( 0.0f );
	state->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	state->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	if ( !m_descriptorState.m_hVtSampler.createTracked( m_context, Terrain3DRdContext::OWNER_INSTANCES,
	             "T3D.Instance.VTSampler", state ) ) {
		setError( "RD instance VT sampler creation failed" );
		return false;
	}
	return true;
}

void Terrain3DRdInstanceRenderer::freeMaterialVtUniformSets( const bool bRuntimeRebuild ) {
	bool had_set = m_descriptorState.m_hMaterialVtUniformSet.is_valid();
	m_vtDescriptors.clear();
	m_descriptorState.m_hMaterialVtUniformSet = RID();
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		had_set = had_set || m_descriptorState.m_hszForwardMaterialVtUniformSets[variant].is_valid();
		m_descriptorState.m_hszForwardMaterialVtUniformSets[variant] = RID();
	}
	m_descriptorState.m_nMaterialVtResourceSignature = 0u;
	if ( bRuntimeRebuild && had_set ) {
		++m_descriptorState.m_nVtDescriptorRebuildsRuntime;
	}
}

bool Terrain3DRdInstanceRenderer::buildMaterialVtUniformSet( const RID &rShader, RID &rUniformSet, const String &rName ) {
	if ( !m_pVirtualTexture || !m_pVirtualTexture->is_initialized() || !m_uploadState.m_hMaterialBuffer.is_valid()
	        || !rShader.is_valid() ) {
		setError( String( "RD instance invariant VT set prerequisites are unavailable: " ) + rName + String( "; attached=" )
		        + String::num_int64( m_pVirtualTexture != NULL ? 1 : 0 ) + String( ", initialized=" )
		        + String::num_int64( m_pVirtualTexture != NULL && m_pVirtualTexture->is_initialized() ? 1 : 0 )
		        + String( ", material_buffer=" ) + String::num_int64( m_uploadState.m_hMaterialBuffer.is_valid() ? 1 : 0 )
		        + String( ", shader=" ) + String::num_int64( rShader.is_valid() ? 1 : 0 ) );
		return false;
	}
	const Terrain3DVtResourceImage *const pResources = m_pVirtualTexture->get_resource_image();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !pResources || !rd || !ensureVtSampler() ) {
		setError( String( "RD instance invariant VT set lost shared VT resources: " ) + rName );
		return false;
	}
	const RID &allocation_table = pResources->get_buffer_rid( T3D_VT_GPU_BUFFER_ALLOCATION_TABLE );
	const RID &page_table = pResources->get_buffer_rid( T3D_VT_GPU_BUFFER_PAGE_TABLE );
	const RID &mip_layout = pResources->get_buffer_rid( T3D_VT_GPU_BUFFER_MIP_LAYOUT_TABLE );
	const RID &tail_metadata = pResources->get_buffer_rid( T3D_VT_GPU_BUFFER_TAIL_METADATA );
	const RID &albedo_tail = pResources->get_vt_tail_rid( T3D_VT_RESOURCE_ALBEDO );
	const RID &normal_tail = pResources->get_vt_tail_rid( T3D_VT_RESOURCE_NORMAL );
	const RID &surface_tail = pResources->get_vt_tail_rid( T3D_VT_RESOURCE_SURFACE );
	if ( !allocation_table.is_valid() || !page_table.is_valid() || !mip_layout.is_valid() || !tail_metadata.is_valid()
	        || !albedo_tail.is_valid() || !normal_tail.is_valid() || !surface_tail.is_valid() ) {
		setError( String( "RD instance invariant VT set has invalid allocation/page/tail resources: " ) + rName );
		return false;
	}
	uint64_t resource_signature = 1469598103934665603ull;
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( m_uploadState.m_hMaterialBuffer.rid().get_id() ) );
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( m_descriptorState.m_hVtSampler.rid().get_id() ) );
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( allocation_table.get_id() ) );
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( page_table.get_id() ) );
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( mip_layout.get_id() ) );
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( tail_metadata.get_id() ) );
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( albedo_tail.get_id() ) );
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( normal_tail.get_id() ) );
	resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( surface_tail.get_id() ) );
	const T3DVtChannel signature_channels[3] = { T3D_VT_RESOURCE_ALBEDO, T3D_VT_RESOURCE_NORMAL, T3D_VT_RESOURCE_SURFACE };
	for ( uint32_t channel = 0u; channel < 3u; ++channel ) {
		const uint32_t bank_count = pResources->get_vt_bank_count( signature_channels[channel] );
		resource_signature = t3dRdInstanceHashU32( resource_signature, bank_count );
		if ( bank_count != 1u ) {
			setError(
			        String( "RD instance VT specialization requires exactly one physical bank; channel=" ) + String::num_int64( channel ) );
			return false;
		}
		const RID &fallback = pResources->get_vt_bank_rid( signature_channels[channel], 0u );
		if ( !fallback.is_valid() ) {
			setError( String( "RD instance invariant VT fallback bank is invalid; channel=" ) + String::num_int64( channel ) );
			return false;
		}
		resource_signature = t3dRdInstanceHashU64( resource_signature, ( uint64_t )( fallback.get_id() ) );
	}
	if ( m_descriptorState.m_nMaterialVtResourceSignature != 0u
	        && m_descriptorState.m_nMaterialVtResourceSignature != resource_signature ) {
		freeMaterialVtUniformSets( m_descriptorState.m_bMaterialVtSetBuiltOnce );
	}
	if ( rUniformSet.is_valid() ) {
		return true;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_uploadState.m_hMaterialBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, allocation_table ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, page_table ) );
	const T3DVtChannel channels[3] = { T3D_VT_RESOURCE_ALBEDO, T3D_VT_RESOURCE_NORMAL, T3D_VT_RESOURCE_SURFACE };
	const uint32_t binding_base[3] = { 3u, 4u, 5u };
	for ( uint32_t channel = 0u; channel < 3u; ++channel ) {
		const uint32_t bank_count = pResources->get_vt_bank_count( channels[channel] );
		if ( bank_count != 1u ) {
			setError(
			        String( "RD instance VT specialization requires exactly one physical bank; channel=" ) + String::num_int64( channel ) );
			return false;
		}
		const RID &fallback = pResources->get_vt_bank_rid( channels[channel], 0u );
		if ( !fallback.is_valid() ) {
			setError( String( "RD instance invariant VT fallback bank is invalid; channel=" ) + String::num_int64( channel ) );
			return false;
		}
		uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( ( int )binding_base[channel], m_descriptorState.m_hVtSampler.rid(), fallback ) );
	}
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 6, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, tail_metadata ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 7, m_descriptorState.m_hVtSampler.rid(), albedo_tail ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 8, m_descriptorState.m_hVtSampler.rid(), normal_tail ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 9, m_descriptorState.m_hVtSampler.rid(), surface_tail ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, mip_layout ) );
	rUniformSet = m_vtDescriptors.getOrCreate( { rShader, resource_signature }, rName, uniforms, rShader, 1u );
	if ( !rUniformSet.is_valid() ) {
		setError( String( "RD instance invariant VT set creation failed: " ) + rName );
		return false;
	}
	m_descriptorState.m_nMaterialVtResourceSignature = resource_signature;
	m_descriptorState.m_bMaterialVtSetBuiltOnce = true;
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureMaterialVtUniformSet( void ) {
	return buildMaterialVtUniformSet( m_hShader.rid(), m_descriptorState.m_hMaterialVtUniformSet, "T3D.Instance.MaterialVTSet" );
}

bool Terrain3DRdInstanceRenderer::ensureForwardMaterialVtUniformSet( const t3dRdForwardShaderVariant_t variant ) {
	if ( variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX || !m_hszForwardShaders[variant].isValid() ) {
		setError( String( "RD Forward+ invariant VT set requested an invalid shader variant; variant=" ) + String::num_int64( variant ) );
		return false;
	}
	return buildMaterialVtUniformSet( m_hszForwardShaders[variant].rid(),
	        m_descriptorState.m_hszForwardMaterialVtUniformSets[variant],
	        String( "T3D.Forward.Instance.MaterialVTSet." ) + String::num_int64( variant ) );
}

bool Terrain3DRdInstanceRenderer::prepareSceneUploadPlan( const t3dRdInstanceSceneSnapshot_t &scene,
        uint32_t &instance_bytes,
        uint32_t &visible_bytes,
        uint32_t &material_bytes,
        uint32_t &indirect_bytes,
        bool &full_instance_upload ) {
	const t3dRdInstanceSceneSnapshot_t *const pScene = &scene;
	if ( pScene->m_instances.size() > UINT32_MAX / sizeof( t3dRdInstanceRecord_t ) ) {
		setError( "RD instance publication exceeds 32-bit buffer limits" );
		return false;
	}

	uint64_t packet_capacity64 = 0u;
	uint64_t visible_index_capacity64 = 0u;
	for ( size_t iCell = 0u; iCell < ( size_t )( ( pScene->m_cells ).size() ); iCell++ ) {
		const t3dRdInstanceCell_t &cell = ( pScene->m_cells )[iCell];
		const t3dRdMeshHandle_t mesh = m_geometryArena.m_meshCache.getMeshHandleBySourceKey(
		        cell.m_nMeshSourceKey != 0u ? cell.m_nMeshSourceKey : t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = m_geometryArena.m_meshCache.resolveMesh( mesh );
		if ( pLod && !t3dRdInstanceMeshIdentityMatchesCell( cell, pLod ) ) {
			setError( "RD instance mesh identity mismatch during upload planning; refusing stale mesh-bank alias" );
			return false;
		}
		if ( pLod ) {
			packet_capacity64 += pLod->m_nSurfaceCount;
			visible_index_capacity64 += ( uint64_t )( cell.m_nInstanceCount ) * ( uint64_t )( pLod->m_nSurfaceCount );
		}
		if ( packet_capacity64 > UINT32_MAX / sizeof( t3dRdIndexedIndirectCommand_t ) ) {
			setError( "RD instance indirect packet capacity exceeds 32-bit buffer limits" );
			return false;
		}
		if ( visible_index_capacity64 > UINT32_MAX / sizeof( uint32_t ) ) {
			setError( "RD instance visible-index capacity exceeds 32-bit buffer limits" );
			return false;
		}
	}

	const uint64_t fixed_command_slots64 = ( uint64_t )( m_geometryArena.m_meshCache.getSurfaceCount() );
	if ( packet_capacity64 + fixed_command_slots64 > UINT32_MAX / sizeof( t3dRdIndexedIndirectCommand_t ) ) {
		setError( "RD P32 fixed command table plus fallback capacity exceeds 32-bit buffer limits" );
		return false;
	}
	m_commandTable.m_nPacketCapacity = ( uint32_t )( packet_capacity64 + fixed_command_slots64 );
	m_visibility.m_nVisibleIndexCapacity = ( uint32_t )( visible_index_capacity64 );
	instance_bytes = ( uint32_t )( pScene->m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
	visible_bytes = m_visibility.m_nVisibleIndexCapacity * sizeof( uint32_t );
	material_bytes = ( uint32_t )( m_geometryArena.m_meshCache.getVtMaterialCount() * sizeof( t3dRenderingMaterialGpu_t ) );
	indirect_bytes = m_commandTable.m_nPacketCapacity * sizeof( t3dRdIndexedIndirectCommand_t );
	const uint32_t required_instance_capacity = MAX( instance_bytes, ( uint32_t )( sizeof( t3dRdInstanceRecord_t ) ) );
	full_instance_upload = pScene->m_bInstanceLayoutChanged || !m_uploadState.m_hInstanceBuffer.is_valid()
	        || m_uploadState.m_hInstanceBuffer.capacityBytes() < required_instance_capacity || m_uploadState.m_hInstanceBuffer.usedBytes() != instance_bytes;

	return true;
}

bool Terrain3DRdInstanceRenderer::hasRequiredSceneVisibilityCpuStorage( const t3dRdInstanceSceneSnapshot_t &scene ) const {
	return m_visibility.m_visibleIndices.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_visibility.m_visibleDrawSlots.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_visibility.m_shadowVisibleDrawSlots.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_visibility.m_visibleLayers.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_visibility.m_shadowVisibleLayers.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_commandTable.m_fixedDrawVisibleBases.size() >= m_geometryArena.m_meshCache.getSurfaceCount()
	        && m_commandTable.m_fixedDrawVisibleCapacities.size() >= m_geometryArena.m_meshCache.getSurfaceCount()
	        && m_commandTable.m_shadowFixedDrawVisibleBases.size() >= m_geometryArena.m_meshCache.getSurfaceCount()
	        && m_commandTable.m_shadowFixedDrawVisibleCapacities.size() >= m_geometryArena.m_meshCache.getSurfaceCount()
	        && m_visibility.m_shadowVisibleIndices.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_visibility.m_instanceCullCellSpans.size() >= scene.m_cells.size()
	        && m_visibility.m_shadowInstanceCullCellSpans.size() >= scene.m_cells.size();
}

bool Terrain3DRdInstanceRenderer::hasRequiredSceneMotionCpuStorage( const t3dRdInstanceSceneSnapshot_t &scene ) const {
	if ( m_uploadState.m_motionHistoryRecords.size() < scene.m_instances.size()
	        || m_uploadState.m_motionPreviousRecords.size() < scene.m_instances.size()
	        || m_uploadState.m_motionHistoryCells.size() < scene.m_cells.size()
	        || m_uploadState.m_motionCurrentOrder.size() < scene.m_cells.size()
	        || m_uploadState.m_motionPreviousOrder.size() < scene.m_cells.size()
	        || m_uploadState.m_motionDirtyRanges.capacity() < scene.m_dirtyRanges.capacity() ) {
		return false;
	}
	if ( !m_visibility.m_bCullingComparisonEnabled ) {
		return true;
	}
	return m_visibility.m_cullParityExpectedIndices.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_visibility.m_cullParityActualIndices.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_visibility.m_cullParityCommandCounts.size() >= m_commandTable.m_nPacketCapacity
	        && m_visibility.m_cullParityCommandWriteOffsets.size() >= m_commandTable.m_nPacketCapacity;
}

bool Terrain3DRdInstanceRenderer::hasRequiredScenePacketCpuStorage( const t3dRdInstanceSceneSnapshot_t &scene ) const {
	return m_commandTable.m_alphaSortScratch.size() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_drawPackets.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_depthPackets.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_shadowPackets.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_visibility.m_hlodDescriptors.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_visibility.m_instanceCullDescriptors.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_visibility.m_shadowInstanceCullDescriptors.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_visibility.m_instanceHiZRegionDescriptors.capacity() >= s_nRegionSlots
	        && m_visibility.m_instanceHiZRegionKeys.capacity() >= s_nRegionSlots
	        && m_commandTable.m_packetBuildSlots.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_packetBuildEntries.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_alphaPackets.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_transparentPacketOrder.size() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_gpuPacketBoundValid.size() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_packetIndex.capacity() >= m_commandTable.m_nPacketCapacity;
}

bool Terrain3DRdInstanceRenderer::hasRequiredSceneCpuStorage( const t3dRdInstanceSceneSnapshot_t &scene ) const {
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	return m_visibility.m_visibleIndices.size() >= m_visibility.m_nVisibleIndexCapacity
	        && m_commandTable.m_alphaSortScratch.size() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_drawPackets.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_depthPackets.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_packetBuildSlots.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_packetBuildEntries.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_alphaPackets.capacity() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_transparentPacketOrder.size() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_gpuPacketBoundValid.size() >= m_commandTable.m_nPacketCapacity
	        && m_commandTable.m_packetIndex.capacity() >= m_commandTable.m_nPacketCapacity;
#else
	return hasRequiredSceneVisibilityCpuStorage( scene ) && hasRequiredSceneMotionCpuStorage( scene )
	        && hasRequiredScenePacketCpuStorage( scene );
#endif
}

bool Terrain3DRdInstanceRenderer::ensureSceneCpuStorage( const t3dRdInstanceSceneSnapshot_t &scene ) {
	const t3dRdInstanceSceneSnapshot_t *const pScene = &scene;
	// The bring-up lane owns only the CPU structures consumed by reference packet
	// construction and direct indexed submission. Shadow, motion, HLOD and GPU-cull
	// backing must not be prerequisites for proving basic instance rasterization.
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	const bool cpu_capacity_rebuild = !hasRequiredSceneCpuStorage( scene );
	if ( cpu_capacity_rebuild ) {
		++m_uploadState.m_nCpuCapacityRebuildCount;
	}
	bool fixed_storage_ok = true;
	if ( m_visibility.m_visibleIndices.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_visibleIndices, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_commandTable.m_alphaSortScratch.size() < m_commandTable.m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_commandTable.m_alphaSortScratch, m_commandTable.m_nPacketCapacity );
	}
	if ( fixed_storage_ok && m_commandTable.m_transparentPacketOrder.size() < m_commandTable.m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_commandTable.m_transparentPacketOrder, m_commandTable.m_nPacketCapacity );
	}
	if ( fixed_storage_ok && m_commandTable.m_gpuPacketBoundValid.size() < m_commandTable.m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_commandTable.m_gpuPacketBoundValid, m_commandTable.m_nPacketCapacity );
	}
	if ( !fixed_storage_ok || !t3dContainerReserveCold( m_commandTable.m_drawPackets, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_depthPackets, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_packetBuildSlots, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_packetBuildEntries, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_alphaPackets, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_packetIndex, m_commandTable.m_nPacketCapacity ) ) {
		++m_uploadState.m_nCpuCapacityRebuildFailureCount;
		setError( "RD direct-surface owner failed to establish minimal packet backing" );
		return false;
	}
#else
	// Scene publication is an explicit renderer-owner capacity transition. It is
	// entered only from a Terrain3DColdPhaseScope on the render thread; generic
	// allocator phase state does not authorize this growth.
	const bool cpu_capacity_rebuild = !hasRequiredSceneCpuStorage( scene );
	if ( cpu_capacity_rebuild ) {
		++m_uploadState.m_nCpuCapacityRebuildCount;
	}
	bool fixed_storage_ok = true;
	if ( m_visibility.m_visibleIndices.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_visibleIndices, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_visibility.m_visibleDrawSlots.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_visibleDrawSlots, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_visibility.m_shadowVisibleDrawSlots.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_shadowVisibleDrawSlots, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_visibility.m_visibleLayers.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_visibleLayers, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_visibility.m_shadowVisibleLayers.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_shadowVisibleLayers, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_commandTable.m_fixedDrawVisibleBases.size() < m_geometryArena.m_meshCache.getSurfaceCount() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_commandTable.m_fixedDrawVisibleBases, m_geometryArena.m_meshCache.getSurfaceCount() );
	}
	if ( fixed_storage_ok && m_commandTable.m_fixedDrawVisibleCapacities.size() < m_geometryArena.m_meshCache.getSurfaceCount() ) {
		fixed_storage_ok =
		        t3dVectorResizeCold( m_commandTable.m_fixedDrawVisibleCapacities, m_geometryArena.m_meshCache.getSurfaceCount() );
	}
	if ( fixed_storage_ok && m_commandTable.m_shadowFixedDrawVisibleBases.size() < m_geometryArena.m_meshCache.getSurfaceCount() ) {
		fixed_storage_ok =
		        t3dVectorResizeCold( m_commandTable.m_shadowFixedDrawVisibleBases, m_geometryArena.m_meshCache.getSurfaceCount() );
	}
	if ( fixed_storage_ok && m_commandTable.m_shadowFixedDrawVisibleCapacities.size() < m_geometryArena.m_meshCache.getSurfaceCount() ) {
		fixed_storage_ok =
		        t3dVectorResizeCold( m_commandTable.m_shadowFixedDrawVisibleCapacities, m_geometryArena.m_meshCache.getSurfaceCount() );
	}
	if ( fixed_storage_ok && m_visibility.m_shadowVisibleIndices.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_shadowVisibleIndices, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_visibility.m_bCullingComparisonEnabled
	        && m_visibility.m_cullParityExpectedIndices.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_cullParityExpectedIndices, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_visibility.m_bCullingComparisonEnabled
	        && m_visibility.m_cullParityActualIndices.size() < m_visibility.m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_cullParityActualIndices, m_visibility.m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_visibility.m_bCullingComparisonEnabled
	        && m_visibility.m_cullParityCommandCounts.size() < m_commandTable.m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_cullParityCommandCounts, m_commandTable.m_nPacketCapacity );
	}
	if ( fixed_storage_ok && m_visibility.m_bCullingComparisonEnabled
	        && m_visibility.m_cullParityCommandWriteOffsets.size() < m_commandTable.m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_cullParityCommandWriteOffsets, m_commandTable.m_nPacketCapacity );
	}
	if ( fixed_storage_ok && m_commandTable.m_alphaSortScratch.size() < m_commandTable.m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_commandTable.m_alphaSortScratch, m_commandTable.m_nPacketCapacity );
	}
	if ( fixed_storage_ok && m_commandTable.m_transparentPacketOrder.size() < m_commandTable.m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_commandTable.m_transparentPacketOrder, m_commandTable.m_nPacketCapacity );
	}
	if ( fixed_storage_ok && m_commandTable.m_gpuPacketBoundValid.size() < m_commandTable.m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_commandTable.m_gpuPacketBoundValid, m_commandTable.m_nPacketCapacity );
	}
	if ( fixed_storage_ok && m_uploadState.m_motionHistoryRecords.size() < pScene->m_instances.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_uploadState.m_motionHistoryRecords, pScene->m_instances.size() );
	}
	if ( fixed_storage_ok && m_uploadState.m_motionPreviousRecords.size() < pScene->m_instances.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_uploadState.m_motionPreviousRecords, pScene->m_instances.size() );
	}
	if ( fixed_storage_ok && m_uploadState.m_motionHistoryCells.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_uploadState.m_motionHistoryCells, pScene->m_cells.size() );
	}
	if ( fixed_storage_ok && m_uploadState.m_motionCurrentOrder.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_uploadState.m_motionCurrentOrder, pScene->m_cells.size() );
	}
	if ( fixed_storage_ok && m_uploadState.m_motionPreviousOrder.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_uploadState.m_motionPreviousOrder, pScene->m_cells.size() );
	}
	if ( fixed_storage_ok && m_uploadState.m_motionDirtyRanges.capacity() < pScene->m_dirtyRanges.capacity() ) {
		fixed_storage_ok = t3dContainerReserveCold( m_uploadState.m_motionDirtyRanges, pScene->m_dirtyRanges.capacity() );
	}
	if ( fixed_storage_ok && m_visibility.m_instanceCullCellSpans.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_instanceCullCellSpans, pScene->m_cells.size() );
	}
	if ( fixed_storage_ok && m_visibility.m_shadowInstanceCullCellSpans.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibility.m_shadowInstanceCullCellSpans, pScene->m_cells.size() );
	}
	if ( !fixed_storage_ok || !t3dContainerReserveCold( m_commandTable.m_drawPackets, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_depthPackets, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_shadowPackets, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_visibility.m_hlodDescriptors, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_visibility.m_instanceCullDescriptors, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_visibility.m_shadowInstanceCullDescriptors, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_visibility.m_instanceHiZRegionDescriptors, s_nRegionSlots )
	        || !t3dContainerReserveCold( m_visibility.m_instanceHiZRegionKeys, s_nRegionSlots )
	        || !t3dContainerReserveCold( m_commandTable.m_packetBuildSlots, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_packetBuildEntries, m_commandTable.m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_commandTable.m_alphaPackets, m_commandTable.m_nPacketCapacity ) ) {
		++m_uploadState.m_nCpuCapacityRebuildFailureCount;
		setError( "RD instance owner failed to establish fixed publication backing" );
		return false;
	}
	if ( !t3dContainerReserveCold( m_commandTable.m_packetIndex, m_commandTable.m_nPacketCapacity ) ) {
		++m_uploadState.m_nCpuCapacityRebuildFailureCount;
		setError( "RD instance owner failed to establish packet-index capacity" );
		return false;
	}
#endif

	return true;
}

bool Terrain3DRdInstanceRenderer::ensureSceneGpuStorage(
        const uint32_t instance_bytes, const uint32_t visible_bytes, const uint32_t material_bytes, const uint32_t indirect_bytes ) {
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	m_context.beginResourceRebuild();
	const bool buffers_ok = ensureBuffers( instance_bytes,
	                                MAX( visible_bytes, 4u ),
	                                MAX( material_bytes, ( uint32_t )( sizeof( t3dRenderingMaterialGpu_t ) ) ),
	                                0u )
	        && ensureUniformSet();
	m_context.endResourceRebuild();
#else
	const bool needs_gpu_cull_resources = T3D_INSTANCER_GPU_CULL_PRODUCTION != 0 || m_visibility.m_bCullingComparisonEnabled;
	if ( !needs_gpu_cull_resources ) {
		m_context.beginResourceRebuild();
		const bool direct_buffers_ok = ensureBuffers( instance_bytes,
		                                       MAX( visible_bytes, 4u ),
		                                       MAX( material_bytes, ( uint32_t )( sizeof( t3dRenderingMaterialGpu_t ) ) ),
		                                       0u )
		        && ensureShadowBuffers( MAX( visible_bytes, 4u ), 0u ) && ensureUniformSet();
		m_context.endResourceRebuild();
		if ( !direct_buffers_ok ) {
			return false;
		}
	} else {
		const uint32_t hlod_descriptor_bytes = MAX( m_commandTable.m_nPacketCapacity * ( uint32_t )( sizeof( t3dRdHlodCullDescriptor_t ) ),
		        ( uint32_t )( sizeof( t3dRdHlodCullDescriptor_t ) ) );
		const uint64_t instance_cull_descriptor_bytes64 =
		        ( uint64_t )MAX( m_commandTable.m_nPacketCapacity, 1u ) * 2u * sizeof( t3dRdInstanceCullDescriptor_t );
		if ( instance_cull_descriptor_bytes64 > UINT32_MAX ) {
			setError( "RD instance cull descriptor capacity exceeds 32-bit buffer limits" );
			return false;
		}
		const uint32_t instance_cull_descriptor_bytes = ( uint32_t )instance_cull_descriptor_bytes64;
		const uint32_t hiz_region_descriptor_bytes = MAX( ( uint32_t )( s_nRegionSlots * sizeof( t3dRdInstanceHiZRegionDescriptor_t ) ),
		        ( uint32_t )( sizeof( t3dRdInstanceHiZRegionDescriptor_t ) ) );
		const uint32_t hiz_region_visibility_bytes = MAX( ( uint32_t )( s_nRegionSlots * sizeof( uint32_t ) ), 4u );
		if ( ( m_visibility.m_hInstanceCullDescriptorBuffer.needsCapacity( instance_cull_descriptor_bytes )
		             || m_visibility.m_hInstanceHiZRegionDescriptorBuffer.needsCapacity( hiz_region_descriptor_bytes )
		             || m_visibility.m_hInstanceHiZRegionVisibilityBuffer.needsCapacity( hiz_region_visibility_bytes ) )
		        && m_visibility.m_hInstanceCullUniformSet.is_valid() ) {
			m_instanceCullDescriptors.erase( m_visibility.m_hInstanceCullUniformSet );
			m_visibility.m_hInstanceCullUniformSet = RID();
		}
		if ( m_visibility.m_hHlodCullDescriptorBuffer.needsCapacity( hlod_descriptor_bytes )
		        && m_visibility.m_hHlodCullUniformSet.is_valid() ) {
			m_hlodCullDescriptors.erase( m_visibility.m_hHlodCullUniformSet );
			m_visibility.m_hHlodCullUniformSet = RID();
		}
		if ( !m_visibility.m_hInstanceCullShader.isValid() ) {
			m_context.beginResourceRebuild();
			const bool shader_ok = ensureInstanceCullShader( m_visibility.m_instanceCullSourceShader );
			m_context.endResourceRebuild();
			if ( !shader_ok ) {
				return false;
			}
		}
		m_context.beginResourceRebuild();
		const bool gpu_buffers_ok = ensureBuffers( instance_bytes,
		                                    MAX( visible_bytes, 4u ),
		                                    MAX( material_bytes, ( uint32_t )( sizeof( t3dRenderingMaterialGpu_t ) ) ),
		                                    MAX( indirect_bytes, ( uint32_t )( sizeof( t3dRdIndexedIndirectCommand_t ) ) ) )
		        && ensureShadowBuffers(
		                MAX( visible_bytes, 4u ), MAX( indirect_bytes, ( uint32_t )( sizeof( t3dRdIndexedIndirectCommand_t ) ) ) )
		        && ensureOwnedBuffer( m_visibility.m_hHlodCullDescriptorBuffer,
		                hlod_descriptor_bytes,
		                "T3D.HLOD.CullDescriptors",
		                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
		                Terrain3DRdContext::MEMORY_INSTANCE,
		                Terrain3DRdContext::OWNER_HLOD )
		        && ensureOwnedBuffer( m_visibility.m_hInstanceCullDescriptorBuffer,
		                instance_cull_descriptor_bytes,
		                "T3D.Instance.CullDescriptors",
		                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
		                Terrain3DRdContext::MEMORY_INSTANCE,
		                Terrain3DRdContext::OWNER_INSTANCES )
		        && ensureOwnedBuffer( m_visibility.m_hInstanceHiZRegionDescriptorBuffer,
		                hiz_region_descriptor_bytes,
		                "T3D.Instance.HiZ.RegionDescriptors",
		                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
		                Terrain3DRdContext::MEMORY_INSTANCE,
		                Terrain3DRdContext::OWNER_INSTANCES )
		        && ensureOwnedBuffer( m_visibility.m_hInstanceHiZRegionVisibilityBuffer,
		                hiz_region_visibility_bytes,
		                "T3D.Instance.HiZ.RegionVisibility",
		                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
		                Terrain3DRdContext::MEMORY_INSTANCE,
		                Terrain3DRdContext::OWNER_INSTANCES )
		        && ensureUniformSet();
		m_context.endResourceRebuild();
		if ( !gpu_buffers_ok ) {
			return false;
		}
	}
	const bool buffers_ok = true;
#endif
	if ( !buffers_ok ) {
		return false;
	}

	return true;
}

void Terrain3DRdInstanceRenderer::ensureSceneUploadByteStorage( void ) {
	if ( m_uploadState.m_instanceBytes.size() < ( int64_t )( m_uploadState.m_hInstanceBuffer.capacityBytes() ) ) {
		m_uploadState.m_instanceBytes.resize( m_uploadState.m_hInstanceBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_previousInstanceBytes.size() < ( int64_t )( m_uploadState.m_hPreviousInstanceBuffer.capacityBytes() ) ) {
		m_uploadState.m_previousInstanceBytes.resize( m_uploadState.m_hPreviousInstanceBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_visibleBytes.size() < ( int64_t )( m_visibility.m_hVisibleIndexBuffer.capacityBytes() ) ) {
		m_uploadState.m_visibleBytes.resize( m_visibility.m_hVisibleIndexBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_shadowVisibleBytes.size() < ( int64_t )( m_visibility.m_hShadowVisibleIndexBuffer.capacityBytes() ) ) {
		m_uploadState.m_shadowVisibleBytes.resize( m_visibility.m_hShadowVisibleIndexBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_visibleDrawSlotBytes.size() < ( int64_t )( m_visibility.m_hVisibleDrawSlotBuffer.capacityBytes() ) ) {
		m_uploadState.m_visibleDrawSlotBytes.resize( m_visibility.m_hVisibleDrawSlotBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_shadowVisibleDrawSlotBytes.size() < ( int64_t )( m_visibility.m_hShadowVisibleDrawSlotBuffer.capacityBytes() ) ) {
		m_uploadState.m_shadowVisibleDrawSlotBytes.resize( m_visibility.m_hShadowVisibleDrawSlotBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_visibleLayerBytes.size() < ( int64_t )( m_visibility.m_hVisibleLayerBuffer.capacityBytes() ) ) {
		m_uploadState.m_visibleLayerBytes.resize( m_visibility.m_hVisibleLayerBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_shadowVisibleLayerBytes.size() < ( int64_t )( m_visibility.m_hShadowVisibleLayerBuffer.capacityBytes() ) ) {
		m_uploadState.m_shadowVisibleLayerBytes.resize( m_visibility.m_hShadowVisibleLayerBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_indirectBytes.size() < ( int64_t )( m_commandTable.m_mainIndirectSlice.m_nCapacityBytes ) ) {
		m_uploadState.m_indirectBytes.resize( m_commandTable.m_mainIndirectSlice.m_nCapacityBytes );
	}
	if ( m_uploadState.m_shadowIndirectBytes.size() < ( int64_t )( m_commandTable.m_shadowIndirectSlice.m_nCapacityBytes ) ) {
		m_uploadState.m_shadowIndirectBytes.resize( m_commandTable.m_shadowIndirectSlice.m_nCapacityBytes );
	}
	if ( m_uploadState.m_drawMetadataBytes.size() < ( int64_t )( m_commandTable.m_hDrawMetadataBuffer.capacityBytes() ) ) {
		m_uploadState.m_drawMetadataBytes.resize( m_commandTable.m_hDrawMetadataBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_hlodDescriptorBytes.size() < ( int64_t )( m_visibility.m_hHlodCullDescriptorBuffer.capacityBytes() ) ) {
		m_uploadState.m_hlodDescriptorBytes.resize( m_visibility.m_hHlodCullDescriptorBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_instanceCullDescriptorBytes.size() < ( int64_t )( m_visibility.m_hInstanceCullDescriptorBuffer.capacityBytes() ) ) {
		m_uploadState.m_instanceCullDescriptorBytes.resize( m_visibility.m_hInstanceCullDescriptorBuffer.capacityBytes() );
	}
	if ( m_uploadState.m_instanceHiZRegionDescriptorBytes.size()
	        < ( int64_t )( m_visibility.m_hInstanceHiZRegionDescriptorBuffer.capacityBytes() ) ) {
		m_uploadState.m_instanceHiZRegionDescriptorBytes.resize( m_visibility.m_hInstanceHiZRegionDescriptorBuffer.capacityBytes() );
	}

	const uint32_t dirty_chunk_bytes = s_nDirtyChunkRecords * sizeof( t3dRdInstanceRecord_t );
	if ( m_uploadState.m_dirtyInstanceBytes.size() < ( int64_t )( dirty_chunk_bytes ) ) {
		m_uploadState.m_dirtyInstanceBytes.resize( dirty_chunk_bytes );
	}
}

bool Terrain3DRdInstanceRenderer::uploadSceneInstanceRecords( const t3dRdInstanceSceneSnapshot_t &scene,
        const uint32_t instance_bytes,
        const bool full_instance_upload ) {
	const t3dRdInstanceSceneSnapshot_t *const pScene = &scene;
	if ( instance_bytes > 0u ) {
		const uint8_t *const pSource = ( const uint8_t * )( pScene->m_instances.data() );
		if ( full_instance_upload ) {
			memcpy( m_uploadState.m_instanceBytes.ptrw(), pSource, instance_bytes );
			if ( !m_uploadState.m_hInstanceBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_instanceBytes, instance_bytes, T3D_GPU_PUBLICATION ) ) {
				return false;
			}
		} else {
			for ( size_t iRange = 0u; iRange < pScene->m_dirtyRanges.size(); ++iRange ) {
				const t3dRdInstanceDirtyRange_t &range = pScene->m_dirtyRanges[iRange];
				if ( range.m_nInstanceCount == 0u ) {
					continue;
				}
				if ( ( uint64_t )range.m_nFirstInstance + range.m_nInstanceCount > pScene->m_instances.size() ) {
					setError( "RD instance dirty range exceeds published instance table" );
					return false;
				}
				uint32_t first_record = range.m_nFirstInstance;
				uint32_t remaining = range.m_nInstanceCount;
				while ( remaining > 0u ) {
					const uint32_t record_count = MIN( remaining, s_nDirtyChunkRecords );
					const uint32_t first_byte = first_record * sizeof( t3dRdInstanceRecord_t );
					const uint32_t chunk_bytes = record_count * sizeof( t3dRdInstanceRecord_t );
					memcpy( m_uploadState.m_dirtyInstanceBytes.ptrw(), pSource + first_byte, chunk_bytes );
					if ( !m_uploadState.m_hInstanceBuffer.writePackedTracked( m_context, first_byte, m_uploadState.m_dirtyInstanceBytes, chunk_bytes, T3D_GPU_DIRTY_UPLOAD ) ) {
						return false;
					}
					first_record += record_count;
					remaining -= record_count;
				}
			}
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadScene( const t3dRdInstanceSceneSnapshot_t *const pScene ) {
	if ( !pScene ) {
		return false;
	}
	if ( pScene->m_nGeneration == m_uploadState.m_nUploadedSceneGeneration
	        && m_geometryArena.m_nSceneCapacityMeshGeneration == m_geometryArena.m_meshCache.getSourceGeneration() ) {
		return true;
	}
	uint32_t instance_bytes = 0u;
	uint32_t visible_bytes = 0u;
	uint32_t material_bytes = 0u;
	uint32_t indirect_bytes = 0u;
	bool full_instance_upload = false;
	if ( !prepareSceneUploadPlan( *pScene, instance_bytes, visible_bytes, material_bytes, indirect_bytes, full_instance_upload ) ) {
		return false;
	}
	if ( !ensureSceneCpuStorage( *pScene ) || !ensureSceneGpuStorage( instance_bytes, visible_bytes, material_bytes, indirect_bytes ) ) {
		return false;
	}
	ensureSceneUploadByteStorage();
	if ( !uploadSceneInstanceRecords( *pScene, instance_bytes, full_instance_upload ) ) {
		return false;
	}
	if ( !m_uploadState.m_hInstanceBuffer.setUsedBytes( instance_bytes ) ) {
		return false;
	}
	++m_uploadState.m_nSceneUploads;
	m_uploadState.m_nUploadedSceneGeneration = pScene->m_nGeneration;
	m_geometryArena.m_nSceneCapacityMeshGeneration = m_geometryArena.m_meshCache.getSourceGeneration();
	m_commandTable.m_nStaticInstances = pScene->m_instances.size();
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadPreviousMotionRanges(
        const t3dRdInstanceRecord_t *const pRecords, const eastl::vector<t3dRdInstanceDirtyRange_t, Terrain3DEastlAllocator> &rRanges ) {
	if ( !pRecords ) {
		return false;
	}
	for ( size_t iRange = 0u; iRange < rRanges.size(); ++iRange ) {
		const t3dRdInstanceDirtyRange_t &range = rRanges[iRange];
		if ( range.m_nInstanceCount == 0u ) {
			continue;
		}
		const uint64_t end = ( uint64_t )range.m_nFirstInstance + range.m_nInstanceCount;
		if ( end > m_uploadState.m_nMotionHistoryRecordCount ) {
			setError( "RD motion dirty range exceeded retained history" );
			return false;
		}
		uint32_t first_record = range.m_nFirstInstance;
		uint32_t remaining = range.m_nInstanceCount;
		while ( remaining > 0u ) {
			const uint32_t record_count = MIN( remaining, s_nDirtyChunkRecords );
			const uint32_t byte_offset = first_record * sizeof( t3dRdInstanceRecord_t );
			const uint32_t byte_count = record_count * sizeof( t3dRdInstanceRecord_t );
			memcpy( m_uploadState.m_dirtyInstanceBytes.ptrw(), pRecords + first_record, byte_count );
			if ( !m_uploadState.m_hPreviousInstanceBuffer.writePackedTracked( m_context, byte_offset, m_uploadState.m_dirtyInstanceBytes, byte_count, T3D_GPU_HISTORY ) ) {
				return false;
			}
			first_record += record_count;
			remaining -= record_count;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::prepareMotionHistory( const t3dRdInstanceSceneSnapshot_t &rScene, const uint64_t nRenderFrame ) {
	if ( m_uploadState.m_nMotionHistoryRenderFrame == nRenderFrame
	        && rScene.m_nGeneration == m_uploadState.m_nMotionHistorySceneGeneration ) {
		return true;
	}
	if ( rScene.m_instances.empty() ) {
		m_uploadState.m_nMotionHistoryRecordCount = 0u;
		m_uploadState.m_nMotionHistoryCellCount = 0u;
		m_uploadState.m_nMotionPreviousRecordCount = 0u;
		m_uploadState.m_nMotionCurrentOrderCount = 0u;
		m_uploadState.m_nMotionPreviousOrderCount = 0u;
		m_uploadState.m_motionDirtyRanges.clear();
		m_uploadState.m_nMotionHistorySceneGeneration = rScene.m_nGeneration;
		m_uploadState.m_bMotionHistoryNeedsAdvance = false;
		m_uploadState.m_nMotionHistoryRenderFrame = nRenderFrame;
		return true;
	}

	const bool reset_history = m_uploadState.m_nMotionHistoryRecordCount == 0u || m_uploadState.m_nMotionHistoryCellCount == 0u;
	const bool scene_changed = rScene.m_nGeneration != m_uploadState.m_nMotionHistorySceneGeneration;
	const bool stable_topology = !reset_history && !rScene.m_bInstanceLayoutChanged
	        && m_uploadState.m_nMotionHistoryRecordCount == rScene.m_instances.size()
	        && m_uploadState.m_nMotionHistoryCellCount == rScene.m_cells.size();

	if ( stable_topology ) {
		if ( scene_changed ) {
			if ( rScene.m_dirtyRanges.size() > m_uploadState.m_motionDirtyRanges.capacity() ) {
				setError( "RD motion dirty-range publication exceeded sealed capacity" );
				return false;
			}
			m_uploadState.m_motionDirtyRanges.clear();
			for ( size_t iRange = 0u; iRange < rScene.m_dirtyRanges.size(); ++iRange ) {
				const t3dRdInstanceDirtyRange_t range = rScene.m_dirtyRanges[iRange];
				const uint64_t end = ( uint64_t )range.m_nFirstInstance + range.m_nInstanceCount;
				if ( end > rScene.m_instances.size() ) {
					setError( "RD motion source dirty range exceeded current scene" );
					return false;
				}
				if ( range.m_nInstanceCount > 0u && !t3dVectorPushBackWithinCapacity( m_uploadState.m_motionDirtyRanges, range ) ) {
					return false;
				}
			}
			if ( !uploadPreviousMotionRanges( m_uploadState.m_motionHistoryRecords.data(), m_uploadState.m_motionDirtyRanges ) ) {
				return false;
			}
			for ( size_t iRange = 0u; iRange < m_uploadState.m_motionDirtyRanges.size(); ++iRange ) {
				const t3dRdInstanceDirtyRange_t &range = m_uploadState.m_motionDirtyRanges[iRange];
				memcpy( m_uploadState.m_motionHistoryRecords.data() + range.m_nFirstInstance,
				        rScene.m_instances.data() + range.m_nFirstInstance,
				        range.m_nInstanceCount * sizeof( t3dRdInstanceRecord_t ) );
			}
			if ( !rScene.m_cells.empty() ) {
				memcpy( m_uploadState.m_motionHistoryCells.data(),
				        rScene.m_cells.data(),
				        rScene.m_cells.size() * sizeof( t3dRdInstanceCell_t ) );
			}
			m_uploadState.m_nMotionHistorySceneGeneration = rScene.m_nGeneration;
			m_uploadState.m_bMotionHistoryNeedsAdvance = !m_uploadState.m_motionDirtyRanges.empty();
		} else if ( m_uploadState.m_bMotionHistoryNeedsAdvance ) {
			if ( !uploadPreviousMotionRanges( m_uploadState.m_motionHistoryRecords.data(), m_uploadState.m_motionDirtyRanges ) ) {
				return false;
			}
			m_uploadState.m_motionDirtyRanges.clear();
			m_uploadState.m_bMotionHistoryNeedsAdvance = false;
		}
		m_uploadState.m_nMotionHistoryRenderFrame = nRenderFrame;
		return true;
	}

	if ( !scene_changed && !m_uploadState.m_bMotionHistoryNeedsAdvance ) {
		m_uploadState.m_nMotionHistoryRenderFrame = nRenderFrame;
		return true;
	}

	bool upload_previous = false;
	if ( rScene.m_instances.size() > m_uploadState.m_motionPreviousRecords.size() ) {
		setError( "RD motion previous-record output exceeded publication-time capacity" );
		return false;
	}
	m_uploadState.m_nMotionPreviousRecordCount = ( uint32_t )rScene.m_instances.size();
	memcpy( m_uploadState.m_motionPreviousRecords.data(),
	        rScene.m_instances.data(),
	        rScene.m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
	m_uploadState.m_motionDirtyRanges.clear();
	if ( reset_history ) {
		upload_previous = true;
		++m_uploadState.m_nMotionHistoryResets;
		m_uploadState.m_bMotionHistoryNeedsAdvance = false;
	} else if ( scene_changed ) {
		if ( rScene.m_cells.size() > m_uploadState.m_motionCurrentOrder.size()
		        || m_uploadState.m_nMotionHistoryCellCount > m_uploadState.m_motionPreviousOrder.size() ) {
			setError( "RD motion cell-order output exceeded publication-time capacity" );
			return false;
		}
		m_uploadState.m_nMotionCurrentOrderCount = ( uint32_t )rScene.m_cells.size();
		m_uploadState.m_nMotionPreviousOrderCount = m_uploadState.m_nMotionHistoryCellCount;
		for ( uint32_t i = 0u; i < m_uploadState.m_nMotionCurrentOrderCount; ++i ) {
			m_uploadState.m_motionCurrentOrder[i] = i;
		}
		for ( uint32_t i = 0u; i < m_uploadState.m_nMotionPreviousOrderCount; ++i ) {
			m_uploadState.m_motionPreviousOrder[i] = i;
		}
		t3dRdMotionCellIndexLess_t currentLess;
		currentLess.m_pCells = rScene.m_cells.data();
		eastl::sort( m_uploadState.m_motionCurrentOrder.begin(),
		        m_uploadState.m_motionCurrentOrder.begin() + m_uploadState.m_nMotionCurrentOrderCount,
		        currentLess );
		t3dRdMotionCellIndexLess_t previousLess;
		previousLess.m_pCells = m_uploadState.m_motionHistoryCells.data();
		eastl::sort( m_uploadState.m_motionPreviousOrder.begin(),
		        m_uploadState.m_motionPreviousOrder.begin() + m_uploadState.m_nMotionPreviousOrderCount,
		        previousLess );

		size_t current = 0u;
		size_t previous = 0u;
		while ( current < m_uploadState.m_nMotionCurrentOrderCount && previous < m_uploadState.m_nMotionPreviousOrderCount ) {
			const t3dRdInstanceCell_t &current_cell = rScene.m_cells[m_uploadState.m_motionCurrentOrder[current]];
			const t3dRdInstanceCell_t &previous_cell = m_uploadState.m_motionHistoryCells[m_uploadState.m_motionPreviousOrder[previous]];
			const int compare = t3dRdMotionCellCompare( current_cell, previous_cell );
			if ( compare < 0 ) {
				++current;
				continue;
			}
			if ( compare > 0 ) {
				++previous;
				continue;
			}
			const uint64_t current_end = ( uint64_t )current_cell.m_nFirstInstance + current_cell.m_nInstanceCount;
			const uint64_t previous_end = ( uint64_t )previous_cell.m_nFirstInstance + previous_cell.m_nInstanceCount;
			if ( current_end <= rScene.m_instances.size() && previous_end <= m_uploadState.m_nMotionHistoryRecordCount
			        && t3dRdMotionCellsMatch( current_cell, previous_cell ) ) {
				memcpy( m_uploadState.m_motionPreviousRecords.data() + current_cell.m_nFirstInstance,
				        m_uploadState.m_motionHistoryRecords.data() + previous_cell.m_nFirstInstance,
				        current_cell.m_nInstanceCount * sizeof( t3dRdInstanceRecord_t ) );
				m_uploadState.m_nMotionHistoryMatches += current_cell.m_nInstanceCount;
			}
			++current;
			++previous;
		}
		upload_previous = true;
		m_uploadState.m_bMotionHistoryNeedsAdvance = true;
	} else if ( m_uploadState.m_bMotionHistoryNeedsAdvance ) {
		upload_previous = true;
		m_uploadState.m_bMotionHistoryNeedsAdvance = false;
	}

	if ( upload_previous ) {
		RenderingDevice *const rd = m_context.getDevice();
		const uint32_t bytes = m_uploadState.m_nMotionPreviousRecordCount * sizeof( t3dRdInstanceRecord_t );
		if ( bytes > m_uploadState.m_hPreviousInstanceBuffer.capacityBytes() || m_uploadState.m_previousInstanceBytes.size() < bytes ) {
			setError( "RD motion history exceeded publication-time capacity" );
			return false;
		}
		memcpy( m_uploadState.m_previousInstanceBytes.ptrw(), m_uploadState.m_motionPreviousRecords.data(), bytes );
		if ( !m_uploadState.m_hPreviousInstanceBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_previousInstanceBytes, bytes, T3D_GPU_HISTORY ) ) {
			return false;
		}
	}

	if ( reset_history || scene_changed ) {
		if ( rScene.m_instances.size() > m_uploadState.m_motionHistoryRecords.size()
		        || rScene.m_cells.size() > m_uploadState.m_motionHistoryCells.size() ) {
			setError( "RD motion history exceeded publication-time backing" );
			return false;
		}
		m_uploadState.m_nMotionHistoryRecordCount = ( uint32_t )rScene.m_instances.size();
		memcpy( m_uploadState.m_motionHistoryRecords.data(),
		        rScene.m_instances.data(),
		        rScene.m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
		m_uploadState.m_nMotionHistoryCellCount = ( uint32_t )rScene.m_cells.size();
		if ( !rScene.m_cells.empty() ) {
			memcpy( m_uploadState.m_motionHistoryCells.data(),
			        rScene.m_cells.data(),
			        rScene.m_cells.size() * sizeof( t3dRdInstanceCell_t ) );
		}
		m_uploadState.m_nMotionHistorySceneGeneration = rScene.m_nGeneration;
	}
	m_uploadState.m_nMotionHistoryRenderFrame = nRenderFrame;
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadMaterials( void ) {
	const uint32_t nVtDatabaseGeneration = m_pVirtualTexture != NULL ? m_pVirtualTexture->getDatabaseGeneration() : 0u;
	if ( m_geometryArena.m_nUploadedMeshGeneration == m_geometryArena.m_meshCache.getSourceGeneration()
	        && m_nUploadedVtDatabaseGeneration == nVtDatabaseGeneration && m_descriptorState.m_hMaterialVtUniformSet.is_valid() ) {
		return true;
	}
	const t3dRenderingMaterialGpu_t *const pMaterials = m_geometryArena.m_meshCache.getVtMaterials();
	const uint64_t *const pStableKeys = m_geometryArena.m_meshCache.getVtStableMaterialKeys();
	const size_t nMaterialCount = m_geometryArena.m_meshCache.getVtMaterialCount();
	if ( nMaterialCount != m_geometryArena.m_meshCache.getMaterialCount()
	        || nMaterialCount != m_geometryArena.m_meshCache.getVtStableMaterialKeyCount() ) {
		setError( "RD instance VT material table lost one-to-one material/stable-key alignment" );
		return false;
	}
	const uint32_t bytes = ( uint32_t )( nMaterialCount * sizeof( t3dRenderingMaterialGpu_t ) );
	m_context.beginResourceRebuild();
	const bool buffers_ok = ensureBuffers( m_uploadState.m_hInstanceBuffer.capacityBytes(),
	                                m_visibility.m_hVisibleIndexBuffer.capacityBytes(),
	                                MAX( bytes, ( uint32_t )( sizeof( t3dRenderingMaterialGpu_t ) ) ),
	                                m_commandTable.m_mainIndirectSlice.m_nCapacityBytes )
	        && ensureUniformSet() && ensureMaterialVtUniformSet();
	m_context.endResourceRebuild();
	if ( !buffers_ok ) {
		if ( m_lastError.is_empty() ) {
			setError( "RD instance VT material upload failed while establishing persistent buffers/uniform set" );
		}
		return false;
	}
	m_nVtStableKeyMissCount = 0u;
	m_nVtInvalidAllocationReferenceCount = 0u;
	m_nVtOutOfRangePageReferenceCount = 0u;
	if ( bytes > 0u ) {
		if ( m_uploadState.m_materialBytes.size() < ( int64_t )( m_uploadState.m_hMaterialBuffer.capacityBytes() ) ) {
			m_uploadState.m_materialBytes.resize( m_uploadState.m_hMaterialBuffer.capacityBytes() );
		}
		if ( m_uploadState.m_materialBytes.size() < ( int64_t )( bytes ) ) {
			setError( "RD instance VT material upload scratch capacity is smaller than the published material table" );
			return false;
		}
		memcpy( m_uploadState.m_materialBytes.ptrw(), pMaterials, bytes );
		t3dRenderingMaterialGpu_t *const pResolvedMaterials = ( t3dRenderingMaterialGpu_t * )( m_uploadState.m_materialBytes.ptrw() );
		const uint32_t nActiveAllocationCount = m_pVirtualTexture != NULL ? m_pVirtualTexture->getActiveAllocationCount() : 0u;
		const uint32_t nActivePageCount = m_pVirtualTexture != NULL ? m_pVirtualTexture->getActiveVirtualPageCount() : 0u;
		const T3DVTResidencyState *const pResidency = m_pVirtualTexture != NULL ? &m_pVirtualTexture->get_residency() : NULL;
		for ( size_t iMaterial = 0u; iMaterial < nMaterialCount; ++iMaterial ) {
			uint32_t nAllocationId = T3D_VT_INVALID_ALLOCATION;
			if ( pStableKeys[iMaterial] == 0u || m_pVirtualTexture == NULL
			        || m_pVirtualTexture->getDatabaseKind() != T3D_VT_RUNTIME_DATABASE_PACK_V3
			        || !m_pVirtualTexture->findMaterialAllocationCold( pStableKeys[iMaterial], nAllocationId ) ) {
				++m_nVtStableKeyMissCount;
				nAllocationId = T3D_VT_INVALID_ALLOCATION;
			} else if ( nAllocationId >= nActiveAllocationCount || pResidency == NULL || !pResidency->bInitialized
			        || pResidency->Buffers.pAllocations == NULL || nAllocationId >= pResidency->nAllocationCount ) {
				++m_nVtInvalidAllocationReferenceCount;
				nAllocationId = T3D_VT_INVALID_ALLOCATION;
			} else {
				const t3dVTAllocation_t &rAllocation = pResidency->Buffers.pAllocations[nAllocationId];
				const uint64_t nPageEnd = ( uint64_t )( rAllocation.m_nBaseVirtualPage ) + rAllocation.m_nPageCount;
				if ( rAllocation.m_nBaseVirtualPage >= nActivePageCount || nPageEnd > nActivePageCount ) {
					++m_nVtOutOfRangePageReferenceCount;
					nAllocationId = T3D_VT_INVALID_ALLOCATION;
				}
			}
			pResolvedMaterials[iMaterial].m_nVtAllocation = nAllocationId;
			if ( nAllocationId != T3D_VT_INVALID_ALLOCATION ) {
				pResolvedMaterials[iMaterial].m_nFlags |= T3D_VT_MATERIAL_V3_LAYOUT;
			} else {
				pResolvedMaterials[iMaterial].m_nFlags &= ~T3D_VT_MATERIAL_V3_LAYOUT;
			}
			pResolvedMaterials[iMaterial].m_nOverlayVtAllocation = T3D_VT_INVALID_ALLOCATION;
		}
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd ) {
			setError( "RD instance VT material upload lost the RenderingDevice before buffer_update" );
			return false;
		}
		if ( !m_uploadState.m_hMaterialBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_materialBytes, bytes, T3D_GPU_PUBLICATION ) ) {
			setError( String( "RD instance VT material buffer_update failed; bytes=" ) + String::num_int64( bytes ) );
			return false;
		}
	}
	m_geometryArena.m_nUploadedMeshGeneration = m_geometryArena.m_meshCache.getSourceGeneration();
	m_nUploadedVtDatabaseGeneration = nVtDatabaseGeneration;
	return true;
}

void Terrain3DRdInstanceRenderer::requestPublishedMaterialResidencyHot( const uint32_t nFrameId ) {
	if ( m_pVirtualTexture == NULL || nFrameId == m_nLastVtResidencyRequestFrame || m_commandTable.m_drawPackets.empty() ) {
		return;
	}
	m_nLastVtResidencyRequestFrame = nFrameId;
	const size_t nMaterialCount = m_geometryArena.m_meshCache.getVtMaterialCount();
	const uint64_t nRequiredBytes = nMaterialCount * sizeof( t3dRenderingMaterialGpu_t );
	if ( nMaterialCount == 0u || nRequiredBytes > ( uint64_t )m_uploadState.m_materialBytes.size() ) {
		return;
	}
	const t3dRenderingMaterialGpu_t *const pMaterials = ( const t3dRenderingMaterialGpu_t * )m_uploadState.m_materialBytes.ptr();
	uint64_t seen[1024];
	memset( seen, 0, sizeof( seen ) );
	const uint32_t nMaterialLimit = MIN( ( uint32_t )nMaterialCount, 65536u );
	const float uvMin[2] = { 0.0f, 0.0f };
	const float uvMax[2] = { 1.0f, 1.0f };
	for ( size_t iPacket = 0u; iPacket < m_commandTable.m_drawPackets.size(); ++iPacket ) {
		const t3dRdInstanceDrawPacket_t &packet = m_commandTable.m_drawPackets[iPacket];
		const uint32_t material_indices[2] = { packet.m_nMaterial, packet.m_nOverlayMaterial };
		for ( uint32_t iMaterialSlot = 0u; iMaterialSlot < 2u; ++iMaterialSlot ) {
			const uint32_t nMaterial = material_indices[iMaterialSlot];
			if ( nMaterial == UINT16_MAX || nMaterial >= nMaterialLimit ) {
				continue;
			}
			const uint32_t nWord = nMaterial >> 6u;
			const uint64_t nBit = 1ull << ( nMaterial & 63u );
			if ( ( seen[nWord] & nBit ) != 0u ) {
				continue;
			}
			seen[nWord] |= nBit;
			const uint32_t nAllocation = pMaterials[nMaterial].m_nVtAllocation;
			if ( nAllocation == T3D_VT_INVALID_ALLOCATION ) {
				continue;
			}
			// GPU-P01 removes the rotating four-page mip-0 window. A coherent
			// material footprint is requested instead; large allocations coarsen
			// deterministically until the rectangle fits this per-material cap.
			for ( uint32_t iChannel = 0u; iChannel < T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION; ++iChannel ) {
				const uint32_t nRuntimeAllocation = nAllocation + iChannel;
				if ( nRuntimeAllocation >= m_pVirtualTexture->getActiveAllocationCount() ) {
					break;
				}
				const t3dVTAllocation_t &rAllocation = m_pVirtualTexture->get_residency().Buffers.pAllocations[nRuntimeAllocation];
				if ( rAllocation.m_nPageCount == 0u ) {
					continue;
				}
				m_nVtResidencyRequestAttempts += m_pVirtualTexture->stage_material_footprint_hot(
				        nRuntimeAllocation, nFrameId, uvMin, uvMax, 0u, 16u, T3D_VT_PRIORITY_NEAR_MIN, T3D_VT_REQUEST_PRODUCER_INSTANCE );
			}
		}
	}
}

template <bool t_bShadowPass>
bool Terrain3DRdInstanceRenderer::beginPacketBuildForPass( const t3dRdInstanceSceneSnapshot_t &rScene, const bool bSkipFrustum ) {
	m_commandTable.m_packetIndex.clear();
	m_commandTable.m_packetBuildSlots.clear();
	m_commandTable.m_packetBuildEntries.clear();
	m_commandTable.m_alphaPackets.clear();
	if constexpr ( t_bShadowPass ) {
		m_visibility.m_nShadowVisibleIndexCount = 0u;
		m_commandTable.m_nShadowCompatibleCommandCount = 0u;
		m_commandTable.m_shadowPackets.clear();
		if ( bSkipFrustum ) {
			m_visibility.m_shadowInstanceCullDescriptors.clear();
			if ( rScene.m_cells.size() > m_visibility.m_shadowInstanceCullCellSpans.size() ) {
				setError( String( "RD shadow instance cell-span backing is smaller than the published cell table; cells=" )
				        + String::num_int64( rScene.m_cells.size() ) + String( ", backing=" )
				        + String::num_int64( m_visibility.m_shadowInstanceCullCellSpans.size() ) );
				return false;
			}
			memset( m_visibility.m_shadowInstanceCullCellSpans.data(), 0, rScene.m_cells.size() * sizeof( t3dRdInstanceDescriptorSpan_t ) );
		}
	} else {
		m_visibility.m_nVisibleIndexCount = 0u;
		m_commandTable.m_nMainCompatibleCommandCount = 0u;
		m_visibility.m_szVisibilityLayerFilteredCells[0] = 0u;
		m_visibility.m_szVisibilityLayerFilteredCells[1] = 0u;
		m_visibility.m_szVisibilityLayerFilteredCells[2] = 0u;
		m_commandTable.m_nPacketBuildInputCells = rScene.m_cells.size();
		m_commandTable.m_nResolvedMeshCells = 0u;
		m_commandTable.m_nUnresolvedMeshCells = 0u;
		m_commandTable.m_drawPackets.clear();
		m_commandTable.m_depthPackets.clear();
		m_visibility.m_hlodDescriptors.clear();
		if ( bSkipFrustum ) {
			m_visibility.m_instanceCullDescriptors.clear();
			if ( rScene.m_cells.size() > m_visibility.m_instanceCullCellSpans.size() ) {
				setError( String( "RD instance cell-span backing is smaller than the published cell table; cells=" )
				        + String::num_int64( rScene.m_cells.size() ) + String( ", backing=" )
				        + String::num_int64( m_visibility.m_instanceCullCellSpans.size() ) );
				return false;
			}
			memset( m_visibility.m_instanceCullCellSpans.data(), 0, rScene.m_cells.size() * sizeof( t3dRdInstanceDescriptorSpan_t ) );
		}
	}
	return true;
}

template <bool t_bShadowPass>
bool Terrain3DRdInstanceRenderer::collectPacketEntriesForPass( const t3dRdInstanceSceneSnapshot_t &rScene,
        const Projection *const pViewProjections,
        const uint32_t nViewCount,
        const Vector3 &rCamera,
        const uint32_t nVisibilityMask,
        const bool bSkipFrustum ) {
	t3dRdInstanceFrustum_t frustums[s_nRdForwardMaxViews];
	if ( !bSkipFrustum ) {
		for ( uint32_t view = 0u; view < nViewCount; ++view ) {
			frustums[view] = t3dRdExtractFrustum( pViewProjections[view] );
		}
	}

	uint32_t unique_key = 1u;

	for ( size_t iCell = 0u; iCell < ( size_t )( ( rScene.m_cells ).size() ); iCell++ ) {
		const t3dRdInstanceCell_t &cell = ( rScene.m_cells )[iCell];
		const bool hlod = ( cell.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u;
		if constexpr ( t_bShadowPass ) {
			if ( ( cell.m_nVisibilityLayers & nVisibilityMask ) == 0u || ( cell.m_nFlags & T3D_RD_INSTANCE_CAST_SHADOWS ) == 0u ) {
				continue;
			}
		} else if ( ( cell.m_nVisibilityLayers & nVisibilityMask ) == 0u ) {
			const uint32_t category = ( cell.m_nFlags & T3D_RD_INSTANCE_CATEGORY_MASK ) >> T3D_RD_INSTANCE_CATEGORY_SHIFT;
			if ( category < 3u ) {
				++m_visibility.m_szVisibilityLayerFilteredCells[category];
			}
			continue;
		}
		if ( ( uint64_t )( cell.m_nFirstInstance ) + cell.m_nInstanceCount > rScene.m_instances.size() ) {
			setError( "RD instance cell range exceeds published instance table" );
			return false;
		}

		const t3dRdMeshHandle_t mesh = m_geometryArena.m_meshCache.getMeshHandleBySourceKey(
		        cell.m_nMeshSourceKey != 0u ? cell.m_nMeshSourceKey : t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = m_geometryArena.m_meshCache.resolveMesh( mesh );
		if ( !pLod ) {
			if constexpr ( !t_bShadowPass ) {
				++m_commandTable.m_nUnresolvedMeshCells;
			}
			continue;
		}
		if ( !t3dRdInstanceMeshIdentityMatchesCell( cell, pLod ) ) {
			setError( "RD instance mesh identity mismatch during packet build; refusing stale mesh-bank alias" );
			return false;
		}
		if constexpr ( !t_bShadowPass ) {
			++m_commandTable.m_nResolvedMeshCells;
		}

		bool visible = false;
		if ( bSkipFrustum ) {
			visible = cell.m_nInstanceCount > 0u;
			if constexpr ( !t_bShadowPass ) {
				visible = visible && ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u;
			}
		} else if constexpr ( !t_bShadowPass ) {
			if ( hlod && nViewCount == 1u ) {
#if T3D_INSTANCER_RD_PORT
				visible = cell.m_nInstanceCount > 0u && ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u
				        && t3dRdInstanceCellVisible( cell, frustums[0], rCamera, true );
#else
				visible = cell.m_nInstanceCount > 0u && ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u;
#endif
			} else {
				for ( uint32_t view = 0u; view < nViewCount && !visible; ++view ) {
					visible = hlod ? ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u
					                && t3dRdInstanceCellVisible( cell, frustums[view], rCamera, true )
					               : t3dRdInstanceCellVisible( cell, frustums[view], rCamera, false );
				}
			}
		} else {
			for ( uint32_t view = 0u; view < nViewCount && !visible; ++view ) {
				visible = t3dRdInstanceCellVisible( cell, frustums[view], rCamera, true );
			}
		}
		if ( !visible ) {
			continue;
		}

		uint32_t cell_draw_flags = cell.m_nFlags;
		if constexpr ( !t_bShadowPass ) {
			if ( bSkipFrustum && cell.m_fFadeMargin > 0.0f ) {
				cell_draw_flags &= ~T3D_RD_INSTANCE_FADE_MASK;
				cell_draw_flags |= T3D_RD_INSTANCE_FADE_ACTIVE | T3D_RD_INSTANCE_FADE_MASK;
			} else if ( cell.m_fFadeMargin > 0.0f ) {
				const float distance_sq = ( float )( cell.m_bounds.get_center().distance_squared_to( rCamera ) );
				const float begin_fade_end = cell.m_fLodBegin + cell.m_fFadeMargin;
				const float end_fade_begin = MAX( 0.0f, cell.m_fLodEnd - cell.m_fFadeMargin );
				const bool near_begin = cell.m_fLodBegin > 0.001f && distance_sq < begin_fade_end * begin_fade_end;
				const bool near_end = cell.m_fLodEnd > 0.001f && distance_sq > end_fade_begin * end_fade_begin;
				if ( near_begin || near_end ) {
					const float distance = sqrt( distance_sq );
					float fade = 1.0f;
					if ( near_begin ) {
						fade = MIN( fade, ( distance - cell.m_fLodBegin ) / cell.m_fFadeMargin );
					}
					if ( near_end ) {
						fade = MIN( fade, ( cell.m_fLodEnd - distance ) / cell.m_fFadeMargin );
					}
					const uint32_t fade_unorm = ( uint32_t )( lround( CLAMP( fade, 0.0f, 1.0f ) * 65535.0f ) );
					cell_draw_flags |=
					        T3D_RD_INSTANCE_FADE_ACTIVE | ( fade_unorm << T3D_RD_INSTANCE_FADE_SHIFT ) & T3D_RD_INSTANCE_FADE_MASK;
				}
			}
		}

		for ( uint32_t surface_index = 0u; surface_index < pLod->m_nSurfaceCount; ++surface_index ) {
			const t3dRdSurfaceHandle_t handle = m_geometryArena.m_meshCache.getSurfaceHandle( mesh, surface_index );
			const t3dRdMeshSourceSurface_t *const pSurface = m_geometryArena.m_meshCache.resolveSurface( handle );
			if ( !pSurface || pSurface->m_nIndexCount == 0u ) {
				continue;
			}
			const uint32_t draw_flags = cell_draw_flags | pSurface->m_nFlags;
			const bool alpha_blend = ( draw_flags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u;
			if constexpr ( t_bShadowPass ) {
				if ( alpha_blend && ( draw_flags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) == 0u ) {
					continue;
				}
			}

			const bool range_fade = ( draw_flags & T3D_RD_INSTANCE_FADE_ACTIVE ) != 0u;
			const bool can_group = t_bShadowPass || ( !hlod && !alpha_blend && !range_fade );
			t3dRdPacketKey_t key;
			key.m_nSurfaceSlot = handle.m_nSlot;
			key.m_nFlags = draw_flags;
			key.m_nMaterial = pSurface->m_nMaterial;
			key.m_nOverlayMaterial = pSurface->m_nOverlayMaterial;
			key.m_nVisibilityLayers = cell.m_nVisibilityLayers;
			key.m_nUnique = can_group ? 0u : unique_key++;

			uint32_t packet_index = UINT32_MAX;
			if ( const uint32_t *const pExisting = t3dMapFind( m_commandTable.m_packetIndex, key ) ) {
				packet_index = *pExisting;
			} else {
				if ( m_commandTable.m_packetBuildSlots.size() >= m_commandTable.m_nPacketCapacity ) {
					setError( "RD instance packet count exceeded publication-time capacity" );
					return false;
				}
				packet_index = ( uint32_t )( m_commandTable.m_packetBuildSlots.size() );
				if ( !t3dMapInsertOrAssignWithinCapacity( m_commandTable.m_packetIndex, key, packet_index ) ) {
					setError( "RD instance packet hash overflowed sealed capacity" );
					return false;
				}
				t3dRdPacketBuildSlot_t slot;
				slot.m_packet.m_nSurfaceSlot = handle.m_nSlot;
				slot.m_packet.m_nDrawSlot = handle.m_nSlot;
				slot.m_packet.m_nMaterial = pSurface->m_nMaterial;
				slot.m_packet.m_nOverlayMaterial = pSurface->m_nOverlayMaterial;
				slot.m_packet.m_nFlags = draw_flags;
				slot.m_packet.m_fSortDistanceSq = ( float )( cell.m_bounds.get_center().distance_squared_to( rCamera ) );
				slot.m_packet.m_cullBounds = cell.m_bounds;
				slot.m_packet.m_nVisibilityLayers = cell.m_nVisibilityLayers;
				slot.m_packet.m_fLodBegin = cell.m_fLodBegin;
				slot.m_packet.m_fLodEnd = cell.m_fLodEnd;
				slot.m_packet.m_fFadeMargin = cell.m_fFadeMargin;
				if ( !t3dVectorPushBackWithinCapacity( m_commandTable.m_packetBuildSlots, slot ) ) {
					setError( String( "RD instance packet-build slot vector rejected a sealed-capacity append; size=" )
					        + String::num_int64( m_commandTable.m_packetBuildSlots.size() ) + String( ", capacity=" )
					        + String::num_int64( m_commandTable.m_packetBuildSlots.capacity() ) );
					return false;
				}
			}
			if ( m_commandTable.m_packetBuildEntries.size() >= m_commandTable.m_nPacketCapacity ) {
				setError( "RD instance packet entry count exceeded publication-time capacity" );
				return false;
			}
			t3dRdPacketBuildSlot_t &slot = m_commandTable.m_packetBuildSlots[packet_index];
			if ( UINT32_MAX - slot.m_nVisibleCount < cell.m_nInstanceCount ) {
				setError( "RD instance packet visible count overflow" );
				return false;
			}
			slot.m_nVisibleCount += cell.m_nInstanceCount;
			if ( !t3dVectorPushBackWithinCapacity( m_commandTable.m_packetBuildEntries,
			             t3dRdPacketBuildEntry_t{ packet_index, cell.m_nFirstInstance, cell.m_nInstanceCount, ( uint32_t )iCell } ) ) {
				setError( String( "RD instance packet-build entry vector rejected a sealed-capacity append; size=" )
				        + String::num_int64( m_commandTable.m_packetBuildEntries.size() ) + String( ", capacity=" )
				        + String::num_int64( m_commandTable.m_packetBuildEntries.capacity() ) );
				return false;
			}
		}
	}

	return true;
}

template <bool t_bShadowPass>
bool Terrain3DRdInstanceRenderer::buildVisibleOutputForPass( const bool bSkipFrustum, bool &rDeferVisibilityOutput ) {
	uint64_t total_visible64 = 0u;
	for ( size_t iSlot = 0u; iSlot < ( size_t )( ( m_commandTable.m_packetBuildSlots ).size() ); iSlot++ ) {
		t3dRdPacketBuildSlot_t &slot = ( m_commandTable.m_packetBuildSlots )[iSlot];
		slot.m_packet.m_nFirstVisible = ( uint32_t )( total_visible64 );
		slot.m_packet.m_nVisibleCount = slot.m_nVisibleCount;
		slot.m_nWriteOffset = slot.m_packet.m_nFirstVisible;
		total_visible64 += slot.m_nVisibleCount;
	}
	if ( total_visible64 > m_visibility.m_nVisibleIndexCapacity ) {
		setError( "RD instance visible-index output exceeded publication-time capacity" );
		return false;
	}
	eastl::vector<uint32_t, Terrain3DEastlAllocator> &visible_indices =
	        t_bShadowPass ? m_visibility.m_shadowVisibleIndices : m_visibility.m_visibleIndices;
	if ( total_visible64 > visible_indices.size() ) {
		setError( "RD instance visible-index backing exceeded publication-time capacity" );
		return false;
	}
	if constexpr ( t_bShadowPass ) {
		m_visibility.m_nShadowVisibleIndexCount = ( uint32_t )( total_visible64 );
	} else {
		m_visibility.m_nVisibleIndexCount = ( uint32_t )( total_visible64 );
	}
	// bSkipFrustum means packet topology is being built for GPU visibility.
	// The compute culler owns visible-index and indirect instance-count output in
	// that mode regardless of whether geometry comes from per-surface resources
	// or a future shared arena. The minimal reference lane remains CPU-owned.
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	const bool defer_visibility_output = false;
#else
	const bool defer_visibility_output = bSkipFrustum;
#endif
	if ( !defer_visibility_output ) {
		for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_commandTable.m_packetBuildEntries ).size() ); iEntry++ ) {
			const t3dRdPacketBuildEntry_t &entry = ( m_commandTable.m_packetBuildEntries )[iEntry];
			t3dRdPacketBuildSlot_t &slot = m_commandTable.m_packetBuildSlots[entry.m_nPacketIndex];
			uint32_t write = slot.m_nWriteOffset;
			for ( uint32_t instance = 0u; instance < entry.m_nInstanceCount; ++instance ) {
				visible_indices[write++] = entry.m_nFirstInstance + instance;
			}
			slot.m_nWriteOffset = write;
		}
	}

	if ( bSkipFrustum ) {
		uint32_t command_index = 0u;
		for ( size_t iSlot = 0u; iSlot < m_commandTable.m_packetBuildSlots.size(); ++iSlot ) {
			t3dRdPacketBuildSlot_t &slot = m_commandTable.m_packetBuildSlots[iSlot];
			if ( t3dRdInstanceOneSubmitCompatible( slot.m_packet ) ) {
				slot.m_nCommandIndex = command_index++;
			}
		}
		if constexpr ( t_bShadowPass ) {
			m_commandTable.m_nShadowCompatibleCommandCount = command_index;
		} else {
			m_commandTable.m_nMainCompatibleCommandCount = command_index;
		}
		for ( size_t iSlot = 0u; iSlot < m_commandTable.m_packetBuildSlots.size(); ++iSlot ) {
			t3dRdPacketBuildSlot_t &slot = m_commandTable.m_packetBuildSlots[iSlot];
			if ( !t3dRdInstanceOneSubmitCompatible( slot.m_packet ) ) {
				slot.m_nCommandIndex = command_index++;
			}
		}
		if constexpr ( t_bShadowPass ) {
			if ( !t3dVectorResizeWithinCapacity( m_commandTable.m_shadowPackets, m_commandTable.m_packetBuildSlots.size() ) ) {
				setError( "RD fixed shadow command table exceeded sealed capacity" );
				return false;
			}
		} else if ( !t3dVectorResizeWithinCapacity( m_commandTable.m_drawPackets, m_commandTable.m_packetBuildSlots.size() ) ) {
			setError( "RD fixed main command table exceeded sealed capacity" );
			return false;
		}
	}

	rDeferVisibilityOutput = defer_visibility_output;
	return true;
}

template <bool t_bShadowPass>
bool Terrain3DRdInstanceRenderer::finalizePacketCommandsForPass( const bool bSkipFrustum, const bool bDeferVisibilityOutput ) {
	const bool defer_visibility_output = bDeferVisibilityOutput;
	for ( size_t iSlot = 0u; iSlot < ( size_t )( ( m_commandTable.m_packetBuildSlots ).size() ); iSlot++ ) {
		t3dRdPacketBuildSlot_t &slot = ( m_commandTable.m_packetBuildSlots )[iSlot];
		const t3dRdMeshSourceSurface_t *const pSurface = m_geometryArena.m_meshCache.resolveSurface(
		        t3dRdSurfaceHandle_t{ slot.m_packet.m_nSurfaceSlot, m_geometryArena.m_meshCache.getGeneration() } );
		if ( !pSurface ) {
			setError( String( "RD instance packet finalization lost a mesh surface handle; surface_slot=" )
			        + String::num_int64( slot.m_packet.m_nSurfaceSlot ) + String( ", mesh_generation=" )
			        + String::num_int64( m_geometryArena.m_meshCache.getGeneration() ) );
			return false;
		}
		slot.m_packet.m_command.m_nIndexCount = pSurface->m_nIndexCount;
		slot.m_packet.m_command.m_nInstanceCount = defer_visibility_output ? 0u : slot.m_packet.m_nVisibleCount;
#if T3D_INSTANCER_SHARED_MESH_ARENA
		// P11 shared arenas retain local surface indices and address the packed
		// vertex/index arenas through indexed-indirect base offsets.
		slot.m_packet.m_command.m_nFirstIndex = pSurface->m_nIndexOffset;
		slot.m_packet.m_command.m_nVertexOffset = ( int32_t )( pSurface->m_nVertexOffset );
#else
		slot.m_packet.m_command.m_nFirstIndex = 0u;
		slot.m_packet.m_command.m_nVertexOffset = 0;
#endif
		slot.m_packet.m_command.m_nFirstInstance = slot.m_packet.m_nFirstVisible;
		if constexpr ( t_bShadowPass ) {
			if ( bSkipFrustum ) {
				m_commandTable.m_shadowPackets[slot.m_nCommandIndex] = slot.m_packet;
			} else {
				slot.m_nCommandIndex = ( uint32_t )m_commandTable.m_shadowPackets.size();
				if ( !t3dVectorPushBackWithinCapacity( m_commandTable.m_shadowPackets, slot.m_packet ) ) {
					setError( "RD shadow packet output exceeded sealed capacity" );
					return false;
				}
			}
		} else {
			const bool alpha = ( slot.m_packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u;
			if ( bSkipFrustum ) {
				m_commandTable.m_drawPackets[slot.m_nCommandIndex] = slot.m_packet;
			} else if ( alpha ) {
				if ( !t3dVectorPushBackWithinCapacity( m_commandTable.m_alphaPackets, slot.m_packet ) ) {
					setError( "RD instance alpha-packet output exceeded sealed capacity" );
					return false;
				}
			} else {
				if ( !t3dVectorPushBackWithinCapacity( m_commandTable.m_drawPackets, slot.m_packet ) ) {
					setError( "RD instance draw-packet output exceeded sealed capacity" );
					return false;
				}
			}
			if ( !alpha || ( slot.m_packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) != 0u ) {
				if ( !t3dVectorPushBackWithinCapacity( m_commandTable.m_depthPackets, slot.m_packet ) ) {
					setError( "RD instance depth-packet output exceeded sealed capacity" );
					return false;
				}
			}
		}
	}

	return true;
}

template <bool t_bShadowPass>
bool Terrain3DRdInstanceRenderer::buildCullDescriptorsForPass(
        const t3dRdInstanceSceneSnapshot_t &rScene, const uint32_t nViewCount, const bool bSkipFrustum ) {
	if constexpr ( !t_bShadowPass ) {
		if ( !bSkipFrustum ) {
			t3dRdStableSortAlphaPackets( m_commandTable.m_alphaPackets, m_commandTable.m_alphaSortScratch );
			if ( m_commandTable.m_drawPackets.size() + m_commandTable.m_alphaPackets.size() > m_commandTable.m_nPacketCapacity ) {
				setError( "RD instance draw packet output exceeded publication-time capacity" );
				return false;
			}
			if ( !t3dVectorAppendWithinCapacity(
			             m_commandTable.m_drawPackets, m_commandTable.m_alphaPackets.data(), m_commandTable.m_alphaPackets.size() ) ) {
				setError( "RD instance draw-packet output could not append sorted alpha packets within sealed capacity" );
				return false;
			}
		}
		if ( bSkipFrustum ) {
			for ( size_t iEntry = 0u; iEntry < m_commandTable.m_packetBuildEntries.size(); ++iEntry ) {
				const t3dRdPacketBuildEntry_t &entry = m_commandTable.m_packetBuildEntries[iEntry];
				const t3dRdPacketBuildSlot_t &slot = m_commandTable.m_packetBuildSlots[entry.m_nPacketIndex];
				const t3dRdInstanceCell_t &cell = rScene.m_cells[entry.m_nCellIndex];
				t3dRdInstanceCullDescriptor_t descriptor;
				t3dRdFillInstanceCullDescriptorCellState( descriptor, cell, false );
				descriptor.m_nCommandIndex = slot.m_nCommandIndex;
				descriptor.m_nFirstInstance = entry.m_nFirstInstance;
				descriptor.m_nInstanceCount = entry.m_nInstanceCount;
				descriptor.m_nFirstVisible = slot.m_packet.m_nFirstVisible;
				t3dRdInstanceDescriptorSpan_t &span = m_visibility.m_instanceCullCellSpans[entry.m_nCellIndex];
				if ( span.m_nCount == 0u ) {
					span.m_nFirst = ( uint32_t )m_visibility.m_instanceCullDescriptors.size();
				}
				++span.m_nCount;
				if ( !t3dVectorPushBackWithinCapacity( m_visibility.m_instanceCullDescriptors, descriptor ) ) {
					setError( "RD instance cull descriptor output exceeded sealed capacity" );
					return false;
				}
			}
		}
		for ( uint32_t command_index = 0u; command_index < ( uint32_t )( m_commandTable.m_drawPackets.size() ); ++command_index ) {
			const t3dRdInstanceDrawPacket_t &packet = m_commandTable.m_drawPackets[command_index];
			if ( nViewCount != 1u || ( packet.m_nFlags & T3D_RD_INSTANCE_HLOD ) == 0u ) {
				continue;
			}
			t3dRdHlodCullDescriptor_t descriptor;
			const Vector3 center = packet.m_cullBounds.get_center();
			const Vector3 extent = packet.m_cullBounds.size * 0.5f;
			descriptor.m_szBoundsCenter[0] = ( float )( center.x );
			descriptor.m_szBoundsCenter[1] = ( float )( center.y );
			descriptor.m_szBoundsCenter[2] = ( float )( center.z );
			descriptor.m_szBoundsExtent[0] = ( float )( extent.x );
			descriptor.m_szBoundsExtent[1] = ( float )( extent.y );
			descriptor.m_szBoundsExtent[2] = ( float )( extent.z );
			descriptor.m_nVisibilityLayers = packet.m_nVisibilityLayers;
			descriptor.m_nCommandIndex = command_index;
			descriptor.m_nFirstVisible = packet.m_nFirstVisible;
			if ( !t3dVectorPushBackWithinCapacity( m_visibility.m_hlodDescriptors, descriptor ) ) {
				setError( "RD instance HLOD descriptor output exceeded sealed capacity" );
				return false;
			}
		}
	}
	if constexpr ( t_bShadowPass ) {
		if ( bSkipFrustum ) {
			for ( size_t iEntry = 0u; iEntry < m_commandTable.m_packetBuildEntries.size(); ++iEntry ) {
				const t3dRdPacketBuildEntry_t &entry = m_commandTable.m_packetBuildEntries[iEntry];
				const t3dRdPacketBuildSlot_t &slot = m_commandTable.m_packetBuildSlots[entry.m_nPacketIndex];
				const t3dRdInstanceCell_t &cell = rScene.m_cells[entry.m_nCellIndex];
				t3dRdInstanceCullDescriptor_t descriptor;
				t3dRdFillInstanceCullDescriptorCellState( descriptor, cell, true );
				descriptor.m_nCommandIndex = slot.m_nCommandIndex;
				descriptor.m_nFirstInstance = entry.m_nFirstInstance;
				descriptor.m_nInstanceCount = entry.m_nInstanceCount;
				descriptor.m_nFirstVisible = slot.m_packet.m_nFirstVisible;
				t3dRdInstanceDescriptorSpan_t &span = m_visibility.m_shadowInstanceCullCellSpans[entry.m_nCellIndex];
				if ( span.m_nCount == 0u ) {
					span.m_nFirst = ( uint32_t )m_visibility.m_shadowInstanceCullDescriptors.size();
				}
				++span.m_nCount;
				if ( !t3dVectorPushBackWithinCapacity( m_visibility.m_shadowInstanceCullDescriptors, descriptor ) ) {
					setError( "RD shadow instance cull descriptor output exceeded sealed capacity" );
					return false;
				}
			}
		}
	}
	return true;
}

template <bool t_bShadowPass>
bool Terrain3DRdInstanceRenderer::buildPacketsForPass( const t3dRdInstanceSceneSnapshot_t &rScene,
        const Projection *const pViewProjections,
        const uint32_t nViewCount,
        const Vector3 &rCamera,
        const uint32_t nVisibilityMask,
        const bool bSkipFrustum ) {
	if ( bSkipFrustum ) {
		return buildGpuCommandTableForPass<t_bShadowPass>( rScene );
	}
	if ( !beginPacketBuildForPass<t_bShadowPass>( rScene, bSkipFrustum )
	        || !collectPacketEntriesForPass<t_bShadowPass>(
	                rScene, pViewProjections, nViewCount, rCamera, nVisibilityMask, bSkipFrustum ) ) {
		return false;
	}
	bool defer_visibility_output = false;
	if ( !buildVisibleOutputForPass<t_bShadowPass>( bSkipFrustum, defer_visibility_output )
	        || !finalizePacketCommandsForPass<t_bShadowPass>( bSkipFrustum, defer_visibility_output )
	        || !buildCullDescriptorsForPass<t_bShadowPass>( rScene, nViewCount, bSkipFrustum ) ) {
		return false;
	}
	m_visibility.m_nHlodCullDescriptors = m_visibility.m_hlodDescriptors.size();
	m_commandTable.m_nVisibleInstances = t_bShadowPass ? m_visibility.m_nShadowVisibleIndexCount : m_visibility.m_nVisibleIndexCount;
	return true;
}

bool Terrain3DRdInstanceRenderer::buildPackets( const t3dRdInstanceSceneSnapshot_t &rScene,
        const Projection *const pViewProjections,
        const uint32_t nViewCount,
        const Vector3 &rCamera,
        const uint32_t nVisibilityMask,
        const bool bShadowPass,
        const bool bSkipFrustum ) {
	if ( !pViewProjections || nViewCount == 0u || nViewCount > s_nRdForwardMaxViews ) {
		setError( String( "RD instance packet builder received invalid view state; projection_ptr=" )
		        + String::num_int64( pViewProjections ? 1 : 0 ) + String( ", views=" ) + String::num_int64( nViewCount ) );
		return false;
	}
	const bool bBuilt = bShadowPass
	        ? buildPacketsForPass<true>( rScene, pViewProjections, nViewCount, rCamera, nVisibilityMask, bSkipFrustum )
	        : buildPacketsForPass<false>( rScene, pViewProjections, nViewCount, rCamera, nVisibilityMask, bSkipFrustum );
	if ( bBuilt && !bSkipFrustum ) {
		m_commandTable.m_nCpuSurfacePacketsBuilt +=
		        bShadowPass ? ( uint64_t )( m_commandTable.m_shadowPackets.size() ) : ( uint64_t )( m_commandTable.m_drawPackets.size() );
	}
	return bBuilt;
}

bool Terrain3DRdInstanceRenderer::uploadVisibility( void ) {
	const uint64_t visible_bytes64 = ( uint64_t )( m_visibility.m_nVisibleIndexCount ) * sizeof( uint32_t );
#if !T3D_INSTANCER_RD_PORT && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	const uint64_t indirect_bytes64 = m_commandTable.m_drawPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	if ( visible_bytes64 > m_visibility.m_hVisibleIndexBuffer.capacityBytes() || indirect_bytes64 > m_commandTable.m_mainIndirectSlice.m_nCapacityBytes ) {
		setError( "RD instance visibility output exceeded publication-time capacity" );
		return false;
	}
#else
	if ( visible_bytes64 > m_visibility.m_hVisibleIndexBuffer.capacityBytes() ) {
		setError( "RD direct-surface visible-index output exceeded publication-time capacity" );
		return false;
	}
#endif
	if ( !ensureUniformSet() ) {
		return false;
	}
	const uint32_t visible_bytes = ( uint32_t )( visible_bytes64 );
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "RD instance visibility upload lost the RenderingDevice" );
		return false;
	}
	if ( visible_bytes > 0u ) {
		memcpy( m_uploadState.m_visibleBytes.ptrw(), m_visibility.m_visibleIndices.data(), visible_bytes );
		if ( !m_visibility.m_hVisibleIndexBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_visibleBytes, visible_bytes, T3D_GPU_SCRATCH ) ) {
			setError( String( "RD instance visible-index buffer_update failed; bytes=" ) + String::num_int64( visible_bytes ) );
			return false;
		}
	}
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	return true;
#else
	const uint32_t indirect_bytes = ( uint32_t )( indirect_bytes64 );
	if ( indirect_bytes > 0u ) {
		for ( uint32_t i = 0u; i < ( uint32_t )( m_commandTable.m_drawPackets.size() ); ++i ) {
			memcpy( m_uploadState.m_indirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
			        &m_commandTable.m_drawPackets[i].m_command,
			        sizeof( t3dRdIndexedIndirectCommand_t ) );
		}
		if ( !m_commandTable.m_hIndirectBuffer.writePackedSliceTracked( m_context, m_commandTable.m_mainIndirectSlice, 0u, m_uploadState.m_indirectBytes, indirect_bytes, T3D_GPU_INDIRECT ) ) {
			setError( String( "RD instance indirect-command buffer_update failed; bytes=" ) + String::num_int64( indirect_bytes ) );
			return false;
		}
	}
	return uploadHlodDescriptors();
#endif
}


bool Terrain3DRdInstanceRenderer::uploadShadowVisibility( void ) {
	const uint64_t visible_bytes64 = ( uint64_t )( m_visibility.m_nShadowVisibleIndexCount ) * sizeof( uint32_t );
#if !T3D_INSTANCER_RD_PORT && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	const uint64_t indirect_bytes64 = m_commandTable.m_shadowPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	if ( visible_bytes64 > m_visibility.m_hShadowVisibleIndexBuffer.capacityBytes()
	        || indirect_bytes64 > m_commandTable.m_shadowIndirectSlice.m_nCapacityBytes ) {
		setError( "RD shadow instance visibility output exceeded publication-time capacity" );
		return false;
	}
#else
	if ( visible_bytes64 > m_visibility.m_hShadowVisibleIndexBuffer.capacityBytes() ) {
		setError( "RD direct-surface shadow visible-index output exceeded publication-time capacity" );
		return false;
	}
#endif
	const uint32_t visible_bytes = ( uint32_t )( visible_bytes64 );
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "RD shadow instance visibility upload lost the RenderingDevice" );
		return false;
	}
	if ( visible_bytes > 0u ) {
		memcpy( m_uploadState.m_shadowVisibleBytes.ptrw(), m_visibility.m_shadowVisibleIndices.data(), visible_bytes );
		if ( !m_visibility.m_hShadowVisibleIndexBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_shadowVisibleBytes, visible_bytes, T3D_GPU_SCRATCH ) ) {
			setError( String( "RD shadow instance visible-index buffer_update failed; bytes=" ) + String::num_int64( visible_bytes ) );
			return false;
		}
	}
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	return true;
#else
	const uint32_t indirect_bytes = ( uint32_t )( indirect_bytes64 );
	if ( indirect_bytes > 0u ) {
		for ( uint32_t i = 0u; i < ( uint32_t )( m_commandTable.m_shadowPackets.size() ); ++i ) {
			memcpy( m_uploadState.m_shadowIndirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
			        &m_commandTable.m_shadowPackets[i].m_command,
			        sizeof( t3dRdIndexedIndirectCommand_t ) );
		}
		if ( !m_commandTable.m_hIndirectBuffer.writePackedSliceTracked( m_context, m_commandTable.m_shadowIndirectSlice, 0u, m_uploadState.m_shadowIndirectBytes, indirect_bytes, T3D_GPU_INDIRECT ) ) {
			setError( String( "RD shadow instance indirect-command buffer_update failed; bytes=" ) + String::num_int64( indirect_bytes ) );
			return false;
		}
	}
	return true;
#endif
}

bool Terrain3DRdInstanceRenderer::uploadHlodDescriptors( void ) {
	if ( m_visibility.m_hlodDescriptors.empty() ) {
		return true;
	}
	m_context.beginResourceRebuild();
	const bool hlod_shader_ready = ensureHlodCullShader( m_visibility.m_hlodCullSourceShader );
	m_context.endResourceRebuild();
	if ( !hlod_shader_ready ) {
		return false;
	}
	const uint64_t bytes64 = m_visibility.m_hlodDescriptors.size() * sizeof( t3dRdHlodCullDescriptor_t );
	if ( bytes64 > m_visibility.m_hHlodCullDescriptorBuffer.capacityBytes() || !m_visibility.m_hHlodCullDescriptorBuffer.is_valid()
	        || m_uploadState.m_hlodDescriptorBytes.size() < ( int64_t )( bytes64 ) ) {
		setError( "RD HLOD descriptor output exceeded publication-time capacity" );
		return false;
	}
	const uint32_t bytes = ( uint32_t )( bytes64 );
	memcpy( m_uploadState.m_hlodDescriptorBytes.ptrw(), m_visibility.m_hlodDescriptors.data(), bytes );
	if ( !m_visibility.m_hHlodCullDescriptorBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_hlodDescriptorBytes, bytes, T3D_GPU_PUBLICATION ) ) {
		return false;
	}
	if ( !m_visibility.m_hHlodCullUniformSet.is_valid() ) {
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_uploadState.m_hFrameBuffer ) );
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hHlodCullDescriptorBuffer ) );
		uniforms.push_back(
		        Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_commandTable.m_hIndirectBuffer ) );
		m_visibility.m_hHlodCullUniformSet = m_hlodCullDescriptors.getOrCreate( m_visibility.m_hHlodCullShader.rid(),
		        "T3D.HLOD.CullUniformSet", uniforms, m_visibility.m_hHlodCullShader.rid(), 0u );
		if ( !m_visibility.m_hHlodCullUniformSet.is_valid() ) {
			setError( "Failed to create Terrain3D HLOD cull uniform set" );
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::dispatchHlodCull( const uint32_t nVisibilityMask ) {
	if ( m_visibility.m_hlodDescriptors.empty() ) {
		return true;
	}
	if ( !m_visibility.m_hHlodCullPipeline.is_valid() || !m_visibility.m_hHlodCullUniformSet.is_valid() ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdHlodCullPush_t push;
	push.m_nDescriptorCount = ( uint32_t )( m_visibility.m_hlodDescriptors.size() );
	push.m_nVisibilityMask = nVisibilityMask;
	push.m_nCommandCount = ( uint32_t )( m_commandTable.m_drawPackets.size() );
	push.m_nMainCommandBase = m_commandTable.m_mainIndirectSlice.m_nOffsetBytes / sizeof( t3dRdIndexedIndirectCommand_t );
	memcpy( m_uploadState.m_hlodCullPushBytes.ptrw(), &push, sizeof( push ) );
	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	rd->compute_list_bind_compute_pipeline( list, m_visibility.m_hHlodCullPipeline );
	rd->compute_list_bind_uniform_set( list, m_visibility.m_hHlodCullUniformSet, 0u );
	rd->compute_list_set_push_constant( list, m_uploadState.m_hlodCullPushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, ( push.m_nDescriptorCount + 31u ) / 32u, 1u, 1u );
	rd->compute_list_add_barrier( list );
	rd->compute_list_end();
	++m_visibility.m_nHlodCullDispatches;
	return true;
}


bool Terrain3DRdInstanceRenderer::buildFixedVisibleLayout( const t3dRdInstanceSceneSnapshot_t &rScene ) {
	const uint32_t surface_count = ( uint32_t )( m_geometryArena.m_meshCache.getSurfaceCount() );
	if ( m_commandTable.m_fixedDrawVisibleBases.size() < surface_count || m_commandTable.m_fixedDrawVisibleCapacities.size() < surface_count
	        || m_commandTable.m_shadowFixedDrawVisibleBases.size() < surface_count
	        || m_commandTable.m_shadowFixedDrawVisibleCapacities.size() < surface_count ) {
		setError( "RD P32 fixed visible layout backing is smaller than the draw-metadata slot table" );
		return false;
	}
	if ( surface_count > 0u ) {
		memset( m_commandTable.m_fixedDrawVisibleCapacities.data(), 0, surface_count * sizeof( uint32_t ) );
		memset( m_commandTable.m_shadowFixedDrawVisibleCapacities.data(), 0, surface_count * sizeof( uint32_t ) );
	}

	for ( size_t iCell = 0u; iCell < rScene.m_cells.size(); ++iCell ) {
		const t3dRdInstanceCell_t &cell = rScene.m_cells[iCell];
		if ( cell.m_nInstanceCount == 0u || cell.m_nVisibilityLayers == 0u
		        || ( uint64_t )( cell.m_nFirstInstance ) + cell.m_nInstanceCount > rScene.m_instances.size() ) {
			continue;
		}
		const t3dRdMeshHandle_t mesh = m_geometryArena.m_meshCache.getMeshHandleBySourceKey(
		        cell.m_nMeshSourceKey != 0u ? cell.m_nMeshSourceKey : t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = m_geometryArena.m_meshCache.resolveMesh( mesh );
		if ( !pLod ) {
			continue;
		}
		if ( !t3dRdInstanceMeshIdentityMatchesCell( cell, pLod ) ) {
			setError( "RD instance mesh identity mismatch during fixed-layout build; refusing stale mesh-bank alias" );
			return false;
		}
		for ( uint32_t iSurface = 0u; iSurface < pLod->m_nSurfaceCount; ++iSurface ) {
			const t3dRdSurfaceHandle_t handle = m_geometryArena.m_meshCache.getSurfaceHandle( mesh, iSurface );
			const t3dRdMeshSourceSurface_t *const pSurface = m_geometryArena.m_meshCache.resolveSurface( handle );
			if ( !pSurface || pSurface->m_nIndexCount == 0u || handle.m_nSlot >= surface_count ) {
				continue;
			}
			t3dRdInstanceDrawPacket_t compatibility;
			compatibility.m_nFlags = cell.m_nFlags | pSurface->m_nFlags;
			if ( cell.m_fFadeMargin > 0.0f ) {
				compatibility.m_nFlags |= T3D_RD_INSTANCE_FADE_ACTIVE;
			}
			if ( ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u && t3dRdInstanceOneSubmitCompatible( compatibility ) ) {
				uint32_t &capacity = m_commandTable.m_fixedDrawVisibleCapacities[handle.m_nSlot];
				if ( UINT32_MAX - capacity < cell.m_nInstanceCount ) {
					setError( "RD P32 main fixed visible span overflow" );
					return false;
				}
				capacity += cell.m_nInstanceCount;
			}

			compatibility.m_nFlags = cell.m_nFlags | pSurface->m_nFlags;
			const bool alpha_blend = ( compatibility.m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u;
			const bool shadow_eligible = ( cell.m_nFlags & T3D_RD_INSTANCE_CAST_SHADOWS ) != 0u
			        && ( !alpha_blend || ( compatibility.m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) != 0u );
			if ( shadow_eligible && t3dRdInstanceOneSubmitCompatible( compatibility ) ) {
				uint32_t &capacity = m_commandTable.m_shadowFixedDrawVisibleCapacities[handle.m_nSlot];
				if ( UINT32_MAX - capacity < cell.m_nInstanceCount ) {
					setError( "RD P32 shadow fixed visible span overflow" );
					return false;
				}
				capacity += cell.m_nInstanceCount;
			}
		}
	}

	uint64_t main_total = 0u;
	uint64_t shadow_total = 0u;
	for ( uint32_t iSurface = 0u; iSurface < surface_count; ++iSurface ) {
		if ( main_total > UINT32_MAX || shadow_total > UINT32_MAX ) {
			setError( "RD P32 fixed visible prefix exceeds 32-bit addressing" );
			return false;
		}
		m_commandTable.m_fixedDrawVisibleBases[iSurface] = ( uint32_t )( main_total );
		m_commandTable.m_shadowFixedDrawVisibleBases[iSurface] = ( uint32_t )( shadow_total );
		main_total += m_commandTable.m_fixedDrawVisibleCapacities[iSurface];
		shadow_total += m_commandTable.m_shadowFixedDrawVisibleCapacities[iSurface];
	}
	if ( main_total > m_visibility.m_nVisibleIndexCapacity || shadow_total > m_visibility.m_nVisibleIndexCapacity ) {
		setError( "RD P32 fixed visible layout exceeds publication-time visible capacity" );
		return false;
	}
	m_visibility.m_nFixedVisibleCapacity = ( uint32_t )( MAX( main_total, shadow_total ) );
	return true;
}

template <bool t_bShadowPass>
bool Terrain3DRdInstanceRenderer::buildGpuCommandTableForPass( const t3dRdInstanceSceneSnapshot_t &rScene ) {
	const uint32_t surface_count = ( uint32_t )( m_geometryArena.m_meshCache.getSurfaceCount() );
	eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &packets =
	        t_bShadowPass ? m_commandTable.m_shadowPackets : m_commandTable.m_drawPackets;
	eastl::vector<t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator> &descriptors =
	        t_bShadowPass ? m_visibility.m_shadowInstanceCullDescriptors : m_visibility.m_instanceCullDescriptors;
	eastl::vector<t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator> &spans =
	        t_bShadowPass ? m_visibility.m_shadowInstanceCullCellSpans : m_visibility.m_instanceCullCellSpans;
	const eastl::vector<uint32_t, Terrain3DEastlAllocator> &fixed_bases =
	        t_bShadowPass ? m_commandTable.m_shadowFixedDrawVisibleBases : m_commandTable.m_fixedDrawVisibleBases;
	const eastl::vector<uint32_t, Terrain3DEastlAllocator> &fixed_capacities =
	        t_bShadowPass ? m_commandTable.m_shadowFixedDrawVisibleCapacities : m_commandTable.m_fixedDrawVisibleCapacities;

	packets.clear();
	descriptors.clear();
	if ( rScene.m_cells.size() > spans.size() ) {
		setError( "RD P32 cull span backing is smaller than the scene cell table" );
		return false;
	}
	if ( !spans.empty() ) {
		memset( spans.data(), 0, spans.size() * sizeof( t3dRdInstanceDescriptorSpan_t ) );
	}
	if constexpr ( t_bShadowPass ) {
		m_commandTable.m_nShadowCompatibleCommandCount = surface_count;
		m_visibility.m_nShadowVisibleIndexCount = 0u;
	} else {
		m_commandTable.m_nMainCompatibleCommandCount = surface_count;
		m_visibility.m_nVisibleIndexCount = 0u;
		m_commandTable.m_nPacketBuildInputCells = rScene.m_cells.size();
		m_commandTable.m_nResolvedMeshCells = 0u;
		m_commandTable.m_nUnresolvedMeshCells = 0u;
		m_visibility.m_hlodDescriptors.clear();
		m_commandTable.m_depthPackets.clear();
		m_commandTable.m_alphaPackets.clear();
		m_commandTable.m_nTransparentPacketOrderCount = 0u;
	}

	if ( !t3dVectorResizeWithinCapacity( packets, surface_count ) ) {
		setError( "RD P32 immutable command prefix exceeded sealed packet capacity" );
		return false;
	}
	const t3dRdMeshSourceSurface_t *const pSurfaces = m_geometryArena.m_meshCache.getSurfaces();
	for ( uint32_t draw_slot = 0u; draw_slot < surface_count; ++draw_slot ) {
		const t3dRdMeshSourceSurface_t &surface = pSurfaces[draw_slot];
		t3dRdInstanceDrawPacket_t packet;
		packet.m_nSurfaceSlot = draw_slot;
		packet.m_nDrawSlot = draw_slot;
		packet.m_nFirstVisible = fixed_bases[draw_slot];
		packet.m_nVisibleCount = fixed_capacities[draw_slot];
		packet.m_nMaterial = surface.m_nMaterial;
		packet.m_nOverlayMaterial = surface.m_nOverlayMaterial;
		packet.m_nFlags = surface.m_nFlags;
		packet.m_nVisibilityLayers = UINT32_MAX;
		packet.m_command.m_nIndexCount = surface.m_nIndexCount;
		packet.m_command.m_nInstanceCount = 0u;
		packet.m_command.m_nFirstIndex = surface.m_nIndexOffset;
		packet.m_command.m_nVertexOffset = ( int32_t )( surface.m_nVertexOffset );
		packet.m_command.m_nFirstInstance = fixed_bases[draw_slot];
		packets[draw_slot] = packet;
	}

	uint64_t visible_cursor = 0u;
	if ( surface_count > 0u ) {
		const uint32_t last = surface_count - 1u;
		visible_cursor = ( uint64_t )( fixed_bases[last] ) + fixed_capacities[last];
	}

	for ( size_t iCell = 0u; iCell < rScene.m_cells.size(); ++iCell ) {
		const t3dRdInstanceCell_t &cell = rScene.m_cells[iCell];
		if ( cell.m_nInstanceCount == 0u || cell.m_nVisibilityLayers == 0u ) {
			continue;
		}
		if ( ( uint64_t )( cell.m_nFirstInstance ) + cell.m_nInstanceCount > rScene.m_instances.size() ) {
			setError( "RD P32 instance cell range exceeds published instance table" );
			return false;
		}
		if constexpr ( t_bShadowPass ) {
			if ( ( cell.m_nFlags & T3D_RD_INSTANCE_CAST_SHADOWS ) == 0u ) {
				continue;
			}
		} else if ( ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) != 0u ) {
			continue;
		}
		const t3dRdMeshHandle_t mesh = m_geometryArena.m_meshCache.getMeshHandleBySourceKey(
		        cell.m_nMeshSourceKey != 0u ? cell.m_nMeshSourceKey : t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = m_geometryArena.m_meshCache.resolveMesh( mesh );
		if ( !pLod ) {
			if constexpr ( !t_bShadowPass ) {
				++m_commandTable.m_nUnresolvedMeshCells;
			}
			continue;
		}
		if ( !t3dRdInstanceMeshIdentityMatchesCell( cell, pLod ) ) {
			setError( "RD instance mesh identity mismatch during one-submit build; refusing stale mesh-bank alias" );
			return false;
		}
		if constexpr ( !t_bShadowPass ) {
			++m_commandTable.m_nResolvedMeshCells;
		}

		for ( uint32_t iSurface = 0u; iSurface < pLod->m_nSurfaceCount; ++iSurface ) {
			const t3dRdSurfaceHandle_t handle = m_geometryArena.m_meshCache.getSurfaceHandle( mesh, iSurface );
			const t3dRdMeshSourceSurface_t *const pSurface = m_geometryArena.m_meshCache.resolveSurface( handle );
			if ( !pSurface || pSurface->m_nIndexCount == 0u || handle.m_nSlot >= surface_count ) {
				continue;
			}
			uint32_t draw_flags = cell.m_nFlags | pSurface->m_nFlags;
			if constexpr ( !t_bShadowPass ) {
				if ( cell.m_fFadeMargin > 0.0f ) {
					draw_flags |= T3D_RD_INSTANCE_FADE_ACTIVE | T3D_RD_INSTANCE_FADE_MASK;
				}
			}
			const bool alpha_blend = ( draw_flags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u;
			if constexpr ( t_bShadowPass ) {
				if ( alpha_blend && ( draw_flags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) == 0u ) {
					continue;
				}
			}

			t3dRdInstanceDrawPacket_t compatibility;
			compatibility.m_nFlags = draw_flags;
			const bool compatible = t3dRdInstanceOneSubmitCompatible( compatibility );
			uint32_t command_index = handle.m_nSlot;
			uint32_t first_visible = fixed_bases[handle.m_nSlot];
			if ( !compatible ) {
				if ( packets.size() >= m_commandTable.m_nPacketCapacity
				        || visible_cursor + cell.m_nInstanceCount > m_visibility.m_nVisibleIndexCapacity ) {
					setError( "RD P32 fallback command/visible range exceeded sealed capacity" );
					return false;
				}
				command_index = ( uint32_t )( packets.size() );
				first_visible = ( uint32_t )( visible_cursor );
				visible_cursor += cell.m_nInstanceCount;
				t3dRdInstanceDrawPacket_t packet;
				packet.m_nSurfaceSlot = handle.m_nSlot;
				packet.m_nDrawSlot = handle.m_nSlot;
				packet.m_nFirstVisible = first_visible;
				packet.m_nVisibleCount = cell.m_nInstanceCount;
				packet.m_nMaterial = pSurface->m_nMaterial;
				packet.m_nOverlayMaterial = pSurface->m_nOverlayMaterial;
				packet.m_nFlags = draw_flags;
				packet.m_cullBounds = cell.m_bounds;
				packet.m_nVisibilityLayers = cell.m_nVisibilityLayers;
				packet.m_fLodBegin = cell.m_fLodBegin;
				packet.m_fLodEnd = cell.m_fLodEnd;
				packet.m_fFadeMargin = cell.m_fFadeMargin;
				packet.m_command.m_nIndexCount = pSurface->m_nIndexCount;
				packet.m_command.m_nInstanceCount = 0u;
				packet.m_command.m_nFirstIndex = pSurface->m_nIndexOffset;
				packet.m_command.m_nVertexOffset = ( int32_t )( pSurface->m_nVertexOffset );
				packet.m_command.m_nFirstInstance = first_visible;
				if ( !t3dVectorPushBackWithinCapacity( packets, packet ) ) {
					setError( "RD P32 fallback packet append exceeded sealed capacity" );
					return false;
				}
				if constexpr ( !t_bShadowPass ) {
					if ( ( draw_flags & T3D_RD_INSTANCE_HLOD ) != 0u ) {
						t3dRdHlodCullDescriptor_t hlod_descriptor;
						const Vector3 center = cell.m_bounds.get_center();
						const Vector3 extent = cell.m_bounds.size * 0.5f;
						hlod_descriptor.m_szBoundsCenter[0] = ( float )( center.x );
						hlod_descriptor.m_szBoundsCenter[1] = ( float )( center.y );
						hlod_descriptor.m_szBoundsCenter[2] = ( float )( center.z );
						hlod_descriptor.m_szBoundsExtent[0] = ( float )( extent.x );
						hlod_descriptor.m_szBoundsExtent[1] = ( float )( extent.y );
						hlod_descriptor.m_szBoundsExtent[2] = ( float )( extent.z );
						hlod_descriptor.m_nVisibilityLayers = cell.m_nVisibilityLayers;
						hlod_descriptor.m_nCommandIndex = command_index;
						hlod_descriptor.m_nFirstVisible = first_visible;
						if ( !t3dVectorPushBackWithinCapacity( m_visibility.m_hlodDescriptors, hlod_descriptor ) ) {
							setError( "RD P32 HLOD compatibility descriptor exceeded sealed capacity" );
							return false;
						}
					}
				}
			}

			t3dRdInstanceCullDescriptor_t descriptor;
			t3dRdFillInstanceCullDescriptorCellState( descriptor, cell, t_bShadowPass );
			descriptor.m_nDrawSlot = handle.m_nSlot;
			descriptor.m_nCommandIndex = command_index;
			descriptor.m_nFirstInstance = cell.m_nFirstInstance;
			descriptor.m_nInstanceCount = cell.m_nInstanceCount;
			descriptor.m_nFirstVisible = first_visible;
			t3dRdInstanceDescriptorSpan_t &span = spans[iCell];
			if ( span.m_nCount == 0u ) {
				span.m_nFirst = ( uint32_t )( descriptors.size() );
			}
			++span.m_nCount;
			if ( !t3dVectorPushBackWithinCapacity( descriptors, descriptor ) ) {
				setError( "RD P32 cull descriptor output exceeded sealed capacity" );
				return false;
			}
		}
	}

	if ( visible_cursor > m_visibility.m_nVisibleIndexCapacity ) {
		setError( "RD P32 final visible cursor exceeded publication-time capacity" );
		return false;
	}
	if constexpr ( t_bShadowPass ) {
		m_visibility.m_nShadowVisibleIndexCount = ( uint32_t )( visible_cursor );
	} else {
		m_visibility.m_nVisibleIndexCount = ( uint32_t )( visible_cursor );
		m_visibility.m_nHlodCullDescriptors = m_visibility.m_hlodDescriptors.size();
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::buildFixedDrawTable( const t3dRdInstanceSceneSnapshot_t &rScene ) {
	if ( !buildFixedVisibleLayout( rScene ) || !buildGpuCommandTableForPass<false>( rScene )
	        || !buildGpuCommandTableForPass<true>( rScene ) ) {
		return false;
	}
	return uploadFixedDrawMetadata();
}

bool Terrain3DRdInstanceRenderer::uploadFixedDrawMetadata( void ) {
	const uint64_t mesh_generation = m_geometryArena.m_meshCache.getSourceGeneration();
	if ( mesh_generation != 0u && m_commandTable.m_nDrawMetadataMeshGeneration == mesh_generation
	        && m_commandTable.m_hDrawMetadataBuffer.is_valid() ) {
		return true;
	}
	const uint32_t draw_count = ( uint32_t )( m_geometryArena.m_meshCache.getSurfaceCount() );
	const uint64_t bytes64 = ( uint64_t )( draw_count ) * sizeof( t3dInstanceDrawMeta_t );
	if ( bytes64 > m_commandTable.m_hDrawMetadataBuffer.capacityBytes() || m_uploadState.m_drawMetadataBytes.size() < ( int64_t )( bytes64 ) ) {
		setError( "RD P32 fixed draw metadata exceeded publication-time capacity" );
		return false;
	}
	if ( draw_count == 0u ) {
		m_commandTable.m_nDrawMetadataMeshGeneration = mesh_generation;
		return true;
	}
	const t3dRdMeshSourceSurface_t *const pSurfaces = m_geometryArena.m_meshCache.getSurfaces();
	uint8_t *const pBytes = m_uploadState.m_drawMetadataBytes.ptrw();
	for ( uint32_t draw_slot = 0u; draw_slot < draw_count; ++draw_slot ) {
		const t3dRdMeshSourceSurface_t &surface = pSurfaces[draw_slot];
		t3dInstanceDrawMeta_t metadata;
		metadata.m_nMaterial = surface.m_nMaterial;
		metadata.m_nOverlayMaterial = surface.m_nOverlayMaterial;
		metadata.m_nFirstIndex = surface.m_nIndexOffset;
		metadata.m_nIndexCount = surface.m_nIndexCount;
		metadata.m_nVertexOffset = ( int32_t )( surface.m_nVertexOffset );
		metadata.m_nFlags = surface.m_nFlags;
		metadata.m_nVisibleBase = UINT32_MAX;
		metadata.m_nReserved = 0u;
		memcpy( pBytes + draw_slot * sizeof( metadata ), &metadata, sizeof( metadata ) );
	}
	const uint32_t bytes = ( uint32_t )( bytes64 );
	if ( !m_commandTable.m_hDrawMetadataBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_drawMetadataBytes, bytes, T3D_GPU_PUBLICATION ) ) {
		setError( "RD P32 fixed draw metadata buffer_update failed" );
		return false;
	}
	m_commandTable.m_nDrawMetadataMeshGeneration = mesh_generation;
	return true;
}

bool Terrain3DRdInstanceRenderer::buildGpuPacketTopology( const t3dRdInstanceSceneSnapshot_t &rScene ) {
	const uint64_t mesh_generation = m_geometryArena.m_meshCache.getSourceGeneration();
	if ( m_visibility.m_nGpuTopologySceneGeneration == rScene.m_nGeneration
	        && m_visibility.m_nGpuTopologyMeshGeneration == mesh_generation ) {
		return true;
	}
	if ( m_visibility.m_nGpuTopologySceneGeneration != 0u && m_visibility.m_nGpuTopologyMeshGeneration == mesh_generation
	        && !rScene.m_bTopologyChanged ) {
		if ( !refreshGpuCullDescriptorsDirty( rScene ) ) {
			return false;
		}
		m_visibility.m_nGpuTopologySceneGeneration = rScene.m_nGeneration;
		m_visibility.m_nGpuCullMainRenderFrame = UINT64_MAX;
		return true;
	}
	if ( !buildFixedDrawTable( rScene ) ) {
		return false;
	}
	const uint64_t main_indirect_bytes64 = m_commandTable.m_drawPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	const uint64_t shadow_indirect_bytes64 = m_commandTable.m_shadowPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	if ( main_indirect_bytes64 > m_commandTable.m_mainIndirectSlice.m_nCapacityBytes
	        || shadow_indirect_bytes64 > m_commandTable.m_shadowIndirectSlice.m_nCapacityBytes ) {
		setError( "RD GPU instance packet topology exceeded indirect capacity" );
		return false;
	}
	const uint32_t main_indirect_bytes = ( uint32_t )main_indirect_bytes64;
	const uint32_t shadow_indirect_bytes = ( uint32_t )shadow_indirect_bytes64;
	for ( uint32_t i = 0u; i < ( uint32_t )m_commandTable.m_drawPackets.size(); ++i ) {
		memcpy( m_uploadState.m_indirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
		        &m_commandTable.m_drawPackets[i].m_command,
		        sizeof( t3dRdIndexedIndirectCommand_t ) );
	}
	for ( uint32_t i = 0u; i < ( uint32_t )m_commandTable.m_shadowPackets.size(); ++i ) {
		memcpy( m_uploadState.m_shadowIndirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
		        &m_commandTable.m_shadowPackets[i].m_command,
		        sizeof( t3dRdIndexedIndirectCommand_t ) );
	}
	if ( main_indirect_bytes > 0u
	        && !m_commandTable.m_hIndirectBuffer.writePackedSliceTracked( m_context, m_commandTable.m_mainIndirectSlice, 0u, m_uploadState.m_indirectBytes, main_indirect_bytes, T3D_GPU_INDIRECT ) ) {
		return false;
	}
	if ( shadow_indirect_bytes > 0u
	        && !m_commandTable.m_hIndirectBuffer.writePackedSliceTracked( m_context, m_commandTable.m_shadowIndirectSlice, 0u, m_uploadState.m_shadowIndirectBytes, shadow_indirect_bytes, T3D_GPU_INDIRECT ) ) {
		return false;
	}
	if ( !rebuildHiZRegionTopology( rScene ) || !uploadGpuCullDescriptors() ) {
		return false;
	}
	m_visibility.m_nGpuTopologySceneGeneration = rScene.m_nGeneration;
	m_visibility.m_nGpuTopologyMeshGeneration = mesh_generation;
	m_visibility.m_nGpuCullMainRenderFrame = UINT64_MAX;
	m_visibility.m_nGpuCullMainViewportKey = 0u;
	m_visibility.m_nGpuCullMainViewKey = 0u;
	m_visibility.m_nGpuCullMainSceneGeneration = 0u;
	m_visibility.m_nGpuCullMainVisibilityMask = UINT32_MAX;
	m_visibility.m_nGpuCullMainViewCount = 0u;
	++m_visibility.m_nGpuTopologyBuilds;
	return true;
}

bool Terrain3DRdInstanceRenderer::updateGpuCullDescriptorSpanDirty( const t3dRdInstanceSceneSnapshot_t &rScene,
        const uint32_t nCellIndex,
        eastl::vector<t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator> &rDescriptors,
        const eastl::vector<t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator> &rSpans,
        const uint32_t nDescriptorBase,
        const bool bShadow,
        uint32_t &rUploadedBytes ) {
	if ( nCellIndex >= rScene.m_cells.size() ) {
		return false;
	}
	const t3dRdInstanceDescriptorSpan_t &span = rSpans[nCellIndex];
	if ( span.m_nCount == 0u ) {
		return true;
	}
	if ( ( uint64_t )span.m_nFirst + span.m_nCount > rDescriptors.size() ) {
		return false;
	}
	const t3dRdInstanceCell_t &cell = rScene.m_cells[nCellIndex];
	for ( uint32_t i = 0u; i < span.m_nCount; ++i ) {
		t3dRdInstanceCullDescriptor_t &descriptor = rDescriptors[span.m_nFirst + i];
		t3dRdFillInstanceCullDescriptorCellState( descriptor, cell, bShadow );
	}
	const uint32_t bytes = span.m_nCount * sizeof( t3dRdInstanceCullDescriptor_t );
	memcpy( m_uploadState.m_instanceCullDescriptorBytes.ptrw(), rDescriptors.data() + span.m_nFirst, bytes );
	const uint32_t byte_offset = ( nDescriptorBase + span.m_nFirst ) * sizeof( t3dRdInstanceCullDescriptor_t );
	if ( !m_visibility.m_hInstanceCullDescriptorBuffer.writePackedTracked( m_context, byte_offset, m_uploadState.m_instanceCullDescriptorBytes, bytes, T3D_GPU_DIRTY_UPLOAD ) ) {
		return false;
	}
	rUploadedBytes += bytes;
	return true;
}


bool Terrain3DRdInstanceRenderer::rebuildHiZRegionTopology( const t3dRdInstanceSceneSnapshot_t &rScene ) {
	m_visibility.m_instanceHiZRegionDescriptors.clear();
	m_visibility.m_instanceHiZRegionKeys.clear();
	for ( size_t iDescriptor = 0u; iDescriptor < m_visibility.m_instanceCullDescriptors.size(); ++iDescriptor ) {
		m_visibility.m_instanceCullDescriptors[iDescriptor].m_nHiZRegionIndex = UINT32_MAX;
	}
	if ( rScene.m_cells.size() > m_visibility.m_instanceCullCellSpans.size() ) {
		return false;
	}
	for ( size_t iCell = 0u; iCell < rScene.m_cells.size(); ++iCell ) {
		const t3dRdInstanceCell_t &cell = rScene.m_cells[iCell];
		if ( ( cell.m_nFlags & T3D_RD_INSTANCE_HIZ_CELL ) == 0u ) {
			continue;
		}
		const t3dRdInstanceDescriptorSpan_t &span = m_visibility.m_instanceCullCellSpans[iCell];
		if ( span.m_nCount == 0u || ( uint64_t )span.m_nFirst + span.m_nCount > m_visibility.m_instanceCullDescriptors.size() ) {
			continue;
		}
		const uint64_t key = ( ( uint64_t )( ( uint32_t )( cell.m_nRegionX ) ) << 32u ) | ( uint32_t )( cell.m_nRegionZ );
		uint32_t region_index = UINT32_MAX;
		for ( size_t iRegion = 0u; iRegion < m_visibility.m_instanceHiZRegionKeys.size(); ++iRegion ) {
			if ( m_visibility.m_instanceHiZRegionKeys[iRegion] == key ) {
				region_index = ( uint32_t )( iRegion );
				break;
			}
		}
		if ( region_index == UINT32_MAX ) {
			if ( m_visibility.m_instanceHiZRegionDescriptors.size() >= s_nRegionSlots
			        || m_visibility.m_instanceHiZRegionKeys.size() >= s_nRegionSlots ) {
				setError( "RD instance Hi-Z region topology exceeded bounded region capacity" );
				return false;
			}
			const Vector3 center = cell.m_bounds.get_center();
			const Vector3 extent = cell.m_bounds.size * 0.5f;
			t3dRdInstanceHiZRegionDescriptor_t region;
			region.m_szBoundsCenter[0] = ( float )center.x;
			region.m_szBoundsCenter[1] = ( float )center.y;
			region.m_szBoundsCenter[2] = ( float )center.z;
			region.m_szBoundsExtent[0] = ( float )extent.x;
			region.m_szBoundsExtent[1] = ( float )extent.y;
			region.m_szBoundsExtent[2] = ( float )extent.z;
			if ( !t3dVectorPushBackWithinCapacity( m_visibility.m_instanceHiZRegionKeys, key )
			        || !t3dVectorPushBackWithinCapacity( m_visibility.m_instanceHiZRegionDescriptors, region ) ) {
				setError( "RD instance Hi-Z region topology exceeded sealed CPU capacity" );
				return false;
			}
			region_index = ( uint32_t )( m_visibility.m_instanceHiZRegionDescriptors.size() - 1u );
		} else {
			t3dRdInstanceHiZRegionDescriptor_t &region = m_visibility.m_instanceHiZRegionDescriptors[region_index];
			const Vector3 previous_center( region.m_szBoundsCenter[0], region.m_szBoundsCenter[1], region.m_szBoundsCenter[2] );
			const Vector3 previous_extent( region.m_szBoundsExtent[0], region.m_szBoundsExtent[1], region.m_szBoundsExtent[2] );
			const AABB merged = AABB( previous_center - previous_extent, previous_extent * 2.0f ).merge( cell.m_bounds );
			const Vector3 center = merged.get_center();
			const Vector3 extent = merged.size * 0.5f;
			region.m_szBoundsCenter[0] = ( float )center.x;
			region.m_szBoundsCenter[1] = ( float )center.y;
			region.m_szBoundsCenter[2] = ( float )center.z;
			region.m_szBoundsExtent[0] = ( float )extent.x;
			region.m_szBoundsExtent[1] = ( float )extent.y;
			region.m_szBoundsExtent[2] = ( float )extent.z;
		}
		for ( uint32_t i = 0u; i < span.m_nCount; ++i ) {
			m_visibility.m_instanceCullDescriptors[span.m_nFirst + i].m_nHiZRegionIndex = region_index;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadHiZRegionDescriptors( void ) {
	const uint64_t bytes64 = m_visibility.m_instanceHiZRegionDescriptors.size() * sizeof( t3dRdInstanceHiZRegionDescriptor_t );
	if ( bytes64 > m_visibility.m_hInstanceHiZRegionDescriptorBuffer.capacityBytes() ) {
		setError( "RD instance Hi-Z region descriptors exceeded fixed GPU capacity" );
		return false;
	}
	const uint32_t bytes = ( uint32_t )( bytes64 );
	if ( bytes == 0u ) {
		return true;
	}
	memcpy( m_uploadState.m_instanceHiZRegionDescriptorBytes.ptrw(), m_visibility.m_instanceHiZRegionDescriptors.data(), bytes );
	if ( !m_visibility.m_hInstanceHiZRegionDescriptorBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_instanceHiZRegionDescriptorBytes, bytes, T3D_GPU_PUBLICATION ) ) {
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::refreshGpuPacketDerivedBounds( void ) {
	if ( m_commandTable.m_drawPackets.empty() ) {
		return true;
	}
	if ( m_commandTable.m_gpuPacketBoundValid.size() < m_commandTable.m_drawPackets.size() ) {
		return false;
	}
	memset( m_commandTable.m_gpuPacketBoundValid.data(), 0, m_commandTable.m_drawPackets.size() * sizeof( uint8_t ) );
	for ( size_t iDescriptor = 0u; iDescriptor < m_visibility.m_instanceCullDescriptors.size(); ++iDescriptor ) {
		const t3dRdInstanceCullDescriptor_t &descriptor = m_visibility.m_instanceCullDescriptors[iDescriptor];
		if ( descriptor.m_nCommandIndex >= m_commandTable.m_drawPackets.size() ) {
			return false;
		}
		const Vector3 center( descriptor.m_szBoundsCenter[0], descriptor.m_szBoundsCenter[1], descriptor.m_szBoundsCenter[2] );
		const Vector3 extent( descriptor.m_szBoundsExtent[0], descriptor.m_szBoundsExtent[1], descriptor.m_szBoundsExtent[2] );
		const AABB bounds( center - extent, extent * 2.0f );
		t3dRdInstanceDrawPacket_t &packet = m_commandTable.m_drawPackets[descriptor.m_nCommandIndex];
		uint8_t &valid = m_commandTable.m_gpuPacketBoundValid[descriptor.m_nCommandIndex];
		if ( valid == 0u ) {
			packet.m_cullBounds = bounds;
			valid = 1u;
		} else {
			packet.m_cullBounds = packet.m_cullBounds.merge( bounds );
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::refreshGpuCullDescriptorsDirty( const t3dRdInstanceSceneSnapshot_t &rScene ) {
	if ( rScene.m_dirtyCells.empty() ) {
		return true;
	}
	if ( rScene.m_cells.size() > m_visibility.m_instanceCullCellSpans.size()
	        || rScene.m_cells.size() > m_visibility.m_shadowInstanceCullCellSpans.size() ) {
		return false;
	}
	const uint32_t main_descriptor_count = ( uint32_t )m_visibility.m_instanceCullDescriptors.size();
	uint32_t uploaded_bytes = 0u;
	for ( size_t i = 0u; i < rScene.m_dirtyCells.size(); ++i ) {
		const uint32_t cell_index = rScene.m_dirtyCells[i];
		if ( !updateGpuCullDescriptorSpanDirty( rScene,
		             cell_index,
		             m_visibility.m_instanceCullDescriptors,
		             m_visibility.m_instanceCullCellSpans,
		             0u,
		             false,
		             uploaded_bytes )
		        || !updateGpuCullDescriptorSpanDirty( rScene,
		                cell_index,
		                m_visibility.m_shadowInstanceCullDescriptors,
		                m_visibility.m_shadowInstanceCullCellSpans,
		                main_descriptor_count,
		                true,
		                uploaded_bytes ) ) {
			setError( "RD instance dirty cull-descriptor update failed" );
			return false;
		}
	}
	if ( !refreshGpuPacketDerivedBounds() ) {
		setError( "RD instance dirty packet-bound refresh failed" );
		return false;
	}
	if ( !rebuildHiZRegionTopology( rScene ) || !uploadHiZRegionDescriptors() ) {
		setError( "RD instance dirty Hi-Z region refresh failed" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadGpuCullDescriptors( void ) {
	m_visibility.m_nHiZCellDescriptors = 0u;
	m_visibility.m_nHiZInstanceDescriptors = 0u;
	for ( size_t iDescriptor = 0u; iDescriptor < m_visibility.m_instanceCullDescriptors.size(); ++iDescriptor ) {
		const uint32_t flags = m_visibility.m_instanceCullDescriptors[iDescriptor].m_nFlags;
		m_visibility.m_nHiZCellDescriptors += ( flags & T3D_RD_INSTANCE_HIZ_CELL ) != 0u ? 1u : 0u;
		m_visibility.m_nHiZInstanceDescriptors += ( flags & T3D_RD_INSTANCE_HIZ_INSTANCE ) != 0u ? 1u : 0u;
	}
	const uint32_t main_count = ( uint32_t )m_visibility.m_instanceCullDescriptors.size();
	const uint32_t shadow_count = ( uint32_t )m_visibility.m_shadowInstanceCullDescriptors.size();
	const uint64_t total_bytes64 = ( uint64_t )( main_count + shadow_count ) * sizeof( t3dRdInstanceCullDescriptor_t );
	if ( total_bytes64 > m_visibility.m_hInstanceCullDescriptorBuffer.capacityBytes() ) {
		setError( "RD instance cull descriptors exceeded publication-time capacity" );
		return false;
	}
	const uint32_t total_bytes = ( uint32_t )total_bytes64;
	uint8_t *const bytes = m_uploadState.m_instanceCullDescriptorBytes.ptrw();
	const uint32_t main_bytes = main_count * sizeof( t3dRdInstanceCullDescriptor_t );
	if ( main_bytes > 0u ) {
		memcpy( bytes, m_visibility.m_instanceCullDescriptors.data(), main_bytes );
	}
	if ( shadow_count > 0u ) {
		memcpy( bytes + main_bytes,
		        m_visibility.m_shadowInstanceCullDescriptors.data(),
		        shadow_count * sizeof( t3dRdInstanceCullDescriptor_t ) );
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( total_bytes > 0u
	        && !m_visibility.m_hInstanceCullDescriptorBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_instanceCullDescriptorBytes, total_bytes, T3D_GPU_PUBLICATION ) ) {
		return false;
	}
	if ( !uploadHiZRegionDescriptors() ) {
		return false;
	}
	if ( m_visibility.m_hInstanceCullUniformSet.is_valid() ) {
		return true;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_uploadState.m_hFrameBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hInstanceCullDescriptorBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hVisibleIndexBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_commandTable.m_hIndirectBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hShadowVisibleIndexBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 5, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_commandTable.m_hIndirectBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single( 6, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_uploadState.m_hInstanceBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::samplerTexture( 7, m_visibility.m_instanceHiZ.getSampler(), m_visibility.m_instanceHiZ.getTexture() ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single(
	        8, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hInstanceHiZRegionDescriptorBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single(
	        9, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hInstanceHiZRegionVisibilityBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hVisibleDrawSlotBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 11, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hShadowVisibleDrawSlotBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 12, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hVisibleLayerBuffer ) );
	uniforms.push_back(
	        Terrain3DRdBindingBuilder::single( 13, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_visibility.m_hShadowVisibleLayerBuffer ) );
	m_visibility.m_hInstanceCullUniformSet = m_instanceCullDescriptors.getOrCreate( m_visibility.m_hInstanceCullShader.rid(),
	        "T3D.Instance.CullUniformSet", uniforms, m_visibility.m_hInstanceCullShader.rid(), 0u );
	if ( !m_visibility.m_hInstanceCullUniformSet.is_valid() ) {
		setError( "Failed to create Terrain3D instance cull uniform set" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::dispatchInstanceCull(
        const uint32_t nVisibilityMask, const uint32_t nViewCount, const bool bShadowPass, const bool bAllowHiZ ) {
	const uint32_t descriptor_count = bShadowPass ? ( uint32_t )m_visibility.m_shadowInstanceCullDescriptors.size()
	                                              : ( uint32_t )m_visibility.m_instanceCullDescriptors.size();
	const uint32_t command_count =
	        bShadowPass ? ( uint32_t )m_commandTable.m_shadowPackets.size() : ( uint32_t )m_commandTable.m_drawPackets.size();
	if ( command_count == 0u ) {
		return true;
	}
	if ( !m_visibility.m_hInstanceCullPipeline.is_valid() || !m_visibility.m_hInstanceCullUniformSet.is_valid() ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdInstanceCullPush_t push;
	push.m_nDescriptorBase = bShadowPass ? ( uint32_t )m_visibility.m_instanceCullDescriptors.size() : 0u;
	push.m_nDescriptorCount = descriptor_count;
	push.m_nCommandCount = command_count;
	push.m_nVisibilityMask = nVisibilityMask;
	push.m_nViewCount = CLAMP( nViewCount, 1u, s_nRdForwardMaxViews );
	push.m_nShadowPass = bShadowPass ? 1u : 0u;
	push.m_nMainCommandBase = m_commandTable.m_mainIndirectSlice.m_nOffsetBytes / sizeof( t3dRdIndexedIndirectCommand_t );
	push.m_nShadowCommandBase = m_commandTable.m_shadowIndirectSlice.m_nOffsetBytes / sizeof( t3dRdIndexedIndirectCommand_t );
#if T3D_INSTANCER_HIZ_OCCLUSION
	push.m_nHiZEnabled = bAllowHiZ && !bShadowPass && push.m_nViewCount == 1u && m_visibility.m_bHiZReadyForFrame ? 1u : 0u;
	push.m_nHiZWidth = push.m_nHiZEnabled ? m_visibility.m_instanceHiZ.getWidth() : 0u;
	push.m_nHiZHeight = push.m_nHiZEnabled ? m_visibility.m_instanceHiZ.getHeight() : 0u;
	push.m_nHiZMipCount = push.m_nHiZEnabled ? m_visibility.m_instanceHiZ.getMipCount() : 0u;
	push.m_nHiZRegionCount = push.m_nHiZEnabled ? ( uint32_t )m_visibility.m_instanceHiZRegionDescriptors.size() : 0u;
#endif
	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	rd->compute_list_bind_compute_pipeline( list, m_visibility.m_hInstanceCullPipeline );
	rd->compute_list_bind_uniform_set( list, m_visibility.m_hInstanceCullUniformSet, 0u );
	push.m_nMode = 0u;
	memcpy( m_uploadState.m_instanceCullPushBytes.ptrw(), &push, sizeof( push ) );
	rd->compute_list_set_push_constant( list, m_uploadState.m_instanceCullPushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, ( command_count + 63u ) / 64u, 1u, 1u );
	rd->compute_list_add_barrier( list );
	uint32_t dispatch_count = 1u;
	if ( push.m_nHiZEnabled != 0u && push.m_nHiZRegionCount > 0u ) {
		push.m_nMode = 2u;
		memcpy( m_uploadState.m_instanceCullPushBytes.ptrw(), &push, sizeof( push ) );
		rd->compute_list_set_push_constant( list, m_uploadState.m_instanceCullPushBytes, sizeof( push ) );
		rd->compute_list_dispatch( list, ( push.m_nHiZRegionCount + 63u ) / 64u, 1u, 1u );
		rd->compute_list_add_barrier( list );
		++dispatch_count;
	}
	if ( descriptor_count > 0u ) {
		push.m_nMode = 1u;
		memcpy( m_uploadState.m_instanceCullPushBytes.ptrw(), &push, sizeof( push ) );
		rd->compute_list_set_push_constant( list, m_uploadState.m_instanceCullPushBytes, sizeof( push ) );
		rd->compute_list_dispatch( list, descriptor_count, 1u, 1u );
		rd->compute_list_add_barrier( list );
		++dispatch_count;
	}
	rd->compute_list_end();
	m_visibility.m_nInstanceCullDispatches += dispatch_count;
	return true;
}

void Terrain3DRdInstanceRenderer::invalidateGpuCullTopologyAfterReferenceBuild( void ) {
	m_visibility.m_nGpuTopologySceneGeneration = 0u;
	m_visibility.m_nGpuTopologyMeshGeneration = 0u;
	m_visibility.m_nGpuCullMainRenderFrame = UINT64_MAX;
	m_visibility.m_nGpuCullMainViewportKey = 0u;
	m_visibility.m_nGpuCullMainViewKey = 0u;
	m_visibility.m_nGpuCullMainSceneGeneration = 0u;
	m_visibility.m_nGpuCullMainVisibilityMask = UINT32_MAX;
	m_visibility.m_nGpuCullMainViewCount = 0u;
}

bool Terrain3DRdInstanceRenderer::buildGpuTransparentOrder( const Vector3 &rCamera ) {
	m_commandTable.m_nTransparentPacketOrderCount = 0u;
	if ( m_commandTable.m_drawPackets.empty() ) {
		return true;
	}
	if ( m_commandTable.m_transparentPacketOrder.size() < m_commandTable.m_drawPackets.size() ) {
		setError( String( "RD GPU transparent packet-order backing is smaller than packet topology; packets=" )
		        + String::num_int64( m_commandTable.m_drawPackets.size() ) + String( ", backing=" )
		        + String::num_int64( m_commandTable.m_transparentPacketOrder.size() ) );
		return false;
	}
	for ( uint32_t iPacket = m_commandTable.m_nMainCompatibleCommandCount; iPacket < ( uint32_t )m_commandTable.m_drawPackets.size();
	        ++iPacket ) {
		if ( ( m_commandTable.m_drawPackets[iPacket].m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) == 0u ) {
			continue;
		}
		m_commandTable.m_transparentPacketOrder[m_commandTable.m_nTransparentPacketOrderCount++] = iPacket;
	}
	if ( m_commandTable.m_nTransparentPacketOrderCount > 1u ) {
		t3dRdTransparentPacketIndexLess_t less;
		less.m_pPackets = m_commandTable.m_drawPackets.data();
		less.m_camera = rCamera;
		eastl::sort( m_commandTable.m_transparentPacketOrder.begin(),
		        m_commandTable.m_transparentPacketOrder.begin() + m_commandTable.m_nTransparentPacketOrderCount,
		        less );
	}
	++m_commandTable.m_nGpuTransparentOrderBuilds;
	return true;
}

bool Terrain3DRdInstanceRenderer::compareGpuCpuInstanceCulling( const t3dRdInstanceSceneSnapshot_t &rScene,
        const Projection *const pViewProjections,
        const uint32_t nViewCount,
        const Vector3 &rCamera,
        const uint32_t nVisibilityMask,
        const bool bShadowPass ) {
	++m_visibility.m_nCullParityComparisons;
	const eastl::vector<t3dRdInstanceCullDescriptor_t, Terrain3DEastlAllocator> &descriptors =
	        bShadowPass ? m_visibility.m_shadowInstanceCullDescriptors : m_visibility.m_instanceCullDescriptors;
	const eastl::vector<t3dRdInstanceDescriptorSpan_t, Terrain3DEastlAllocator> &spans =
	        bShadowPass ? m_visibility.m_shadowInstanceCullCellSpans : m_visibility.m_instanceCullCellSpans;
	const eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &packets =
	        bShadowPass ? m_commandTable.m_shadowPackets : m_commandTable.m_drawPackets;
	const Terrain3DRdGpuBuffer &visible_buffer =
	        bShadowPass ? m_visibility.m_hShadowVisibleIndexBuffer : m_visibility.m_hVisibleIndexBuffer;
	const t3dRdGpuBufferSlice_t &indirect_slice =
	        bShadowPass ? m_commandTable.m_shadowIndirectSlice : m_commandTable.m_mainIndirectSlice;
	if ( !pViewProjections || nViewCount == 0u || nViewCount > s_nRdForwardMaxViews || !visible_buffer.isValid()
	        || !indirect_slice.m_hBuffer.is_valid() || rScene.m_cells.size() > spans.size()
	        || packets.size() > m_visibility.m_cullParityCommandCounts.size()
	        || packets.size() > m_visibility.m_cullParityCommandWriteOffsets.size() ) {
		++m_visibility.m_nCullParityMismatches;
		++m_visibility.m_nCullParityTopologyMismatches;
		return false;
	}
	if ( packets.empty() ) {
		if ( !descriptors.empty() ) {
			++m_visibility.m_nCullParityMismatches;
			++m_visibility.m_nCullParityTopologyMismatches;
			return false;
		}
		return true;
	}

	// Validate the immutable publication -> GPU packet topology for every cell,
	// including cells rejected by the current view. This is the P9 LOD/source
	// parity check: the compute pass does not choose LODs, so its descriptor
	// span must reference exactly the surfaces of the source selected by P5/P8.
	for ( size_t iCell = 0u; iCell < rScene.m_cells.size(); ++iCell ) {
		const t3dRdInstanceCell_t &cell = rScene.m_cells[iCell];
		const t3dRdInstanceDescriptorSpan_t &span = spans[iCell];
		bool topology_eligible = cell.m_nInstanceCount > 0u && cell.m_nVisibilityLayers != 0u;
		if ( bShadowPass ) {
			topology_eligible = topology_eligible && ( cell.m_nFlags & T3D_RD_INSTANCE_CAST_SHADOWS ) != 0u;
		} else {
			topology_eligible = topology_eligible && ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u;
		}
		const t3dRdMeshHandle_t mesh = m_geometryArena.m_meshCache.getMeshHandleBySourceKey(
		        cell.m_nMeshSourceKey != 0u ? cell.m_nMeshSourceKey : t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = topology_eligible ? m_geometryArena.m_meshCache.resolveMesh( mesh ) : NULL;
		uint32_t expected_span_count = 0u;
		if ( pLod ) {
			for ( uint32_t surface_index = 0u; surface_index < pLod->m_nSurfaceCount; ++surface_index ) {
				const t3dRdSurfaceHandle_t handle = m_geometryArena.m_meshCache.getSurfaceHandle( mesh, surface_index );
				const t3dRdMeshSourceSurface_t *const pSurface = m_geometryArena.m_meshCache.resolveSurface( handle );
				if ( !pSurface || pSurface->m_nIndexCount == 0u ) {
					continue;
				}
				const uint32_t draw_flags = cell.m_nFlags | pSurface->m_nFlags;
				if ( bShadowPass && ( draw_flags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u
				        && ( draw_flags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) == 0u ) {
					continue;
				}
				if ( expected_span_count >= span.m_nCount || ( uint64_t )span.m_nFirst + expected_span_count >= descriptors.size() ) {
					++m_visibility.m_nCullParityMismatches;
					++m_visibility.m_nCullParityTopologyMismatches;
					return false;
				}
				const t3dRdInstanceCullDescriptor_t &descriptor = descriptors[span.m_nFirst + expected_span_count];
				if ( descriptor.m_nCommandIndex >= packets.size() ) {
					++m_visibility.m_nCullParityMismatches;
					++m_visibility.m_nCullParityTopologyMismatches;
					return false;
				}
				const t3dRdInstanceDrawPacket_t &packet = packets[descriptor.m_nCommandIndex];
				const Vector3 center = cell.m_bounds.get_center();
				const Vector3 extent = cell.m_bounds.size * 0.5f;
				const float expected_lod_begin = bShadowPass ? 0.0f : cell.m_fLodBegin;
				const float expected_lod_end = bShadowPass ? 0.0f : cell.m_fLodEnd;
				if ( packet.m_nSurfaceSlot != handle.m_nSlot || packet.m_nMaterial != pSurface->m_nMaterial
				        || packet.m_nOverlayMaterial != pSurface->m_nOverlayMaterial || descriptor.m_nFirstInstance != cell.m_nFirstInstance
				        || descriptor.m_nInstanceCount != cell.m_nInstanceCount || descriptor.m_nFirstVisible != packet.m_nFirstVisible
				        || descriptor.m_nVisibilityLayers != cell.m_nVisibilityLayers || descriptor.m_nFlags != cell.m_nFlags
				        || descriptor.m_fLodBegin != expected_lod_begin || descriptor.m_fLodEnd != expected_lod_end
				        || descriptor.m_szBoundsCenter[0] != ( float )center.x || descriptor.m_szBoundsCenter[1] != ( float )center.y
				        || descriptor.m_szBoundsCenter[2] != ( float )center.z || descriptor.m_szBoundsExtent[0] != ( float )extent.x
				        || descriptor.m_szBoundsExtent[1] != ( float )extent.y || descriptor.m_szBoundsExtent[2] != ( float )extent.z ) {
					++m_visibility.m_nCullParityMismatches;
					++m_visibility.m_nCullParityTopologyMismatches;
					return false;
				}
				++expected_span_count;
			}
		}
		if ( span.m_nCount != expected_span_count ) {
			++m_visibility.m_nCullParityMismatches;
			++m_visibility.m_nCullParityTopologyMismatches;
			return false;
		}
	}

	t3dRdInstanceFrustum_t frustums[s_nRdForwardMaxViews];
	for ( uint32_t view = 0u; view < nViewCount; ++view ) {
		frustums[view] = t3dRdExtractFrustum( pViewProjections[view] );
	}
	for ( size_t iPacket = 0u; iPacket < packets.size(); ++iPacket ) {
		m_visibility.m_cullParityCommandCounts[iPacket] = 0u;
		m_visibility.m_cullParityCommandWriteOffsets[iPacket] = packets[iPacket].m_nFirstVisible;
	}

	uint64_t visible_cells = 0u;
	uint64_t visible_instances = 0u;
	for ( size_t iCell = 0u; iCell < rScene.m_cells.size(); ++iCell ) {
		const t3dRdInstanceCell_t &cell = rScene.m_cells[iCell];
		if ( ( cell.m_nVisibilityLayers & nVisibilityMask ) == 0u ) {
			continue;
		}
		if ( bShadowPass && ( cell.m_nFlags & T3D_RD_INSTANCE_CAST_SHADOWS ) == 0u ) {
			continue;
		}
		if ( !bShadowPass && ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) != 0u ) {
			continue;
		}
		bool visible = false;
		const bool hlod = ( cell.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u;
		for ( uint32_t view = 0u; view < nViewCount && !visible; ++view ) {
			visible = t3dRdInstanceCellVisible( cell, frustums[view], rCamera, bShadowPass || hlod );
		}
		if ( !visible ) {
			continue;
		}
		const t3dRdInstanceDescriptorSpan_t &span = spans[iCell];
		if ( span.m_nCount == 0u ) {
			continue;
		}
		if ( ( uint64_t )span.m_nFirst + span.m_nCount > descriptors.size() ) {
			++m_visibility.m_nCullParityMismatches;
			++m_visibility.m_nCullParityTopologyMismatches;
			return false;
		}
		++visible_cells;
		visible_instances += cell.m_nInstanceCount;
		for ( uint32_t iDescriptor = 0u; iDescriptor < span.m_nCount; ++iDescriptor ) {
			const t3dRdInstanceCullDescriptor_t &descriptor = descriptors[span.m_nFirst + iDescriptor];
			const float expected_lod_begin = bShadowPass ? 0.0f : cell.m_fLodBegin;
			const float expected_lod_end = bShadowPass ? 0.0f : cell.m_fLodEnd;
			if ( descriptor.m_nCommandIndex >= packets.size() || descriptor.m_nFirstInstance != cell.m_nFirstInstance
			        || descriptor.m_nInstanceCount != cell.m_nInstanceCount || descriptor.m_nVisibilityLayers != cell.m_nVisibilityLayers
			        || descriptor.m_nFlags != cell.m_nFlags || descriptor.m_fLodBegin != expected_lod_begin
			        || descriptor.m_fLodEnd != expected_lod_end ) {
				++m_visibility.m_nCullParityMismatches;
				++m_visibility.m_nCullParityTopologyMismatches;
				return false;
			}
			const uint32_t command_index = descriptor.m_nCommandIndex;
			if ( UINT32_MAX - m_visibility.m_cullParityCommandCounts[command_index] < descriptor.m_nInstanceCount ) {
				++m_visibility.m_nCullParityMismatches;
				return false;
			}
			const uint32_t write = m_visibility.m_cullParityCommandWriteOffsets[command_index];
			const uint64_t write_end = ( uint64_t )write + descriptor.m_nInstanceCount;
			const uint64_t packet_end = ( uint64_t )packets[command_index].m_nFirstVisible + packets[command_index].m_nVisibleCount;
			if ( write_end > m_visibility.m_cullParityExpectedIndices.size() || write_end > packet_end ) {
				++m_visibility.m_nCullParityMismatches;
				++m_visibility.m_nCullParityTopologyMismatches;
				return false;
			}
			for ( uint32_t instance = 0u; instance < descriptor.m_nInstanceCount; ++instance ) {
				m_visibility.m_cullParityExpectedIndices[write + instance] = descriptor.m_nFirstInstance + instance;
			}
			m_visibility.m_cullParityCommandWriteOffsets[command_index] = ( uint32_t )write_end;
			m_visibility.m_cullParityCommandCounts[command_index] += descriptor.m_nInstanceCount;
		}
	}

	const uint32_t indirect_bytes = ( uint32_t )( packets.size() * sizeof( t3dRdIndexedIndirectCommand_t ) );
	const PackedByteArray gpu_indirect = m_commandTable.m_hIndirectBuffer.readbackSliceTracked( m_context, indirect_slice, 0u, indirect_bytes );
	m_visibility.m_nCullParityReadbackBytes += indirect_bytes;
	if ( gpu_indirect.size() != ( int64_t )indirect_bytes ) {
		++m_visibility.m_nCullParityMismatches;
		return false;
	}
	uint32_t visible_read_count = 0u;
	for ( size_t iPacket = 0u; iPacket < packets.size(); ++iPacket ) {
		t3dRdIndexedIndirectCommand_t actual;
		memcpy( &actual, gpu_indirect.ptr() + iPacket * sizeof( actual ), sizeof( actual ) );
		const t3dRdIndexedIndirectCommand_t &topology = packets[iPacket].m_command;
		if ( actual.m_nIndexCount != topology.m_nIndexCount || actual.m_nFirstIndex != topology.m_nFirstIndex
		        || actual.m_nVertexOffset != topology.m_nVertexOffset || actual.m_nFirstInstance != topology.m_nFirstInstance
		        || actual.m_nInstanceCount != m_visibility.m_cullParityCommandCounts[iPacket]
		        || actual.m_nInstanceCount > packets[iPacket].m_nVisibleCount ) {
			++m_visibility.m_nCullParityMismatches;
			return false;
		}
		const uint64_t packet_end = ( uint64_t )packets[iPacket].m_nFirstVisible + actual.m_nInstanceCount;
		if ( packet_end > m_visibility.m_nVisibleIndexCapacity ) {
			++m_visibility.m_nCullParityMismatches;
			return false;
		}
		visible_read_count = MAX( visible_read_count, ( uint32_t )packet_end );
	}
	const uint32_t visible_bytes = visible_read_count * sizeof( uint32_t );
	if ( visible_bytes > 0u ) {
		const PackedByteArray gpu_visible = visible_buffer.readbackTracked( m_context, 0u, visible_bytes );
		m_visibility.m_nCullParityReadbackBytes += visible_bytes;
		if ( gpu_visible.size() != ( int64_t )visible_bytes || visible_read_count > m_visibility.m_cullParityActualIndices.size() ) {
			++m_visibility.m_nCullParityMismatches;
			return false;
		}
		memcpy( m_visibility.m_cullParityActualIndices.data(), gpu_visible.ptr(), visible_bytes );
	}
	for ( size_t iPacket = 0u; iPacket < packets.size(); ++iPacket ) {
		const uint32_t first = packets[iPacket].m_nFirstVisible;
		const uint32_t count = m_visibility.m_cullParityCommandCounts[iPacket];
		if ( count < 2u ) {
			if ( count == 1u && m_visibility.m_cullParityExpectedIndices[first] != m_visibility.m_cullParityActualIndices[first] ) {
				++m_visibility.m_nCullParityMismatches;
				return false;
			}
			continue;
		}
		eastl::sort( m_visibility.m_cullParityExpectedIndices.begin() + first,
		        m_visibility.m_cullParityExpectedIndices.begin() + first + count );
		eastl::sort(
		        m_visibility.m_cullParityActualIndices.begin() + first, m_visibility.m_cullParityActualIndices.begin() + first + count );
		if ( memcmp( m_visibility.m_cullParityExpectedIndices.data() + first,
		             m_visibility.m_cullParityActualIndices.data() + first,
		             count * sizeof( uint32_t ) )
		        != 0 ) {
			++m_visibility.m_nCullParityMismatches;
			return false;
		}
	}
	if ( bShadowPass ) {
		m_visibility.m_nCullParityShadowVisibleCells = visible_cells;
		m_visibility.m_nCullParityShadowVisibleInstances = visible_instances;
	} else {
		m_visibility.m_nCullParityVisibleCells = visible_cells;
		m_visibility.m_nCullParityVisibleInstances = visible_instances;
	}
	if ( !m_visibility.m_bCullParityValidatedOnce ) {
		m_visibility.m_bCullParityValidatedOnce = true;
		UtilityFunctions::print( "[RD validation] Instance GPU/CPU culling parity passed" );
	}
	return true;
}

void Terrain3DRdInstanceRenderer::freeViewState( t3dRdInstanceViewState_t &rState ) {
	for ( size_t iPipeline = 0u; iPipeline < T3D_ARRAY_COUNT( rState.m_szPipeline ); iPipeline++ ) {
		RID &pipeline = ( rState.m_szPipeline )[iPipeline];
		m_viewPipelines.erase( pipeline );
		pipeline = RID();
	}
	rState.m_hFramebuffer.releaseTracked( m_context );
	rState.m_hColor = RID();
	rState.m_hDepth = RID();
	rState.m_samples = RenderingDevice::TEXTURE_SAMPLES_1;
}

bool Terrain3DRdInstanceRenderer::ensureViewState(
        const uint32_t nView, const RID &rColor, const RID &rDepth, const RenderingDevice::TextureSamples samples ) {
	if ( nView >= s_nMaxViews ) {
		return false;
	}
	t3dRdInstanceViewState_t &state = m_szViewStates[nView];
	bool pipelines_valid = true;
	for ( size_t iPipeline = 0u; iPipeline < T3D_ARRAY_COUNT( state.m_szPipeline ); iPipeline++ ) {
		const RID &pipeline = ( state.m_szPipeline )[iPipeline];
		pipelines_valid = pipelines_valid && pipeline.is_valid();
	}
	if ( state.m_hFramebuffer.isValid() && pipelines_valid && state.m_hColor == rColor && state.m_hDepth == rDepth
	        && state.m_samples == samples ) {
		return true;
	}
	m_context.beginResourceRebuild();
	freeViewState( state );
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		m_context.endResourceRebuild();
		setError( "RD instance view-state creation lost the RenderingDevice" );
		return false;
	}
	if ( m_geometryArena.m_meshCache.getVertexFormat() == RenderingDevice::INVALID_FORMAT_ID ) {
		m_context.endResourceRebuild();
		setError( "RD instance view-state creation has no valid mesh vertex format" );
		return false;
	}
	TypedArray<RID> attachments;
	attachments.push_back( rColor );
	attachments.push_back( rDepth );
	const String framebuffer_name = String( "T3D.Instance.Framebuffer." ) + String::num_int64( nView );
	if ( !state.m_hFramebuffer.createTracked( m_context, Terrain3DRdContext::OWNER_INSTANCES, framebuffer_name, attachments ) ) {
		m_context.endResourceRebuild();
		setError( String( "RD instance framebuffer creation or registration failed; view=" ) + String::num_int64( nView ) );
		return false;
	}
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( samples );
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	const RenderingDevice::PolygonCullMode cull_modes[3] = {
		RenderingDevice::POLYGON_CULL_DISABLED,
		RenderingDevice::POLYGON_CULL_DISABLED,
		RenderingDevice::POLYGON_CULL_DISABLED,
	};
#else
	const RenderingDevice::PolygonCullMode cull_modes[3] = {
		RenderingDevice::POLYGON_CULL_BACK,
		RenderingDevice::POLYGON_CULL_FRONT,
		RenderingDevice::POLYGON_CULL_DISABLED,
	};
#endif
	for ( uint32_t pipeline_index = 0u; pipeline_index < 9u; ++pipeline_index ) {
		const uint32_t cull_index = pipeline_index % 3u;
		const uint32_t opacity_mode = pipeline_index / 3u;
		Ref<RDPipelineRasterizationState> raster;
		raster.instantiate();
		raster->set_cull_mode( cull_modes[cull_index] );
		raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE );

		Ref<RDPipelineDepthStencilState> depth;
		depth.instantiate();
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		depth->set_enable_depth_test( false );
		depth->set_enable_depth_write( false );
		depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_ALWAYS );
#else
		depth->set_enable_depth_test( true );
		depth->set_enable_depth_write( opacity_mode != 1u );
		depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_LESS_OR_EQUAL );
#endif

		Ref<RDPipelineColorBlendStateAttachment> blend_attachment;
		blend_attachment.instantiate();
		const bool alpha_blend = opacity_mode == 1u;
		blend_attachment->set_enable_blend( alpha_blend );
		if ( opacity_mode == 2u ) {
			blend_attachment->set_write_r( false );
			blend_attachment->set_write_g( false );
			blend_attachment->set_write_b( false );
			blend_attachment->set_write_a( false );
		}
		if ( alpha_blend ) {
			blend_attachment->set_src_color_blend_factor( RenderingDevice::BLEND_FACTOR_SRC_ALPHA );
			blend_attachment->set_dst_color_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
			blend_attachment->set_color_blend_op( RenderingDevice::BLEND_OP_ADD );
			blend_attachment->set_src_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE );
			blend_attachment->set_dst_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
			blend_attachment->set_alpha_blend_op( RenderingDevice::BLEND_OP_ADD );
		}
		Ref<RDPipelineColorBlendState> blend;
		blend.instantiate();
		TypedArray<Ref<RDPipelineColorBlendStateAttachment>> blend_attachments;
		blend_attachments.push_back( blend_attachment );
		blend->set_attachments( blend_attachments );

		const String pipeline_name =
		        String( "T3D.Instance.Pipeline." ) + String::num_int64( nView ) + "." + String::num_int64( pipeline_index );
		state.m_szPipeline[pipeline_index] = m_viewPipelines.getOrCreateRaster( nView * 9u + pipeline_index,
		        pipeline_name, m_hShader.rid(),
		        state.m_hFramebuffer.format( rd ),
		        m_geometryArena.m_meshCache.getVertexFormat(),
		        RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
		        raster,
		        multisample,
		        depth,
		        blend,
		        0u );
		if ( !state.m_szPipeline[pipeline_index].is_valid() ) {
			freeViewState( state );
			m_context.endResourceRebuild();
			setError( String( "RD instance preview pipeline creation failed; view=" ) + String::num_int64( nView ) + String( ", pipeline=" )
			        + String::num_int64( pipeline_index ) );
			return false;
		}
	}
	state.m_hColor = rColor;
	state.m_hDepth = rDepth;
	state.m_samples = samples;
	m_context.endResourceRebuild();
	return true;
}

bool Terrain3DRdInstanceRenderer::drawPacket( const int64_t nDrawList,
        t3dRdInstanceViewState_t &rState,
        uint32_t &nBoundPipeline,
        const uint32_t nPacket,
        const bool bDepthPrepass ) {
	RenderingDevice *const pRd = m_context.getDevice();
	const t3dRdInstanceDrawPacket_t &rPacket = m_commandTable.m_drawPackets[nPacket];
	if ( !pRd || rPacket.m_nVisibleCount == 0u ) {
		return pRd != NULL;
	}
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	const RID hVertexArray = m_geometryArena.m_meshCache.getSurfaceVertexArray( rPacket.m_nSurfaceSlot );
	const RID hIndexArray = m_geometryArena.m_meshCache.getSurfaceIndexArray( rPacket.m_nSurfaceSlot );
	if ( !hVertexArray.is_valid() || !hIndexArray.is_valid() ) {
		setError( String( "RD instancer shared-arena CPU reference is missing a surface view; surface=" )
		        + String::num_int64( rPacket.m_nSurfaceSlot ) + String( ", surface_views=" )
		        + String::num_int64( m_geometryArena.m_meshCache.getSurfaceViewCount() ) );
		return false;
	}
	pRd->draw_list_bind_vertex_array( nDrawList, hVertexArray );
#else
	const RID hIndexArray = m_geometryArena.m_meshCache.getIndexArray();
	if ( !hIndexArray.is_valid() ) {
		return false;
	}
#endif
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	const uint32_t nPipelineIndex = 2u;
#else
	uint32_t nPipelineIndex = 0u;
	if ( ( rPacket.m_nFlags & ( T3D_RD_INSTANCE_DOUBLE_SIDED | T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED ) ) != 0u ) {
		nPipelineIndex = 2u;
	} else if ( ( rPacket.m_nFlags & T3D_RD_INSTANCE_CULL_FRONT ) != 0u ) {
		nPipelineIndex = 1u;
	}
	if ( bDepthPrepass ) {
		nPipelineIndex += 6u;
	} else if ( ( rPacket.m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u ) {
		nPipelineIndex += 3u;
	}
#endif
	if ( nPipelineIndex != nBoundPipeline ) {
		pRd->draw_list_bind_render_pipeline( nDrawList, rState.m_szPipeline[nPipelineIndex] );
		nBoundPipeline = nPipelineIndex;
	}
	pRd->draw_list_bind_index_array( nDrawList, hIndexArray );
	const uint32_t nActiveAllocationCount = m_pVirtualTexture != NULL ? m_pVirtualTexture->getActiveAllocationCount() : 0u;
	const uint32_t nActivePageCount = m_pVirtualTexture != NULL ? m_pVirtualTexture->getActiveVirtualPageCount() : 0u;
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	t3dRdInstancePush_t push{
		UINT16_MAX, UINT16_MAX, rPacket.m_nFirstVisible, 0u, UINT32_MAX, 1u, nActiveAllocationCount, nActivePageCount
	};
#else
	const uint32_t nDrawFlags = bDepthPrepass ? rPacket.m_nFlags | T3D_RD_INSTANCE_DEPTH_PREPASS_PASS : rPacket.m_nFlags;
	t3dRdInstancePush_t push{ rPacket.m_nMaterial,
		rPacket.m_nOverlayMaterial,
#if T3D_INSTANCER_RD_PORT
		rPacket.m_nFirstVisible,
#else
		0u,
#endif
		nDrawFlags,
		rPacket.m_nVisibilityLayers,
		0u,
		nActiveAllocationCount,
		nActivePageCount };
#endif
	memcpy( m_uploadState.m_pushBytes.ptrw(), &push, sizeof( push ) );
	pRd->draw_list_set_push_constant( nDrawList, m_uploadState.m_pushBytes, sizeof( push ) );
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	pRd->draw_list_draw( nDrawList, true, rPacket.m_nVisibleCount, 0u );
#else
	pRd->draw_list_draw_indirect( nDrawList,
	        true,
	        m_commandTable.m_hIndirectBuffer,
	        m_commandTable.m_mainIndirectSlice.m_nOffsetBytes + nPacket * sizeof( t3dRdIndexedIndirectCommand_t ),
	        1u,
	        sizeof( t3dRdIndexedIndirectCommand_t ) );
	m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_INSTANCES, 1u );
#endif
	++m_commandTable.m_nDraws;
	return true;
}

bool Terrain3DRdInstanceRenderer::drawView(
        const uint32_t nView, const Projection &rViewProjection, const Vector3 &rCamera, const uint32_t nVisibilityMask ) {
	if ( nView >= s_nMaxViews ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdInstanceViewState_t &state = m_szViewStates[nView];
	t3dRdInstanceFrame_t frame;
	memset( &frame, 0, sizeof( frame ) );
	for ( uint32_t view_index = 0u; view_index < s_nRdForwardMaxViews; ++view_index ) {
		for ( int column = 0; column < 4; ++column ) {
			const Vector4 c = rViewProjection[column];
			frame.m_szViewProjection[view_index][column * 4 + 0] = ( float )( c.x );
			frame.m_szViewProjection[view_index][column * 4 + 1] = ( float )( c.y );
			frame.m_szViewProjection[view_index][column * 4 + 2] = ( float )( c.z );
			frame.m_szViewProjection[view_index][column * 4 + 3] = ( float )( c.w );
		}
		frame.m_szCamera[view_index][0] = ( float )( rCamera.x );
		frame.m_szCamera[view_index][1] = ( float )( rCamera.y );
		frame.m_szCamera[view_index][2] = ( float )( rCamera.z );
	}
	copyStylizedSunGlobals( frame.m_szNomadSunShape, frame.m_szNomadSunDiffuse, frame.m_szNomadSunSpecular, frame.m_szNomadSunRim );
	memcpy( m_uploadState.m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	if ( !rd ) {
		setError( "RD instance draw lost the RenderingDevice" );
		return false;
	}
	if ( !m_uploadState.m_hFrameBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_frameBytes, sizeof( frame ), T3D_GPU_HOT_FRAME ) ) {
		setError( "RD instance frame buffer_update failed" );
		return false;
	}
#if !T3D_INSTANCER_RD_PORT && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !dispatchHlodCull( nVisibilityMask ) ) {
		return false;
	}
#else
	( void )nVisibilityMask;
#endif
	PackedColorArray clear_colors;
	if ( m_bPreviewClear ) {
		clear_colors.push_back( Color( 0.0f, 0.0f, 0.0f, 0.0f ) );
	}
	const int64_t list = m_bPreviewClear
	        ? rd->draw_list_begin( state.m_hFramebuffer.rid(), RenderingDevice::DRAW_DEFAULT_ALL, clear_colors, 1.0f )
	        : rd->draw_list_begin( state.m_hFramebuffer.rid(), RenderingDevice::DRAW_DEFAULT_ALL );
	if ( list == RenderingDevice::INVALID_ID ) {
		setError( "RD instance draw-list creation failed" );
		return false;
	}
	rd->draw_list_bind_uniform_set( list, m_descriptorState.m_hUniformSet, 0u );
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !m_descriptorState.m_hMaterialVtUniformSet.is_valid() ) {
		rd->draw_list_end();
		setError( "RD instancer preview invariant VT set is invalid" );
		return false;
	}
	rd->draw_list_bind_uniform_set( list, m_descriptorState.m_hMaterialVtUniformSet, 1u );
	++m_descriptorState.m_nVtDescriptorBindsInstancerPass;
#endif
	uint32_t bound_pipeline = UINT32_MAX;
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
#if !T3D_INSTANCER_RD_PORT
	rd->draw_list_bind_vertex_array( list, m_geometryArena.m_meshCache.getVertexArray() );
#endif
	for ( uint32_t i = 0u; i < ( uint32_t )( m_commandTable.m_drawPackets.size() ); ++i ) {
		if ( ( m_commandTable.m_drawPackets[i].m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) != 0u
		        && !drawPacket( list, state, bound_pipeline, i, true ) ) {
			rd->draw_list_end();
			return false;
		}
	}
	bound_pipeline = UINT32_MAX;
#endif
	for ( uint32_t i = 0u; i < ( uint32_t )( m_commandTable.m_drawPackets.size() ); ++i ) {
		if ( !drawPacket( list, state, bound_pipeline, i, false ) ) {
			rd->draw_list_end();
			return false;
		}
	}
	rd->draw_list_end();
	return true;
}

void Terrain3DRdInstanceRenderer::freePreviewTargets( void ) {
	if ( m_szViewStates[0].m_hColor == m_hPreviewColorTexture || m_szViewStates[0].m_hDepth == m_hPreviewDepthTexture ) {
		freeViewState( m_szViewStates[0] );
	}
	m_targetPool.release( m_hPreviewColorTexture );
	m_targetPool.release( m_hPreviewDepthTexture );
	m_previewSize = Vector2i();
}

bool Terrain3DRdInstanceRenderer::ensurePreviewTargets( const Vector2i &rSize ) {
	if ( rSize.x < 2 || rSize.y < 2 ) {
		setError( String( "RD editor preview target size is invalid; size=" ) + String::num_int64( rSize.x ) + String( "x" )
		        + String::num_int64( rSize.y ) );
		return false;
	}
	if ( m_hPreviewColorTexture.is_valid() && m_hPreviewDepthTexture.is_valid() && m_previewSize == rSize ) {
		return true;
	}
	freePreviewTargets();
	if ( !m_context.getDevice() ) {
		setError( "RD editor preview target creation lost the RenderingDevice" );
		return false;
	}
	m_context.beginResourceRebuild();
	const Terrain3DRdRenderTargetPool<4u>::Key color_key {
		( uint32_t )rSize.x, ( uint32_t )rSize.y, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
		( uint32_t )( RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT ),
		4u, 2u, false
	};
	RID unused_framebuffer;
	if ( !m_targetPool.acquire( color_key, "T3D.EditorPreview.Color", m_hPreviewColorTexture, unused_framebuffer ) ) {
		m_context.endResourceRebuild();
		setError( String( "RD editor preview color target creation failed; size=" ) + String::num_int64( rSize.x ) + String( "x" )
		        + String::num_int64( rSize.y ) );
		return false;
	}
	const Terrain3DRdRenderTargetPool<4u>::Key depth_key {
		( uint32_t )rSize.x, ( uint32_t )rSize.y, RenderingDevice::DATA_FORMAT_D32_SFLOAT,
		( uint32_t )RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 4u, 3u, false
	};
	if ( !m_targetPool.acquire( depth_key, "T3D.EditorPreview.Depth", m_hPreviewDepthTexture, unused_framebuffer ) ) {
		m_context.endResourceRebuild();
		freePreviewTargets();
		setError( String( "RD editor preview depth target creation failed; size=" ) + String::num_int64( rSize.x ) + String( "x" )
		        + String::num_int64( rSize.y ) );
		return false;
	}
	m_previewSize = rSize;
	m_context.endResourceRebuild();
	return true;
}

bool Terrain3DRdInstanceRenderer::initialize( const Ref<RDShaderSPIRV> &rShader,
        const Ref<RDShaderSPIRV> &rHlodCullShader,
        const Ref<RDShaderSPIRV> &rInstanceCullShader,
        const Ref<RDShaderSPIRV> &rInstanceHiZReduceShader,
        const Ref<RDShaderSPIRV> &rOccluderDepthShader,
        const t3dRdForwardShaderBundle_t &rForwardShaders ) {
	if ( m_pVirtualTexture == NULL ) {
		setError( "Terrain3D production instance renderer has no shared Terrain3DVirtualTexture owner" );
		return false;
	}
	m_forwardShaderBundle = rForwardShaders;
	m_visibility.m_hlodCullSourceShader = rHlodCullShader;
	m_visibility.m_instanceCullSourceShader = rInstanceCullShader;
	m_visibility.m_instanceHiZReduceSourceShader = rInstanceHiZReduceShader;
	m_occluderDepth.m_sourceShader = rOccluderDepthShader;
	// P10 promotes compute visibility; P11 binds that visibility to the shared
	// mesh arenas. The raster shader and compute culler remain separate so the
	// CPU direct-draw reference can still isolate failures through arena views.
	if ( !ensureDevice() || !ensureShader( rShader ) || !ensureOccluderDepthShader() ) {
		return false;
	}
#if T3D_INSTANCER_HIZ_OCCLUSION
	if ( !m_visibility.m_instanceHiZ.initialize( m_visibility.m_instanceHiZReduceSourceShader ) ) {
		setError( "Failed to initialize Terrain3D P12 instance Hi-Z resources" );
		return false;
	}
#endif
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( ( T3D_INSTANCER_GPU_CULL_PRODUCTION != 0 || m_visibility.m_bCullingComparisonEnabled )
	        && !ensureInstanceCullShader( m_visibility.m_instanceCullSourceShader ) ) {
		return false;
	}
#endif
	m_uploadState.m_frameBytes.resize( sizeof( t3dRdInstanceFrame_t ) );
	m_uploadState.m_pushBytes.resize( sizeof( t3dRdInstancePush_t ) );
	m_uploadState.m_hlodCullPushBytes.resize( sizeof( t3dRdHlodCullPush_t ) );
	m_uploadState.m_instanceCullPushBytes.resize( sizeof( t3dRdInstanceCullPush_t ) );
	return true;
}

bool Terrain3DRdInstanceRenderer::initializePreview( const Ref<RDShaderSPIRV> &rShader ) {
	// Editor cards exercise only the shared raster/mesh/material path. They must
	// not fail because an unrelated Forward+, HLOD, or runtime cull variant is
	// unavailable.
	if ( !ensureDevice() || !ensureShader( rShader ) ) {
		return false;
	}
	m_uploadState.m_frameBytes.resize( sizeof( t3dRdInstanceFrame_t ) );
	m_uploadState.m_pushBytes.resize( sizeof( t3dRdInstancePush_t ) );
	return true;
}

void Terrain3DRdInstanceRenderer::shutdown( void ) {
	for ( size_t iView = 0u; iView < T3D_ARRAY_COUNT( m_szViewStates ); iView++ ) {
		t3dRdInstanceViewState_t &state = ( m_szViewStates )[iView];
		freeViewState( state );
	}
	freePreviewTargets();
	m_forwardLighting.shutdown();
	freeForwardPipelines();
	freeMaterialVtUniformSets( false );
	m_descriptorState.m_hVtSampler.releaseTracked( m_context );
	freeOccluderDepthResources();
	m_targetPool.clear();
	m_geometryArena.m_meshCache.shutdown();
	m_instanceSet0Descriptors.erase( m_descriptorState.m_hUniformSet );
	m_descriptorState.m_hUniformSet = RID();
	freeForwardUniformSets();
	m_uploadState.m_hFrameBuffer.releaseTracked( m_context );
	m_uploadState.m_hInstanceBuffer.releaseTracked( m_context );
	m_uploadState.m_hPreviousInstanceBuffer.releaseTracked( m_context );
	m_visibility.m_hVisibleIndexBuffer.releaseTracked( m_context );
	m_visibility.m_hShadowVisibleIndexBuffer.releaseTracked( m_context );
	m_visibility.m_hVisibleDrawSlotBuffer.releaseTracked( m_context );
	m_visibility.m_hShadowVisibleDrawSlotBuffer.releaseTracked( m_context );
	m_visibility.m_hVisibleLayerBuffer.releaseTracked( m_context );
	m_visibility.m_hShadowVisibleLayerBuffer.releaseTracked( m_context );
	m_commandTable.m_hDrawMetadataBuffer.releaseTracked( m_context );
	m_uploadState.m_hMaterialBuffer.releaseTracked( m_context );
	m_commandTable.m_hIndirectBuffer.releaseTracked( m_context );
	m_commandTable.m_mainIndirectSlice = {};
	m_commandTable.m_shadowIndirectSlice = {};
	m_instanceCullDescriptors.erase( m_visibility.m_hInstanceCullUniformSet );
	m_visibility.m_hInstanceCullUniformSet = RID();
	m_visibility.m_instanceHiZ.shutdown();
	m_visibility.m_hInstanceCullDescriptorBuffer.releaseTracked( m_context );
	m_visibility.m_hInstanceHiZRegionDescriptorBuffer.releaseTracked( m_context );
	m_visibility.m_hInstanceHiZRegionVisibilityBuffer.releaseTracked( m_context );
	m_instanceCullPipelines.clear();
	m_visibility.m_hInstanceCullPipeline = RID();
	m_visibility.m_hInstanceCullShader.releaseTracked( m_context );
	m_hlodCullDescriptors.erase( m_visibility.m_hHlodCullUniformSet );
	m_visibility.m_hHlodCullUniformSet = RID();
	m_visibility.m_hHlodCullDescriptorBuffer.releaseTracked( m_context );
	m_hlodCullPipelines.clear();
	m_visibility.m_hHlodCullPipeline = RID();
	m_visibility.m_hHlodCullShader.releaseTracked( m_context );
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_hszForwardShaders[variant].releaseTracked( m_context );
	}
	m_hShader.releaseTracked( m_context );
	m_forwardShaderBundle = t3dRdForwardShaderBundle_t{};
	m_visibility.m_hlodCullSourceShader.unref();
	m_visibility.m_instanceCullSourceShader.unref();
	m_visibility.m_instanceHiZReduceSourceShader.unref();
	m_visibility.m_nHiZBuildRenderFrame = UINT64_MAX;
	m_visibility.m_nHiZBuildViewportKey = 0u;
	m_visibility.m_bHiZReadyForFrame = false;
	m_visibility.m_nHiZStableSceneGeneration = 0u;
	m_visibility.m_bHiZOcclusionSubmittedOnce = false;
	m_visibility.m_nHiZCellDescriptors = 0u;
	m_visibility.m_nHiZInstanceDescriptors = 0u;
	m_uploadState.m_nUploadedSceneGeneration = m_geometryArena.m_nUploadedMeshGeneration = 0u;
	m_commandTable.m_nDrawMetadataMeshGeneration = 0u;
	m_descriptorState.m_bMaterialVtSetBuiltOnce = false;
	m_descriptorState.m_nVtDescriptorBindsInstancerPass = 0u;
	m_descriptorState.m_nVtDescriptorRebuildsRuntime = 0u;
	m_descriptorState.m_nMaterialVtResourceSignature = 0u;
	m_commandTable.m_nCpuSurfacePacketsBuilt = 0u;
	m_commandTable.m_nOneSubmitOpaque = m_commandTable.m_nOneSubmitDepth = m_commandTable.m_nOneSubmitMotion =
	        m_commandTable.m_nOneSubmitShadow = m_commandTable.m_nOneSubmitFallbackDraws = 0u;
	m_geometryArena.m_nSceneCapacityMeshGeneration = 0u;
	m_visibility.m_nVisibleIndexCount = 0u;
	m_visibility.m_nShadowVisibleIndexCount = 0u;
	m_commandTable.m_nMainCompatibleCommandCount = 0u;
	m_commandTable.m_nShadowCompatibleCommandCount = 0u;
	m_visibility.m_nFixedVisibleCapacity = 0u;
	m_uploadState.m_nMotionHistoryRecordCount = 0u;
	m_uploadState.m_nMotionHistoryCellCount = 0u;
	m_uploadState.m_nMotionPreviousRecordCount = 0u;
	m_uploadState.m_nMotionCurrentOrderCount = 0u;
	m_uploadState.m_nMotionPreviousOrderCount = 0u;
	t3dVectorRelease( m_visibility.m_visibleIndices );
	t3dVectorRelease( m_visibility.m_visibleDrawSlots );
	t3dVectorRelease( m_visibility.m_shadowVisibleDrawSlots );
	t3dVectorRelease( m_visibility.m_visibleLayers );
	t3dVectorRelease( m_visibility.m_shadowVisibleLayers );
	t3dVectorRelease( m_commandTable.m_fixedDrawVisibleBases );
	t3dVectorRelease( m_commandTable.m_fixedDrawVisibleCapacities );
	t3dVectorRelease( m_commandTable.m_shadowFixedDrawVisibleBases );
	t3dVectorRelease( m_commandTable.m_shadowFixedDrawVisibleCapacities );
	t3dVectorRelease( m_uploadState.m_motionHistoryRecords );
	t3dVectorRelease( m_uploadState.m_motionHistoryCells );
	t3dVectorRelease( m_uploadState.m_motionPreviousRecords );
	t3dVectorRelease( m_uploadState.m_motionDirtyRanges );
	m_uploadState.m_previousInstanceBytes.clear();
	m_uploadState.m_instanceCullDescriptorBytes.clear();
	m_uploadState.m_instanceCullPushBytes.clear();
	t3dVectorRelease( m_visibility.m_shadowVisibleIndices );
	t3dVectorRelease( m_visibility.m_cullParityExpectedIndices );
	t3dVectorRelease( m_visibility.m_cullParityActualIndices );
	t3dVectorRelease( m_visibility.m_cullParityCommandCounts );
	t3dVectorRelease( m_visibility.m_cullParityCommandWriteOffsets );
	t3dVectorRelease( m_commandTable.m_drawPackets );
	t3dVectorRelease( m_commandTable.m_depthPackets );
	t3dVectorRelease( m_commandTable.m_shadowPackets );
	t3dVectorRelease( m_visibility.m_hlodDescriptors );
	t3dVectorRelease( m_visibility.m_instanceCullDescriptors );
	t3dVectorRelease( m_visibility.m_shadowInstanceCullDescriptors );
	t3dVectorRelease( m_visibility.m_instanceHiZRegionDescriptors );
	t3dVectorRelease( m_visibility.m_instanceHiZRegionKeys );
	t3dVectorRelease( m_visibility.m_instanceCullCellSpans );
	t3dVectorRelease( m_visibility.m_shadowInstanceCullCellSpans );
	t3dVectorRelease( m_commandTable.m_packetBuildSlots );
	t3dVectorRelease( m_commandTable.m_packetBuildEntries );
	t3dVectorRelease( m_commandTable.m_alphaPackets );
	t3dVectorRelease( m_commandTable.m_alphaSortScratch );
	t3dVectorRelease( m_commandTable.m_transparentPacketOrder );
	t3dVectorRelease( m_commandTable.m_gpuPacketBoundValid );
	t3dVectorRelease( m_occluderDepth.m_packets );
	t3dVectorRelease( m_occluderDepth.m_visibleIndices );
	t3dVectorRelease( m_uploadState.m_motionCurrentOrder );
	t3dVectorRelease( m_uploadState.m_motionPreviousOrder );
	m_commandTable.m_nStaticInstances = 0u;
	m_commandTable.m_nVisibleInstances = 0u;
	m_uploadState.m_nPreviewSceneGeneration = 0u;
	m_visibility.m_nHlodCullDescriptors = 0u;
	m_visibility.m_nGpuTopologySceneGeneration = 0u;
	m_visibility.m_nGpuTopologyMeshGeneration = 0u;
	m_visibility.m_nGpuCullMainRenderFrame = UINT64_MAX;
	m_visibility.m_nGpuCullMainViewportKey = 0u;
	m_visibility.m_nGpuCullMainViewKey = 0u;
	m_visibility.m_nGpuCullMainSceneGeneration = 0u;
	m_visibility.m_nGpuCullMainVisibilityMask = UINT32_MAX;
	m_visibility.m_nGpuCullMainViewCount = 0u;
	m_commandTable.m_nTransparentPacketOrderCount = 0u;
	m_visibility.m_nCullParityMainRenderFrame = UINT64_MAX;
	m_visibility.m_nCullParityMainViewportKey = 0u;
	m_visibility.m_nCullParityMainViewKey = 0u;
	m_visibility.m_nCullParityMainSceneGeneration = 0u;
	m_visibility.m_nCullParityMainVisibilityMask = UINT32_MAX;
	m_visibility.m_nCullParityMainViewCount = 0u;
	m_visibility.m_bCullParityValidatedOnce = false;
	m_nForwardPreparedScenePass = 0u;
	m_nForwardPreparedViewportKey = 0u;
	m_uploadState.m_nMotionHistoryRenderFrame = UINT64_MAX;
	m_uploadState.m_nMotionHistorySceneGeneration = 0u;
	m_uploadState.m_bMotionHistoryNeedsAdvance = false;
	m_nForwardPreparedSceneGeneration = 0u;
	m_bLastForwardPipelineKeyValid = false;
	m_bForwardPreparedHasDraws = false;
	m_bForwardPreparedGpuVisibility = false;
	m_nUploadedVtDatabaseGeneration = 0u;
	m_nVtStableKeyMissCount = 0u;
	m_nVtInvalidAllocationReferenceCount = 0u;
	m_nVtOutOfRangePageReferenceCount = 0u;
	m_nLastVtResidencyRequestFrame = UINT64_MAX;
	m_nVtResidencyRequestAttempts = 0u;
	m_nVtResidencyRequestsAccepted = 0u;
	m_forwardPreparedPacketPassKind = T3D_RD_FORWARD_PASS_INVALID;
	m_nForwardPreparedVisibilityMask = UINT32_MAX;
	t3dVectorRelease( m_commandTable.m_packetIndex );
}

bool Terrain3DRdInstanceRenderer::render( RenderData *const pRenderData,
        const t3dRdMeshSourceSnapshot_t *const pMeshes,
        const t3dRdInstanceSceneSnapshot_t *const pScene,
        const uint32_t nVisibilityMask ) {
	if ( !pRenderData || !pScene ) {
		return true;
	}
	const bool scene_empty = pScene->m_instances.empty() && pScene->m_cells.empty();
	const bool scene_incomplete = pScene->m_instances.empty() != pScene->m_cells.empty();
	if ( scene_empty ) {
		m_commandTable.m_nStaticInstances = pScene->m_instances.size();
		m_commandTable.m_nVisibleInstances = 0u;
		m_visibility.m_nVisibleIndexCount = 0u;
		m_visibility.m_nShadowVisibleIndexCount = 0u;
		m_commandTable.m_nMainCompatibleCommandCount = 0u;
		m_commandTable.m_nShadowCompatibleCommandCount = 0u;
		m_commandTable.m_drawPackets.clear();
		m_commandTable.m_depthPackets.clear();
		m_commandTable.m_shadowPackets.clear();
		m_visibility.m_hlodDescriptors.clear();
		m_visibility.m_nHlodCullDescriptors = 0u;
		return true;
	}
	if ( scene_incomplete ) {
		setError( String( "Terrain3D RD instance scene publication is structurally incomplete; records=" )
		        + String::num_int64( pScene->m_instances.size() ) + String( ", cells=" ) + String::num_int64( pScene->m_cells.size() ) );
		return false;
	}
	if ( !pMeshes || pMeshes->m_vertexBytes.is_empty() || pMeshes->m_lods.empty() || pMeshes->m_surfaces.empty() ) {
		setError( String( "Terrain3D RD instance scene has records but no renderable mesh publication; records=" )
		        + String::num_int64( pScene->m_instances.size() ) + String( ", cells=" ) + String::num_int64( pScene->m_cells.size() )
		        + String( ", vertex_bytes=" ) + String::num_int64( pMeshes ? pMeshes->m_vertexBytes.size() : 0 ) + String( ", lods=" )
		        + String::num_int64( pMeshes ? pMeshes->m_lods.size() : 0u ) + String( ", surfaces=" )
		        + String::num_int64( pMeshes ? pMeshes->m_surfaces.size() : 0u ) );
		return false;
	}
	{
		Terrain3DColdPhaseScope cold_phase_scope;
		if ( !m_geometryArena.m_meshCache.upload( pMeshes ) ) {
			setError( String( "Terrain3D RD instance mesh-cache upload failed: " ) + m_geometryArena.m_meshCache.getLastError() );
			return false;
		}
		if ( !uploadMaterials() ) {
			return false;
		}
		if ( !uploadScene( pScene ) ) {
			if ( m_lastError.is_empty() ) {
				setError( "Terrain3D RD instance scene upload failed" );
			}
			return false;
		}
	}
	Ref<RenderSceneBuffers> scene_buffers = pRenderData->get_render_scene_buffers();
	Ref<RenderSceneBuffersRD> buffers = scene_buffers;
	RenderSceneData *const scene_data = pRenderData->get_render_scene_data();
	if ( buffers.is_null() || !scene_data ) {
		return false;
	}
	const uint32_t view_count = buffers->get_view_count();
	if ( view_count == 0u || view_count > s_nMaxViews ) {
		return false;
	}
	const RenderingDevice::TextureSamples samples = buffers->get_texture_samples();
	const bool msaa = samples != RenderingDevice::TEXTURE_SAMPLES_1;
	const Transform3D camera_transform = scene_data->get_cam_transform();
	for ( uint32_t view = 0u; view < view_count; ++view ) {
		const Projection view_matrix = Projection( camera_transform.affine_inverse() );
		const Projection view_projection = scene_data->get_view_projection( view ) * view_matrix;
		Vector3 eye = camera_transform.origin;
		if ( view_count > 1u ) {
			eye += camera_transform.basis.xform( scene_data->get_view_eye_offset( view ) );
		}
		if ( !buildPackets( *pScene, &view_projection, 1u, eye, nVisibilityMask, false ) || !uploadVisibility() ) {
			return false;
		}
		const RID color = view_count > 1u ? buffers->get_color_layer( view, msaa ) : buffers->get_color_texture( msaa );
		const RID depth = view_count > 1u ? buffers->get_depth_layer( view, msaa ) : buffers->get_depth_texture( msaa );
		if ( !ensureViewState( view, color, depth, samples ) || !drawView( view, view_projection, eye, nVisibilityMask ) ) {
			return false;
		}
	}
	++m_uploadState.m_nFrames;
	m_commandTable.m_nStaticInstances = pScene->m_instances.size();
	m_lastError = String();
	return true;
}

bool Terrain3DRdInstanceRenderer::renderPreviewToTarget(
        const t3dRdMeshSourceSnapshot_t *const pMeshes, const uint16_t nAssetId, const Vector2i &rSize ) {
	if ( !pMeshes ) {
		setError( "RD editor preview received a null mesh publication" );
		return false;
	}
	if ( !ensureDevice() ) {
		setError( String( "RD editor preview could not acquire the shared RenderingDevice; context_state=" )
		        + String::num_int64( ( int64_t )( m_context.getState() ) ) + String( ", failure=" )
		        + String::num_int64( ( int64_t )( m_context.getFailureCode() ) ) );
		return false;
	}
	if ( !ensurePreviewTargets( rSize ) ) {
		if ( m_lastError.is_empty() ) {
			setError( "RD editor preview render-target setup failed" );
		}
		return false;
	}
	if ( !m_geometryArena.m_meshCache.upload( pMeshes ) ) {
		setError( String( "RD editor preview mesh-cache upload failed: " ) + m_geometryArena.m_meshCache.getLastError() );
		return false;
	}
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !uploadMaterials() ) {
		if ( m_lastError.is_empty() ) {
			setError( "RD editor preview material upload failed" );
		} else {
			setError( String( "RD editor preview material upload failed: " ) + m_lastError );
		}
		return false;
	}
#endif
	const t3dRdMeshHandle_t mesh = m_geometryArena.m_meshCache.getMeshHandle( nAssetId, 0 );
	const t3dRdMeshSourceLod_t *const lod = m_geometryArena.m_meshCache.resolveMesh( mesh );
	if ( !lod ) {
		setError( "RD editor preview mesh handle is unavailable" );
		return false;
	}
	const AABB source_bounds = lod->m_bounds;
	const Vector3 center = source_bounds.get_center();
	AABB centered = source_bounds;
	centered.position -= center;
	Transform3D xform;
	xform.basis = Basis().rotated( Vector3( 0.0f, 1.0f, 0.0f ), -Math::PI * 0.125f );
	xform.basis = Basis().rotated( Vector3( 1.0f, 0.0f, 0.0f ), Math::PI * 0.125f ) * xform.basis;
	const AABB rotated = xform.xform( centered );
	float extent = ( float )( MAX( rotated.size.x, rotated.size.y ) );
	if ( extent <= 0.0001f ) {
		extent = 1.0f;
	}
	const float scale = 0.9f / extent;
	xform.basis.scale( Vector3( scale, scale, scale ) );
	xform.origin = -xform.basis.xform( center );

	t3dRdInstanceSceneSnapshot_t scene;
	scene.m_nGeneration = ++m_uploadState.m_nPreviewSceneGeneration;
	if ( scene.m_nGeneration == 0u ) {
		scene.m_nGeneration = ++m_uploadState.m_nPreviewSceneGeneration;
	}
	if ( !scene.m_instances.reserveCold( 1u ) || !scene.m_instances.resizeUninitialized( 1u ) ) {
		setError( "RD editor preview instance scratch allocation failed" );
		return false;
	}
	scene.m_instances[0] = t3dRdPackInstanceRecord( xform );
	t3dRdInstanceCell_t cell;
	cell.m_nMeshSourceKey = t3dRdRegularMeshSourceKey( nAssetId, 0 );
	cell.m_nMeshAssetId = nAssetId;
	cell.m_nFirstInstance = 0u;
	cell.m_nInstanceCount = 1u;
	cell.m_nVisibilityLayers = 1u;
	cell.m_bounds = xform.xform( source_bounds );
	if ( !scene.m_cells.reserveCold( 1u ) || !scene.m_cells.resizeUninitialized( 1u ) ) {
		setError( "RD editor preview cell scratch allocation failed" );
		return false;
	}
	scene.m_cells[0] = cell;
	if ( !uploadScene( &scene ) ) {
		setError( "Failed to upload RD editor preview instance" );
		return false;
	}

	const float aspect = ( float )( rSize.x ) / ( float )( MAX( rSize.y, 1 ) );
	const Projection projection = Projection::create_orthogonal_aspect( 1.1f, aspect, 0.01f, 1000.0f );
	Projection depth_correction;
	depth_correction.set_depth_correction( false, false, true );
	const Transform3D camera( Basis(), Vector3( 0.0f, 0.0f, 3.0f ) );
	const Projection view( camera.affine_inverse() );
	// RenderingDevice clip depth is 0..1. The preview depth state is conventional
	// LESS_OR_EQUAL with a clear value of 1, so remap OpenGL-style -1..1 depth
	// without enabling reverse-Z. Use the corrected matrix for both CPU culling
	// and rasterization so the two paths agree exactly.
	const Projection view_projection = depth_correction * projection * view;
	if ( !buildPackets( scene, &view_projection, 1u, camera.origin, 1u, false, false ) ) {
		if ( m_lastError.is_empty() ) {
			setError( "RD editor preview packet build failed" );
		}
		return false;
	}
	if ( !uploadVisibility() ) {
		if ( m_lastError.is_empty() ) {
			setError( "RD editor preview visibility upload failed" );
		}
		return false;
	}
	if ( !ensureViewState( 0u, m_hPreviewColorTexture, m_hPreviewDepthTexture, RenderingDevice::TEXTURE_SAMPLES_1 ) ) {
		if ( m_lastError.is_empty() ) {
			setError( "RD editor preview view-state setup failed" );
		}
		return false;
	}
	m_bPreviewClear = true;
	const bool draw_ok = drawView( 0u, view_projection, camera.origin, 1u );
	m_bPreviewClear = false;
	if ( !draw_ok ) {
		if ( m_lastError.is_empty() ) {
			setError( "Failed to draw RD editor preview" );
		}
		return false;
	}
	m_lastError = String();
	return true;
}

bool Terrain3DRdInstanceRenderer::schedulePreviewReadback( const Callable &rCallback ) {
	if ( !m_context.getDevice() || !m_hPreviewColorTexture.is_valid() || !rCallback.is_valid() ) {
		setError( "RD editor preview async readback prerequisites are unavailable" );
		return false;
	}
	const Error error = m_targetPool.readbackAsync( m_hPreviewColorTexture, 0u, rCallback );
	if ( error != OK ) {
		setError( String( "RD editor preview async readback scheduling failed: " ) + String::num_int64( ( int64_t )error ) );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::renderPreview(
        const t3dRdMeshSourceSnapshot_t *const pMeshes, const uint16_t nAssetId, const Vector2i &rSize, Ref<Image> &rImage ) {
	rImage.unref();
	if ( !renderPreviewToTarget( pMeshes, nAssetId, rSize ) ) {
		return false;
	}
	if ( !m_context.getDevice() ) {
		setError( "RD editor preview readback lost the RenderingDevice" );
		return false;
	}
	const PackedByteArray bytes = m_targetPool.readback( m_hPreviewColorTexture, 0u );
	if ( bytes.size() != ( int64_t )( rSize.x ) * ( int64_t )( rSize.y ) * 4 ) {
		setError( "RD editor preview readback size is invalid" );
		return false;
	}
	rImage = Image::create_from_data( rSize.x, rSize.y, false, Image::FORMAT_RGBA8, bytes );
	if ( rImage.is_null() ) {
		setError( "RD editor preview image creation failed" );
		return false;
	}
	rImage->flip_y();
	m_lastError = String();
	return true;
}

bool Terrain3DRdInstanceRenderer::prepareForwardResources( const t3dRdForwardPassContext_t &rContext,
        const t3dRdMeshSourceSnapshot_t *const pMeshes,
        const t3dRdInstanceSceneSnapshot_t *const pScene,
        const t3dRdForwardShaderVariant_t variant,
        const bool bShadowPass,
        bool &rHiZSourceReady ) {
	rHiZSourceReady = false;
	Terrain3DColdPhaseScope cold_phase_scope;
	if ( !ensureForwardShaderVariant( variant ) ) {
		return false;
	}
	if ( !pMeshes ) {
		setError( "Terrain3D native Forward+ instance prepare lost the mesh publication" );
		return false;
	}
	if ( !m_geometryArena.m_meshCache.upload( pMeshes ) ) {
		setError( String( "Terrain3D native Forward+ instance mesh-cache upload failed: " ) + m_geometryArena.m_meshCache.getLastError() );
		return false;
	}
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !uploadMaterials() ) {
		if ( m_lastError.is_empty() ) {
			setError( "Terrain3D native Forward+ instance material upload failed" );
		}
		return false;
	}
#endif
	if ( !uploadScene( pScene ) ) {
		if ( m_lastError.is_empty() ) {
			setError( "Terrain3D native Forward+ instance scene upload failed" );
		}
		return false;
	}
	if ( !bShadowPass && rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_nViewCount == 1u
	        && !ensureOccluderDepthStorage( *pScene, rContext.m_renderSize ) ) {
		return false;
	}
#if T3D_INSTANCER_HIZ_OCCLUSION
	if ( !bShadowPass ) {
		RID previous_proxy_depth;
		const uint32_t write_slot = ( uint32_t )( rContext.m_nRenderFrame & 1u );
		const uint32_t previous_slot = write_slot ^ 1u;
		const bool frame_contiguous =
		        rContext.m_nRenderFrame > 0u && m_occluderDepth.m_szRasterRenderFrames[previous_slot] == rContext.m_nRenderFrame - 1u;
		const bool proxy_history_valid = m_occluderDepth.m_szDepthValid[previous_slot] && frame_contiguous
		        && m_occluderDepth.m_szRasterViewportKeys[previous_slot] == rContext.m_nViewportKey
		        && m_occluderDepth.m_szRasterSceneGenerations[previous_slot] == pScene->m_nGeneration
		        && m_occluderDepth.m_szRasterMeshGenerations[previous_slot] == m_geometryArena.m_meshCache.getSourceGeneration()
		        && m_occluderDepth.m_hszDepthTextures[previous_slot].is_valid();
		if ( proxy_history_valid ) {
			previous_proxy_depth = m_occluderDepth.m_hszDepthTextures[previous_slot];
			++m_occluderDepth.m_nHistorySamples;
		} else {
			++m_occluderDepth.m_nHistoryInvalidations;
		}
		if ( m_occluderDepth.m_nLastHistoryDecisionRenderFrame != rContext.m_nRenderFrame ) {
			m_occluderDepth.m_nLastHistoryDecisionRenderFrame = rContext.m_nRenderFrame;
			if ( proxy_history_valid ) {
				++m_occluderDepth.m_nHistoryValidFrames;
			} else {
				++m_occluderDepth.m_nHistoryInvalidFrames;
			}
		}
		rHiZSourceReady = m_visibility.m_instanceHiZ.bindPreviousDepth( rContext.m_hPreviousDepthTexture, previous_proxy_depth );
	}
#else
	( void )rContext;
	( void )bShadowPass;
#endif
	if ( !ensureForwardUniformSet( variant, bShadowPass ) ) {
		setError( String( "Terrain3D native Forward+ instance set-0 binding failed; variant=" ) + String::num_int64( variant ) );
		return false;
	}
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !ensureForwardMaterialVtUniformSet( variant ) ) {
		setError( String( "Terrain3D native Forward+ invariant VT set-1 binding failed; variant=" ) + String::num_int64( variant ) );
		return false;
	}
#endif
	return true;
}

bool Terrain3DRdInstanceRenderer::prepareForwardVisibility( const t3dRdForwardPassContext_t &rContext,
        const t3dRdInstanceSceneSnapshot_t &rScene,
        const uint32_t nVisibilityMask,
        const bool bTemporalHistoryValid,
        const bool bShadowPass,
        const bool bHiZSourceReady,
        bool &rGpuVisibility ) {
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !bShadowPass && !prepareMotionHistory( rScene, rContext.m_nRenderFrame ) ) {
		setError( "Failed to prepare Terrain3D instance/HLOD previous-transform history" );
		return false;
	}
#endif
#if T3D_INSTANCER_HIZ_OCCLUSION
	if ( !bShadowPass ) {
		const bool new_hiz_frame = m_visibility.m_nHiZBuildRenderFrame != rContext.m_nRenderFrame
		        || m_visibility.m_nHiZBuildViewportKey != rContext.m_nViewportKey;
		if ( new_hiz_frame ) {
			const bool scene_stable = m_visibility.m_nHiZStableSceneGeneration == rScene.m_nGeneration;
			m_visibility.m_bHiZReadyForFrame = scene_stable && bHiZSourceReady && bTemporalHistoryValid && rContext.m_bTemporalHistoryValid
			        && m_visibility.m_instanceHiZ.build( rContext.m_renderSize );
			m_visibility.m_nHiZBuildRenderFrame = rContext.m_nRenderFrame;
			m_visibility.m_nHiZBuildViewportKey = rContext.m_nViewportKey;
		}
	}
#else
	( void )bHiZSourceReady;
#endif
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	rGpuVisibility = false;
#else
	rGpuVisibility = T3D_INSTANCER_GPU_CULL_PRODUCTION != 0 && !m_bDirectDrawDebug;
#endif
	if ( rGpuVisibility ) {
		++m_commandTable.m_nGpuProductionPasses;
	} else {
		++m_commandTable.m_nCpuReferencePasses;
	}
	if ( rGpuVisibility ) {
		if ( !buildGpuPacketTopology( rScene ) ) {
			setError( "Failed to build Terrain3D GPU instance packet topology" );
			return false;
		}
		if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT && !buildGpuTransparentOrder( rContext.m_cameraTransform.origin ) ) {
			if ( m_lastError.is_empty() ) {
				setError( "Failed to build Terrain3D GPU transparent packet order" );
			}
			return false;
		}
		return true;
	}
	const bool rebuild_packets = bShadowPass || m_nForwardPreparedScenePass != rContext.m_nScenePass
	        || m_nForwardPreparedViewportKey != rContext.m_nViewportKey || m_nForwardPreparedSceneGeneration != rScene.m_nGeneration
	        || m_forwardPreparedPacketPassKind != rContext.m_passKind || m_nForwardPreparedVisibilityMask != nVisibilityMask;
	if ( !rebuild_packets ) {
		return true;
	}
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	const bool bHlodVisibilityReady = true;
#else
	const bool bHlodVisibilityReady = rContext.m_nViewCount != 1u || dispatchHlodCull( nVisibilityMask );
#endif
	if ( !buildPackets( rScene,
	             rContext.m_szViewProjection,
	             rContext.m_nViewCount,
	             rContext.m_cameraTransform.origin,
	             nVisibilityMask,
	             bShadowPass,
	             false ) ) {
		if ( m_lastError.is_empty() ) {
			setError( "Failed to build Terrain3D debug/reference instance packets" );
		}
		return false;
	}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !bShadowPass && m_commandTable.m_nPacketBuildInputCells > 0u && m_commandTable.m_nResolvedMeshCells == 0u
	        && m_commandTable.m_nUnresolvedMeshCells > 0u ) {
		setError( String( "RD minimal instance prepare could not resolve any published mesh source keys; input_cells=" )
		        + String::num_int64( m_commandTable.m_nPacketBuildInputCells ) + String( ", unresolved_cells=" )
		        + String::num_int64( m_commandTable.m_nUnresolvedMeshCells ) + String( ", mesh_lods=" )
		        + String::num_int64( m_geometryArena.m_meshCache.getLodCount() ) + String( ", mesh_surfaces=" )
		        + String::num_int64( m_geometryArena.m_meshCache.getSurfaceCount() ) );
		return false;
	}
#endif
	if ( bShadowPass ? !uploadShadowVisibility() : !uploadVisibility() ) {
		if ( m_lastError.is_empty() ) {
			setError( "Failed to upload Terrain3D debug/reference instance visibility" );
		}
		return false;
	}
	if ( !bShadowPass && !bHlodVisibilityReady ) {
		setError( "Failed to prepare Terrain3D HLOD visibility for the reference instance path" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::updateForwardFrameBuffer( const t3dRdForwardPassContext_t &rContext, const bool bTemporalHistoryValid ) {
	t3dRdInstanceFrame_t frame;
	memset( &frame, 0, sizeof( frame ) );
	const Projection view_matrix = Projection( rContext.m_cameraTransform.affine_inverse() );
	for ( int column = 0; column < 4; ++column ) {
		const Vector4 vm = view_matrix[column];
		frame.m_szView[column * 4 + 0] = ( float )( vm.x );
		frame.m_szView[column * 4 + 1] = ( float )( vm.y );
		frame.m_szView[column * 4 + 2] = ( float )( vm.z );
		frame.m_szView[column * 4 + 3] = ( float )( vm.w );
	}
	const bool history_valid = rContext.m_bMotionVectorsEnabled && bTemporalHistoryValid;
	for ( uint32_t view_index = 0u; view_index < s_nRdForwardMaxViews; ++view_index ) {
		const uint32_t source = MIN( view_index, rContext.m_nViewCount - 1u );
		for ( int column = 0; column < 4; ++column ) {
			const Vector4 vp = rContext.m_szViewProjection[source][column];
			frame.m_szViewProjection[view_index][column * 4 + 0] = ( float )( vp.x );
			frame.m_szViewProjection[view_index][column * 4 + 1] = ( float )( vp.y );
			frame.m_szViewProjection[view_index][column * 4 + 2] = ( float )( vp.z );
			frame.m_szViewProjection[view_index][column * 4 + 3] = ( float )( vp.w );
			const Vector4 previous_vp =
			        ( history_valid ? rContext.m_szPreviousViewProjection[source] : rContext.m_szViewProjection[source] )[column];
			frame.m_szPreviousViewProjection[view_index][column * 4 + 0] = ( float )( previous_vp.x );
			frame.m_szPreviousViewProjection[view_index][column * 4 + 1] = ( float )( previous_vp.y );
			frame.m_szPreviousViewProjection[view_index][column * 4 + 2] = ( float )( previous_vp.z );
			frame.m_szPreviousViewProjection[view_index][column * 4 + 3] = ( float )( previous_vp.w );
		}
		Vector3 eye = rContext.m_cameraTransform.origin;
		if ( rContext.m_nViewCount > 1u ) {
			eye += rContext.m_cameraTransform.basis.xform( rContext.m_szEyeOffset[source] );
		}
		frame.m_szCamera[view_index][0] = ( float )( eye.x );
		frame.m_szCamera[view_index][1] = ( float )( eye.y );
		frame.m_szCamera[view_index][2] = ( float )( eye.z );
	}
	frame.m_szMotionJitter[0] = ( float )( rContext.m_taaJitter.x );
	frame.m_szMotionJitter[1] = ( float )( rContext.m_taaJitter.y );
	frame.m_szMotionJitter[2] = ( float )( history_valid ? rContext.m_previousTaaJitter.x : rContext.m_taaJitter.x );
	frame.m_szMotionJitter[3] = ( float )( history_valid ? rContext.m_previousTaaJitter.y : rContext.m_taaJitter.y );
	frame.m_szMotionParams[0] = history_valid ? 1.0f : 0.0f;
	frame.m_szMotionParams[1] = m_bMotionDebug ? 1.0f : 0.0f;
	frame.m_szMotionParams[2] = ( float )( rContext.m_nViewCount );
	copyStylizedSunGlobals( frame.m_szNomadSunShape, frame.m_szNomadSunDiffuse, frame.m_szNomadSunSpecular, frame.m_szNomadSunRim );
	memcpy( m_uploadState.m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	if ( !m_uploadState.m_hFrameBuffer.writePackedTracked( m_context, 0u, m_uploadState.m_frameBytes, sizeof( frame ), T3D_GPU_HOT_FRAME ) ) {
		setError( "Failed to update Terrain3D native Forward+ instance frame buffer" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::runForwardCullParity( const t3dRdForwardPassContext_t &rContext,
        const t3dRdInstanceSceneSnapshot_t &rScene,
        const uint32_t nVisibilityMask,
        const bool bShadowPass,
        const bool bGpuVisibility,
        bool &rCullDispatchedForParity ) {
	rCullDispatchedForParity = false;
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	bool run_cull_parity = m_visibility.m_bCullingComparisonEnabled;
	const uint64_t parity_view_key = t3dRdInstanceViewKey( rContext );
	if ( run_cull_parity && !bShadowPass && m_visibility.m_nCullParityMainRenderFrame == rContext.m_nRenderFrame
	        && m_visibility.m_nCullParityMainViewportKey == rContext.m_nViewportKey
	        && m_visibility.m_nCullParityMainViewKey == parity_view_key
	        && m_visibility.m_nCullParityMainSceneGeneration == rScene.m_nGeneration
	        && m_visibility.m_nCullParityMainVisibilityMask == nVisibilityMask
	        && m_visibility.m_nCullParityMainViewCount == rContext.m_nViewCount ) {
		run_cull_parity = false;
		++m_visibility.m_nCullParityMainReuses;
	}
	if ( run_cull_parity ) {
		if ( !buildGpuPacketTopology( rScene ) ) {
			setError( "Failed to build Terrain3D P9 GPU visibility topology" );
			return false;
		}
		if ( !dispatchInstanceCull( nVisibilityMask, rContext.m_nViewCount, bShadowPass, false ) ) {
			setError( bShadowPass ? "Failed to dispatch Terrain3D P9 GPU shadow visibility parity"
			                      : "Failed to dispatch Terrain3D P9 GPU visibility parity" );
			return false;
		}
		rCullDispatchedForParity = bShadowPass || !m_visibility.m_bHiZReadyForFrame || rContext.m_nViewCount != 1u;
		if ( !compareGpuCpuInstanceCulling( rScene,
		             rContext.m_szViewProjection,
		             rContext.m_nViewCount,
		             rContext.m_cameraTransform.origin,
		             nVisibilityMask,
		             bShadowPass ) ) {
			setError(
			        bShadowPass ? "Terrain3D P9 GPU/CPU shadow visibility mismatch" : "Terrain3D P9 GPU/CPU instance visibility mismatch" );
			return false;
		}
		if ( !bShadowPass ) {
			m_visibility.m_nCullParityMainRenderFrame = rContext.m_nRenderFrame;
			m_visibility.m_nCullParityMainViewportKey = rContext.m_nViewportKey;
			m_visibility.m_nCullParityMainViewKey = parity_view_key;
			m_visibility.m_nCullParityMainSceneGeneration = rScene.m_nGeneration;
			m_visibility.m_nCullParityMainVisibilityMask = nVisibilityMask;
			m_visibility.m_nCullParityMainViewCount = rContext.m_nViewCount;
		}
		if ( !bGpuVisibility ) {
			invalidateGpuCullTopologyAfterReferenceBuild();
#if T3D_INSTANCER_RD_PORT
			const bool hlod_visibility_ready = true;
#else
			const bool hlod_visibility_ready = rContext.m_nViewCount != 1u || dispatchHlodCull( nVisibilityMask );
#endif
			if ( !buildPackets( rScene,
			             rContext.m_szViewProjection,
			             rContext.m_nViewCount,
			             rContext.m_cameraTransform.origin,
			             nVisibilityMask,
			             bShadowPass,
			             false )
			        || ( bShadowPass ? !uploadShadowVisibility() : !uploadVisibility() ) || ( !bShadowPass && !hlod_visibility_ready ) ) {
				setError( "Failed to restore Terrain3D CPU reference visibility after P9 parity dispatch" );
				return false;
			}
		}
	}
#else
	( void )rContext;
	( void )rScene;
	( void )nVisibilityMask;
	( void )bShadowPass;
	( void )bGpuVisibility;
#endif
	return true;
}

bool Terrain3DRdInstanceRenderer::dispatchForwardGpuVisibility( const t3dRdForwardPassContext_t &rContext,
        const t3dRdInstanceSceneSnapshot_t &rScene,
        const uint32_t nVisibilityMask,
        const bool bShadowPass,
        const bool bGpuVisibility,
        const bool bCullDispatchedForParity ) {
	if ( !bGpuVisibility ) {
		return true;
	}
	if ( bShadowPass ) {
		if ( !bCullDispatchedForParity && !dispatchInstanceCull( nVisibilityMask, rContext.m_nViewCount, true ) ) {
			setError( "Failed to dispatch Terrain3D GPU shadow instance visibility" );
			return false;
		}
		return true;
	}
	const uint64_t view_key = t3dRdInstanceViewKey( rContext );
	const bool reuse_cull = m_visibility.m_nGpuCullMainRenderFrame == rContext.m_nRenderFrame
	        && m_visibility.m_nGpuCullMainViewportKey == rContext.m_nViewportKey && m_visibility.m_nGpuCullMainViewKey == view_key
	        && m_visibility.m_nGpuCullMainSceneGeneration == rScene.m_nGeneration
	        && m_visibility.m_nGpuCullMainVisibilityMask == nVisibilityMask
	        && m_visibility.m_nGpuCullMainViewCount == rContext.m_nViewCount;
	if ( bCullDispatchedForParity || reuse_cull ) {
		if ( reuse_cull && !bCullDispatchedForParity ) {
			++m_visibility.m_nGpuCullReuses;
		}
		return true;
	}
	if ( !dispatchInstanceCull( nVisibilityMask, rContext.m_nViewCount, false ) ) {
		setError( "Failed to dispatch Terrain3D GPU instance visibility" );
		return false;
	}
	m_visibility.m_nGpuCullMainRenderFrame = rContext.m_nRenderFrame;
	m_visibility.m_nGpuCullMainViewportKey = rContext.m_nViewportKey;
	m_visibility.m_nGpuCullMainViewKey = view_key;
	m_visibility.m_nGpuCullMainSceneGeneration = rScene.m_nGeneration;
	m_visibility.m_nGpuCullMainVisibilityMask = nVisibilityMask;
	m_visibility.m_nGpuCullMainViewCount = rContext.m_nViewCount;
	return true;
}

bool Terrain3DRdInstanceRenderer::prepareForwardLighting( const t3dRdForwardPassContext_t &rContext,
        const uint32_t nVisibilityMask,
        const t3dRdForwardShaderVariant_t variant,
        const bool bViewportReconfigured ) {
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION
	        || rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT ) {
		if ( bViewportReconfigured ) {
			m_forwardLighting.invalidateBorrowedState();
		}
		if ( !m_forwardLighting.prepare( rContext, nVisibilityMask )
		        || !m_forwardLighting.ensureSet( m_hszForwardShaders[variant].rid() ).is_valid() ) {
			setError( "Failed to prepare Terrain3D Forward+ instance/HLOD native lighting" );
			return false;
		}
	}
#else
	( void )nVisibilityMask;
	( void )bViewportReconfigured;
#endif
	return ensureForwardPipelines( rContext, variant );
}

void Terrain3DRdInstanceRenderer::commitForwardPreparedState( const t3dRdForwardPassContext_t &rContext,
        const t3dRdInstanceSceneSnapshot_t &rScene,
        const uint32_t nVisibilityMask,
        const bool bShadowPass,
        const bool bGpuVisibility ) {
#if T3D_INSTANCER_HIZ_OCCLUSION
	if ( !bShadowPass ) {
		m_visibility.m_nHiZStableSceneGeneration = rScene.m_nGeneration;
	}
#endif
	m_nForwardPreparedScenePass = rContext.m_nScenePass;
	m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
	m_nForwardPreparedSceneGeneration = rScene.m_nGeneration;
	m_forwardPreparedPacketPassKind = rContext.m_passKind;
	m_nForwardPreparedVisibilityMask = nVisibilityMask;
	m_bForwardPreparedHasDraws = bShadowPass ? !m_commandTable.m_shadowPackets.empty() : !m_commandTable.m_drawPackets.empty();
	m_bForwardPreparedGpuVisibility = bGpuVisibility;
	m_lastError = String();
}

bool Terrain3DRdInstanceRenderer::prepareForwardPass( const t3dRdForwardPassContext_t &rContext,
        const t3dRdMeshSourceSnapshot_t *const pMeshes,
        const t3dRdInstanceSceneSnapshot_t *const pScene,
        const uint32_t nVisibilityMask,
        const bool bTemporalHistoryValid,
        const bool bViewportReconfigured ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE || !pScene ) {
		return false;
	}
	if ( rContext.m_nViewCount == 0u || rContext.m_nViewCount > s_nRdForwardMaxViews ) {
		setError( "Terrain3D native instance view count exceeds bridge bounds" );
		return false;
	}
	t3dRdForwardShaderVariant_t variant;
	if ( !t3dRdSelectForwardVariant( rContext, T3D_RD_FORWARD_VARIANT_ALLOW_TRANSPARENT, variant ) ) {
		setError( String( "Terrain3D instance renderer does not support Forward+ pass/mode: pass=" )
		        + String::num_int64( rContext.m_passKind ) + String( ", mode=" ) + String::num_int64( rContext.m_nativePassMode ) );
		return false;
	}
	const bool scene_empty = pScene->m_instances.empty() && pScene->m_cells.empty();
	const bool scene_incomplete = pScene->m_instances.empty() != pScene->m_cells.empty();
	const bool has_renderable_meshes =
	        pMeshes && !pMeshes->m_vertexBytes.is_empty() && !pMeshes->m_lods.empty() && !pMeshes->m_surfaces.empty();
	if ( scene_empty ) {
		m_commandTable.m_drawPackets.clear();
		m_commandTable.m_depthPackets.clear();
		m_commandTable.m_shadowPackets.clear();
		m_occluderDepth.m_packets.clear();
		m_occluderDepth.m_visibleIndices.clear();
		m_occluderDepth.m_nVisibleIndexCount = 0u;
		m_occluderDepth.m_bDepthValid = false;
		m_commandTable.m_nPacketBuildInputCells = 0u;
		m_commandTable.m_nResolvedMeshCells = 0u;
		m_commandTable.m_nUnresolvedMeshCells = 0u;
		m_visibility.m_nVisibleIndexCount = 0u;
		m_visibility.m_nShadowVisibleIndexCount = 0u;
		m_commandTable.m_nMainCompatibleCommandCount = 0u;
		m_commandTable.m_nShadowCompatibleCommandCount = 0u;
		m_visibility.m_hlodDescriptors.clear();
		m_commandTable.m_nVisibleInstances = 0u;
		m_commandTable.m_nStaticInstances = 0u;
		m_nForwardPreparedScenePass = rContext.m_nScenePass;
		m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
		m_nForwardPreparedSceneGeneration = pScene->m_nGeneration;
		m_bForwardPreparedHasDraws = false;
		m_bForwardPreparedGpuVisibility = false;
		m_commandTable.m_nTransparentPacketOrderCount = 0u;
		m_bLastForwardPipelineKeyValid = false;
		m_lastError = String();
		return true;
	}
	if ( scene_incomplete ) {
		setError( String( "RD instance publication is structurally incomplete; records=" ) + String::num_int64( pScene->m_instances.size() )
		        + String( ", cells=" ) + String::num_int64( pScene->m_cells.size() ) );
		return false;
	}
	if ( !has_renderable_meshes ) {
		const int64_t vertex_bytes = pMeshes ? pMeshes->m_vertexBytes.size() : 0;
		const int64_t lods = pMeshes ? ( int64_t )( pMeshes->m_lods.size() ) : 0;
		const int64_t surfaces = pMeshes ? ( int64_t )( pMeshes->m_surfaces.size() ) : 0;
		setError( String( "RD instance publication contains renderable scene records but no renderable mesh publication; records=" )
		        + String::num_int64( pScene->m_instances.size() ) + String( ", cells=" ) + String::num_int64( pScene->m_cells.size() )
		        + String( ", vertex_bytes=" ) + String::num_int64( vertex_bytes ) + String( ", lods=" ) + String::num_int64( lods )
		        + String( ", surfaces=" ) + String::num_int64( surfaces ) );
		m_bForwardPreparedHasDraws = false;
		m_bLastForwardPipelineKeyValid = false;
		return false;
	}
	const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	bool hiz_source_ready = false;
	bool gpu_visibility = false;
	bool cull_dispatched_for_parity = false;
	if ( !prepareForwardResources( rContext, pMeshes, pScene, variant, shadow_pass, hiz_source_ready )
	        || !prepareForwardVisibility(
	                rContext, *pScene, nVisibilityMask, bTemporalHistoryValid, shadow_pass, hiz_source_ready, gpu_visibility )
	        || !updateForwardFrameBuffer( rContext, bTemporalHistoryValid ) ) {
		return false;
	}
	if ( !shadow_pass && rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_nViewCount == 1u
	        && ( !buildOccluderDepthPackets( rContext, *pScene, nVisibilityMask ) || !uploadOccluderDepthVisibility()
	                || !rasterOccluderDepth( rContext, *pScene ) ) ) {
		return false;
	}
	if ( !runForwardCullParity( rContext, *pScene, nVisibilityMask, shadow_pass, gpu_visibility, cull_dispatched_for_parity )
	        || !dispatchForwardGpuVisibility( rContext, *pScene, nVisibilityMask, shadow_pass, gpu_visibility, cull_dispatched_for_parity )
	        || !prepareForwardLighting( rContext, nVisibilityMask, variant, bViewportReconfigured ) ) {
		return false;
	}
	if ( !shadow_pass
	        && ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION
	                || rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT ) ) {
		requestPublishedMaterialResidencyHot( ( uint32_t )rContext.m_nRenderFrame );
	}
	commitForwardPreparedState( rContext, *pScene, nVisibilityMask, shadow_pass, gpu_visibility );
	return true;
}

bool Terrain3DRdInstanceRenderer::drawForwardView( const t3dRdForwardPassContext_t &rContext ) {
	if ( !m_bForwardPreparedHasDraws ) {
		return true;
	}
	if ( rContext.m_nDrawList < 0 || !m_bLastForwardPipelineKeyValid ) {
		return false;
	}
	t3dRdForwardShaderVariant_t variant;
	if ( !t3dRdSelectForwardVariant( rContext, T3D_RD_FORWARD_VARIANT_ALLOW_TRANSPARENT, variant ) || variant != m_lastForwardPipelineKey.m_variant
	        || rContext.m_nFramebufferFormat != m_lastForwardPipelineKey.m_nFramebufferFormat
	        || rContext.m_nViewCount != m_lastForwardPipelineKey.m_nViewCount || rContext.m_passKind != m_lastForwardPipelineKey.m_passKind
	        || rContext.m_bReverseCull != m_lastForwardPipelineKey.m_bReverseCull
	        || ( ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION )
	                   && rContext.m_bDepthPrepassActive )
	                != m_lastForwardPipelineKey.m_bDepthPrepassActive ) {
		return false;
	}
	const t3dRdInstanceForwardPipelineEntry_t *const entry = findForwardPipeline( m_lastForwardPipelineKey );
	if ( !entry ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const int64_t draw_list = rContext.m_nDrawList;
	const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
	const eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &packets =
	        shadow_pass ? m_commandTable.m_shadowPackets : m_commandTable.m_drawPackets;
	const RID instance_set =
	        shadow_pass ? m_descriptorState.m_hszForwardShadowUniformSets[variant] : m_descriptorState.m_hszForwardUniformSets[variant];
	if ( !instance_set.is_valid() ) {
		return false;
	}
	rd->draw_list_bind_uniform_set( draw_list, instance_set, 0u );
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	const RID material_vt_set = m_descriptorState.m_hszForwardMaterialVtUniformSets[variant];
	if ( !material_vt_set.is_valid() ) {
		setError( "RD Forward+ instance invariant VT set is invalid" );
		return false;
	}
	rd->draw_list_bind_uniform_set( draw_list, material_vt_set, 1u );
	++m_descriptorState.m_nVtDescriptorBindsInstancerPass;
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || transparent_pass ) {
		const RID lighting_set = m_forwardLighting.getSet( m_hszForwardShaders[variant].rid() );
		if ( !lighting_set.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_uniform_set( draw_list, lighting_set, 2u );
	}
#endif
#if T3D_INSTANCER_SHARED_MESH_ARENA && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( m_bForwardPreparedGpuVisibility ) {
		rd->draw_list_bind_vertex_array( draw_list, m_geometryArena.m_meshCache.getVertexArray() );
	}
#endif
	uint32_t bound_pipeline = UINT32_MAX;
	const bool gpu_visibility = m_bForwardPreparedGpuVisibility;
	const uint32_t compatible_command_count =
	        shadow_pass ? m_commandTable.m_nShadowCompatibleCommandCount : m_commandTable.m_nMainCompatibleCommandCount;
#if T3D_INSTANCER_SHARED_MESH_ARENA && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( gpu_visibility && !transparent_pass && compatible_command_count > 0u ) {
		const RID index_array = m_geometryArena.m_meshCache.getIndexArray();
		if ( !index_array.is_valid() ) {
			setError( "RD P32 compatible one-submit lane is missing the shared U32 index arena" );
			return false;
		}
		rd->draw_list_bind_render_pipeline( draw_list, entry->m_hszPipelines[0] );
		bound_pipeline = 0u;
		rd->draw_list_bind_index_array( draw_list, index_array );
		const uint32_t nActiveAllocationCount = m_pVirtualTexture != NULL ? m_pVirtualTexture->getActiveAllocationCount() : 0u;
		const uint32_t nActivePageCount = m_pVirtualTexture != NULL ? m_pVirtualTexture->getActiveVirtualPageCount() : 0u;
		t3dRdInstancePush_t push{ UINT16_MAX, UINT16_MAX, 0u, 0u, UINT32_MAX, 1u, nActiveAllocationCount, nActivePageCount };
		memcpy( m_uploadState.m_pushBytes.ptrw(), &push, sizeof( push ) );
		rd->draw_list_set_push_constant( draw_list, m_uploadState.m_pushBytes, sizeof( push ) );
		rd->draw_list_draw_indirect( draw_list,
		        true,
		        shadow_pass ? m_commandTable.m_hIndirectBuffer : m_commandTable.m_hIndirectBuffer,
		        shadow_pass ? m_commandTable.m_shadowIndirectSlice.m_nOffsetBytes : m_commandTable.m_mainIndirectSlice.m_nOffsetBytes,
		        compatible_command_count,
		        sizeof( t3dRdIndexedIndirectCommand_t ) );
		m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_INSTANCES, compatible_command_count );
		++m_commandTable.m_nGpuProductionIndirectSubmissions;
		++m_commandTable.m_nDraws;
		switch ( rContext.m_passKind ) {
			case T3D_RD_FORWARD_PASS_DEPTH:
				++m_commandTable.m_nDepthSubmittedDraws;
				++m_commandTable.m_nOneSubmitDepth;
				break;
			case T3D_RD_FORWARD_PASS_OPAQUE:
				++m_commandTable.m_nOpaqueSubmittedDraws;
				++m_commandTable.m_nOneSubmitOpaque;
				break;
			case T3D_RD_FORWARD_PASS_SHADOW:
				++m_commandTable.m_nShadowSubmittedDraws;
				++m_commandTable.m_nOneSubmitShadow;
				break;
			case T3D_RD_FORWARD_PASS_MOTION:
				++m_commandTable.m_nMotionSubmittedDraws;
				++m_commandTable.m_nOneSubmitMotion;
				break;
			default:
				break;
		}
	}
#endif
	const uint32_t packet_iteration_count =
	        transparent_pass && gpu_visibility ? m_commandTable.m_nTransparentPacketOrderCount : ( uint32_t )packets.size();
	const uint32_t first_packet_iteration = gpu_visibility && !transparent_pass ? compatible_command_count : 0u;
	for ( uint32_t iIteration = first_packet_iteration; iIteration < packet_iteration_count; ++iIteration ) {
		const uint32_t i = transparent_pass && gpu_visibility ? m_commandTable.m_transparentPacketOrder[iIteration] : iIteration;
		if ( i >= packets.size() ) {
			setError( "RD instance packet order referenced an invalid packet" );
			return false;
		}
		const t3dRdInstanceDrawPacket_t &packet = packets[i];
		bool depth_prepass = false;
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
		const bool alpha_blend = ( packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u;
		if ( shadow_pass ) {
			if ( ( packet.m_nVisibilityLayers & rContext.m_nShadowCasterMask ) == 0u
			        || ( alpha_blend && ( packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) == 0u ) ) {
				continue;
			}
		} else if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
			if ( alpha_blend && ( packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) == 0u ) {
				continue;
			}
			depth_prepass = alpha_blend;
		} else if ( transparent_pass ) {
			if ( !alpha_blend ) {
				continue;
			}
		} else if ( alpha_blend ) {
			continue;
		}
#else
		( void )shadow_pass;
		( void )transparent_pass;
#endif
		if ( packet.m_nVisibleCount == 0u ) {
			continue;
		}
#if T3D_INSTANCER_SHARED_MESH_ARENA && !T3D_INSTANCER_MINIMAL_RENDER_PATH
		RID index_array;
		if ( gpu_visibility ) {
			index_array = m_geometryArena.m_meshCache.getIndexArray();
			if ( !index_array.is_valid() ) {
				setError( "RD shared-arena GPU submit is missing an index arena view" );
				return false;
			}
		} else {
			const RID vertex_array = m_geometryArena.m_meshCache.getSurfaceVertexArray( packet.m_nSurfaceSlot );
			index_array = m_geometryArena.m_meshCache.getSurfaceIndexArray( packet.m_nSurfaceSlot );
			if ( !vertex_array.is_valid() || !index_array.is_valid() ) {
				setError( String( "RD shared-arena CPU reference is missing a surface view; surface=" )
				        + String::num_int64( packet.m_nSurfaceSlot ) + String( ", surface_views=" )
				        + String::num_int64( m_geometryArena.m_meshCache.getSurfaceViewCount() ) );
				return false;
			}
			rd->draw_list_bind_vertex_array( draw_list, vertex_array );
		}
#else
		const RID vertex_array = m_geometryArena.m_meshCache.getSurfaceVertexArray( packet.m_nSurfaceSlot );
		const RID index_array = m_geometryArena.m_meshCache.getSurfaceIndexArray( packet.m_nSurfaceSlot );
		if ( !vertex_array.is_valid() || !index_array.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_vertex_array( draw_list, vertex_array );
#endif
		uint32_t pipeline_index = 0u;
		if ( ( packet.m_nFlags & ( T3D_RD_INSTANCE_DOUBLE_SIDED | T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED ) ) != 0u ) {
			pipeline_index = 2u;
		} else if ( ( packet.m_nFlags & T3D_RD_INSTANCE_CULL_FRONT ) != 0u ) {
			pipeline_index = 1u;
		}
		if ( pipeline_index != bound_pipeline ) {
			rd->draw_list_bind_render_pipeline( draw_list, entry->m_hszPipelines[pipeline_index] );
			bound_pipeline = pipeline_index;
		}
		rd->draw_list_bind_index_array( draw_list, index_array );
		uint32_t draw_flags = packet.m_nFlags;
		if ( !shadow_pass && ( draw_flags & T3D_RD_INSTANCE_FADE_ACTIVE ) != 0u && packet.m_fFadeMargin > 0.0f ) {
			const float distance_sq = ( float )packet.m_cullBounds.get_center().distance_squared_to( rContext.m_cameraTransform.origin );
			const float begin_fade_end = packet.m_fLodBegin + packet.m_fFadeMargin;
			const float end_fade_begin = MAX( 0.0f, packet.m_fLodEnd - packet.m_fFadeMargin );
			const bool near_begin = packet.m_fLodBegin > 0.001f && distance_sq < begin_fade_end * begin_fade_end;
			const bool near_end = packet.m_fLodEnd > 0.001f && distance_sq > end_fade_begin * end_fade_begin;
			float fade = 1.0f;
			if ( near_begin || near_end ) {
				const float distance = sqrt( distance_sq );
				if ( near_begin ) {
					fade = MIN( fade, ( distance - packet.m_fLodBegin ) / packet.m_fFadeMargin );
				}
				if ( near_end ) {
					fade = MIN( fade, ( packet.m_fLodEnd - distance ) / packet.m_fFadeMargin );
				}
			}
			const uint32_t fade_unorm = ( uint32_t )lround( CLAMP( fade, 0.0f, 1.0f ) * 65535.0f );
			draw_flags &= ~T3D_RD_INSTANCE_FADE_MASK;
			draw_flags |= ( fade_unorm << T3D_RD_INSTANCE_FADE_SHIFT ) & T3D_RD_INSTANCE_FADE_MASK;
		}
		if ( depth_prepass ) {
			draw_flags |= T3D_RD_INSTANCE_DEPTH_PREPASS_PASS;
		}
		const uint32_t nActiveAllocationCount = m_pVirtualTexture != NULL ? m_pVirtualTexture->getActiveAllocationCount() : 0u;
		const uint32_t nActivePageCount = m_pVirtualTexture != NULL ? m_pVirtualTexture->getActiveVirtualPageCount() : 0u;
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		t3dRdInstancePush_t push{
			UINT16_MAX, UINT16_MAX, packet.m_nFirstVisible, 0u, UINT32_MAX, 1u, nActiveAllocationCount, nActivePageCount
		};
#else
		t3dRdInstancePush_t push{ packet.m_nMaterial,
			packet.m_nOverlayMaterial,
			gpu_visibility ? 0u : packet.m_nFirstVisible,
			draw_flags,
			packet.m_nVisibilityLayers,
			0u,
			nActiveAllocationCount,
			nActivePageCount };
#endif
		memcpy( m_uploadState.m_pushBytes.ptrw(), &push, sizeof( push ) );
		rd->draw_list_set_push_constant( draw_list, m_uploadState.m_pushBytes, sizeof( push ) );
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		rd->draw_list_draw( draw_list, true, packet.m_nVisibleCount, 0u );
		++m_commandTable.m_nCpuReferenceDirectSubmissions;
#else
		if ( gpu_visibility ) {
			rd->draw_list_draw_indirect( draw_list,
			        true,
			        shadow_pass ? m_commandTable.m_hIndirectBuffer : m_commandTable.m_hIndirectBuffer,
			        ( shadow_pass ? m_commandTable.m_shadowIndirectSlice.m_nOffsetBytes : m_commandTable.m_mainIndirectSlice.m_nOffsetBytes ) + i * sizeof( t3dRdIndexedIndirectCommand_t ),
			        1u,
			        sizeof( t3dRdIndexedIndirectCommand_t ) );
			m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_INSTANCES, 1u );
			++m_commandTable.m_nGpuProductionIndirectSubmissions;
			if ( !transparent_pass ) {
				++m_commandTable.m_nOneSubmitFallbackDraws;
			}
		} else {
			rd->draw_list_draw( draw_list, true, packet.m_nVisibleCount, 0u );
			++m_commandTable.m_nCpuReferenceDirectSubmissions;
		}
#endif
		++m_commandTable.m_nDraws;
		switch ( rContext.m_passKind ) {
			case T3D_RD_FORWARD_PASS_DEPTH:
				++m_commandTable.m_nDepthSubmittedDraws;
				break;
			case T3D_RD_FORWARD_PASS_OPAQUE:
				++m_commandTable.m_nOpaqueSubmittedDraws;
				break;
			case T3D_RD_FORWARD_PASS_SHADOW:
				++m_commandTable.m_nShadowSubmittedDraws;
				break;
			case T3D_RD_FORWARD_PASS_MOTION:
				++m_commandTable.m_nMotionSubmittedDraws;
				break;
			case T3D_RD_FORWARD_PASS_TRANSPARENT:
				++m_commandTable.m_nTransparentSubmittedDraws;
				break;
			default:
				break;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::renderForwardPass( const t3dRdForwardPassContext_t &rContext ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || m_nForwardPreparedScenePass != rContext.m_nScenePass
	        || m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
		return false;
	}
	if ( !drawForwardView( rContext ) ) {
		setError( "Failed to submit Terrain3D instances/HLOD onto native Forward+ draw list" );
		return false;
	}
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( m_bForwardPreparedGpuVisibility && !m_commandTable.m_bGpuProductionSubmittedOnce && m_bForwardPreparedHasDraws ) {
		m_commandTable.m_bGpuProductionSubmittedOnce = true;
		UtilityFunctions::print( "[RD validation] Instance GPU cull production submission passed" );
	}
#endif
#if T3D_INSTANCER_SHARED_MESH_ARENA && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( m_bForwardPreparedGpuVisibility && !m_geometryArena.m_bSharedMeshArenaSubmittedOnce && m_bForwardPreparedHasDraws ) {
		m_geometryArena.m_bSharedMeshArenaSubmittedOnce = true;
		UtilityFunctions::print( "[RD validation] Instance shared mesh arena submission passed" );
	}
#endif
#if T3D_INSTANCER_HIZ_OCCLUSION && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( m_bForwardPreparedGpuVisibility && m_visibility.m_bHiZReadyForFrame && rContext.m_nViewCount == 1u
	        && m_visibility.m_nHiZCellDescriptors > 0u && !m_visibility.m_bHiZOcclusionSubmittedOnce ) {
		m_visibility.m_bHiZOcclusionSubmittedOnce = true;
		UtilityFunctions::print( "[RD validation] Instance P12 hierarchical Hi-Z occlusion submission passed" );
	}
#endif
	++m_uploadState.m_nFrames;
	m_lastError = String();
	return true;
}

size_t Terrain3DRdInstanceRenderer::getRetainedCpuBytes( void ) const {
	size_t bytes = 0u;
	bytes += m_visibility.m_visibleIndices.capacity() * sizeof( uint32_t );
	bytes += m_visibility.m_shadowVisibleIndices.capacity() * sizeof( uint32_t );
	bytes += m_visibility.m_visibleDrawSlots.capacity() * sizeof( uint32_t );
	bytes += m_visibility.m_shadowVisibleDrawSlots.capacity() * sizeof( uint32_t );
	bytes += m_visibility.m_visibleLayers.capacity() * sizeof( uint32_t );
	bytes += m_visibility.m_shadowVisibleLayers.capacity() * sizeof( uint32_t );
	bytes += m_commandTable.m_fixedDrawVisibleBases.capacity() * sizeof( uint32_t );
	bytes += m_commandTable.m_fixedDrawVisibleCapacities.capacity() * sizeof( uint32_t );
	bytes += m_commandTable.m_shadowFixedDrawVisibleBases.capacity() * sizeof( uint32_t );
	bytes += m_commandTable.m_shadowFixedDrawVisibleCapacities.capacity() * sizeof( uint32_t );
	bytes += m_uploadState.m_motionHistoryRecords.capacity() * sizeof( t3dRdInstanceRecord_t );
	bytes += m_uploadState.m_motionHistoryCells.capacity() * sizeof( t3dRdInstanceCell_t );
	bytes += m_uploadState.m_motionPreviousRecords.capacity() * sizeof( t3dRdInstanceRecord_t );
	bytes += m_uploadState.m_motionCurrentOrder.capacity() * sizeof( uint32_t );
	bytes += m_uploadState.m_motionPreviousOrder.capacity() * sizeof( uint32_t );
	bytes += m_uploadState.m_motionDirtyRanges.capacity() * sizeof( t3dRdInstanceDirtyRange_t );
	bytes += m_commandTable.m_drawPackets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_commandTable.m_depthPackets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_commandTable.m_shadowPackets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_occluderDepth.m_packets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_occluderDepth.m_visibleIndices.capacity() * sizeof( uint32_t );
	bytes += m_occluderDepth.m_visibleBytes.size();
	bytes += m_commandTable.m_alphaPackets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_commandTable.m_alphaSortScratch.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_commandTable.m_transparentPacketOrder.capacity() * sizeof( uint32_t );
	bytes += m_commandTable.m_gpuPacketBoundValid.capacity() * sizeof( uint8_t );
	bytes += m_commandTable.m_packetBuildSlots.capacity() * sizeof( t3dRdPacketBuildSlot_t );
	bytes += m_commandTable.m_packetBuildEntries.capacity() * sizeof( t3dRdPacketBuildEntry_t );
	bytes += m_visibility.m_hlodDescriptors.capacity() * sizeof( t3dRdHlodCullDescriptor_t );
	bytes += m_visibility.m_instanceCullDescriptors.capacity() * sizeof( t3dRdInstanceCullDescriptor_t );
	bytes += m_visibility.m_shadowInstanceCullDescriptors.capacity() * sizeof( t3dRdInstanceCullDescriptor_t );
	bytes += m_visibility.m_instanceHiZRegionDescriptors.capacity() * sizeof( t3dRdInstanceHiZRegionDescriptor_t );
	bytes += m_visibility.m_instanceHiZRegionKeys.capacity() * sizeof( uint64_t );
	bytes += m_visibility.m_instanceCullCellSpans.capacity() * sizeof( t3dRdInstanceDescriptorSpan_t );
	bytes += m_visibility.m_shadowInstanceCullCellSpans.capacity() * sizeof( t3dRdInstanceDescriptorSpan_t );
	bytes += m_visibility.m_cullParityExpectedIndices.capacity() * sizeof( uint32_t );
	bytes += m_visibility.m_cullParityActualIndices.capacity() * sizeof( uint32_t );
	bytes += m_visibility.m_cullParityCommandCounts.capacity() * sizeof( uint32_t );
	bytes += m_visibility.m_cullParityCommandWriteOffsets.capacity() * sizeof( uint32_t );
	bytes += m_uploadState.m_frameBytes.size() + m_uploadState.m_instanceBytes.size() + m_uploadState.m_previousInstanceBytes.size()
	        + m_uploadState.m_dirtyInstanceBytes.size();
	bytes += m_uploadState.m_visibleBytes.size() + m_uploadState.m_shadowVisibleBytes.size() + m_uploadState.m_visibleDrawSlotBytes.size()
	        + m_uploadState.m_shadowVisibleDrawSlotBytes.size();
	bytes += m_uploadState.m_visibleLayerBytes.size() + m_uploadState.m_shadowVisibleLayerBytes.size()
	        + m_uploadState.m_drawMetadataBytes.size() + m_uploadState.m_materialBytes.size() + m_uploadState.m_indirectBytes.size();
	bytes += m_uploadState.m_shadowIndirectBytes.size() + m_uploadState.m_pushBytes.size() + m_uploadState.m_hlodDescriptorBytes.size()
	        + m_uploadState.m_hlodCullPushBytes.size();
	bytes += m_uploadState.m_instanceCullDescriptorBytes.size() + m_uploadState.m_instanceHiZRegionDescriptorBytes.size()
	        + m_uploadState.m_instanceCullPushBytes.size();
	return bytes;
}

Dictionary Terrain3DRdInstanceRenderer::getDiagnostics( void ) const {
	Dictionary result;
	uint32_t alpha_packet_count = 0u;
	for ( size_t iPacket = 0u; iPacket < m_commandTable.m_drawPackets.size(); ++iPacket ) {
		alpha_packet_count += ( m_commandTable.m_drawPackets[iPacket].m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u ? 1u : 0u;
	}
	result["instance_frames"] = ( int64_t )( m_uploadState.m_nFrames );
	result["virtual_texture_attached"] = m_pVirtualTexture != NULL;
	result["virtual_texture_initialized"] = m_pVirtualTexture != NULL && m_pVirtualTexture->is_initialized();
	result["instance_vt_stable_key_misses"] = ( int64_t )( m_nVtStableKeyMissCount );
	result["instance_vt_invalid_allocation_references"] = ( int64_t )( m_nVtInvalidAllocationReferenceCount );
	result["instance_vt_out_of_range_page_references"] = ( int64_t )( m_nVtOutOfRangePageReferenceCount );
	result["instance_vt_residency_request_attempts"] = ( int64_t )( m_nVtResidencyRequestAttempts );
	result["instance_vt_residency_requests_accepted"] = ( int64_t )( m_nVtResidencyRequestsAccepted );
	result["instance_draws"] = ( int64_t )( m_commandTable.m_nDraws );
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	result["instance_indirect_draws"] = 0;
	result["instance_direct_draws"] = ( int64_t )( m_commandTable.m_nCpuReferenceDirectSubmissions );
	result["instance_cpu_visibility_reference"] = true;
	result["instance_backend"] = "rd_minimal_direct";
#else
	result["instance_indirect_draws"] = ( int64_t )( m_commandTable.m_nGpuProductionIndirectSubmissions );
	result["instance_direct_draws"] = ( int64_t )( m_commandTable.m_nCpuReferenceDirectSubmissions );
	result["instance_cpu_visibility_reference"] = m_bDirectDrawDebug;
#if T3D_INSTANCER_SHARED_MESH_ARENA
	result["instance_backend"] = "rd_shared_arena_gpu_cull_indirect";
	result["instance_mesh_surface_views"] = ( int64_t )( m_geometryArena.m_meshCache.getSurfaceViewCount() );
#else
	result["instance_backend"] = "rd_direct_surface_gpu_cull_indirect";
#endif
#endif
	result["static_instances"] = ( int64_t )( m_commandTable.m_nStaticInstances );
	result["visible_instances"] = ( int64_t )( m_commandTable.m_nVisibleInstances );
	result["visible_index_count"] = ( int64_t )( m_visibility.m_nVisibleIndexCount );
	result["shadow_visible_index_count"] = ( int64_t )( m_visibility.m_nShadowVisibleIndexCount );
	result["instance_scene_uploads"] = ( int64_t )( m_uploadState.m_nSceneUploads );
	result["instance_dirty_upload_bytes"] = ( int64_t )m_context.getRenderUploadBytes(
	        Terrain3DRdContext::OWNER_INSTANCES, T3D_GPU_DIRTY_UPLOAD );
	result["instance_dirty_upload_ranges"] = ( int64_t )m_context.getRenderDirtyRangesUploaded( Terrain3DRdContext::OWNER_INSTANCES );
	result["instance_buffer_capacity_bytes"] = ( int64_t )( m_uploadState.m_hInstanceBuffer.capacityBytes() );
	result["previous_instance_buffer_capacity_bytes"] = ( int64_t )( m_uploadState.m_hPreviousInstanceBuffer.capacityBytes() );
	result["motion_history_resets"] = ( int64_t )( m_uploadState.m_nMotionHistoryResets );
	result["motion_history_matches"] = ( int64_t )( m_uploadState.m_nMotionHistoryMatches );
	result["owner_buffer_capacity_misses"] = ( int64_t )( m_uploadState.m_nBufferCapacityMissCount );
	result["owner_buffer_rebuild_failures"] = ( int64_t )( m_uploadState.m_nBufferRebuildFailureCount );
	result["owner_cpu_capacity_rebuilds"] = ( int64_t )( m_uploadState.m_nCpuCapacityRebuildCount );
	result["owner_cpu_capacity_rebuild_failures"] = ( int64_t )( m_uploadState.m_nCpuCapacityRebuildFailureCount );
	result["mesh_owner_arena_capacity_misses"] = ( int64_t )( m_geometryArena.m_meshCache.getArenaCapacityMissCount() );
	result["mesh_owner_arena_growth_requests"] = ( int64_t )( m_geometryArena.m_meshCache.getArenaGrowthRequestCount() );
	result["mesh_owner_arena_rebuilds"] = ( int64_t )( m_geometryArena.m_meshCache.getArenaRebuildCount() );
	result["mesh_owner_arena_rebuild_failures"] = ( int64_t )( m_geometryArena.m_meshCache.getArenaRebuildFailureCount() );
	result["mesh_upload_attempts"] = ( int64_t )( m_geometryArena.m_meshCache.getUploadAttemptCount() );
	result["mesh_upload_failures"] = ( int64_t )( m_geometryArena.m_meshCache.getUploadFailureCount() );
	result["mesh_source_generation"] = ( int64_t )( m_geometryArena.m_meshCache.getSourceGeneration() );
	result["mesh_cache_generation"] = ( int64_t )( m_geometryArena.m_meshCache.getGeneration() );
	result["mesh_texture_fallbacks"] = ( int64_t )( 0 );
	result["mesh_last_error"] = m_geometryArena.m_meshCache.getLastError();
	result["mesh_vertex_used_bytes"] = ( int64_t )( m_geometryArena.m_meshCache.getVertexUsedBytes() );
	result["mesh_vertex_capacity_bytes"] = ( int64_t )( m_geometryArena.m_meshCache.getVertexCapacityBytes() );
	result["mesh_index32_used_bytes"] = ( int64_t )( m_geometryArena.m_meshCache.getIndex32UsedBytes() );
	result["mesh_index32_capacity_bytes"] = ( int64_t )( m_geometryArena.m_meshCache.getIndex32CapacityBytes() );
	result["motion_debug_enabled"] = m_bMotionDebug;
	result["visible_index_capacity_bytes"] = ( int64_t )( m_visibility.m_hVisibleIndexBuffer.capacityBytes() );
	result["shadow_visible_index_capacity_bytes"] = ( int64_t )( m_visibility.m_hShadowVisibleIndexBuffer.capacityBytes() );
	result["indirect_capacity_bytes"] = ( int64_t )( m_commandTable.m_mainIndirectSlice.m_nCapacityBytes );
	result["shadow_indirect_capacity_bytes"] = ( int64_t )( m_commandTable.m_shadowIndirectSlice.m_nCapacityBytes );
	result["indirect_arena_capacity_bytes"] = ( int64_t )( m_commandTable.m_hIndirectBuffer.capacityBytes() );
	result["main_indirect_offset_bytes"] = ( int64_t )( m_commandTable.m_mainIndirectSlice.m_nOffsetBytes );
	result["shadow_indirect_offset_bytes"] = ( int64_t )( m_commandTable.m_shadowIndirectSlice.m_nOffsetBytes );
	result["instance_mesh_arena_bytes"] = ( int64_t )( m_geometryArena.m_meshCache.getGpuBytes() );
	result["instance_material_textures"] = ( int64_t )( 0 );
	result["instance_material_bindings"] = 0;
	result["vt_descriptor_binds_instancer_pass"] = ( int64_t )( m_descriptorState.m_nVtDescriptorBindsInstancerPass );
	result["vt_descriptor_rebuilds_runtime"] = ( int64_t )( m_descriptorState.m_nVtDescriptorRebuildsRuntime );
	result["cpu_surface_packets_built"] = ( int64_t )( m_commandTable.m_nCpuSurfacePacketsBuilt );
	result["instance_submissions_opaque"] = ( int64_t )( m_commandTable.m_nOneSubmitOpaque );
	result["instance_submissions_depth"] = ( int64_t )( m_commandTable.m_nOneSubmitDepth );
	result["instance_submissions_motion"] = ( int64_t )( m_commandTable.m_nOneSubmitMotion );
	result["instance_submissions_shadow"] = ( int64_t )( m_commandTable.m_nOneSubmitShadow );
	result["instance_one_submit_fallback_draws"] = ( int64_t )( m_commandTable.m_nOneSubmitFallbackDraws );
	result["instance_one_submit_main_commands"] = ( int64_t )( m_commandTable.m_nMainCompatibleCommandCount );
	result["instance_one_submit_shadow_commands"] = ( int64_t )( m_commandTable.m_nShadowCompatibleCommandCount );
	result["instance_one_submit_main_fallback_commands"] =
	        ( int64_t )( m_commandTable.m_drawPackets.size() > m_commandTable.m_nMainCompatibleCommandCount
	                        ? m_commandTable.m_drawPackets.size() - m_commandTable.m_nMainCompatibleCommandCount
	                        : 0u );
	result["instance_one_submit_shadow_fallback_commands"] =
	        ( int64_t )( m_commandTable.m_shadowPackets.size() > m_commandTable.m_nShadowCompatibleCommandCount
	                        ? m_commandTable.m_shadowPackets.size() - m_commandTable.m_nShadowCompatibleCommandCount
	                        : 0u );
	result["instance_one_submit_fixed_visible_capacity"] = ( int64_t )( m_visibility.m_nFixedVisibleCapacity );
	result["instance_draw_metadata_capacity_bytes"] = ( int64_t )( m_commandTable.m_hDrawMetadataBuffer.capacityBytes() );
	result["instance_visible_draw_slot_capacity_bytes"] = ( int64_t )( m_visibility.m_hVisibleDrawSlotBuffer.capacityBytes() );
	result["instance_shadow_visible_draw_slot_capacity_bytes"] = ( int64_t )( m_visibility.m_hShadowVisibleDrawSlotBuffer.capacityBytes() );
	result["instance_mesh_lods"] = ( int64_t )( m_geometryArena.m_meshCache.getLodCount() );
	result["instance_mesh_surfaces"] = ( int64_t )( m_geometryArena.m_meshCache.getSurfaceCount() );
	result["instance_mesh_materials"] = ( int64_t )( m_geometryArena.m_meshCache.getMaterialCount() );
	result["instance_packet_build_input_cells"] = ( int64_t )( m_commandTable.m_nPacketBuildInputCells );
	result["instance_resolved_mesh_cells"] = ( int64_t )( m_commandTable.m_nResolvedMeshCells );
	result["instance_unresolved_mesh_cells"] = ( int64_t )( m_commandTable.m_nUnresolvedMeshCells );
	result["clutter_visibility_layer_filtered_cells"] = ( int64_t )( m_visibility.m_szVisibilityLayerFilteredCells[0] );
	result["ground_cover_visibility_layer_filtered_cells"] = ( int64_t )( m_visibility.m_szVisibilityLayerFilteredCells[1] );
	result["occluder_visibility_layer_filtered_cells"] = ( int64_t )( m_visibility.m_szVisibilityLayerFilteredCells[2] );
	result["instance_draw_packets"] = ( int64_t )( m_commandTable.m_drawPackets.size() );
	result["instance_alpha_draw_packets"] = ( int64_t )( alpha_packet_count );
	result["depth_packets"] = ( int64_t )( m_commandTable.m_depthPackets.size() );
	result["shadow_packets"] = ( int64_t )( m_commandTable.m_shadowPackets.size() );
	result["occluder_depth_packets"] = ( int64_t )( m_occluderDepth.m_packets.size() );
	result["occluder_depth_visible_indices"] = ( int64_t )( m_occluderDepth.m_nVisibleIndexCount );
	result["occluder_depth_target_width"] = ( int64_t )( m_occluderDepth.m_targetSize.x );
	result["occluder_depth_target_height"] = ( int64_t )( m_occluderDepth.m_targetSize.y );
	result["occluder_depth_valid"] = m_occluderDepth.m_bDepthValid;
	result["occluder_depth_history_samples"] = ( int64_t )( m_occluderDepth.m_nHistorySamples );
	result["occluder_depth_history_invalidations"] = ( int64_t )( m_occluderDepth.m_nHistoryInvalidations );
	result["occluder_depth_history_valid_frames"] = ( int64_t )( m_occluderDepth.m_nHistoryValidFrames );
	result["occluder_depth_history_invalid_frames"] = ( int64_t )( m_occluderDepth.m_nHistoryInvalidFrames );
	result["occluder_depth_target_rebuilds"] = ( int64_t )( m_occluderDepth.m_nTargetRebuilds );
	result["occluder_depth_capacity_growths"] = ( int64_t )( m_occluderDepth.m_nCapacityGrowths );
	result["occluder_depth_packet_capacity"] = ( int64_t )( m_occluderDepth.m_packets.capacity() );
	result["occluder_depth_visible_index_capacity"] = ( int64_t )( m_occluderDepth.m_visibleIndices.capacity() );
	result["occluder_depth_hiz_bound"] = m_visibility.m_instanceHiZ.hasProxyDepthBound();
	result["occluder_depth_raster_frames"] = ( int64_t )( m_occluderDepth.m_nRasterFrames );
	result["occluder_depth_raster_draw_packets"] = ( int64_t )( m_occluderDepth.m_nRasterDrawPackets );
	result["occluder_depth_raster_instances"] = ( int64_t )( m_occluderDepth.m_nRasterInstances );
	result["instance_depth_submitted_draws"] = ( int64_t )( m_commandTable.m_nDepthSubmittedDraws );
	result["instance_opaque_submitted_draws"] = ( int64_t )( m_commandTable.m_nOpaqueSubmittedDraws );
	result["instance_shadow_submitted_draws"] = ( int64_t )( m_commandTable.m_nShadowSubmittedDraws );
	result["instance_motion_submitted_draws"] = ( int64_t )( m_commandTable.m_nMotionSubmittedDraws );
	result["instance_transparent_submitted_draws"] = ( int64_t )( m_commandTable.m_nTransparentSubmittedDraws );
	result["hlod_gpu_cull_descriptors"] = ( int64_t )( m_visibility.m_nHlodCullDescriptors );
	result["hlod_gpu_cull_dispatches"] = ( int64_t )( m_visibility.m_nHlodCullDispatches );
	result["hlod_descriptor_capacity_bytes"] = ( int64_t )( m_visibility.m_hHlodCullDescriptorBuffer.capacityBytes() );
	result["instance_gpu_cull_dispatches"] = ( int64_t )( m_visibility.m_nInstanceCullDispatches );
	result["instance_gpu_topology_builds"] = ( int64_t )( m_visibility.m_nGpuTopologyBuilds );
	result["instance_gpu_cull_reuses"] = ( int64_t )( m_visibility.m_nGpuCullReuses );
	result["instance_gpu_cull_production_enabled"] = T3D_INSTANCER_GPU_CULL_PRODUCTION != 0;
	result["instance_gpu_cull_production_passes"] = ( int64_t )( m_commandTable.m_nGpuProductionPasses );
	result["instance_gpu_cull_production_submitted_once"] = m_commandTable.m_bGpuProductionSubmittedOnce;
	result["instance_shared_mesh_arena_enabled"] = T3D_INSTANCER_SHARED_MESH_ARENA != 0;
	result["instance_shared_mesh_arena_submitted_once"] = m_geometryArena.m_bSharedMeshArenaSubmittedOnce;
	result["instance_hiz_occlusion_enabled"] = T3D_INSTANCER_HIZ_OCCLUSION != 0;
	result["instance_hiz_ready_for_frame"] = m_visibility.m_bHiZReadyForFrame;
	result["instance_hiz_cell_descriptors"] = ( int64_t )( m_visibility.m_nHiZCellDescriptors );
	result["instance_hiz_per_instance_descriptors"] = ( int64_t )( m_visibility.m_nHiZInstanceDescriptors );
	result["instance_hiz_region_descriptors"] = ( int64_t )( m_visibility.m_instanceHiZRegionDescriptors.size() );
	result["instance_hiz_region_descriptor_capacity_bytes"] = ( int64_t )( m_visibility.m_hInstanceHiZRegionDescriptorBuffer.capacityBytes() );
	result["instance_hiz_region_visibility_capacity_bytes"] = ( int64_t )( m_visibility.m_hInstanceHiZRegionVisibilityBuffer.capacityBytes() );
	result["instance_hiz_width"] = ( int64_t )( m_visibility.m_instanceHiZ.getWidth() );
	result["instance_hiz_height"] = ( int64_t )( m_visibility.m_instanceHiZ.getHeight() );
	result["instance_hiz_mip_count"] = ( int64_t )( m_visibility.m_instanceHiZ.getMipCount() );
	result["instance_hiz_build_dispatches"] = ( int64_t )( m_visibility.m_instanceHiZ.getBuildDispatches() );
	result["instance_hiz_source_rebinds"] = ( int64_t )( m_visibility.m_instanceHiZ.getSourceRebinds() );
	result["instance_hiz_unavailable_frames"] = ( int64_t )( m_visibility.m_instanceHiZ.getUnavailableFrames() );
	result["instance_hiz_submitted_once"] = m_visibility.m_bHiZOcclusionSubmittedOnce;
	result["instance_gpu_cull_indirect_submissions"] = ( int64_t )( m_commandTable.m_nGpuProductionIndirectSubmissions );
	result["instance_gpu_transparent_order_builds"] = ( int64_t )( m_commandTable.m_nGpuTransparentOrderBuilds );
	result["instance_cpu_reference_passes"] = ( int64_t )( m_commandTable.m_nCpuReferencePasses );
	result["instance_cpu_reference_direct_submissions"] = ( int64_t )( m_commandTable.m_nCpuReferenceDirectSubmissions );
	result["instance_gpu_cull_descriptors"] = ( int64_t )( m_visibility.m_instanceCullDescriptors.size() );
	result["instance_gpu_shadow_cull_descriptors"] = ( int64_t )( m_visibility.m_shadowInstanceCullDescriptors.size() );
	result["instance_gpu_cull_descriptor_capacity_bytes"] = ( int64_t )( m_visibility.m_hInstanceCullDescriptorBuffer.capacityBytes() );
	result["instance_cull_parity_enabled"] = m_visibility.m_bCullingComparisonEnabled;
	result["instance_cull_parity_gpu_drives_draws"] = T3D_INSTANCER_GPU_CULL_PRODUCTION != 0 && !m_bDirectDrawDebug;
	result["instance_cull_parity_comparisons"] = ( int64_t )( m_visibility.m_nCullParityComparisons );
	result["instance_cull_parity_mismatches"] = ( int64_t )( m_visibility.m_nCullParityMismatches );
	result["instance_cull_parity_topology_mismatches"] = ( int64_t )( m_visibility.m_nCullParityTopologyMismatches );
	result["instance_cull_parity_readback_bytes"] = ( int64_t )( m_visibility.m_nCullParityReadbackBytes );
	result["instance_cull_parity_main_reuses"] = ( int64_t )( m_visibility.m_nCullParityMainReuses );
	result["instance_cull_parity_visible_cells"] = ( int64_t )( m_visibility.m_nCullParityVisibleCells );
	result["instance_cull_parity_visible_instances"] = ( int64_t )( m_visibility.m_nCullParityVisibleInstances );
	result["instance_cull_parity_shadow_visible_cells"] = ( int64_t )( m_visibility.m_nCullParityShadowVisibleCells );
	result["instance_cull_parity_shadow_visible_instances"] = ( int64_t )( m_visibility.m_nCullParityShadowVisibleInstances );
	result["instance_cull_parity_shader_instantiated"] = m_visibility.m_hInstanceCullShader.isValid();
	result["instance_cull_parity_validated_once"] = m_visibility.m_bCullParityValidatedOnce;
	result["direct_draw_debug"] = m_bDirectDrawDebug;
	result["instance_forward_prepared_has_draws"] = m_bForwardPreparedHasDraws;
	result["instance_forward_prepared_pass_kind"] = ( int64_t )( m_forwardPreparedPacketPassKind );
	result["instance_forward_prepared_scene_pass"] = ( int64_t )( m_nForwardPreparedScenePass );
	result["instance_forward_pipeline_key_valid"] = m_bLastForwardPipelineKeyValid;
	uint32_t forward_shader_mask = 0u;
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX && variant < 32u; ++variant ) {
		if ( m_hszForwardShaders[variant].isValid() ) {
			forward_shader_mask |= 1u << variant;
		}
	}
	result["instance_forward_shader_ready_mask"] = ( int64_t )forward_shader_mask;
	result["hlod_cull_shader_instantiated"] = m_visibility.m_hHlodCullShader.isValid();
	result["forward_lighting"] = m_forwardLighting.getDiagnostics();
	result["last_error"] = m_lastError;
	return result;
}
