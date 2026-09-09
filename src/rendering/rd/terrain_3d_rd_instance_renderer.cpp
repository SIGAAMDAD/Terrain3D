// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"
#include "instances/terrain_3d_instancer_bringup.h"

#include "data/terrain_3d_half_buffer.h"
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
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

typedef struct alignas( 16 ) t3dRdInstanceFrame_s {
	float m_szViewProjection[s_nRdForwardMaxViews][16];
	float m_szView[16];
	float m_szPreviousViewProjection[s_nRdForwardMaxViews][16];
	float m_szCamera[s_nRdForwardMaxViews][4];
	float m_szMotionJitter[4];
	float m_szMotionParams[4];
} t3dRdInstanceFrame_t;
static_assert( sizeof( t3dRdInstanceFrame_t ) == 384u, "Instance frame ABI changed" );

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
	uint32_t m_nReserved0 = 0u;
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
	uint32_t m_nReserved0 = 0u;
} t3dRdInstanceCullPush_t;
static_assert( sizeof( t3dRdInstanceCullPush_t ) == 32u, "Instance cull push ABI changed" );

static Ref<RDUniform> t3dRdInstanceBufferUniform( const int nBinding, const RenderingDevice::UniformType type, const RID &rRid ) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_binding( nBinding );
	uniform->set_uniform_type( type );
	uniform->add_id( rRid );
	return uniform;
}

static Ref<RDUniform> t3dRdInstanceTextureUniform( const int nBinding, const RID &rSampler, const RID &rTexture ) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_binding( nBinding );
	uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE );
	uniform->add_id( rSampler );
	uniform->add_id( rTexture );
	return uniform;
}

static uint64_t t3dRdInstanceTextureId( const uint32_t nLow, const uint32_t nHigh ) {
	return ( uint64_t )( nLow ) | ( ( uint64_t )( nHigh ) << 32u );
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

typedef struct t3dRdInstanceFrustum_s {
	Vector4 m_szPlanes[6];
} t3dRdInstanceFrustum_t;

static Vector4 t3dRdInstanceNormalizePlane( const Vector4 &rPlane ) {
	const double length_sq = rPlane.x * rPlane.x + rPlane.y * rPlane.y + rPlane.z * rPlane.z;
	if ( length_sq <= 1e-20 ) {
		return rPlane;
	}
	const double inverse_length = 1.0 / sqrt( length_sq );
	return rPlane * inverse_length;
}

static t3dRdInstanceFrustum_t t3dRdInstanceExtractFrustum( const Projection &rProjection ) {
	const Vector4 c0 = rProjection[0];
	const Vector4 c1 = rProjection[1];
	const Vector4 c2 = rProjection[2];
	const Vector4 c3 = rProjection[3];
	const Vector4 row0( c0.x, c1.x, c2.x, c3.x );
	const Vector4 row1( c0.y, c1.y, c2.y, c3.y );
	const Vector4 row2( c0.z, c1.z, c2.z, c3.z );
	const Vector4 row3( c0.w, c1.w, c2.w, c3.w );
	t3dRdInstanceFrustum_t result;
	result.m_szPlanes[0] = t3dRdInstanceNormalizePlane( row3 + row0 );
	result.m_szPlanes[1] = t3dRdInstanceNormalizePlane( row3 - row0 );
	result.m_szPlanes[2] = t3dRdInstanceNormalizePlane( row3 + row1 );
	result.m_szPlanes[3] = t3dRdInstanceNormalizePlane( row3 - row1 );
	result.m_szPlanes[4] = t3dRdInstanceNormalizePlane( row3 - row2 );
	result.m_szPlanes[5] = t3dRdInstanceNormalizePlane( row2 );
	return result;
}

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

static uint32_t t3dRdNextCapacity( const uint32_t nRequired, const uint32_t nMinimum ) {
	if ( nRequired == 0u ) {
		return nMinimum;
	}
	uint64_t capacity = MAX( ( uint64_t )( nMinimum ), 256ull );
	while ( capacity < nRequired ) {
		capacity <<= 1u;
	}
	return capacity > UINT32_MAX ? 0u : ( uint32_t )( capacity );
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
	if ( !bShadowPass ) {
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

static uint32_t t3dRdPreviewPackSnormPair( const float x, const float y ) {
	const int16_t px = ( int16_t )( lround( CLAMP( x, -1.0f, 1.0f ) * 32767.0f ) );
	const int16_t py = ( int16_t )( lround( CLAMP( y, -1.0f, 1.0f ) * 32767.0f ) );
	return ( uint32_t )( ( uint16_t )( px ) ) | ( ( uint32_t )( ( uint16_t )( py ) ) << 16u );
}

static t3dRdInstanceRecord_t t3dRdPreviewInstanceRecord( const Transform3D &rTransform ) {
	t3dRdInstanceRecord_t record;
	record.m_szPosition[0] = ( float )( rTransform.origin.x );
	record.m_szPosition[1] = ( float )( rTransform.origin.y );
	record.m_szPosition[2] = ( float )( rTransform.origin.z );
	const Vector3 scale = rTransform.basis.get_scale();
	const Quaternion rotation = rTransform.basis.orthonormalized().get_rotation_quaternion().normalized();
	record.m_nQuaternion01 = t3dRdPreviewPackSnormPair( ( float )( rotation.x ), ( float )( rotation.y ) );
	record.m_nQuaternion23 = t3dRdPreviewPackSnormPair( ( float )( rotation.z ), ( float )( rotation.w ) );
	record.m_nScale01 =
	        ( uint32_t )( t3dHalfEncode( ( float )( scale.x ) ) ) | ( ( uint32_t )( t3dHalfEncode( ( float )( scale.y ) ) ) << 16u );
	record.m_nScale2Flags = ( uint32_t )( t3dHalfEncode( ( float )( scale.z ) ) );
	record.m_nColorRgba8 = t3dRdPackGodotRgba32ForGpu( 0xffffffffu );
	return record;
}

bool Terrain3DRdInstanceRenderer::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone || !m_meshCache.bindRuntimeAllocator( pZone, ePolicy ) ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_visibleIndices.set_allocator( allocator );
	m_motionHistoryRecords.set_allocator( allocator );
	m_motionHistoryCells.set_allocator( allocator );
	m_motionPreviousRecords.set_allocator( allocator );
	m_shadowVisibleIndices.set_allocator( allocator );
	m_drawPackets.set_allocator( allocator );
	m_depthPackets.set_allocator( allocator );
	m_shadowPackets.set_allocator( allocator );
	m_materialBindings.set_allocator( allocator );
	m_hlodDescriptors.set_allocator( allocator );
	m_instanceCullDescriptors.set_allocator( allocator );
	m_shadowInstanceCullDescriptors.set_allocator( allocator );
	m_instanceCullCellSpans.set_allocator( allocator );
	m_shadowInstanceCullCellSpans.set_allocator( allocator );
	m_packetBuildSlots.set_allocator( allocator );
	m_packetBuildEntries.set_allocator( allocator );
	m_alphaPackets.set_allocator( allocator );
	m_alphaSortScratch.set_allocator( allocator );
	m_packetIndex.set_allocator( allocator );
	m_motionCurrentOrder.set_allocator( allocator );
	m_motionPreviousOrder.set_allocator( allocator );
	m_motionDirtyRanges.set_allocator( allocator );
	return true;
}

Terrain3DRdInstanceRenderer::Terrain3DRdInstanceRenderer( void ) :
        m_context( Terrain3DRdContext::getShared() ), m_forwardLighting( m_context ) {
}
Terrain3DRdInstanceRenderer::~Terrain3DRdInstanceRenderer( void ) {
	shutdown();
}

void Terrain3DRdInstanceRenderer::setError( const String &rError ) {
	m_lastError = rError;
}
bool Terrain3DRdInstanceRenderer::ensureDevice( void ) {
	return m_context.initialize() && m_context.validateDevice() && m_context.getDevice();
}

bool Terrain3DRdInstanceRenderer::ensureShader( const Ref<RDShaderSPIRV> &rShader ) {
	if ( m_hShader.is_valid() ) {
		return true;
	}
	if ( rShader.is_null() ) {
		setError( "Terrain3D instance shader source is null" );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "Terrain3D instance shader creation lost the RenderingDevice" );
		return false;
	}
	m_hShader = rd->shader_create_from_spirv( rShader, "T3D.InstanceShader" );
	if ( !m_hShader.is_valid() ) {
		setError( "Failed to create Terrain3D instance shader RID" );
		return false;
	}
	if ( !m_context.registerResource( m_hShader,
	             "T3D.Instance.Shader",
	             Terrain3DRdContext::RESOURCE_SHADER,
	             Terrain3DRdContext::MEMORY_NONE,
	             0u,
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		rd->free_rid( m_hShader );
		m_hShader = RID();
		setError( "Failed to register Terrain3D instance shader RID" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureForwardShaderVariant( const t3dRdForwardShaderVariant_t variant ) {
	if ( variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ) {
		setError( "Invalid Terrain3D Forward+ instance shader variant" );
		return false;
	}
	if ( m_hszForwardShaders[variant].is_valid() ) {
		return true;
	}
	const Ref<RDShaderSPIRV> spirv = m_forwardShaderBundle.m_szInstances[variant];
	if ( spirv.is_null() ) {
		setError( String( "Missing Terrain3D Forward+ instance shader variant " ) + String::num_int64( variant ) );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( String( "Terrain3D Forward+ instance shader creation lost the RenderingDevice; variant=" )
		        + String::num_int64( variant ) );
		return false;
	}
	m_context.beginResourceRebuild();
	RID shader = rd->shader_create_from_spirv( spirv, String( "T3D.Forward.InstanceShader." ) + String::num_int64( variant ) );
	const String name = String( "T3D.Forward.Shader.Instance." ) + String::num_int64( variant );
	if ( !shader.is_valid()
	        || !m_context.registerResource( shader,
	                name,
	                Terrain3DRdContext::RESOURCE_SHADER,
	                Terrain3DRdContext::MEMORY_NONE,
	                0u,
	                Terrain3DRdContext::OWNER_INSTANCES ) ) {
		if ( shader.is_valid() ) {
			rd->free_rid( shader );
		}
		m_context.endResourceRebuild();
		setError( String( "Failed to create Terrain3D Forward+ instance shader variant " ) + String::num_int64( variant ) );
		return false;
	}
	m_hszForwardShaders[variant] = shader;
	m_context.endResourceRebuild();
	return true;
}

void Terrain3DRdInstanceRenderer::freeForwardPipelines( void ) {
	for ( uint32_t i = 0u; i < m_nForwardPipelineCount; ++i ) {
		for ( size_t iPipeline = 0u; iPipeline < T3D_ARRAY_COUNT( m_szForwardPipelines[i].m_hszPipelines ); iPipeline++ ) {
			RID &pipeline = ( m_szForwardPipelines[i].m_hszPipelines )[iPipeline];
			m_context.freeResource( pipeline );
		}
		m_szForwardPipelines[i] = t3dRdInstanceForwardPipelineEntry_t{};
	}
	m_nForwardPipelineCount = 0u;
	m_nPipelineEvictCursor = 0u;
	m_bLastForwardPipelineKeyValid = false;
}

bool Terrain3DRdInstanceRenderer::selectForwardVariant(
        const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t &rVariant ) const {
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) {
		if ( rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_SHADOW ) {
			return false;
		}
		rVariant = T3D_RD_FORWARD_SHADER_DEPTH;
		return true;
	}
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
		switch ( rContext.m_nativePassMode ) {
			case T3D_RD_FORWARD_NATIVE_PASS_DEPTH:
				rVariant = T3D_RD_FORWARD_SHADER_DEPTH;
				return true;
			case T3D_RD_FORWARD_NATIVE_PASS_DEPTH_NORMAL_ROUGHNESS:
				rVariant = T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS;
				return true;
			case T3D_RD_FORWARD_NATIVE_PASS_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI:
				rVariant = T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI;
				return true;
			default:
				return false;
		}
	}
	if ( ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION
	             && rContext.m_passKind != T3D_RD_FORWARD_PASS_TRANSPARENT )
	        || rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR ) {
		return false;
	}
	const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
	const bool specular = !transparent_pass && ( rContext.m_nColorPassFlags & ( 1u << 1u ) ) != 0u;
	const bool motion = !transparent_pass
	        && ( rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || ( rContext.m_nColorPassFlags & ( 1u << 3u ) ) != 0u );
	const bool radiance_array = ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	if ( radiance_array ) {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY
		                               : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY )
		                    : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY : T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY );
	} else {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR )
		                    : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION : T3D_RD_FORWARD_SHADER_COLOR );
	}
	return true;
}

const Terrain3DRdInstanceRenderer::t3dRdInstanceForwardPipelineEntry_t *Terrain3DRdInstanceRenderer::findForwardPipeline(
        const t3dRdInstanceForwardPipelineKey_t &rKey ) const {
	for ( uint32_t i = 0u; i < m_nForwardPipelineCount; ++i ) {
		if ( m_szForwardPipelines[i].m_key == rKey ) {
			return &m_szForwardPipelines[i];
		}
	}
	return NULL;
}

bool Terrain3DRdInstanceRenderer::ensureForwardPipelines(
        const t3dRdForwardPassContext_t &rContext, const t3dRdForwardShaderVariant_t variant ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 || m_meshCache.getVertexFormat() == RenderingDevice::INVALID_FORMAT_ID ) {
		return false;
	}
	t3dRdInstanceForwardPipelineKey_t key;
	key.m_nFramebufferFormat = rContext.m_nFramebufferFormat;
	key.m_variant = variant;
	key.m_passKind = rContext.m_passKind;
	key.m_samples = rd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u );
	if ( ( 1u << ( uint32_t )( key.m_samples ) ) != rContext.m_nSampleCount ) {
		setError( "Terrain3D Forward+ sample-count metadata does not match native framebuffer format" );
		return false;
	}
	key.m_nViewCount = rContext.m_nViewCount;
	key.m_bReverseCull = rContext.m_bReverseCull;
	key.m_bDepthPrepassActive = ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION )
	        && rContext.m_bDepthPrepassActive;
	if ( findForwardPipeline( key ) ) {
		m_lastForwardPipelineKey = key;
		m_bLastForwardPipelineKeyValid = true;
		return true;
	}
	const bool replace_pipeline = m_nForwardPipelineCount >= s_nForwardPipelineCapacity;
	const uint32_t replace_index = replace_pipeline ? ( m_nPipelineEvictCursor++ % s_nForwardPipelineCapacity ) : UINT32_MAX;
	t3dRdInstanceForwardPipelineEntry_t entry;
	entry.m_key = key;
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( key.m_samples );
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	depth->set_enable_depth_test( false );
	depth->set_enable_depth_write( false );
	depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_ALWAYS );
#else
	depth->set_enable_depth_test( true );
	depth->set_enable_depth_write( !transparent_pass && !key.m_bDepthPrepassActive );
	depth->set_depth_compare_operator(
	        key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
#endif
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	uint32_t blend_count = 0u;
	if ( transparent_pass ) {
		blend_count = 1u;
	} else if ( variant <= T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY ) {
		blend_count = 3u;
	} else if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS ) {
		blend_count = 1u;
	} else if ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI ) {
		blend_count = 2u;
	}
	if ( blend_count > 0u ) {
		TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
		for ( uint32_t i = 0u; i < blend_count; ++i ) {
			Ref<RDPipelineColorBlendStateAttachment> a;
			a.instantiate();
			a->set_enable_blend( transparent_pass );
			if ( transparent_pass ) {
				a->set_src_color_blend_factor( RenderingDevice::BLEND_FACTOR_SRC_ALPHA );
				a->set_dst_color_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
				a->set_src_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE );
				a->set_dst_alpha_blend_factor( RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
			}
			attachments.push_back( a );
		}
		blend->set_attachments( attachments );
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
		raster.instantiate();
		raster->set_cull_mode( cull_modes[cull] );
		raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE );
		entry.m_hszPipelines[cull] = rd->render_pipeline_create( m_hszForwardShaders[variant],
		        rContext.m_nFramebufferFormat,
		        m_meshCache.getVertexFormat(),
		        RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
		        raster,
		        multisample,
		        depth,
		        blend,
		        0u );
		const String name = String( "T3D.Forward.Pipeline.Instance." ) + String::num_int64( variant ) + "." + String::num_int64( cull );
		if ( !entry.m_hszPipelines[cull].is_valid() ) {
			for ( size_t iPipeline = 0u; iPipeline < T3D_ARRAY_COUNT( entry.m_hszPipelines ); iPipeline++ ) {
				RID &pipeline = ( entry.m_hszPipelines )[iPipeline];
				m_context.freeResource( pipeline );
			}
			m_context.endResourceRebuild();
			setError( String( "Failed to create framebuffer-compatible Terrain3D Forward+ instance pipeline RID; variant=" )
			        + String::num_int64( variant ) + String( ", cull=" ) + String::num_int64( cull ) );
			return false;
		}
		if ( !m_context.registerResource( entry.m_hszPipelines[cull],
		             name,
		             Terrain3DRdContext::RESOURCE_RASTER_PIPELINE,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			// The current RID was never registered. Free it directly; previous cull
			// variants in this temporary entry are registered and use normal cleanup.
			rd->free_rid( entry.m_hszPipelines[cull] );
			entry.m_hszPipelines[cull] = RID();
			for ( uint32_t registered = 0u; registered < cull; ++registered ) {
				m_context.freeResource( entry.m_hszPipelines[registered] );
			}
			m_context.endResourceRebuild();
			setError( String( "Failed to register framebuffer-compatible Terrain3D Forward+ instance pipeline; variant=" )
			        + String::num_int64( variant ) + String( ", cull=" ) + String::num_int64( cull ) );
			return false;
		}
	}
	m_context.endResourceRebuild();
	if ( replace_pipeline ) {
		for ( size_t iPipeline = 0u; iPipeline < T3D_ARRAY_COUNT( m_szForwardPipelines[replace_index].m_hszPipelines ); iPipeline++ ) {
			RID &pipeline = ( m_szForwardPipelines[replace_index].m_hszPipelines )[iPipeline];
			m_context.freeResource( pipeline );
		}
		m_szForwardPipelines[replace_index] = entry;
	} else {
		m_szForwardPipelines[m_nForwardPipelineCount++] = entry;
	}
	m_lastForwardPipelineKey = key;
	m_bLastForwardPipelineKeyValid = true;
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureHlodCullShader( const Ref<RDShaderSPIRV> &rShader ) {
	if ( m_hHlodCullShader.is_valid() && m_hHlodCullPipeline.is_valid() ) {
		return true;
	}
	if ( rShader.is_null() ) {
		setError( "Missing Terrain3D HLOD cull shader" );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "Terrain3D HLOD cull resource creation lost the RenderingDevice" );
		return false;
	}
	if ( !m_hHlodCullShader.is_valid() ) {
		m_hHlodCullShader = rd->shader_create_from_spirv( rShader, "T3D.HlodCullShader" );
		if ( !m_hHlodCullShader.is_valid() ) {
			setError( "Failed to create Terrain3D HLOD cull shader" );
			return false;
		}
		if ( !m_context.registerResource( m_hHlodCullShader,
		             "T3D.HLOD.CullShader",
		             Terrain3DRdContext::RESOURCE_SHADER,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_HLOD ) ) {
			rd->free_rid( m_hHlodCullShader );
			m_hHlodCullShader = RID();
			setError( "Failed to register Terrain3D HLOD cull shader" );
			return false;
		}
	}
	if ( !m_hHlodCullPipeline.is_valid() ) {
		m_hHlodCullPipeline = rd->compute_pipeline_create( m_hHlodCullShader );
		if ( !m_hHlodCullPipeline.is_valid() ) {
			setError( "Failed to create Terrain3D HLOD cull pipeline" );
			return false;
		}
		if ( !m_context.registerResource( m_hHlodCullPipeline,
		             "T3D.HLOD.CullPipeline",
		             Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_HLOD ) ) {
			rd->free_rid( m_hHlodCullPipeline );
			m_hHlodCullPipeline = RID();
			setError( "Failed to register Terrain3D HLOD cull pipeline" );
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureInstanceCullShader( const Ref<RDShaderSPIRV> &rShader ) {
	if ( m_hInstanceCullShader.is_valid() && m_hInstanceCullPipeline.is_valid() ) {
		return true;
	}
	if ( rShader.is_null() ) {
		setError( "Missing Terrain3D instance cull shader" );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "Terrain3D instance cull resource creation lost the RenderingDevice" );
		return false;
	}
	if ( !m_hInstanceCullShader.is_valid() ) {
		m_hInstanceCullShader = rd->shader_create_from_spirv( rShader, "T3D.InstanceCullShader" );
		if ( !m_hInstanceCullShader.is_valid() ) {
			setError( "Failed to create Terrain3D instance cull shader" );
			return false;
		}
		if ( !m_context.registerResource( m_hInstanceCullShader,
		             "T3D.Instance.CullShader",
		             Terrain3DRdContext::RESOURCE_SHADER,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( m_hInstanceCullShader );
			m_hInstanceCullShader = RID();
			setError( "Failed to register Terrain3D instance cull shader" );
			return false;
		}
	}
	if ( !m_hInstanceCullPipeline.is_valid() ) {
		m_hInstanceCullPipeline = rd->compute_pipeline_create( m_hInstanceCullShader );
		if ( !m_hInstanceCullPipeline.is_valid() ) {
			setError( "Failed to create Terrain3D instance cull pipeline" );
			return false;
		}
		if ( !m_context.registerResource( m_hInstanceCullPipeline,
		             "T3D.Instance.CullPipeline",
		             Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( m_hInstanceCullPipeline );
			m_hInstanceCullPipeline = RID();
			setError( "Failed to register Terrain3D instance cull pipeline" );
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureBuffer( RID &rRid,
        uint32_t &rCapacity,
        const uint32_t nRequired,
        const String &rName,
        const Terrain3DRdContext::ResourceType type,
        const Terrain3DRdContext::MemoryCategory memory,
        const Terrain3DRdContext::ResourceOwner owner ) {
	if ( rRid.is_valid() && rCapacity >= nRequired ) {
		return true;
	}
	++m_nBufferCapacityMissCount;
	const uint32_t minimum = type == Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER ? 256u : 4096u;
	const uint32_t capacity = t3dRdNextCapacity( nRequired, minimum );
	if ( capacity == 0u ) {
		++m_nBufferRebuildFailureCount;
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		++m_nBufferRebuildFailureCount;
		setError( String( "RD instance buffer rebuild has no RenderingDevice: " ) + rName );
		return false;
	}
	if ( rRid.is_valid() ) {
		m_context.freeResource( rRid, rCapacity );
	}
	if ( type == Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER ) {
		rRid = rd->uniform_buffer_create( capacity );
	} else if ( type == Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER ) {
		// RenderingDevice indirect draw/dispatch buffers require the indirect usage bit.
		// A plain storage buffer can be written by compute, but is not valid input for
		// draw_list_draw_indirect() on the Vulkan/D3D12 backends.
		rRid = rd->storage_buffer_create(
		        capacity, PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT );
	} else {
		rRid = rd->storage_buffer_create( capacity );
	}
	if ( !rRid.is_valid() ) {
		++m_nBufferRebuildFailureCount;
		setError( String( "RD instance buffer creation failed: " ) + rName + String( ", bytes=" ) + String::num_int64( capacity ) );
		return false;
	}
	if ( !m_context.registerResource( rRid, rName, type, memory, capacity, owner ) ) {
		rd->free_rid( rRid );
		rRid = RID();
		++m_nBufferRebuildFailureCount;
		setError( String( "RD instance buffer registration failed: " ) + rName + String( ", bytes=" ) + String::num_int64( capacity ) );
		return false;
	}
	rCapacity = capacity;
	++m_nBufferRebuildCount;
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureBuffers(
        const uint32_t nInstanceBytes, const uint32_t nVisibleBytes, const uint32_t nMaterialBytes, const uint32_t nIndirectBytes ) {
	const bool replace_instance = !m_hInstanceBuffer.is_valid() || m_nInstanceCapacityBytes < nInstanceBytes;
	const bool replace_visible = !m_hVisibleIndexBuffer.is_valid() || m_nVisibleCapacityBytes < nVisibleBytes;
#if T3D_INSTANCER_RD_PORT
	const bool replace_previous_instance = !m_hPreviousInstanceBuffer.is_valid() || m_nPreviousInstanceCapacityBytes < nInstanceBytes;
	const bool replace_material = !m_hMaterialBuffer.is_valid() || m_nMaterialCapacityBytes < nMaterialBytes;
	const bool replace_indirect = false;
	( void )nIndirectBytes;
#elif T3D_INSTANCER_MINIMAL_RENDER_PATH
	const bool replace_previous_instance = false;
	const bool replace_material = false;
	const bool replace_indirect = false;
	( void )nMaterialBytes;
	( void )nIndirectBytes;
#else
	const bool replace_previous_instance = !m_hPreviousInstanceBuffer.is_valid() || m_nPreviousInstanceCapacityBytes < nInstanceBytes;
	const bool replace_material = !m_hMaterialBuffer.is_valid() || m_nMaterialCapacityBytes < nMaterialBytes;
	const bool replace_indirect = !m_hIndirectBuffer.is_valid() || m_nIndirectCapacityBytes < nIndirectBytes;
#endif
	if ( ( replace_instance || replace_previous_instance || replace_visible || replace_material || replace_indirect )
	        && m_hUniformSet.is_valid() ) {
		m_context.freeResource( m_hUniformSet );
	}
	if ( replace_instance || replace_previous_instance || replace_visible || replace_material ) {
		freeForwardUniformSets();
	}
#if !T3D_INSTANCER_RD_PORT
	if ( replace_indirect && m_hHlodCullUniformSet.is_valid() ) {
		m_context.freeResource( m_hHlodCullUniformSet );
	}
	if ( ( replace_visible || replace_indirect || replace_instance ) && m_hInstanceCullUniformSet.is_valid() ) {
		m_context.freeResource( m_hInstanceCullUniformSet );
	}
#endif
	if ( !m_hFrameBuffer.is_valid() ) {
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd ) {
			setError( "RD instance frame buffer creation lost the RenderingDevice" );
			return false;
		}
		m_hFrameBuffer = rd->uniform_buffer_create( sizeof( t3dRdInstanceFrame_t ) );
		if ( !m_hFrameBuffer.is_valid() ) {
			setError( "RD instance frame buffer creation returned an invalid RID" );
			return false;
		}
		if ( !m_context.registerResource( m_hFrameBuffer,
		             "T3D.Instance.FrameUBO",
		             Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER,
		             Terrain3DRdContext::MEMORY_UNIFORM,
		             sizeof( t3dRdInstanceFrame_t ),
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( m_hFrameBuffer );
			m_hFrameBuffer = RID();
			setError( "RD instance frame buffer registration failed" );
			return false;
		}
	}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	return ensureBuffer( m_hInstanceBuffer,
	               m_nInstanceCapacityBytes,
	               MAX( nInstanceBytes, ( uint32_t )( sizeof( t3dRdInstanceRecord_t ) ) ),
	               "T3D.Instance.Records",
	               Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	               Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureBuffer( m_hVisibleIndexBuffer,
	                m_nVisibleCapacityBytes,
	                MAX( nVisibleBytes, 4u ),
	                "T3D.Instance.VisibleIndices",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                Terrain3DRdContext::MEMORY_INSTANCE );
#else
	const bool base_buffers_ok = ensureBuffer( m_hInstanceBuffer,
	                                     m_nInstanceCapacityBytes,
	                                     MAX( nInstanceBytes, ( uint32_t )( sizeof( t3dRdInstanceRecord_t ) ) ),
	                                     "T3D.Instance.Records",
	                                     Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                                     Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureBuffer( m_hPreviousInstanceBuffer,
	                m_nPreviousInstanceCapacityBytes,
	                MAX( nInstanceBytes, ( uint32_t )( sizeof( t3dRdInstanceRecord_t ) ) ),
	                "T3D.Instance.PreviousRecords",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureBuffer( m_hVisibleIndexBuffer,
	                m_nVisibleCapacityBytes,
	                MAX( nVisibleBytes, 4u ),
	                "T3D.Instance.VisibleIndices",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureBuffer( m_hMaterialBuffer,
	                m_nMaterialCapacityBytes,
	                MAX( nMaterialBytes, ( uint32_t )( sizeof( t3dRdInstanceMaterial_t ) ) ),
	                "T3D.Instance.MaterialTable",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                Terrain3DRdContext::MEMORY_MATERIAL_TABLE );
#if T3D_INSTANCER_RD_PORT
	return base_buffers_ok;
#else
	return base_buffers_ok && ensureBuffer( m_hIndirectBuffer,
	                                  m_nIndirectCapacityBytes,
	                                  MAX( nIndirectBytes, ( uint32_t )( sizeof( t3dRdIndexedIndirectCommand_t ) ) ),
	                                  "T3D.Instance.Indirect",
	                                  Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER,
	                                  Terrain3DRdContext::MEMORY_INDIRECT );
#endif
#endif
}

bool Terrain3DRdInstanceRenderer::ensureShadowBuffers( const uint32_t nVisibleBytes, const uint32_t nIndirectBytes ) {
	const bool replace_visible = !m_hShadowVisibleIndexBuffer.is_valid() || m_nShadowVisibleCapacityBytes < nVisibleBytes;
	if ( replace_visible ) {
		freeForwardUniformSets();
	}
#if T3D_INSTANCER_RD_PORT
	( void )nIndirectBytes;
	return ensureBuffer( m_hShadowVisibleIndexBuffer,
	               m_nShadowVisibleCapacityBytes,
	               MAX( nVisibleBytes, 4u ),
	               "T3D.Instance.ShadowVisibleIndices",
	               Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	               Terrain3DRdContext::MEMORY_INSTANCE );
#else
	const bool replace_indirect = !m_hShadowIndirectBuffer.is_valid() || m_nShadowIndirectCapacityBytes < nIndirectBytes;
	if ( ( replace_visible || replace_indirect ) && m_hInstanceCullUniformSet.is_valid() ) {
		m_context.freeResource( m_hInstanceCullUniformSet );
	}
	return ensureBuffer( m_hShadowVisibleIndexBuffer,
	               m_nShadowVisibleCapacityBytes,
	               MAX( nVisibleBytes, 4u ),
	               "T3D.Instance.ShadowVisibleIndices",
	               Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	               Terrain3DRdContext::MEMORY_INSTANCE )
	        && ensureBuffer( m_hShadowIndirectBuffer,
	                m_nShadowIndirectCapacityBytes,
	                MAX( nIndirectBytes, ( uint32_t )( sizeof( t3dRdIndexedIndirectCommand_t ) ) ),
	                "T3D.Instance.ShadowIndirect",
	                Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER,
	                Terrain3DRdContext::MEMORY_INDIRECT );
#endif
}

bool Terrain3DRdInstanceRenderer::ensureUniformSet( void ) {
	if ( m_hUniformSet.is_valid() ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( t3dRdInstanceBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hInstanceBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleIndexBuffer ) );
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	uniforms.push_back( t3dRdInstanceBufferUniform( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hMaterialBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPreviousInstanceBuffer ) );
#endif
	m_hUniformSet = rd->uniform_set_create( uniforms, m_hShader, 0u );
	if ( !m_hUniformSet.is_valid() ) {
		setError( "RD instance set-0 uniform creation failed" );
		return false;
	}
	if ( !m_context.registerResource( m_hUniformSet,
	             "T3D.Instance.UniformSet",
	             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
	             Terrain3DRdContext::MEMORY_NONE,
	             0u,
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		rd->free_rid( m_hUniformSet );
		m_hUniformSet = RID();
		setError( "RD instance set-0 uniform registration failed" );
		return false;
	}
	return true;
}

void Terrain3DRdInstanceRenderer::freeForwardUniformSets( void ) {
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_context.freeResource( m_hszForwardUniformSets[variant] );
		m_context.freeResource( m_hszForwardShadowUniformSets[variant] );
	}
}

bool Terrain3DRdInstanceRenderer::ensureForwardUniformSet( const t3dRdForwardShaderVariant_t variant, const bool bShadow ) {
	if ( variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX || !m_hszForwardShaders[variant].is_valid() ) {
		setError( String( "RD Forward+ instance set-0 requested an invalid shader variant; variant=" ) + String::num_int64( variant ) );
		return false;
	}
	RID &uniform_set = bShadow ? m_hszForwardShadowUniformSets[variant] : m_hszForwardUniformSets[variant];
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
	uniforms.push_back( t3dRdInstanceBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hInstanceBuffer ) );
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	( void )bShadow;
	uniforms.push_back( t3dRdInstanceBufferUniform( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleIndexBuffer ) );
#else
	uniforms.push_back( t3dRdInstanceBufferUniform(
	        2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, bShadow ? m_hShadowVisibleIndexBuffer : m_hVisibleIndexBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hMaterialBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPreviousInstanceBuffer ) );
#endif
	uniform_set = rd->uniform_set_create( uniforms, m_hszForwardShaders[variant], 0u );
	const String name = String( bShadow ? "T3D.Forward.Instance.ShadowUniformSet." : "T3D.Forward.Instance.UniformSet." )
	        + String::num_int64( variant );
	if ( !uniform_set.is_valid() ) {
		setError( String( "RD Forward+ instance set-0 creation failed; variant=" ) + String::num_int64( variant ) + String( ", shadow=" )
		        + String::num_int64( bShadow ? 1 : 0 ) );
		return false;
	}
	if ( !m_context.registerResource( uniform_set,
	             name,
	             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
	             Terrain3DRdContext::MEMORY_NONE,
	             0u,
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		// Registration failure transitions the shared context to FAILED. Use the
		// already-borrowed RD pointer to release the never-registered RID.
		rd->free_rid( uniform_set );
		uniform_set = RID();
		setError( String( "RD Forward+ instance set-0 registration failed; variant=" ) + String::num_int64( variant )
		        + String( ", shadow=" ) + String::num_int64( bShadow ? 1 : 0 ) );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureMaterialSamplers( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	for ( uint32_t i = 0u; i < 12u; ++i ) {
		if ( m_hMaterialSamplers[i].is_valid() ) {
			continue;
		}
		const uint32_t filter = i % 6u;
		const bool repeat = i >= 6u;
		const bool nearest = filter == 0u || filter == 2u || filter == 4u;
		const bool mipmaps = filter >= 2u;
		const bool anisotropic = filter >= 4u;
		Ref<RDSamplerState> state;
		state.instantiate();
		state->set_mag_filter( nearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_min_filter( nearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_mip_filter(
		        ( filter == 3u || filter == 5u ) ? RenderingDevice::SAMPLER_FILTER_LINEAR : RenderingDevice::SAMPLER_FILTER_NEAREST );
		state->set_min_lod( 0.0f );
		state->set_max_lod( mipmaps ? 1000.0f : 0.0f );
		state->set_use_anisotropy( anisotropic );
		state->set_anisotropy_max( anisotropic ? 16.0f : 1.0f );
		const RenderingDevice::SamplerRepeatMode repeat_mode =
		        repeat ? RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT : RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		state->set_repeat_u( repeat_mode );
		state->set_repeat_v( repeat_mode );
		state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hMaterialSamplers[i] = rd->sampler_create( state );
		const String name = String( "T3D.Instance.MaterialSampler." ) + String::num_int64( i );
		if ( !m_hMaterialSamplers[i].is_valid() ) {
			setError( String( "RD instance material sampler creation failed; sampler=" ) + String::num_int64( i ) );
			return false;
		}
		if ( !m_context.registerResource( m_hMaterialSamplers[i],
		             name,
		             Terrain3DRdContext::RESOURCE_SAMPLER,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( m_hMaterialSamplers[i] );
			m_hMaterialSamplers[i] = RID();
			setError( String( "RD instance material sampler registration failed; sampler=" ) + String::num_int64( i ) );
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::createFallbackMaterialTexture( RID &rTexture, const uint32_t nRgba, const String &rName ) {
	if ( rTexture.is_valid() ) {
		return true;
	}
	RenderingDevice *const pRd = m_context.getDevice();
	if ( !pRd ) {
		return false;
	}
	Ref<RDTextureFormat> format;
	format.instantiate();
	format->set_width( 1u );
	format->set_height( 1u );
	format->set_depth( 1u );
	format->set_array_layers( 1u );
	format->set_mipmaps( 1u );
	format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
	format->set_format( RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM );
	format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT );
	Ref<RDTextureView> view;
	view.instantiate();
	PackedByteArray bytes;
	bytes.resize( sizeof( nRgba ) );
	memcpy( bytes.ptrw(), &nRgba, sizeof( nRgba ) );
	TypedArray<PackedByteArray> initial;
	initial.push_back( bytes );
	rTexture = pRd->texture_create( format, view, initial );
	if ( !rTexture.is_valid() ) {
		m_context.recordResourceCreationFailure(
		        rName, Terrain3DRdContext::RESOURCE_TEXTURE, "Initial-data fallback texture creation failed" );
		setError( String( "Failed to create Terrain3D instance fallback texture: " ) + rName );
		return false;
	}
	if ( !m_context.registerResource( rTexture,
	             rName,
	             Terrain3DRdContext::RESOURCE_TEXTURE,
	             Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
	             sizeof( nRgba ),
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		pRd->free_rid( rTexture );
		rTexture = RID();
		setError( String( "Failed to register Terrain3D instance fallback texture: " ) + rName );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureFallbackMaterialTextures( void ) {
	// RGBA byte order is low-to-high for R,G,B,A in the upload payload.
	return createFallbackMaterialTexture( m_hFallbackAlbedoTexture, 0xFFFFFFFFu, "T3D.Instance.FallbackAlbedo" )
	        && createFallbackMaterialTexture( m_hFallbackNormalTexture, 0xFFFF8080u, "T3D.Instance.FallbackNormal" )
	        && createFallbackMaterialTexture( m_hFallbackOrmTexture, 0xFF00FFFFu, "T3D.Instance.FallbackOrm" );
}

void Terrain3DRdInstanceRenderer::freeMaterialBindings( void ) {
	for ( size_t iBinding = 0u; iBinding < ( size_t )( ( m_materialBindings ).size() ); iBinding++ ) {
		t3dRdInstanceMaterialPairBinding_t &binding = ( m_materialBindings )[iBinding];
		m_context.freeResource( binding.m_hUniformSet );
		for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
			m_context.freeResource( binding.m_hszForwardUniformSets[variant] );
		}
	}
	m_materialBindings.clear();
	m_nMaterialBindingGeneration = 0u;
}

RID Terrain3DRdInstanceRenderer::getMaterialBinding( const uint32_t nMaterial, const uint32_t nOverlayMaterial ) const {
	for ( size_t iBinding = 0u; iBinding < ( size_t )( ( m_materialBindings ).size() ); iBinding++ ) {
		const t3dRdInstanceMaterialPairBinding_t &binding = ( m_materialBindings )[iBinding];
		if ( binding.m_nMaterial == nMaterial && binding.m_nOverlayMaterial == nOverlayMaterial ) {
			return binding.m_hUniformSet;
		}
	}
	return RID();
}

RID Terrain3DRdInstanceRenderer::getForwardMaterialBinding(
		const uint32_t nMaterial, const uint32_t nOverlayMaterial, const t3dRdForwardShaderVariant_t variant ) const {
	if ( variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ) {
		return RID();
	}
	for ( size_t iBinding = 0u; iBinding < m_materialBindings.size(); ++iBinding ) {
		const t3dRdInstanceMaterialPairBinding_t &binding = m_materialBindings[iBinding];
		if ( binding.m_nMaterial == nMaterial && binding.m_nOverlayMaterial == nOverlayMaterial ) {
			return binding.m_hszForwardUniformSets[variant];
		}
	}
	return RID();
}

const t3dRdInstanceMaterial_t *Terrain3DRdInstanceRenderer::getMaterialPtr( const uint32_t nIndex ) const {
	const t3dRdInstanceMaterial_t *pMaterials = m_meshCache.getMaterials();
	const size_t nMaterialCount = m_meshCache.getMaterialCount();
	return nIndex != UINT16_MAX && nIndex < nMaterialCount ? &pMaterials[nIndex] : NULL;
}

RID Terrain3DRdInstanceRenderer::getMaterialTexture(
        const t3dRdInstanceMaterial_t *const pMaterial, const uint32_t nSlot, const RID &rFallback ) const {
	if ( !pMaterial ) {
		return rFallback;
	}
	uint64_t nId = 0u;
	if ( nSlot == 0u ) {
		nId = t3dRdInstanceTextureId( pMaterial->m_nAlbedoTextureIdLow, pMaterial->m_nAlbedoTextureIdHigh );
	} else if ( nSlot == 1u ) {
		nId = t3dRdInstanceTextureId( pMaterial->m_nNormalTextureIdLow, pMaterial->m_nNormalTextureIdHigh );
	} else {
		nId = t3dRdInstanceTextureId( pMaterial->m_nOrmTextureIdLow, pMaterial->m_nOrmTextureIdHigh );
	}
	const RID hTexture = m_meshCache.getTextureRid( nId );
	return hTexture.is_valid() ? hTexture : rFallback;
}

RID Terrain3DRdInstanceRenderer::getMaterialSampler( const t3dRdInstanceMaterial_t *const pMaterial ) const {
	if ( !pMaterial ) {
		return m_hMaterialSamplers[7u];
	}
	const uint32_t nFilter = ( pMaterial->m_nFlags & T3D_RD_INSTANCE_MATERIAL_FILTER_MASK ) >> T3D_RD_INSTANCE_MATERIAL_FILTER_SHIFT;
	const uint32_t nRepeat = ( pMaterial->m_nFlags & T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT ) != 0u ? 6u : 0u;
	return m_hMaterialSamplers[MIN( nFilter, 5u ) + nRepeat];
}

bool Terrain3DRdInstanceRenderer::addMaterialBinding( const uint32_t nMaterial, const uint32_t nOverlay ) {
	if ( getMaterialBinding( nMaterial, nOverlay ).is_valid() ) {
		return true;
	}
	const t3dRdInstanceMaterial_t *const pBase = getMaterialPtr( nMaterial );
	const t3dRdInstanceMaterial_t *const pOverlay = getMaterialPtr( nOverlay );
	const RID hBaseSampler = getMaterialSampler( pBase );
	const RID hOverlaySampler = getMaterialSampler( pOverlay );
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( String( "RD instance material set-1 lost the RenderingDevice; material=" ) + String::num_int64( nMaterial )
		        + String( ", overlay=" ) + String::num_int64( nOverlay ) );
		return false;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( t3dRdInstanceTextureUniform( 0, hBaseSampler, getMaterialTexture( pBase, 0u, m_hFallbackAlbedoTexture ) ) );
	uniforms.push_back( t3dRdInstanceTextureUniform( 1, hBaseSampler, getMaterialTexture( pBase, 1u, m_hFallbackNormalTexture ) ) );
	uniforms.push_back( t3dRdInstanceTextureUniform( 2, hBaseSampler, getMaterialTexture( pBase, 2u, m_hFallbackOrmTexture ) ) );
	uniforms.push_back( t3dRdInstanceTextureUniform( 3, hOverlaySampler, getMaterialTexture( pOverlay, 0u, m_hFallbackAlbedoTexture ) ) );
	uniforms.push_back( t3dRdInstanceTextureUniform( 4, hOverlaySampler, getMaterialTexture( pOverlay, 1u, m_hFallbackNormalTexture ) ) );
	uniforms.push_back( t3dRdInstanceTextureUniform( 5, hOverlaySampler, getMaterialTexture( pOverlay, 2u, m_hFallbackOrmTexture ) ) );
	RID hSet = rd->uniform_set_create( uniforms, m_hShader, 1u );
	const String name = String( "T3D.Instance.MaterialPair." ) + String::num_int64( nMaterial ) + "." + String::num_int64( nOverlay );
	if ( !hSet.is_valid() ) {
		setError( String( "RD instance material set-1 creation failed; material=" ) + String::num_int64( nMaterial )
		        + String( ", overlay=" ) + String::num_int64( nOverlay ) );
		return false;
	}
	if ( !m_context.registerResource( hSet,
	             name,
	             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
	             Terrain3DRdContext::MEMORY_NONE,
	             0u,
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		rd->free_rid( hSet );
		setError( String( "RD instance material set-1 registration failed; material=" ) + String::num_int64( nMaterial )
		        + String( ", overlay=" ) + String::num_int64( nOverlay ) );
		return false;
	}
	t3dRdInstanceMaterialPairBinding_t binding;
	binding.m_nMaterial = ( uint16_t )( nMaterial );
	binding.m_nOverlayMaterial = ( uint16_t )( nOverlay );
	binding.m_hUniformSet = hSet;
	if ( !t3dVectorPushBackWithinCapacity( m_materialBindings, binding ) ) {
		m_context.freeResource( hSet );
		setError( String( "RD instance material binding table exceeded sealed capacity; material=" ) + String::num_int64( nMaterial )
		        + String( ", overlay=" ) + String::num_int64( nOverlay ) + String( ", capacity=" )
		        + String::num_int64( m_materialBindings.capacity() ) );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureForwardMaterialBindings( const t3dRdForwardShaderVariant_t variant ) {
	if ( variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX || !m_hszForwardShaders[variant].is_valid() ) {
		setError( String( "RD Forward+ instance material set-1 requested an invalid shader variant; variant=" )
		        + String::num_int64( variant ) );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( String( "RD Forward+ instance material set-1 lost the RenderingDevice; variant=" ) + String::num_int64( variant ) );
		return false;
	}
	for ( size_t iBinding = 0u; iBinding < m_materialBindings.size(); ++iBinding ) {
		t3dRdInstanceMaterialPairBinding_t &binding = m_materialBindings[iBinding];
		if ( binding.m_hszForwardUniformSets[variant].is_valid() ) {
			continue;
		}
		const t3dRdInstanceMaterial_t *const pBase = getMaterialPtr( binding.m_nMaterial );
		const t3dRdInstanceMaterial_t *const pOverlay = getMaterialPtr( binding.m_nOverlayMaterial );
		const RID hBaseSampler = getMaterialSampler( pBase );
		const RID hOverlaySampler = getMaterialSampler( pOverlay );
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back( t3dRdInstanceTextureUniform( 0, hBaseSampler, getMaterialTexture( pBase, 0u, m_hFallbackAlbedoTexture ) ) );
		uniforms.push_back( t3dRdInstanceTextureUniform( 1, hBaseSampler, getMaterialTexture( pBase, 1u, m_hFallbackNormalTexture ) ) );
		uniforms.push_back( t3dRdInstanceTextureUniform( 2, hBaseSampler, getMaterialTexture( pBase, 2u, m_hFallbackOrmTexture ) ) );
		uniforms.push_back(
		        t3dRdInstanceTextureUniform( 3, hOverlaySampler, getMaterialTexture( pOverlay, 0u, m_hFallbackAlbedoTexture ) ) );
		uniforms.push_back(
		        t3dRdInstanceTextureUniform( 4, hOverlaySampler, getMaterialTexture( pOverlay, 1u, m_hFallbackNormalTexture ) ) );
		uniforms.push_back(
		        t3dRdInstanceTextureUniform( 5, hOverlaySampler, getMaterialTexture( pOverlay, 2u, m_hFallbackOrmTexture ) ) );
		RID hSet = rd->uniform_set_create( uniforms, m_hszForwardShaders[variant], 1u );
		const String name = String( "T3D.Forward.Instance.MaterialPair." ) + String::num_int64( variant ) + String( "." )
		        + String::num_int64( binding.m_nMaterial ) + String( "." ) + String::num_int64( binding.m_nOverlayMaterial );
		if ( !hSet.is_valid() ) {
			setError( String( "RD Forward+ instance material set-1 creation failed; variant=" ) + String::num_int64( variant )
			        + String( ", material=" ) + String::num_int64( binding.m_nMaterial ) + String( ", overlay=" )
			        + String::num_int64( binding.m_nOverlayMaterial ) );
			return false;
		}
		if ( !m_context.registerResource( hSet,
		             name,
		             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( hSet );
			setError( String( "RD Forward+ instance material set-1 registration failed; variant=" ) + String::num_int64( variant )
			        + String( ", material=" ) + String::num_int64( binding.m_nMaterial ) + String( ", overlay=" )
			        + String::num_int64( binding.m_nOverlayMaterial ) );
			return false;
		}
		binding.m_hszForwardUniformSets[variant] = hSet;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::rebuildMaterialBindings( void ) {
	if ( m_nMaterialBindingGeneration == m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
		return true;
	}
	m_context.beginResourceRebuild();
	freeMaterialBindings();
	if ( !ensureMaterialSamplers() || !ensureFallbackMaterialTextures() ) {
		m_context.endResourceRebuild();
		return false;
	}
	const size_t nMaterialBindingCapacity = m_meshCache.getSurfaceCount() + 1u;
	if ( !t3dContainerReserveCold( m_materialBindings, nMaterialBindingCapacity ) ) {
		setError( String( "RD instance material binding reserve failed; required=" ) + String::num_int64( nMaterialBindingCapacity )
		        + String( ", current_capacity=" ) + String::num_int64( m_materialBindings.capacity() ) );
		m_context.endResourceRebuild();
		return false;
	}
	if ( !addMaterialBinding( UINT16_MAX, UINT16_MAX ) ) {
		m_context.endResourceRebuild();
		return false;
	}
	const t3dRdMeshSourceSurface_t *pSurfaces = m_meshCache.getSurfaces();
	const size_t nSurfaceCount = m_meshCache.getSurfaceCount();
	for ( size_t i = 0u; i < nSurfaceCount; ++i ) {
		const t3dRdMeshSourceSurface_t &rSurface = pSurfaces[i];
		if ( !addMaterialBinding( rSurface.m_nMaterial, rSurface.m_nOverlayMaterial ) ) {
			m_context.endResourceRebuild();
			return false;
		}
	}
	m_nMaterialBindingGeneration = m_meshCache.getSourceGeneration();
	m_context.endResourceRebuild();
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadScene( const t3dRdInstanceSceneSnapshot_t *const pScene ) {
	if ( !pScene ) {
		return false;
	}
	if ( pScene->m_nGeneration == m_nUploadedSceneGeneration && m_nSceneCapacityMeshGeneration == m_meshCache.getSourceGeneration() ) {
		return true;
	}
	if ( pScene->m_instances.size() > UINT32_MAX / sizeof( t3dRdInstanceRecord_t ) ) {
		setError( "RD instance publication exceeds 32-bit buffer limits" );
		return false;
	}

	uint64_t packet_capacity64 = 0u;
	uint64_t visible_index_capacity64 = 0u;
	for ( size_t iCell = 0u; iCell < ( size_t )( ( pScene->m_cells ).size() ); iCell++ ) {
		const t3dRdInstanceCell_t &cell = ( pScene->m_cells )[iCell];
		const t3dRdMeshHandle_t mesh = m_meshCache.getMeshHandleBySourceKey(
		        cell.m_nMeshSourceKey != 0u ? cell.m_nMeshSourceKey : t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = m_meshCache.resolveMesh( mesh );
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

	m_nPacketCapacity = ( uint32_t )( packet_capacity64 );
	m_nVisibleIndexCapacity = ( uint32_t )( visible_index_capacity64 );
	const uint32_t instance_bytes = ( uint32_t )( pScene->m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
	const uint32_t visible_bytes = m_nVisibleIndexCapacity * sizeof( uint32_t );
	const uint32_t material_bytes = ( uint32_t )( m_meshCache.getMaterialCount() * sizeof( t3dRdInstanceMaterial_t ) );
	const uint32_t indirect_bytes = m_nPacketCapacity * sizeof( t3dRdIndexedIndirectCommand_t );
	const uint32_t required_instance_capacity = MAX( instance_bytes, ( uint32_t )( sizeof( t3dRdInstanceRecord_t ) ) );
	const bool full_instance_upload = pScene->m_bTopologyChanged || !m_hInstanceBuffer.is_valid()
	        || m_nInstanceCapacityBytes < required_instance_capacity || m_nInstanceUsedBytes != instance_bytes;

	// The bring-up lane owns only the CPU structures consumed by reference packet
	// construction and direct indexed submission. Shadow, motion, HLOD and GPU-cull
	// backing must not be prerequisites for proving basic instance rasterization.
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	const bool cpu_capacity_rebuild = m_visibleIndices.size() < m_nVisibleIndexCapacity || m_alphaSortScratch.size() < m_nPacketCapacity
	        || m_drawPackets.capacity() < m_nPacketCapacity || m_depthPackets.capacity() < m_nPacketCapacity
	        || m_packetBuildSlots.capacity() < m_nPacketCapacity || m_packetBuildEntries.capacity() < m_nPacketCapacity
	        || m_alphaPackets.capacity() < m_nPacketCapacity || m_packetIndex.capacity() < m_nPacketCapacity;
	if ( cpu_capacity_rebuild ) {
		++m_nCpuCapacityRebuildCount;
	}
	bool fixed_storage_ok = true;
	if ( m_visibleIndices.size() < m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibleIndices, m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_alphaSortScratch.size() < m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_alphaSortScratch, m_nPacketCapacity );
	}
	if ( !fixed_storage_ok || !t3dContainerReserveCold( m_drawPackets, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_depthPackets, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_packetBuildSlots, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_packetBuildEntries, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_alphaPackets, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_packetIndex, m_nPacketCapacity ) ) {
		++m_nCpuCapacityRebuildFailureCount;
		setError( "RD direct-surface owner failed to establish minimal packet backing" );
		return false;
	}
#else
	// Scene publication is an explicit renderer-owner capacity transition. It is
	// entered only from a Terrain3DColdPhaseScope on the render thread; generic
	// allocator phase state does not authorize this growth.
	const bool cpu_capacity_rebuild = m_visibleIndices.size() < m_nVisibleIndexCapacity
	        || m_shadowVisibleIndices.size() < m_nVisibleIndexCapacity || m_alphaSortScratch.size() < m_nPacketCapacity
	        || m_motionHistoryRecords.size() < pScene->m_instances.size() || m_motionPreviousRecords.size() < pScene->m_instances.size()
	        || m_motionHistoryCells.size() < pScene->m_cells.size() || m_motionCurrentOrder.size() < pScene->m_cells.size()
	        || m_motionPreviousOrder.size() < pScene->m_cells.size() || m_motionDirtyRanges.capacity() < pScene->m_dirtyRanges.capacity()
	        || m_drawPackets.capacity() < m_nPacketCapacity || m_depthPackets.capacity() < m_nPacketCapacity
	        || m_shadowPackets.capacity() < m_nPacketCapacity || m_hlodDescriptors.capacity() < m_nPacketCapacity
	        || m_instanceCullDescriptors.capacity() < m_nPacketCapacity || m_shadowInstanceCullDescriptors.capacity() < m_nPacketCapacity
	        || m_instanceCullCellSpans.size() < pScene->m_cells.size() || m_shadowInstanceCullCellSpans.size() < pScene->m_cells.size()
	        || m_packetBuildSlots.capacity() < m_nPacketCapacity || m_packetBuildEntries.capacity() < m_nPacketCapacity
	        || m_alphaPackets.capacity() < m_nPacketCapacity || m_packetIndex.capacity() < m_nPacketCapacity;
	if ( cpu_capacity_rebuild ) {
		++m_nCpuCapacityRebuildCount;
	}
	bool fixed_storage_ok = true;
	if ( m_visibleIndices.size() < m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_visibleIndices, m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_shadowVisibleIndices.size() < m_nVisibleIndexCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_shadowVisibleIndices, m_nVisibleIndexCapacity );
	}
	if ( fixed_storage_ok && m_alphaSortScratch.size() < m_nPacketCapacity ) {
		fixed_storage_ok = t3dVectorResizeCold( m_alphaSortScratch, m_nPacketCapacity );
	}
	if ( fixed_storage_ok && m_motionHistoryRecords.size() < pScene->m_instances.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_motionHistoryRecords, pScene->m_instances.size() );
	}
	if ( fixed_storage_ok && m_motionPreviousRecords.size() < pScene->m_instances.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_motionPreviousRecords, pScene->m_instances.size() );
	}
	if ( fixed_storage_ok && m_motionHistoryCells.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_motionHistoryCells, pScene->m_cells.size() );
	}
	if ( fixed_storage_ok && m_motionCurrentOrder.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_motionCurrentOrder, pScene->m_cells.size() );
	}
	if ( fixed_storage_ok && m_motionPreviousOrder.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_motionPreviousOrder, pScene->m_cells.size() );
	}
	if ( fixed_storage_ok && m_motionDirtyRanges.capacity() < pScene->m_dirtyRanges.capacity() ) {
		fixed_storage_ok = t3dContainerReserveCold( m_motionDirtyRanges, pScene->m_dirtyRanges.capacity() );
	}
	if ( fixed_storage_ok && m_instanceCullCellSpans.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_instanceCullCellSpans, pScene->m_cells.size() );
	}
	if ( fixed_storage_ok && m_shadowInstanceCullCellSpans.size() < pScene->m_cells.size() ) {
		fixed_storage_ok = t3dVectorResizeCold( m_shadowInstanceCullCellSpans, pScene->m_cells.size() );
	}
	if ( !fixed_storage_ok || !t3dContainerReserveCold( m_drawPackets, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_depthPackets, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_shadowPackets, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_hlodDescriptors, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_instanceCullDescriptors, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_shadowInstanceCullDescriptors, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_packetBuildSlots, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_packetBuildEntries, m_nPacketCapacity )
	        || !t3dContainerReserveCold( m_alphaPackets, m_nPacketCapacity ) ) {
		++m_nCpuCapacityRebuildFailureCount;
		setError( "RD instance owner failed to establish fixed publication backing" );
		return false;
	}
	if ( !t3dContainerReserveCold( m_packetIndex, m_nPacketCapacity ) ) {
		++m_nCpuCapacityRebuildFailureCount;
		setError( "RD instance owner failed to establish packet-index capacity" );
		return false;
	}
#endif

#if T3D_INSTANCER_RD_PORT
	m_context.beginResourceRebuild();
	const bool buffers_ok = ensureBuffers( instance_bytes,
	                                MAX( visible_bytes, 4u ),
	                                MAX( material_bytes, ( uint32_t )( sizeof( t3dRdInstanceMaterial_t ) ) ),
	                                0u )
	        && ensureShadowBuffers( MAX( visible_bytes, 4u ), 0u )
	        && ensureUniformSet();
	m_context.endResourceRebuild();
#elif T3D_INSTANCER_MINIMAL_RENDER_PATH
	m_context.beginResourceRebuild();
	const bool buffers_ok = ensureBuffers( instance_bytes,
	                                MAX( visible_bytes, 4u ),
	                                MAX( material_bytes, ( uint32_t )( sizeof( t3dRdInstanceMaterial_t ) ) ),
	                                0u )
	        && ensureUniformSet();
	m_context.endResourceRebuild();
#else
	const uint32_t hlod_descriptor_bytes = MAX(
	        m_nPacketCapacity * ( uint32_t )( sizeof( t3dRdHlodCullDescriptor_t ) ), ( uint32_t )( sizeof( t3dRdHlodCullDescriptor_t ) ) );
	const uint64_t instance_cull_descriptor_bytes64 =
	        ( uint64_t )MAX( m_nPacketCapacity, 1u ) * 2u * sizeof( t3dRdInstanceCullDescriptor_t );
	if ( instance_cull_descriptor_bytes64 > UINT32_MAX ) {
		setError( "RD instance cull descriptor capacity exceeds 32-bit buffer limits" );
		return false;
	}
	const uint32_t instance_cull_descriptor_bytes = ( uint32_t )instance_cull_descriptor_bytes64;
	if ( ( !m_hInstanceCullDescriptorBuffer.is_valid() || m_nInstanceCullDescriptorCapacityBytes < instance_cull_descriptor_bytes )
	        && m_hInstanceCullUniformSet.is_valid() ) {
		m_context.freeResource( m_hInstanceCullUniformSet );
	}
	if ( ( !m_hHlodCullDescriptorBuffer.is_valid() || m_nHlodDescriptorCapacityBytes < hlod_descriptor_bytes )
	        && m_hHlodCullUniformSet.is_valid() ) {
		m_context.freeResource( m_hHlodCullUniformSet );
	}
	m_context.beginResourceRebuild();
	const bool buffers_ok = ensureBuffers( instance_bytes,
	                                MAX( visible_bytes, 4u ),
	                                MAX( material_bytes, ( uint32_t )( sizeof( t3dRdInstanceMaterial_t ) ) ),
	                                MAX( indirect_bytes, ( uint32_t )( sizeof( t3dRdIndexedIndirectCommand_t ) ) ) )
	        && ensureShadowBuffers(
	                MAX( visible_bytes, 4u ), MAX( indirect_bytes, ( uint32_t )( sizeof( t3dRdIndexedIndirectCommand_t ) ) ) )
	        && ensureBuffer( m_hHlodCullDescriptorBuffer,
	                m_nHlodDescriptorCapacityBytes,
	                hlod_descriptor_bytes,
	                "T3D.HLOD.CullDescriptors",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                Terrain3DRdContext::MEMORY_INSTANCE,
	                Terrain3DRdContext::OWNER_HLOD )
	        && ensureBuffer( m_hInstanceCullDescriptorBuffer,
	                m_nInstanceCullDescriptorCapacityBytes,
	                instance_cull_descriptor_bytes,
	                "T3D.Instance.CullDescriptors",
	                Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                Terrain3DRdContext::MEMORY_INSTANCE,
	                Terrain3DRdContext::OWNER_INSTANCES )
	        && ensureUniformSet();
	m_context.endResourceRebuild();
#endif
	if ( !buffers_ok ) {
		return false;
	}

	if ( m_instanceBytes.size() < ( int64_t )( m_nInstanceCapacityBytes ) ) {
		m_instanceBytes.resize( m_nInstanceCapacityBytes );
	}
	if ( m_previousInstanceBytes.size() < ( int64_t )( m_nPreviousInstanceCapacityBytes ) ) {
		m_previousInstanceBytes.resize( m_nPreviousInstanceCapacityBytes );
	}
	if ( m_visibleBytes.size() < ( int64_t )( m_nVisibleCapacityBytes ) ) {
		m_visibleBytes.resize( m_nVisibleCapacityBytes );
	}
	if ( m_shadowVisibleBytes.size() < ( int64_t )( m_nShadowVisibleCapacityBytes ) ) {
		m_shadowVisibleBytes.resize( m_nShadowVisibleCapacityBytes );
	}
	if ( m_indirectBytes.size() < ( int64_t )( m_nIndirectCapacityBytes ) ) {
		m_indirectBytes.resize( m_nIndirectCapacityBytes );
	}
	if ( m_shadowIndirectBytes.size() < ( int64_t )( m_nShadowIndirectCapacityBytes ) ) {
		m_shadowIndirectBytes.resize( m_nShadowIndirectCapacityBytes );
	}
	if ( m_hlodDescriptorBytes.size() < ( int64_t )( m_nHlodDescriptorCapacityBytes ) ) {
		m_hlodDescriptorBytes.resize( m_nHlodDescriptorCapacityBytes );
	}
	if ( m_instanceCullDescriptorBytes.size() < ( int64_t )( m_nInstanceCullDescriptorCapacityBytes ) ) {
		m_instanceCullDescriptorBytes.resize( m_nInstanceCullDescriptorCapacityBytes );
	}

	const uint32_t dirty_chunk_bytes = s_nDirtyChunkRecords * sizeof( t3dRdInstanceRecord_t );
	if ( m_dirtyInstanceBytes.size() < ( int64_t )( dirty_chunk_bytes ) ) {
		m_dirtyInstanceBytes.resize( dirty_chunk_bytes );
	}

	uint32_t uploaded_bytes = 0u;
	if ( instance_bytes > 0u ) {
		const uint8_t *const pSource = ( const uint8_t * )( pScene->m_instances.data() );
		RenderingDevice *const pRd = m_context.getDevice();
		if ( full_instance_upload ) {
			memcpy( m_instanceBytes.ptrw(), pSource, instance_bytes );
			if ( pRd->buffer_update( m_hInstanceBuffer, 0u, instance_bytes, m_instanceBytes ) != OK ) {
				return false;
			}
			uploaded_bytes = instance_bytes;
			++m_nDirtyUploadRanges;
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
					memcpy( m_dirtyInstanceBytes.ptrw(), pSource + first_byte, chunk_bytes );
					if ( pRd->buffer_update( m_hInstanceBuffer, first_byte, chunk_bytes, m_dirtyInstanceBytes ) != OK ) {
						return false;
					}
					uploaded_bytes += chunk_bytes;
					++m_nDirtyUploadRanges;
					first_record += record_count;
					remaining -= record_count;
				}
			}
		}
	}
	if ( uploaded_bytes > 0u ) {
		m_context.recordBufferUpdate( uploaded_bytes );
	}
	m_nInstanceUsedBytes = instance_bytes;
	m_nDirtyUploadBytes += uploaded_bytes;
	++m_nSceneUploads;
	m_nUploadedSceneGeneration = pScene->m_nGeneration;
	m_nSceneCapacityMeshGeneration = m_meshCache.getSourceGeneration();
	m_nStaticInstances = pScene->m_instances.size();
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadPreviousMotionRanges( const t3dRdInstanceRecord_t *const pRecords,
        const eastl::vector<t3dRdInstanceDirtyRange_t, Terrain3DEastlAllocator> &rRanges ) {
	if ( !pRecords ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	uint32_t uploaded_bytes = 0u;
	for ( size_t iRange = 0u; iRange < rRanges.size(); ++iRange ) {
		const t3dRdInstanceDirtyRange_t &range = rRanges[iRange];
		if ( range.m_nInstanceCount == 0u ) {
			continue;
		}
		const uint64_t end = ( uint64_t )range.m_nFirstInstance + range.m_nInstanceCount;
		if ( end > m_nMotionHistoryRecordCount ) {
			setError( "RD motion dirty range exceeded retained history" );
			return false;
		}
		uint32_t first_record = range.m_nFirstInstance;
		uint32_t remaining = range.m_nInstanceCount;
		while ( remaining > 0u ) {
			const uint32_t record_count = MIN( remaining, s_nDirtyChunkRecords );
			const uint32_t byte_offset = first_record * sizeof( t3dRdInstanceRecord_t );
			const uint32_t byte_count = record_count * sizeof( t3dRdInstanceRecord_t );
			memcpy( m_dirtyInstanceBytes.ptrw(), pRecords + first_record, byte_count );
			if ( rd->buffer_update( m_hPreviousInstanceBuffer, byte_offset, byte_count, m_dirtyInstanceBytes ) != OK ) {
				return false;
			}
			uploaded_bytes += byte_count;
			first_record += record_count;
			remaining -= record_count;
		}
	}
	if ( uploaded_bytes > 0u ) {
		m_context.recordBufferUpdate( uploaded_bytes );
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::prepareMotionHistory( const t3dRdInstanceSceneSnapshot_t &rScene, const uint64_t nRenderFrame ) {
	if ( m_nMotionHistoryRenderFrame == nRenderFrame && rScene.m_nGeneration == m_nMotionHistorySceneGeneration ) {
		return true;
	}
	if ( rScene.m_instances.empty() ) {
		m_nMotionHistoryRecordCount = 0u;
		m_nMotionHistoryCellCount = 0u;
		m_nMotionPreviousRecordCount = 0u;
		m_nMotionCurrentOrderCount = 0u;
		m_nMotionPreviousOrderCount = 0u;
		m_motionDirtyRanges.clear();
		m_nMotionHistorySceneGeneration = rScene.m_nGeneration;
		m_bMotionHistoryNeedsAdvance = false;
		m_nMotionHistoryRenderFrame = nRenderFrame;
		return true;
	}

	const bool reset_history = m_nMotionHistoryRecordCount == 0u || m_nMotionHistoryCellCount == 0u;
	const bool scene_changed = rScene.m_nGeneration != m_nMotionHistorySceneGeneration;
	const bool stable_topology = !reset_history && !rScene.m_bTopologyChanged && m_nMotionHistoryRecordCount == rScene.m_instances.size()
	        && m_nMotionHistoryCellCount == rScene.m_cells.size();

	if ( stable_topology ) {
		if ( scene_changed ) {
			if ( rScene.m_dirtyRanges.size() > m_motionDirtyRanges.capacity() ) {
				setError( "RD motion dirty-range publication exceeded sealed capacity" );
				return false;
			}
			m_motionDirtyRanges.clear();
			for ( size_t iRange = 0u; iRange < rScene.m_dirtyRanges.size(); ++iRange ) {
				const t3dRdInstanceDirtyRange_t range = rScene.m_dirtyRanges[iRange];
				const uint64_t end = ( uint64_t )range.m_nFirstInstance + range.m_nInstanceCount;
				if ( end > rScene.m_instances.size() ) {
					setError( "RD motion source dirty range exceeded current scene" );
					return false;
				}
				if ( range.m_nInstanceCount > 0u && !t3dVectorPushBackWithinCapacity( m_motionDirtyRanges, range ) ) {
					return false;
				}
			}
			if ( !uploadPreviousMotionRanges( m_motionHistoryRecords.data(), m_motionDirtyRanges ) ) {
				return false;
			}
			for ( size_t iRange = 0u; iRange < m_motionDirtyRanges.size(); ++iRange ) {
				const t3dRdInstanceDirtyRange_t &range = m_motionDirtyRanges[iRange];
				memcpy( m_motionHistoryRecords.data() + range.m_nFirstInstance,
				        rScene.m_instances.data() + range.m_nFirstInstance,
				        range.m_nInstanceCount * sizeof( t3dRdInstanceRecord_t ) );
			}
			if ( !rScene.m_cells.empty() ) {
				memcpy( m_motionHistoryCells.data(), rScene.m_cells.data(), rScene.m_cells.size() * sizeof( t3dRdInstanceCell_t ) );
			}
			m_nMotionHistorySceneGeneration = rScene.m_nGeneration;
			m_bMotionHistoryNeedsAdvance = !m_motionDirtyRanges.empty();
		} else if ( m_bMotionHistoryNeedsAdvance ) {
			if ( !uploadPreviousMotionRanges( m_motionHistoryRecords.data(), m_motionDirtyRanges ) ) {
				return false;
			}
			m_motionDirtyRanges.clear();
			m_bMotionHistoryNeedsAdvance = false;
		}
		m_nMotionHistoryRenderFrame = nRenderFrame;
		return true;
	}

	if ( !scene_changed && !m_bMotionHistoryNeedsAdvance ) {
		m_nMotionHistoryRenderFrame = nRenderFrame;
		return true;
	}

	bool upload_previous = false;
	if ( rScene.m_instances.size() > m_motionPreviousRecords.size() ) {
		setError( "RD motion previous-record output exceeded publication-time capacity" );
		return false;
	}
	m_nMotionPreviousRecordCount = ( uint32_t )rScene.m_instances.size();
	memcpy( m_motionPreviousRecords.data(), rScene.m_instances.data(), rScene.m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
	m_motionDirtyRanges.clear();
	if ( reset_history ) {
		upload_previous = true;
		++m_nMotionHistoryResets;
		m_bMotionHistoryNeedsAdvance = false;
	} else if ( scene_changed ) {
		if ( rScene.m_cells.size() > m_motionCurrentOrder.size() || m_nMotionHistoryCellCount > m_motionPreviousOrder.size() ) {
			setError( "RD motion cell-order output exceeded publication-time capacity" );
			return false;
		}
		m_nMotionCurrentOrderCount = ( uint32_t )rScene.m_cells.size();
		m_nMotionPreviousOrderCount = m_nMotionHistoryCellCount;
		for ( uint32_t i = 0u; i < m_nMotionCurrentOrderCount; ++i ) {
			m_motionCurrentOrder[i] = i;
		}
		for ( uint32_t i = 0u; i < m_nMotionPreviousOrderCount; ++i ) {
			m_motionPreviousOrder[i] = i;
		}
		t3dRdMotionCellIndexLess_t currentLess;
		currentLess.m_pCells = rScene.m_cells.data();
		eastl::sort( m_motionCurrentOrder.begin(), m_motionCurrentOrder.begin() + m_nMotionCurrentOrderCount, currentLess );
		t3dRdMotionCellIndexLess_t previousLess;
		previousLess.m_pCells = m_motionHistoryCells.data();
		eastl::sort( m_motionPreviousOrder.begin(), m_motionPreviousOrder.begin() + m_nMotionPreviousOrderCount, previousLess );

		size_t current = 0u;
		size_t previous = 0u;
		while ( current < m_nMotionCurrentOrderCount && previous < m_nMotionPreviousOrderCount ) {
			const t3dRdInstanceCell_t &current_cell = rScene.m_cells[m_motionCurrentOrder[current]];
			const t3dRdInstanceCell_t &previous_cell = m_motionHistoryCells[m_motionPreviousOrder[previous]];
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
			if ( current_end <= rScene.m_instances.size() && previous_end <= m_nMotionHistoryRecordCount
			        && t3dRdMotionCellsMatch( current_cell, previous_cell ) ) {
				memcpy( m_motionPreviousRecords.data() + current_cell.m_nFirstInstance,
				        m_motionHistoryRecords.data() + previous_cell.m_nFirstInstance,
				        current_cell.m_nInstanceCount * sizeof( t3dRdInstanceRecord_t ) );
				m_nMotionHistoryMatches += current_cell.m_nInstanceCount;
			}
			++current;
			++previous;
		}
		upload_previous = true;
		m_bMotionHistoryNeedsAdvance = true;
	} else if ( m_bMotionHistoryNeedsAdvance ) {
		upload_previous = true;
		m_bMotionHistoryNeedsAdvance = false;
	}

	if ( upload_previous ) {
		RenderingDevice *const rd = m_context.getDevice();
		const uint32_t bytes = m_nMotionPreviousRecordCount * sizeof( t3dRdInstanceRecord_t );
		if ( bytes > m_nPreviousInstanceCapacityBytes || m_previousInstanceBytes.size() < bytes ) {
			setError( "RD motion history exceeded publication-time capacity" );
			return false;
		}
		memcpy( m_previousInstanceBytes.ptrw(), m_motionPreviousRecords.data(), bytes );
		if ( rd->buffer_update( m_hPreviousInstanceBuffer, 0u, bytes, m_previousInstanceBytes ) != OK ) {
			return false;
		}
		m_context.recordBufferUpdate( bytes );
	}

	if ( reset_history || scene_changed ) {
		if ( rScene.m_instances.size() > m_motionHistoryRecords.size() || rScene.m_cells.size() > m_motionHistoryCells.size() ) {
			setError( "RD motion history exceeded publication-time backing" );
			return false;
		}
		m_nMotionHistoryRecordCount = ( uint32_t )rScene.m_instances.size();
		memcpy( m_motionHistoryRecords.data(), rScene.m_instances.data(), rScene.m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
		m_nMotionHistoryCellCount = ( uint32_t )rScene.m_cells.size();
		if ( !rScene.m_cells.empty() ) {
			memcpy( m_motionHistoryCells.data(), rScene.m_cells.data(), rScene.m_cells.size() * sizeof( t3dRdInstanceCell_t ) );
		}
		m_nMotionHistorySceneGeneration = rScene.m_nGeneration;
	}
	m_nMotionHistoryRenderFrame = nRenderFrame;
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadMaterials( void ) {
	if ( m_nUploadedMeshGeneration == m_meshCache.getSourceGeneration()
	        && m_nMaterialBindingGeneration == m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
		return true;
	}
	const t3dRdInstanceMaterial_t *pMaterials = m_meshCache.getMaterials();
	const size_t nMaterialCount = m_meshCache.getMaterialCount();
	const uint32_t bytes = ( uint32_t )( nMaterialCount * sizeof( t3dRdInstanceMaterial_t ) );
	m_context.beginResourceRebuild();
	const bool buffers_ok = ensureBuffers( m_nInstanceCapacityBytes,
	                                m_nVisibleCapacityBytes,
	                                MAX( bytes, ( uint32_t )( sizeof( t3dRdInstanceMaterial_t ) ) ),
	                                m_nIndirectCapacityBytes )
	        && ensureUniformSet();
	m_context.endResourceRebuild();
	if ( !buffers_ok ) {
		if ( m_lastError.is_empty() ) {
			setError( "RD instance material upload failed while establishing material buffers/uniform set" );
		}
		return false;
	}
	if ( bytes > 0u ) {
		if ( m_materialBytes.size() < ( int64_t )( m_nMaterialCapacityBytes ) ) {
			m_materialBytes.resize( m_nMaterialCapacityBytes );
		}
		if ( m_materialBytes.size() < ( int64_t )( bytes ) ) {
			setError( "RD instance material upload scratch capacity is smaller than the published material table" );
			return false;
		}
		memcpy( m_materialBytes.ptrw(), pMaterials, bytes );
		RenderingDevice *const rd = m_context.getDevice();
		if ( !rd ) {
			setError( "RD instance material upload lost the RenderingDevice before buffer_update" );
			return false;
		}
		if ( rd->buffer_update( m_hMaterialBuffer, 0u, bytes, m_materialBytes ) != OK ) {
			setError( String( "RD instance material buffer_update failed; bytes=" ) + String::num_int64( bytes ) );
			return false;
		}
		m_context.recordBufferUpdate( bytes );
		m_nDirtyUploadBytes += bytes;
	}
	if ( !rebuildMaterialBindings() ) {
		if ( m_lastError.is_empty() ) {
			setError( "RD instance material upload failed while rebuilding sampler/material bindings" );
		}
		return false;
	}
	// Commit only after both the material table and all required bindings are valid.
	// The previous order committed this generation before rebuildMaterialBindings(),
	// turning a transient binding failure into a false cache hit on the next attempt.
	m_nUploadedMeshGeneration = m_meshCache.getSourceGeneration();
	return true;
}

template <bool t_bShadowPass>
bool Terrain3DRdInstanceRenderer::buildPacketsForPass( const t3dRdInstanceSceneSnapshot_t &rScene,
        const Projection *const pViewProjections,
        const uint32_t nViewCount,
        const Vector3 &rCamera,
        const uint32_t nVisibilityMask,
        const bool bSkipFrustum ) {
	t3dRdInstanceFrustum_t frustums[s_nRdForwardMaxViews];
	if ( !bSkipFrustum ) {
		for ( uint32_t view = 0u; view < nViewCount; ++view ) {
			frustums[view] = t3dRdInstanceExtractFrustum( pViewProjections[view] );
		}
	}

	m_packetIndex.clear();
	m_packetBuildSlots.clear();
	m_packetBuildEntries.clear();
	m_alphaPackets.clear();
	if constexpr ( t_bShadowPass ) {
		m_nShadowVisibleIndexCount = 0u;
		m_shadowPackets.clear();
		if ( bSkipFrustum ) {
			m_shadowInstanceCullDescriptors.clear();
			if ( rScene.m_cells.size() > m_shadowInstanceCullCellSpans.size() ) {
				setError( String( "RD shadow instance cell-span backing is smaller than the published cell table; cells=" )
				        + String::num_int64( rScene.m_cells.size() ) + String( ", backing=" )
				        + String::num_int64( m_shadowInstanceCullCellSpans.size() ) );
				return false;
			}
			memset( m_shadowInstanceCullCellSpans.data(), 0, rScene.m_cells.size() * sizeof( t3dRdInstanceDescriptorSpan_t ) );
		}
	} else {
		m_nVisibleIndexCount = 0u;
		m_nPacketBuildInputCells = rScene.m_cells.size();
		m_nResolvedMeshCells = 0u;
		m_nUnresolvedMeshCells = 0u;
		m_drawPackets.clear();
		m_depthPackets.clear();
		m_hlodDescriptors.clear();
		if ( bSkipFrustum ) {
			m_instanceCullDescriptors.clear();
			if ( rScene.m_cells.size() > m_instanceCullCellSpans.size() ) {
				setError( String( "RD instance cell-span backing is smaller than the published cell table; cells=" )
				        + String::num_int64( rScene.m_cells.size() ) + String( ", backing=" )
				        + String::num_int64( m_instanceCullCellSpans.size() ) );
				return false;
			}
			memset( m_instanceCullCellSpans.data(), 0, rScene.m_cells.size() * sizeof( t3dRdInstanceDescriptorSpan_t ) );
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
		} else if ( !hlod && ( cell.m_nVisibilityLayers & nVisibilityMask ) == 0u ) {
			continue;
		}
		if ( ( uint64_t )( cell.m_nFirstInstance ) + cell.m_nInstanceCount > rScene.m_instances.size() ) {
			setError( "RD instance cell range exceeds published instance table" );
			return false;
		}

		const t3dRdMeshHandle_t mesh = m_meshCache.getMeshHandleBySourceKey(
		        cell.m_nMeshSourceKey != 0u ? cell.m_nMeshSourceKey : t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = m_meshCache.resolveMesh( mesh );
		if ( !pLod ) {
			if constexpr ( !t_bShadowPass ) {
				++m_nUnresolvedMeshCells;
			}
			continue;
		}
		if constexpr ( !t_bShadowPass ) {
			++m_nResolvedMeshCells;
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
			const t3dRdSurfaceHandle_t handle = m_meshCache.getSurfaceHandle( mesh, surface_index );
			const t3dRdMeshSourceSurface_t *const pSurface = m_meshCache.resolveSurface( handle );
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
			if ( const uint32_t *const pExisting = t3dMapFind( m_packetIndex, key ) ) {
				packet_index = *pExisting;
			} else {
				if ( m_packetBuildSlots.size() >= m_nPacketCapacity ) {
					setError( "RD instance packet count exceeded publication-time capacity" );
					return false;
				}
				packet_index = ( uint32_t )( m_packetBuildSlots.size() );
				if ( !t3dMapInsertOrAssignWithinCapacity( m_packetIndex, key, packet_index ) ) {
					setError( "RD instance packet hash overflowed sealed capacity" );
					return false;
				}
				t3dRdPacketBuildSlot_t slot;
				slot.m_packet.m_nSurfaceSlot = handle.m_nSlot;
				slot.m_packet.m_nMaterial = pSurface->m_nMaterial;
				slot.m_packet.m_nOverlayMaterial = pSurface->m_nOverlayMaterial;
				slot.m_packet.m_nFlags = draw_flags;
				slot.m_packet.m_fSortDistanceSq = ( float )( cell.m_bounds.get_center().distance_squared_to( rCamera ) );
				slot.m_packet.m_indexFormat = ( RenderingDevice::IndexBufferFormat )( pSurface->m_nIndexFormat );
				slot.m_packet.m_cullBounds = cell.m_bounds;
				slot.m_packet.m_nVisibilityLayers = cell.m_nVisibilityLayers;
				slot.m_packet.m_fLodBegin = cell.m_fLodBegin;
				slot.m_packet.m_fLodEnd = cell.m_fLodEnd;
				slot.m_packet.m_fFadeMargin = cell.m_fFadeMargin;
				if ( !t3dVectorPushBackWithinCapacity( m_packetBuildSlots, slot ) ) {
					setError( String( "RD instance packet-build slot vector rejected a sealed-capacity append; size=" )
					        + String::num_int64( m_packetBuildSlots.size() ) + String( ", capacity=" )
					        + String::num_int64( m_packetBuildSlots.capacity() ) );
					return false;
				}
			}
			if ( m_packetBuildEntries.size() >= m_nPacketCapacity ) {
				setError( "RD instance packet entry count exceeded publication-time capacity" );
				return false;
			}
			t3dRdPacketBuildSlot_t &slot = m_packetBuildSlots[packet_index];
			if ( UINT32_MAX - slot.m_nVisibleCount < cell.m_nInstanceCount ) {
				setError( "RD instance packet visible count overflow" );
				return false;
			}
			slot.m_nVisibleCount += cell.m_nInstanceCount;
			if ( !t3dVectorPushBackWithinCapacity( m_packetBuildEntries,
			             t3dRdPacketBuildEntry_t{ packet_index, cell.m_nFirstInstance, cell.m_nInstanceCount, ( uint32_t )iCell } ) ) {
				setError( String( "RD instance packet-build entry vector rejected a sealed-capacity append; size=" )
				        + String::num_int64( m_packetBuildEntries.size() ) + String( ", capacity=" )
				        + String::num_int64( m_packetBuildEntries.capacity() ) );
				return false;
			}
		}
	}

	uint64_t total_visible64 = 0u;
	for ( size_t iSlot = 0u; iSlot < ( size_t )( ( m_packetBuildSlots ).size() ); iSlot++ ) {
		t3dRdPacketBuildSlot_t &slot = ( m_packetBuildSlots )[iSlot];
		slot.m_packet.m_nFirstVisible = ( uint32_t )( total_visible64 );
		slot.m_packet.m_nVisibleCount = slot.m_nVisibleCount;
		slot.m_nWriteOffset = slot.m_packet.m_nFirstVisible;
		total_visible64 += slot.m_nVisibleCount;
	}
	if ( total_visible64 > m_nVisibleIndexCapacity ) {
		setError( "RD instance visible-index output exceeded publication-time capacity" );
		return false;
	}
	eastl::vector<uint32_t, Terrain3DEastlAllocator> &visible_indices = t_bShadowPass ? m_shadowVisibleIndices : m_visibleIndices;
	if ( total_visible64 > visible_indices.size() ) {
		setError( "RD instance visible-index backing exceeded publication-time capacity" );
		return false;
	}
	if constexpr ( t_bShadowPass ) {
		m_nShadowVisibleIndexCount = ( uint32_t )( total_visible64 );
	} else {
		m_nVisibleIndexCount = ( uint32_t )( total_visible64 );
	}
	if ( !bSkipFrustum ) {
		for ( size_t iEntry = 0u; iEntry < ( size_t )( ( m_packetBuildEntries ).size() ); iEntry++ ) {
			const t3dRdPacketBuildEntry_t &entry = ( m_packetBuildEntries )[iEntry];
			t3dRdPacketBuildSlot_t &slot = m_packetBuildSlots[entry.m_nPacketIndex];
			uint32_t write = slot.m_nWriteOffset;
			for ( uint32_t instance = 0u; instance < entry.m_nInstanceCount; ++instance ) {
				visible_indices[write++] = entry.m_nFirstInstance + instance;
			}
			slot.m_nWriteOffset = write;
		}
	}

	for ( size_t iSlot = 0u; iSlot < ( size_t )( ( m_packetBuildSlots ).size() ); iSlot++ ) {
		t3dRdPacketBuildSlot_t &slot = ( m_packetBuildSlots )[iSlot];
		const t3dRdMeshSourceSurface_t *const pSurface =
		        m_meshCache.resolveSurface( t3dRdSurfaceHandle_t{ slot.m_packet.m_nSurfaceSlot, m_meshCache.getGeneration() } );
		if ( !pSurface ) {
			setError( String( "RD instance packet finalization lost a mesh surface handle; surface_slot=" )
			        + String::num_int64( slot.m_packet.m_nSurfaceSlot ) + String( ", mesh_generation=" )
			        + String::num_int64( m_meshCache.getGeneration() ) );
			return false;
		}
		slot.m_packet.m_command.m_nIndexCount = pSurface->m_nIndexCount;
		slot.m_packet.m_command.m_nInstanceCount = bSkipFrustum ? 0u : slot.m_packet.m_nVisibleCount;
		slot.m_packet.m_command.m_nFirstIndex = pSurface->m_nIndexOffset;
		slot.m_packet.m_command.m_nVertexOffset = ( int32_t )( pSurface->m_nVertexOffset );
		slot.m_packet.m_command.m_nFirstInstance = slot.m_packet.m_nFirstVisible;
		if constexpr ( t_bShadowPass ) {
			slot.m_nCommandIndex = ( uint32_t )m_shadowPackets.size();
			if ( !t3dVectorPushBackWithinCapacity( m_shadowPackets, slot.m_packet ) ) {
				setError( "RD shadow packet output exceeded sealed capacity" );
				return false;
			}
		} else {
			const bool alpha = ( slot.m_packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u;
			if ( bSkipFrustum ) {
				slot.m_nCommandIndex = ( uint32_t )m_drawPackets.size();
				if ( !t3dVectorPushBackWithinCapacity( m_drawPackets, slot.m_packet ) ) {
					setError( "RD instance draw-packet output exceeded sealed capacity" );
					return false;
				}
			} else if ( alpha ) {
				if ( !t3dVectorPushBackWithinCapacity( m_alphaPackets, slot.m_packet ) ) {
					setError( "RD instance alpha-packet output exceeded sealed capacity" );
					return false;
				}
			} else {
				if ( !t3dVectorPushBackWithinCapacity( m_drawPackets, slot.m_packet ) ) {
					setError( "RD instance draw-packet output exceeded sealed capacity" );
					return false;
				}
			}
			if ( !alpha || ( slot.m_packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) != 0u ) {
				if ( !t3dVectorPushBackWithinCapacity( m_depthPackets, slot.m_packet ) ) {
					setError( "RD instance depth-packet output exceeded sealed capacity" );
					return false;
				}
			}
		}
	}

	if constexpr ( !t_bShadowPass ) {
		if ( !bSkipFrustum ) {
			t3dRdStableSortAlphaPackets( m_alphaPackets, m_alphaSortScratch );
			if ( m_drawPackets.size() + m_alphaPackets.size() > m_nPacketCapacity ) {
				setError( "RD instance draw packet output exceeded publication-time capacity" );
				return false;
			}
			if ( !t3dVectorAppendWithinCapacity( m_drawPackets, m_alphaPackets.data(), m_alphaPackets.size() ) ) {
				setError( "RD instance draw-packet output could not append sorted alpha packets within sealed capacity" );
				return false;
			}
		}
		if ( bSkipFrustum ) {
			for ( size_t iEntry = 0u; iEntry < m_packetBuildEntries.size(); ++iEntry ) {
				const t3dRdPacketBuildEntry_t &entry = m_packetBuildEntries[iEntry];
				const t3dRdPacketBuildSlot_t &slot = m_packetBuildSlots[entry.m_nPacketIndex];
				const t3dRdInstanceCell_t &cell = rScene.m_cells[entry.m_nCellIndex];
				t3dRdInstanceCullDescriptor_t descriptor;
				const Vector3 center = cell.m_bounds.get_center();
				const Vector3 extent = cell.m_bounds.size * 0.5f;
				descriptor.m_szBoundsCenter[0] = ( float )center.x;
				descriptor.m_szBoundsCenter[1] = ( float )center.y;
				descriptor.m_szBoundsCenter[2] = ( float )center.z;
				descriptor.m_szBoundsExtent[0] = ( float )extent.x;
				descriptor.m_szBoundsExtent[1] = ( float )extent.y;
				descriptor.m_szBoundsExtent[2] = ( float )extent.z;
				descriptor.m_nCommandIndex = slot.m_nCommandIndex;
				descriptor.m_nFirstInstance = entry.m_nFirstInstance;
				descriptor.m_nInstanceCount = entry.m_nInstanceCount;
				descriptor.m_nFirstVisible = slot.m_packet.m_nFirstVisible;
				descriptor.m_nVisibilityLayers = cell.m_nVisibilityLayers;
				descriptor.m_nFlags = cell.m_nFlags;
				descriptor.m_fLodBegin = cell.m_fLodBegin;
				descriptor.m_fLodEnd = cell.m_fLodEnd;
				t3dRdInstanceDescriptorSpan_t &span = m_instanceCullCellSpans[entry.m_nCellIndex];
				if ( span.m_nCount == 0u ) {
					span.m_nFirst = ( uint32_t )m_instanceCullDescriptors.size();
				}
				++span.m_nCount;
				if ( !t3dVectorPushBackWithinCapacity( m_instanceCullDescriptors, descriptor ) ) {
					setError( "RD instance cull descriptor output exceeded sealed capacity" );
					return false;
				}
			}
		}
		for ( uint32_t command_index = 0u; command_index < ( uint32_t )( m_drawPackets.size() ); ++command_index ) {
			const t3dRdInstanceDrawPacket_t &packet = m_drawPackets[command_index];
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
			if ( !t3dVectorPushBackWithinCapacity( m_hlodDescriptors, descriptor ) ) {
				setError( "RD instance HLOD descriptor output exceeded sealed capacity" );
				return false;
			}
		}
	}
	if constexpr ( t_bShadowPass ) {
		if ( bSkipFrustum ) {
			for ( size_t iEntry = 0u; iEntry < m_packetBuildEntries.size(); ++iEntry ) {
				const t3dRdPacketBuildEntry_t &entry = m_packetBuildEntries[iEntry];
				const t3dRdPacketBuildSlot_t &slot = m_packetBuildSlots[entry.m_nPacketIndex];
				const t3dRdInstanceCell_t &cell = rScene.m_cells[entry.m_nCellIndex];
				t3dRdInstanceCullDescriptor_t descriptor;
				const Vector3 center = cell.m_bounds.get_center();
				const Vector3 extent = cell.m_bounds.size * 0.5f;
				descriptor.m_szBoundsCenter[0] = ( float )center.x;
				descriptor.m_szBoundsCenter[1] = ( float )center.y;
				descriptor.m_szBoundsCenter[2] = ( float )center.z;
				descriptor.m_szBoundsExtent[0] = ( float )extent.x;
				descriptor.m_szBoundsExtent[1] = ( float )extent.y;
				descriptor.m_szBoundsExtent[2] = ( float )extent.z;
				descriptor.m_nCommandIndex = slot.m_nCommandIndex;
				descriptor.m_nFirstInstance = entry.m_nFirstInstance;
				descriptor.m_nInstanceCount = entry.m_nInstanceCount;
				descriptor.m_nFirstVisible = slot.m_packet.m_nFirstVisible;
				descriptor.m_nVisibilityLayers = cell.m_nVisibilityLayers;
				descriptor.m_nFlags = cell.m_nFlags;
				descriptor.m_fLodBegin = 0.0f;
				descriptor.m_fLodEnd = 0.0f;
				t3dRdInstanceDescriptorSpan_t &span = m_shadowInstanceCullCellSpans[entry.m_nCellIndex];
				if ( span.m_nCount == 0u ) {
					span.m_nFirst = ( uint32_t )m_shadowInstanceCullDescriptors.size();
				}
				++span.m_nCount;
				if ( !t3dVectorPushBackWithinCapacity( m_shadowInstanceCullDescriptors, descriptor ) ) {
					setError( "RD shadow instance cull descriptor output exceeded sealed capacity" );
					return false;
				}
			}
		}
	}
	m_nHlodCullDescriptors = m_hlodDescriptors.size();
	m_nVisibleInstances = t_bShadowPass ? m_nShadowVisibleIndexCount : m_nVisibleIndexCount;
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
	return bShadowPass ? buildPacketsForPass<true>( rScene, pViewProjections, nViewCount, rCamera, nVisibilityMask, bSkipFrustum )
	                   : buildPacketsForPass<false>( rScene, pViewProjections, nViewCount, rCamera, nVisibilityMask, bSkipFrustum );
}

bool Terrain3DRdInstanceRenderer::uploadVisibility( void ) {
	const uint64_t visible_bytes64 = ( uint64_t )( m_nVisibleIndexCount ) * sizeof( uint32_t );
#if !T3D_INSTANCER_RD_PORT && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	const uint64_t indirect_bytes64 = m_drawPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	if ( visible_bytes64 > m_nVisibleCapacityBytes || indirect_bytes64 > m_nIndirectCapacityBytes ) {
		setError( "RD instance visibility output exceeded publication-time capacity" );
		return false;
	}
#else
	if ( visible_bytes64 > m_nVisibleCapacityBytes ) {
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
		memcpy( m_visibleBytes.ptrw(), m_visibleIndices.data(), visible_bytes );
		if ( rd->buffer_update( m_hVisibleIndexBuffer, 0u, visible_bytes, m_visibleBytes ) != OK ) {
			setError( String( "RD instance visible-index buffer_update failed; bytes=" ) + String::num_int64( visible_bytes ) );
			return false;
		}
	}
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	m_context.recordBufferUpdate( visible_bytes );
	return true;
#else
	const uint32_t indirect_bytes = ( uint32_t )( indirect_bytes64 );
	if ( indirect_bytes > 0u ) {
		for ( uint32_t i = 0u; i < ( uint32_t )( m_drawPackets.size() ); ++i ) {
			memcpy( m_indirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
			        &m_drawPackets[i].m_command,
			        sizeof( t3dRdIndexedIndirectCommand_t ) );
		}
		if ( rd->buffer_update( m_hIndirectBuffer, 0u, indirect_bytes, m_indirectBytes ) != OK ) {
			setError( String( "RD instance indirect-command buffer_update failed; bytes=" ) + String::num_int64( indirect_bytes ) );
			return false;
		}
	}
	m_context.recordBufferUpdate( visible_bytes + indirect_bytes );
	return uploadHlodDescriptors();
#endif
}


bool Terrain3DRdInstanceRenderer::uploadShadowVisibility( void ) {
	const uint64_t visible_bytes64 = ( uint64_t )( m_nShadowVisibleIndexCount ) * sizeof( uint32_t );
#if !T3D_INSTANCER_RD_PORT && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	const uint64_t indirect_bytes64 = m_shadowPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	if ( visible_bytes64 > m_nShadowVisibleCapacityBytes || indirect_bytes64 > m_nShadowIndirectCapacityBytes ) {
		setError( "RD shadow instance visibility output exceeded publication-time capacity" );
		return false;
	}
#else
	if ( visible_bytes64 > m_nShadowVisibleCapacityBytes ) {
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
		memcpy( m_shadowVisibleBytes.ptrw(), m_shadowVisibleIndices.data(), visible_bytes );
		if ( rd->buffer_update( m_hShadowVisibleIndexBuffer, 0u, visible_bytes, m_shadowVisibleBytes ) != OK ) {
			setError( String( "RD shadow instance visible-index buffer_update failed; bytes=" ) + String::num_int64( visible_bytes ) );
			return false;
		}
	}
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	m_context.recordBufferUpdate( visible_bytes );
	return true;
#else
	const uint32_t indirect_bytes = ( uint32_t )( indirect_bytes64 );
	if ( indirect_bytes > 0u ) {
		for ( uint32_t i = 0u; i < ( uint32_t )( m_shadowPackets.size() ); ++i ) {
			memcpy( m_shadowIndirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
			        &m_shadowPackets[i].m_command,
			        sizeof( t3dRdIndexedIndirectCommand_t ) );
		}
		if ( rd->buffer_update( m_hShadowIndirectBuffer, 0u, indirect_bytes, m_shadowIndirectBytes ) != OK ) {
			setError( String( "RD shadow instance indirect-command buffer_update failed; bytes=" ) + String::num_int64( indirect_bytes ) );
			return false;
		}
	}
	m_context.recordBufferUpdate( visible_bytes + indirect_bytes );
	return true;
#endif
}

bool Terrain3DRdInstanceRenderer::uploadHlodDescriptors( void ) {
	if ( m_hlodDescriptors.empty() ) {
		return true;
	}
	m_context.beginResourceRebuild();
	const bool hlod_shader_ready = ensureHlodCullShader( m_hlodCullSourceShader );
	m_context.endResourceRebuild();
	if ( !hlod_shader_ready ) {
		return false;
	}
	const uint64_t bytes64 = m_hlodDescriptors.size() * sizeof( t3dRdHlodCullDescriptor_t );
	if ( bytes64 > m_nHlodDescriptorCapacityBytes || !m_hHlodCullDescriptorBuffer.is_valid()
	        || m_hlodDescriptorBytes.size() < ( int64_t )( bytes64 ) ) {
		setError( "RD HLOD descriptor output exceeded publication-time capacity" );
		return false;
	}
	const uint32_t bytes = ( uint32_t )( bytes64 );
	memcpy( m_hlodDescriptorBytes.ptrw(), m_hlodDescriptors.data(), bytes );
	if ( m_context.getDevice()->buffer_update( m_hHlodCullDescriptorBuffer, 0u, bytes, m_hlodDescriptorBytes ) != OK ) {
		return false;
	}
	m_context.recordBufferUpdate( bytes );
	if ( !m_hHlodCullUniformSet.is_valid() ) {
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back( t3dRdInstanceBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
		uniforms.push_back( t3dRdInstanceBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hHlodCullDescriptorBuffer ) );
		uniforms.push_back( t3dRdInstanceBufferUniform( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hIndirectBuffer ) );
		RenderingDevice *const rd = m_context.getDevice();
		m_hHlodCullUniformSet = rd->uniform_set_create( uniforms, m_hHlodCullShader, 0u );
		if ( !m_hHlodCullUniformSet.is_valid() ) {
			setError( "Failed to create Terrain3D HLOD cull uniform set" );
			return false;
		}
		if ( !m_context.registerResource( m_hHlodCullUniformSet,
		             "T3D.HLOD.CullUniformSet",
		             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_HLOD ) ) {
			rd->free_rid( m_hHlodCullUniformSet );
			m_hHlodCullUniformSet = RID();
			setError( "Failed to register Terrain3D HLOD cull uniform set" );
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::dispatchHlodCull( const uint32_t nVisibilityMask ) {
	if ( m_hlodDescriptors.empty() ) {
		return true;
	}
	if ( !m_hHlodCullPipeline.is_valid() || !m_hHlodCullUniformSet.is_valid() ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdHlodCullPush_t push;
	push.m_nDescriptorCount = ( uint32_t )( m_hlodDescriptors.size() );
	push.m_nVisibilityMask = nVisibilityMask;
	push.m_nCommandCount = ( uint32_t )( m_drawPackets.size() );
	memcpy( m_hlodCullPushBytes.ptrw(), &push, sizeof( push ) );
	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	rd->compute_list_bind_compute_pipeline( list, m_hHlodCullPipeline );
	rd->compute_list_bind_uniform_set( list, m_hHlodCullUniformSet, 0u );
	rd->compute_list_set_push_constant( list, m_hlodCullPushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, ( push.m_nDescriptorCount + 31u ) / 32u, 1u, 1u );
	rd->compute_list_add_barrier( list );
	rd->compute_list_end();
	++m_nHlodCullDispatches;
	return true;
}


bool Terrain3DRdInstanceRenderer::buildGpuPacketTopology( const t3dRdInstanceSceneSnapshot_t &rScene ) {
	const uint64_t mesh_generation = m_meshCache.getSourceGeneration();
	if ( m_nGpuTopologySceneGeneration == rScene.m_nGeneration && m_nGpuTopologyMeshGeneration == mesh_generation ) {
		return true;
	}
	if ( m_nGpuTopologySceneGeneration != 0u && m_nGpuTopologyMeshGeneration == mesh_generation && !rScene.m_bTopologyChanged ) {
		if ( !refreshGpuCullDescriptorsDirty( rScene ) ) {
			return false;
		}
		m_nGpuTopologySceneGeneration = rScene.m_nGeneration;
		m_nGpuCullMainRenderFrame = UINT64_MAX;
		return true;
	}
	Projection unused_projection;
	if ( !buildPackets( rScene, &unused_projection, 1u, Vector3(), UINT32_MAX, false, true )
	        || !buildPackets( rScene, &unused_projection, 1u, Vector3(), UINT32_MAX, true, true ) ) {
		return false;
	}
	const uint64_t main_indirect_bytes64 = m_drawPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	const uint64_t shadow_indirect_bytes64 = m_shadowPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	if ( main_indirect_bytes64 > m_nIndirectCapacityBytes || shadow_indirect_bytes64 > m_nShadowIndirectCapacityBytes ) {
		setError( "RD GPU instance packet topology exceeded indirect capacity" );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const uint32_t main_indirect_bytes = ( uint32_t )main_indirect_bytes64;
	const uint32_t shadow_indirect_bytes = ( uint32_t )shadow_indirect_bytes64;
	for ( uint32_t i = 0u; i < ( uint32_t )m_drawPackets.size(); ++i ) {
		memcpy( m_indirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
		        &m_drawPackets[i].m_command,
		        sizeof( t3dRdIndexedIndirectCommand_t ) );
	}
	for ( uint32_t i = 0u; i < ( uint32_t )m_shadowPackets.size(); ++i ) {
		memcpy( m_shadowIndirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
		        &m_shadowPackets[i].m_command,
		        sizeof( t3dRdIndexedIndirectCommand_t ) );
	}
	if ( main_indirect_bytes > 0u && rd->buffer_update( m_hIndirectBuffer, 0u, main_indirect_bytes, m_indirectBytes ) != OK ) {
		return false;
	}
	if ( shadow_indirect_bytes > 0u
	        && rd->buffer_update( m_hShadowIndirectBuffer, 0u, shadow_indirect_bytes, m_shadowIndirectBytes ) != OK ) {
		return false;
	}
	m_context.recordBufferUpdate( main_indirect_bytes + shadow_indirect_bytes );
	if ( !uploadGpuCullDescriptors() ) {
		return false;
	}
	m_nGpuTopologySceneGeneration = rScene.m_nGeneration;
	m_nGpuTopologyMeshGeneration = mesh_generation;
	m_nGpuCullMainRenderFrame = UINT64_MAX;
	m_nGpuCullMainViewportKey = 0u;
	m_nGpuCullMainViewKey = 0u;
	m_nGpuCullMainSceneGeneration = 0u;
	m_nGpuCullMainVisibilityMask = UINT32_MAX;
	m_nGpuCullMainViewCount = 0u;
	++m_nGpuTopologyBuilds;
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
	const Vector3 center = cell.m_bounds.get_center();
	const Vector3 extent = cell.m_bounds.size * 0.5f;
	for ( uint32_t i = 0u; i < span.m_nCount; ++i ) {
		t3dRdInstanceCullDescriptor_t &descriptor = rDescriptors[span.m_nFirst + i];
		descriptor.m_szBoundsCenter[0] = ( float )center.x;
		descriptor.m_szBoundsCenter[1] = ( float )center.y;
		descriptor.m_szBoundsCenter[2] = ( float )center.z;
		descriptor.m_szBoundsExtent[0] = ( float )extent.x;
		descriptor.m_szBoundsExtent[1] = ( float )extent.y;
		descriptor.m_szBoundsExtent[2] = ( float )extent.z;
		if ( !bShadow ) {
			descriptor.m_nVisibilityLayers = cell.m_nVisibilityLayers;
			descriptor.m_nFlags = cell.m_nFlags;
			descriptor.m_fLodBegin = cell.m_fLodBegin;
			descriptor.m_fLodEnd = cell.m_fLodEnd;
			if ( descriptor.m_nCommandIndex < m_drawPackets.size()
			        && ( m_drawPackets[descriptor.m_nCommandIndex].m_nFlags & T3D_RD_INSTANCE_FADE_ACTIVE ) != 0u ) {
				m_drawPackets[descriptor.m_nCommandIndex].m_cullBounds = cell.m_bounds;
			}
		}
	}
	const uint32_t bytes = span.m_nCount * sizeof( t3dRdInstanceCullDescriptor_t );
	memcpy( m_instanceCullDescriptorBytes.ptrw(), rDescriptors.data() + span.m_nFirst, bytes );
	const uint32_t byte_offset = ( nDescriptorBase + span.m_nFirst ) * sizeof( t3dRdInstanceCullDescriptor_t );
	if ( m_context.getDevice()->buffer_update( m_hInstanceCullDescriptorBuffer, byte_offset, bytes, m_instanceCullDescriptorBytes ) != OK ) {
		return false;
	}
	rUploadedBytes += bytes;
	return true;
}

bool Terrain3DRdInstanceRenderer::refreshGpuCullDescriptorsDirty( const t3dRdInstanceSceneSnapshot_t &rScene ) {
	if ( rScene.m_dirtyCells.empty() ) {
		return true;
	}
	if ( rScene.m_cells.size() > m_instanceCullCellSpans.size() || rScene.m_cells.size() > m_shadowInstanceCullCellSpans.size() ) {
		return false;
	}
	const uint32_t main_descriptor_count = ( uint32_t )m_instanceCullDescriptors.size();
	uint32_t uploaded_bytes = 0u;
	for ( size_t i = 0u; i < rScene.m_dirtyCells.size(); ++i ) {
		const uint32_t cell_index = rScene.m_dirtyCells[i];
		if ( !updateGpuCullDescriptorSpanDirty(
		             rScene, cell_index, m_instanceCullDescriptors, m_instanceCullCellSpans, 0u, false, uploaded_bytes )
		        || !updateGpuCullDescriptorSpanDirty( rScene,
		                cell_index,
		                m_shadowInstanceCullDescriptors,
		                m_shadowInstanceCullCellSpans,
		                main_descriptor_count,
		                true,
		                uploaded_bytes ) ) {
			setError( "RD instance dirty cull-descriptor update failed" );
			return false;
		}
	}
	if ( uploaded_bytes > 0u ) {
		m_context.recordBufferUpdate( uploaded_bytes );
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadGpuCullDescriptors( void ) {
	const uint32_t main_count = ( uint32_t )m_instanceCullDescriptors.size();
	const uint32_t shadow_count = ( uint32_t )m_shadowInstanceCullDescriptors.size();
	const uint64_t total_bytes64 = ( uint64_t )( main_count + shadow_count ) * sizeof( t3dRdInstanceCullDescriptor_t );
	if ( total_bytes64 > m_nInstanceCullDescriptorCapacityBytes ) {
		setError( "RD instance cull descriptors exceeded publication-time capacity" );
		return false;
	}
	const uint32_t total_bytes = ( uint32_t )total_bytes64;
	uint8_t *const bytes = m_instanceCullDescriptorBytes.ptrw();
	const uint32_t main_bytes = main_count * sizeof( t3dRdInstanceCullDescriptor_t );
	if ( main_bytes > 0u ) {
		memcpy( bytes, m_instanceCullDescriptors.data(), main_bytes );
	}
	if ( shadow_count > 0u ) {
		memcpy( bytes + main_bytes, m_shadowInstanceCullDescriptors.data(), shadow_count * sizeof( t3dRdInstanceCullDescriptor_t ) );
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( total_bytes > 0u && rd->buffer_update( m_hInstanceCullDescriptorBuffer, 0u, total_bytes, m_instanceCullDescriptorBytes ) != OK ) {
		return false;
	}
	m_context.recordBufferUpdate( total_bytes );
	if ( m_hInstanceCullUniformSet.is_valid() ) {
		return true;
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( t3dRdInstanceBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hInstanceCullDescriptorBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleIndexBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hIndirectBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hShadowVisibleIndexBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform( 5, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hShadowIndirectBuffer ) );
	m_hInstanceCullUniformSet = rd->uniform_set_create( uniforms, m_hInstanceCullShader, 0u );
	if ( !m_hInstanceCullUniformSet.is_valid() ) {
		setError( "Failed to create Terrain3D instance cull uniform set" );
		return false;
	}
	if ( !m_context.registerResource( m_hInstanceCullUniformSet,
	             "T3D.Instance.CullUniformSet",
	             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
	             Terrain3DRdContext::MEMORY_NONE,
	             0u,
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		rd->free_rid( m_hInstanceCullUniformSet );
		m_hInstanceCullUniformSet = RID();
		setError( "Failed to register Terrain3D instance cull uniform set" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::dispatchInstanceCull(
        const uint32_t nVisibilityMask, const uint32_t nViewCount, const bool bShadowPass ) {
	const uint32_t descriptor_count =
	        bShadowPass ? ( uint32_t )m_shadowInstanceCullDescriptors.size() : ( uint32_t )m_instanceCullDescriptors.size();
	const uint32_t command_count = bShadowPass ? ( uint32_t )m_shadowPackets.size() : ( uint32_t )m_drawPackets.size();
	if ( descriptor_count == 0u || command_count == 0u ) {
		return true;
	}
	if ( !m_hInstanceCullPipeline.is_valid() || !m_hInstanceCullUniformSet.is_valid() ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdInstanceCullPush_t push;
	push.m_nDescriptorBase = bShadowPass ? ( uint32_t )m_instanceCullDescriptors.size() : 0u;
	push.m_nDescriptorCount = descriptor_count;
	push.m_nCommandCount = command_count;
	push.m_nVisibilityMask = nVisibilityMask;
	push.m_nViewCount = CLAMP( nViewCount, 1u, s_nRdForwardMaxViews );
	push.m_nShadowPass = bShadowPass ? 1u : 0u;
	const int64_t list = rd->compute_list_begin();
	if ( list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	rd->compute_list_bind_compute_pipeline( list, m_hInstanceCullPipeline );
	rd->compute_list_bind_uniform_set( list, m_hInstanceCullUniformSet, 0u );
	push.m_nMode = 0u;
	memcpy( m_instanceCullPushBytes.ptrw(), &push, sizeof( push ) );
	rd->compute_list_set_push_constant( list, m_instanceCullPushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, ( command_count + 63u ) / 64u, 1u, 1u );
	rd->compute_list_add_barrier( list );
	push.m_nMode = 1u;
	memcpy( m_instanceCullPushBytes.ptrw(), &push, sizeof( push ) );
	rd->compute_list_set_push_constant( list, m_instanceCullPushBytes, sizeof( push ) );
	rd->compute_list_dispatch( list, descriptor_count, 1u, 1u );
	rd->compute_list_add_barrier( list );
	rd->compute_list_end();
	m_nInstanceCullDispatches += 2u;
	return true;
}

void Terrain3DRdInstanceRenderer::freeViewState( t3dRdInstanceViewState_t &rState ) {
	for ( size_t iPipeline = 0u; iPipeline < T3D_ARRAY_COUNT( rState.m_szPipeline ); iPipeline++ ) {
		RID &pipeline = ( rState.m_szPipeline )[iPipeline];
		m_context.freeResource( pipeline );
	}
	m_context.freeResource( rState.m_hFramebuffer );
	rState = t3dRdInstanceViewState_t{};
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
	if ( state.m_hFramebuffer.is_valid() && pipelines_valid && state.m_hColor == rColor && state.m_hDepth == rDepth
	        && state.m_samples == samples ) {
		return true;
	}
	m_context.beginResourceRebuild();
	freeViewState( state );
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || m_meshCache.getVertexFormat() == RenderingDevice::INVALID_FORMAT_ID ) {
		m_context.endResourceRebuild();
		return false;
	}
	TypedArray<RID> attachments;
	attachments.push_back( rColor );
	attachments.push_back( rDepth );
	state.m_hFramebuffer = rd->framebuffer_create( attachments, -1, 1u );
	const String framebuffer_name = String( "T3D.Instance.Framebuffer." ) + String::num_int64( nView );
	if ( !state.m_hFramebuffer.is_valid() ) {
		m_context.endResourceRebuild();
		setError( String( "RD instance framebuffer creation failed; view=" ) + String::num_int64( nView ) );
		return false;
	}
	if ( !m_context.registerResource( state.m_hFramebuffer,
	             framebuffer_name,
	             Terrain3DRdContext::RESOURCE_FRAMEBUFFER,
	             Terrain3DRdContext::MEMORY_NONE,
	             0u,
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		rd->free_rid( state.m_hFramebuffer );
		state.m_hFramebuffer = RID();
		m_context.endResourceRebuild();
		setError( String( "RD instance framebuffer registration failed; view=" ) + String::num_int64( nView ) );
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

		state.m_szPipeline[pipeline_index] = rd->render_pipeline_create( m_hShader,
		        rd->framebuffer_get_format( state.m_hFramebuffer ),
		        m_meshCache.getVertexFormat(),
		        RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
		        raster,
		        multisample,
		        depth,
		        blend,
		        0u );
		const String pipeline_name =
		        String( "T3D.Instance.Pipeline." ) + String::num_int64( nView ) + "." + String::num_int64( pipeline_index );
		if ( !state.m_szPipeline[pipeline_index].is_valid() ) {
			freeViewState( state );
			m_context.endResourceRebuild();
			setError( String( "RD instance preview pipeline creation failed; view=" ) + String::num_int64( nView )
			        + String( ", pipeline=" ) + String::num_int64( pipeline_index ) );
			return false;
		}
		if ( !m_context.registerResource( state.m_szPipeline[pipeline_index],
		             pipeline_name,
		             Terrain3DRdContext::RESOURCE_RASTER_PIPELINE,
		             Terrain3DRdContext::MEMORY_NONE,
		             0u,
		             Terrain3DRdContext::OWNER_INSTANCES ) ) {
			rd->free_rid( state.m_szPipeline[pipeline_index] );
			state.m_szPipeline[pipeline_index] = RID();
			freeViewState( state );
			m_context.endResourceRebuild();
			setError( String( "RD instance preview pipeline registration failed; view=" ) + String::num_int64( nView )
			        + String( ", pipeline=" ) + String::num_int64( pipeline_index ) );
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
	const t3dRdInstanceDrawPacket_t &rPacket = m_drawPackets[nPacket];
	if ( !pRd || rPacket.m_nVisibleCount == 0u ) {
		return pRd != NULL;
	}
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	const RID hVertexArray = m_meshCache.getDirectSurfaceVertexArray( rPacket.m_nSurfaceSlot );
	const RID hIndexArray = m_meshCache.getDirectSurfaceIndexArray( rPacket.m_nSurfaceSlot );
	if ( !hVertexArray.is_valid() || !hIndexArray.is_valid() ) {
		setError( String( "RD instancer direct-surface submit is missing standalone geometry; surface=" )
		        + String::num_int64( rPacket.m_nSurfaceSlot ) + String( ", direct_surfaces=" )
		        + String::num_int64( m_meshCache.getDirectSurfaceCount() ) );
		return false;
	}
	pRd->draw_list_bind_vertex_array( nDrawList, hVertexArray );
#else
	const RID hIndexArray = m_meshCache.getIndexArray( rPacket.m_indexFormat );
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
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	t3dRdInstancePush_t push{ UINT16_MAX, UINT16_MAX, rPacket.m_nFirstVisible, 0u, UINT32_MAX, 1u, 0u, 0u };
#else
	const RID hMaterialSet = getMaterialBinding( rPacket.m_nMaterial, rPacket.m_nOverlayMaterial );
	if ( !hMaterialSet.is_valid() ) {
		setError( String( "RD instancer preview lost material binding; material=" ) + String::num_int64( rPacket.m_nMaterial )
		        + String( ", overlay=" ) + String::num_int64( rPacket.m_nOverlayMaterial ) );
		return false;
	}
	pRd->draw_list_bind_uniform_set( nDrawList, hMaterialSet, 1u );
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
		0u,
		0u };
#endif
	memcpy( m_pushBytes.ptrw(), &push, sizeof( push ) );
	pRd->draw_list_set_push_constant( nDrawList, m_pushBytes, sizeof( push ) );
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
	pRd->draw_list_draw( nDrawList, true, rPacket.m_nVisibleCount, 0u );
#else
	pRd->draw_list_draw_indirect( nDrawList,
	        true,
	        m_hIndirectBuffer,
	        nPacket * sizeof( t3dRdIndexedIndirectCommand_t ),
	        1u,
	        sizeof( t3dRdIndexedIndirectCommand_t ) );
#endif
	++m_nDraws;
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
	memcpy( m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	if ( rd->buffer_update( m_hFrameBuffer, 0u, sizeof( frame ), m_frameBytes ) != OK ) {
		return false;
	}
	m_context.recordBufferUpdate( sizeof( frame ) );
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
	        ? rd->draw_list_begin( state.m_hFramebuffer, RenderingDevice::DRAW_DEFAULT_ALL, clear_colors, 1.0f )
	        : rd->draw_list_begin( state.m_hFramebuffer, RenderingDevice::DRAW_DEFAULT_ALL );
	if ( list == RenderingDevice::INVALID_ID ) {
		return false;
	}
	rd->draw_list_bind_uniform_set( list, m_hUniformSet, 0u );
	uint32_t bound_pipeline = UINT32_MAX;
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
#if !T3D_INSTANCER_RD_PORT
	rd->draw_list_bind_vertex_array( list, m_meshCache.getVertexArray() );
#endif
	for ( uint32_t i = 0u; i < ( uint32_t )( m_drawPackets.size() ); ++i ) {
		if ( ( m_drawPackets[i].m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) != 0u
		        && !drawPacket( list, state, bound_pipeline, i, true ) ) {
			rd->draw_list_end();
			return false;
		}
	}
	bound_pipeline = UINT32_MAX;
#endif
	for ( uint32_t i = 0u; i < ( uint32_t )( m_drawPackets.size() ); ++i ) {
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
	const uint64_t color_bytes = ( uint64_t )( MAX( m_previewSize.x, 0 ) ) * ( uint64_t )( MAX( m_previewSize.y, 0 ) ) * 4u;
	const uint64_t depth_bytes = ( uint64_t )( MAX( m_previewSize.x, 0 ) ) * ( uint64_t )( MAX( m_previewSize.y, 0 ) ) * 4u;
	m_context.freeResource( m_hPreviewColorTexture, color_bytes );
	m_context.freeResource( m_hPreviewDepthTexture, depth_bytes );
	m_hPreviewColorTexture = RID();
	m_hPreviewDepthTexture = RID();
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
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		setError( "RD editor preview target creation lost the RenderingDevice" );
		return false;
	}
	m_context.beginResourceRebuild();
	Ref<RDTextureView> view;
	view.instantiate();
	Ref<RDTextureFormat> color_format;
	color_format.instantiate();
	color_format->set_width( ( uint32_t )( rSize.x ) );
	color_format->set_height( ( uint32_t )( rSize.y ) );
	color_format->set_depth( 1u );
	color_format->set_array_layers( 1u );
	color_format->set_mipmaps( 1u );
	color_format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
	color_format->set_format( RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM );
	color_format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT );
	m_hPreviewColorTexture = rd->texture_create( color_format, view );
	const uint64_t bytes = ( uint64_t )( rSize.x ) * ( uint64_t )( rSize.y ) * 4u;
	if ( !m_hPreviewColorTexture.is_valid() ) {
		m_context.endResourceRebuild();
		setError( String( "RD editor preview color target creation failed; size=" ) + String::num_int64( rSize.x ) + String( "x" )
		        + String::num_int64( rSize.y ) );
		return false;
	}
	if ( !m_context.registerResource( m_hPreviewColorTexture,
	             "T3D.EditorPreview.Color",
	             Terrain3DRdContext::RESOURCE_TEXTURE,
	             Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
	             bytes,
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		rd->free_rid( m_hPreviewColorTexture );
		m_hPreviewColorTexture = RID();
		m_context.endResourceRebuild();
		setError( "RD editor preview color target registration failed" );
		return false;
	}
	Ref<RDTextureFormat> depth_format;
	depth_format.instantiate();
	depth_format->set_width( ( uint32_t )( rSize.x ) );
	depth_format->set_height( ( uint32_t )( rSize.y ) );
	depth_format->set_depth( 1u );
	depth_format->set_array_layers( 1u );
	depth_format->set_mipmaps( 1u );
	depth_format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
	depth_format->set_format( RenderingDevice::DATA_FORMAT_D32_SFLOAT );
	depth_format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT );
	m_hPreviewDepthTexture = rd->texture_create( depth_format, view );
	if ( !m_hPreviewDepthTexture.is_valid() ) {
		m_context.endResourceRebuild();
		freePreviewTargets();
		setError( String( "RD editor preview depth target creation failed; size=" ) + String::num_int64( rSize.x ) + String( "x" )
		        + String::num_int64( rSize.y ) );
		return false;
	}
	if ( !m_context.registerResource( m_hPreviewDepthTexture,
	             "T3D.EditorPreview.Depth",
	             Terrain3DRdContext::RESOURCE_TEXTURE,
	             Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
	             bytes,
	             Terrain3DRdContext::OWNER_INSTANCES ) ) {
		rd->free_rid( m_hPreviewDepthTexture );
		m_hPreviewDepthTexture = RID();
		m_context.endResourceRebuild();
		freePreviewTargets();
		setError( "RD editor preview depth target registration failed" );
		return false;
	}
	m_previewSize = rSize;
	m_context.endResourceRebuild();
	return true;
}

bool Terrain3DRdInstanceRenderer::initialize( const Ref<RDShaderSPIRV> &rShader,
        const Ref<RDShaderSPIRV> &rHlodCullShader,
        const Ref<RDShaderSPIRV> &rInstanceCullShader,
        const t3dRdForwardShaderBundle_t &rForwardShaders ) {
	m_forwardShaderBundle = rForwardShaders;
	m_hlodCullSourceShader = rHlodCullShader;
	// Base instancing only requires the shared raster shader and the regular
	// GPU visibility shader. Forward+ variants and HLOD culling are created
	// lazily when a pass actually requests those capabilities.
	if ( !ensureDevice() || !ensureShader( rShader ) ) {
		return false;
	}
#if !T3D_INSTANCER_RD_PORT && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !ensureInstanceCullShader( rInstanceCullShader ) ) {
		return false;
	}
#else
	( void )rInstanceCullShader;
#endif
	m_frameBytes.resize( sizeof( t3dRdInstanceFrame_t ) );
	m_pushBytes.resize( sizeof( t3dRdInstancePush_t ) );
	m_hlodCullPushBytes.resize( sizeof( t3dRdHlodCullPush_t ) );
	m_instanceCullPushBytes.resize( sizeof( t3dRdInstanceCullPush_t ) );
	return true;
}

bool Terrain3DRdInstanceRenderer::initializePreview( const Ref<RDShaderSPIRV> &rShader ) {
	// Editor cards exercise only the shared raster/mesh/material path. They must
	// not fail because an unrelated Forward+, HLOD, or runtime cull variant is
	// unavailable.
	if ( !ensureDevice() || !ensureShader( rShader ) ) {
		return false;
	}
	m_frameBytes.resize( sizeof( t3dRdInstanceFrame_t ) );
	m_pushBytes.resize( sizeof( t3dRdInstancePush_t ) );
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
	freeMaterialBindings();
	m_meshCache.shutdown();
	for ( size_t iMaterial = 0u; iMaterial < T3D_ARRAY_COUNT( m_hMaterialSamplers ); iMaterial++ ) {
		RID &sampler = ( m_hMaterialSamplers )[iMaterial];
		m_context.freeResource( sampler );
	}
	m_context.freeResource( m_hFallbackAlbedoTexture, 4u );
	m_context.freeResource( m_hFallbackNormalTexture, 4u );
	m_context.freeResource( m_hFallbackOrmTexture, 4u );
	m_context.freeResource( m_hUniformSet );
	freeForwardUniformSets();
	m_context.freeResource( m_hFrameBuffer, sizeof( t3dRdInstanceFrame_t ) );
	m_context.freeResource( m_hInstanceBuffer, m_nInstanceCapacityBytes );
	m_context.freeResource( m_hPreviousInstanceBuffer, m_nPreviousInstanceCapacityBytes );
	m_context.freeResource( m_hVisibleIndexBuffer, m_nVisibleCapacityBytes );
	m_context.freeResource( m_hShadowVisibleIndexBuffer, m_nShadowVisibleCapacityBytes );
	m_context.freeResource( m_hMaterialBuffer, m_nMaterialCapacityBytes );
	m_context.freeResource( m_hIndirectBuffer, m_nIndirectCapacityBytes );
	m_context.freeResource( m_hShadowIndirectBuffer, m_nShadowIndirectCapacityBytes );
	m_context.freeResource( m_hInstanceCullUniformSet );
	m_context.freeResource( m_hInstanceCullDescriptorBuffer, m_nInstanceCullDescriptorCapacityBytes );
	m_context.freeResource( m_hInstanceCullPipeline );
	m_context.freeResource( m_hInstanceCullShader );
	m_context.freeResource( m_hHlodCullUniformSet );
	m_context.freeResource( m_hHlodCullDescriptorBuffer, m_nHlodDescriptorCapacityBytes );
	m_context.freeResource( m_hHlodCullPipeline );
	m_context.freeResource( m_hHlodCullShader );
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_context.freeResource( m_hszForwardShaders[variant] );
	}
	m_context.freeResource( m_hShader );
	for ( size_t iShader = 0u; iShader < T3D_ARRAY_COUNT( m_hszForwardShaders ); iShader++ ) {
		RID &shader = ( m_hszForwardShaders )[iShader];
		shader = RID();
	}
	m_forwardShaderBundle = t3dRdForwardShaderBundle_t{};
	m_hlodCullSourceShader.unref();
	m_nInstanceCapacityBytes = m_nPreviousInstanceCapacityBytes = m_nVisibleCapacityBytes = m_nShadowVisibleCapacityBytes =
	        m_nMaterialCapacityBytes = m_nIndirectCapacityBytes = m_nShadowIndirectCapacityBytes = 0u;
	m_nHlodDescriptorCapacityBytes = 0u;
	m_nInstanceCullDescriptorCapacityBytes = 0u;
	m_nUploadedSceneGeneration = m_nUploadedMeshGeneration = 0u;
	m_nSceneCapacityMeshGeneration = 0u;
	m_nVisibleIndexCount = 0u;
	m_nShadowVisibleIndexCount = 0u;
	m_nMotionHistoryRecordCount = 0u;
	m_nMotionHistoryCellCount = 0u;
	m_nMotionPreviousRecordCount = 0u;
	m_nMotionCurrentOrderCount = 0u;
	m_nMotionPreviousOrderCount = 0u;
	t3dVectorRelease( m_visibleIndices );
	t3dVectorRelease( m_motionHistoryRecords );
	t3dVectorRelease( m_motionHistoryCells );
	t3dVectorRelease( m_motionPreviousRecords );
	t3dVectorRelease( m_motionDirtyRanges );
	m_previousInstanceBytes.clear();
	m_instanceCullDescriptorBytes.clear();
	m_instanceCullPushBytes.clear();
	t3dVectorRelease( m_shadowVisibleIndices );
	t3dVectorRelease( m_drawPackets );
	t3dVectorRelease( m_depthPackets );
	t3dVectorRelease( m_shadowPackets );
	t3dVectorRelease( m_materialBindings );
	t3dVectorRelease( m_hlodDescriptors );
	t3dVectorRelease( m_instanceCullDescriptors );
	t3dVectorRelease( m_shadowInstanceCullDescriptors );
	t3dVectorRelease( m_instanceCullCellSpans );
	t3dVectorRelease( m_shadowInstanceCullCellSpans );
	t3dVectorRelease( m_packetBuildSlots );
	t3dVectorRelease( m_packetBuildEntries );
	t3dVectorRelease( m_alphaPackets );
	t3dVectorRelease( m_alphaSortScratch );
	t3dVectorRelease( m_motionCurrentOrder );
	t3dVectorRelease( m_motionPreviousOrder );
	m_nStaticInstances = 0u;
	m_nVisibleInstances = 0u;
	m_nPreviewSceneGeneration = 0u;
	m_nHlodCullDescriptors = 0u;
	m_nGpuTopologySceneGeneration = 0u;
	m_nGpuTopologyMeshGeneration = 0u;
	m_nGpuCullMainRenderFrame = UINT64_MAX;
	m_nGpuCullMainViewportKey = 0u;
	m_nGpuCullMainViewKey = 0u;
	m_nGpuCullMainSceneGeneration = 0u;
	m_nGpuCullMainVisibilityMask = UINT32_MAX;
	m_nGpuCullMainViewCount = 0u;
	m_nForwardPreparedScenePass = 0u;
	m_nForwardPreparedViewportKey = 0u;
	m_nMotionHistoryRenderFrame = UINT64_MAX;
	m_nMotionHistorySceneGeneration = 0u;
	m_bMotionHistoryNeedsAdvance = false;
	m_nForwardPreparedSceneGeneration = 0u;
	m_bLastForwardPipelineKeyValid = false;
	m_bForwardPreparedHasDraws = false;
	m_forwardPreparedPacketPassKind = T3D_RD_FORWARD_PASS_INVALID;
	m_nForwardPreparedVisibilityMask = UINT32_MAX;
	t3dVectorRelease( m_packetIndex );
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
		m_nStaticInstances = pScene->m_instances.size();
		m_nVisibleInstances = 0u;
		m_nVisibleIndexCount = 0u;
		m_nShadowVisibleIndexCount = 0u;
		m_drawPackets.clear();
		m_depthPackets.clear();
		m_shadowPackets.clear();
		m_hlodDescriptors.clear();
		m_nHlodCullDescriptors = 0u;
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
		if ( pMeshes && pMeshes->m_nGeneration != m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
			m_context.beginResourceRebuild();
			freeMaterialBindings();
			m_context.endResourceRebuild();
		}
		if ( !m_meshCache.upload( pMeshes ) ) {
			setError( String( "Terrain3D RD instance mesh-cache upload failed: " ) + m_meshCache.getLastError() );
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
	++m_nFrames;
	m_nStaticInstances = pScene->m_instances.size();
	m_lastError = String();
	return true;
}

bool Terrain3DRdInstanceRenderer::renderPreview(
        const t3dRdMeshSourceSnapshot_t *const pMeshes, const uint16_t nAssetId, const Vector2i &rSize, Ref<Image> &rImage ) {
	rImage.unref();
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
	if ( pMeshes->m_nGeneration != m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
		m_context.beginResourceRebuild();
		freeMaterialBindings();
		m_context.endResourceRebuild();
	}
	if ( !m_meshCache.upload( pMeshes ) ) {
		setError( String( "RD editor preview mesh-cache upload failed: " ) + m_meshCache.getLastError() );
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
	const t3dRdMeshHandle_t mesh = m_meshCache.getMeshHandle( nAssetId, 0 );
	const t3dRdMeshSourceLod_t *const lod = m_meshCache.resolveMesh( mesh );
	if ( !lod ) {
		setError( "RD editor preview mesh handle is unavailable" );
		return false;
	}
	const AABB source_bounds = lod->m_bounds;
	const Vector3 center = source_bounds.get_center();
	AABB centered = source_bounds;
	centered.position -= center;
	Transform3D xform;
	xform.basis = Basis().rotated( Vector3( 0.0f, 1.0f, 0.0f ), -Math_PI * 0.125f );
	xform.basis = Basis().rotated( Vector3( 1.0f, 0.0f, 0.0f ), Math_PI * 0.125f ) * xform.basis;
	const AABB rotated = xform.xform( centered );
	float extent = ( float )( MAX( rotated.size.x, rotated.size.y ) );
	if ( extent <= 0.0001f ) {
		extent = 1.0f;
	}
	const float scale = 0.9f / extent;
	xform.basis.scale( Vector3( scale, scale, scale ) );
	xform.origin = -xform.basis.xform( center );

	t3dRdInstanceSceneSnapshot_t scene;
	scene.m_nGeneration = ++m_nPreviewSceneGeneration;
	if ( scene.m_nGeneration == 0u ) {
		scene.m_nGeneration = ++m_nPreviewSceneGeneration;
	}
	if ( !scene.m_instances.reserveCold( 1u ) || !scene.m_instances.resizeUninitialized( 1u ) ) {
		return false;
	}
	scene.m_instances[0] = t3dRdPreviewInstanceRecord( xform );
	t3dRdInstanceCell_t cell;
	cell.m_nMeshSourceKey = t3dRdRegularMeshSourceKey( nAssetId, 0 );
	cell.m_nMeshAssetId = nAssetId;
	cell.m_nFirstInstance = 0u;
	cell.m_nInstanceCount = 1u;
	cell.m_nVisibilityLayers = 1u;
	cell.m_bounds = xform.xform( source_bounds );
	if ( !scene.m_cells.reserveCold( 1u ) || !scene.m_cells.resizeUninitialized( 1u ) ) {
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
	if ( !buildPackets( scene, &view_projection, 1u, camera.origin, 1u, false, false ) || !uploadVisibility()
	        || !ensureViewState( 0u, m_hPreviewColorTexture, m_hPreviewDepthTexture, RenderingDevice::TEXTURE_SAMPLES_1 ) ) {
		setError( "Failed to prepare RD editor preview draw" );
		return false;
	}
	m_bPreviewClear = true;
	const bool draw_ok = drawView( 0u, view_projection, camera.origin, 1u );
	m_bPreviewClear = false;
	if ( !draw_ok ) {
		setError( "Failed to draw RD editor preview" );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const PackedByteArray bytes = rd->texture_get_data( m_hPreviewColorTexture, 0u );
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
	if ( !selectForwardVariant( rContext, variant ) ) {
		setError( String( "Terrain3D instance renderer does not support Forward+ pass/mode: pass=" )
		        + String::num_int64( rContext.m_passKind ) + String( ", mode=" ) + String::num_int64( rContext.m_nativePassMode ) );
		return false;
	}
	const bool scene_empty = pScene->m_instances.empty() && pScene->m_cells.empty();
	const bool scene_incomplete = pScene->m_instances.empty() != pScene->m_cells.empty();
	const bool has_renderable_meshes =
	        pMeshes && !pMeshes->m_vertexBytes.is_empty() && !pMeshes->m_lods.empty() && !pMeshes->m_surfaces.empty();
	if ( scene_empty ) {
		m_drawPackets.clear();
		m_depthPackets.clear();
		m_shadowPackets.clear();
		m_nPacketBuildInputCells = 0u;
		m_nResolvedMeshCells = 0u;
		m_nUnresolvedMeshCells = 0u;
		m_nVisibleIndexCount = 0u;
		m_nShadowVisibleIndexCount = 0u;
		m_hlodDescriptors.clear();
		m_nVisibleInstances = 0u;
		m_nStaticInstances = 0u;
		m_nForwardPreparedScenePass = rContext.m_nScenePass;
		m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
		m_nForwardPreparedSceneGeneration = pScene->m_nGeneration;
		m_bForwardPreparedHasDraws = false;
		m_bLastForwardPipelineKeyValid = false;
		m_lastError = String();
		return true;
	}
	if ( scene_incomplete ) {
		setError( String( "RD instance publication is structurally incomplete; records=" )
		        + String::num_int64( pScene->m_instances.size() ) + String( ", cells=" ) + String::num_int64( pScene->m_cells.size() ) );
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
	{
		Terrain3DColdPhaseScope cold_phase_scope;
		if ( !ensureForwardShaderVariant( variant ) ) {
			return false;
		}
		if ( pMeshes && pMeshes->m_nGeneration != m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
			m_context.beginResourceRebuild();
			freeMaterialBindings();
			m_context.endResourceRebuild();
		}
		if ( !pMeshes ) {
			setError( "Terrain3D native Forward+ instance prepare lost the mesh publication" );
			return false;
		}
		if ( !m_meshCache.upload( pMeshes ) ) {
			setError( String( "Terrain3D native Forward+ instance mesh-cache upload failed: " ) + m_meshCache.getLastError() );
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
		if ( !ensureForwardUniformSet( variant, shadow_pass ) ) {
			setError( String( "Terrain3D native Forward+ instance set-0 binding failed; variant=" ) + String::num_int64( variant ) );
			return false;
		}
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
		if ( !ensureForwardMaterialBindings( variant ) ) {
			setError( String( "Terrain3D native Forward+ instance material set-1 binding failed; variant=" ) + String::num_int64( variant ) );
			return false;
		}
#endif
	}
	// Shadow passes may arrive before the scene motion pass. They must never advance or
	// reset temporal history shared by the opaque/motion scene submission. The
	// correctness-first bring-up lane has no temporal dependency at all.
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( !shadow_pass && !prepareMotionHistory( *pScene, rContext.m_nRenderFrame ) ) {
		setError( "Failed to prepare Terrain3D instance/HLOD previous-transform history" );
		return false;
	}
#endif

#if T3D_INSTANCER_RD_PORT
	const bool gpu_visibility = false;
#else
	const bool gpu_visibility = !m_bDirectDrawDebug;
#endif
	if ( gpu_visibility ) {
		if ( !buildGpuPacketTopology( *pScene ) ) {
			setError( "Failed to build Terrain3D GPU instance packet topology" );
			return false;
		}
	} else {
		const bool rebuild_packets = shadow_pass || m_nForwardPreparedScenePass != rContext.m_nScenePass
		        || m_nForwardPreparedViewportKey != rContext.m_nViewportKey || m_nForwardPreparedSceneGeneration != pScene->m_nGeneration
		        || m_forwardPreparedPacketPassKind != rContext.m_passKind || m_nForwardPreparedVisibilityMask != nVisibilityMask;
		if ( rebuild_packets ) {
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
			const bool bHlodVisibilityReady = true;
#else
			const bool bHlodVisibilityReady = rContext.m_nViewCount != 1u || dispatchHlodCull( nVisibilityMask );
#endif
#if T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT
			const uint32_t packet_visibility_mask = UINT32_MAX;
			const bool skip_instance_frustum = true;
#else
			const uint32_t packet_visibility_mask = nVisibilityMask;
			const bool skip_instance_frustum = false;
#endif
			if ( !buildPackets( *pScene,
			             rContext.m_szViewProjection,
			             rContext.m_nViewCount,
			             rContext.m_cameraTransform.origin,
			             packet_visibility_mask,
			             shadow_pass,
			             skip_instance_frustum ) ) {
				if ( m_lastError.is_empty() ) {
					setError( "Failed to build Terrain3D debug/reference instance packets" );
				}
				return false;
			}
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
			if ( !shadow_pass && m_nPacketBuildInputCells > 0u && m_nResolvedMeshCells == 0u && m_nUnresolvedMeshCells > 0u ) {
				setError( String( "RD minimal instance prepare could not resolve any published mesh source keys; input_cells=" )
				        + String::num_int64( m_nPacketBuildInputCells ) + String( ", unresolved_cells=" )
				        + String::num_int64( m_nUnresolvedMeshCells ) + String( ", mesh_lods=" )
				        + String::num_int64( m_meshCache.getLodCount() ) + String( ", mesh_surfaces=" )
				        + String::num_int64( m_meshCache.getSurfaceCount() ) );
				return false;
			}
#endif
			if ( shadow_pass ? !uploadShadowVisibility() : !uploadVisibility() ) {
				if ( m_lastError.is_empty() ) {
					setError( "Failed to upload Terrain3D debug/reference instance visibility" );
				}
				return false;
			}
			if ( !shadow_pass && !bHlodVisibilityReady ) {
				setError( "Failed to prepare Terrain3D HLOD visibility for the reference instance path" );
				return false;
			}
		}
	}

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
	memcpy( m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	if ( m_context.getDevice()->buffer_update( m_hFrameBuffer, 0u, sizeof( frame ), m_frameBytes ) != OK ) {
		setError( "Failed to update Terrain3D native Forward+ instance frame buffer" );
		return false;
	}
	m_context.recordBufferUpdate( sizeof( frame ) );
	if ( gpu_visibility ) {
		if ( shadow_pass ) {
			if ( !dispatchInstanceCull( nVisibilityMask, rContext.m_nViewCount, true ) ) {
				setError( "Failed to dispatch Terrain3D GPU shadow instance visibility" );
				return false;
			}
		} else {
			const uint64_t view_key = t3dRdInstanceViewKey( rContext );
			const bool reuse_cull = m_nGpuCullMainRenderFrame == rContext.m_nRenderFrame
			        && m_nGpuCullMainViewportKey == rContext.m_nViewportKey && m_nGpuCullMainViewKey == view_key
			        && m_nGpuCullMainSceneGeneration == pScene->m_nGeneration && m_nGpuCullMainVisibilityMask == nVisibilityMask
			        && m_nGpuCullMainViewCount == rContext.m_nViewCount;
			if ( reuse_cull ) {
				++m_nGpuCullReuses;
			} else {
				if ( !dispatchInstanceCull( nVisibilityMask, rContext.m_nViewCount, false ) ) {
					setError( "Failed to dispatch Terrain3D GPU instance visibility" );
					return false;
				}
				m_nGpuCullMainRenderFrame = rContext.m_nRenderFrame;
				m_nGpuCullMainViewportKey = rContext.m_nViewportKey;
				m_nGpuCullMainViewKey = view_key;
				m_nGpuCullMainSceneGeneration = pScene->m_nGeneration;
				m_nGpuCullMainVisibilityMask = nVisibilityMask;
				m_nGpuCullMainViewCount = rContext.m_nViewCount;
			}
		}
	}
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION
	        || rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT ) {
		if ( bViewportReconfigured ) {
			m_forwardLighting.invalidateBorrowedState();
		}
		if ( !m_forwardLighting.prepare( rContext, nVisibilityMask )
		        || !m_forwardLighting.ensureSet( m_hszForwardShaders[variant] ).is_valid() ) {
			setError( "Failed to prepare Terrain3D Forward+ instance/HLOD native lighting" );
			return false;
		}
	}
#else
	( void )bViewportReconfigured;
#endif
	if ( !ensureForwardPipelines( rContext, variant ) ) {
		return false;
	}
	m_nForwardPreparedScenePass = rContext.m_nScenePass;
	m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
	m_nForwardPreparedSceneGeneration = pScene->m_nGeneration;
	m_forwardPreparedPacketPassKind = rContext.m_passKind;
	m_nForwardPreparedVisibilityMask = nVisibilityMask;
	m_bForwardPreparedHasDraws = shadow_pass ? !m_shadowPackets.empty() : !m_drawPackets.empty();
	m_lastError = String();
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
	if ( !selectForwardVariant( rContext, variant ) || variant != m_lastForwardPipelineKey.m_variant
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
	const eastl::vector<t3dRdInstanceDrawPacket_t, Terrain3DEastlAllocator> &packets = shadow_pass ? m_shadowPackets : m_drawPackets;
	const RID instance_set = shadow_pass ? m_hszForwardShadowUniformSets[variant] : m_hszForwardUniformSets[variant];
	if ( !instance_set.is_valid() ) {
		return false;
	}
	rd->draw_list_bind_uniform_set( draw_list, instance_set, 0u );
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION
	        || transparent_pass ) {
		const RID lighting_set = m_forwardLighting.getSet( m_hszForwardShaders[variant] );
		if ( !lighting_set.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_uniform_set( draw_list, lighting_set, 2u );
	}
#endif
#if !T3D_INSTANCER_RD_PORT && !T3D_INSTANCER_MINIMAL_RENDER_PATH
	rd->draw_list_bind_vertex_array( draw_list, m_meshCache.getVertexArray() );
#endif
	uint32_t bound_pipeline = UINT32_MAX;
	for ( uint32_t i = 0u; i < ( uint32_t )( packets.size() ); ++i ) {
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
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
		const RID vertex_array = m_meshCache.getDirectSurfaceVertexArray( packet.m_nSurfaceSlot );
		const RID index_array = m_meshCache.getDirectSurfaceIndexArray( packet.m_nSurfaceSlot );
		if ( !vertex_array.is_valid() || !index_array.is_valid() ) {
			setError( String( "RD direct-surface submit is missing standalone geometry; surface=" )
			        + String::num_int64( packet.m_nSurfaceSlot ) + String( ", direct_surfaces=" )
			        + String::num_int64( m_meshCache.getDirectSurfaceCount() ) );
			return false;
		}
		rd->draw_list_bind_vertex_array( draw_list, vertex_array );
#else
		const RID index_array = m_meshCache.getIndexArray( packet.m_indexFormat );
		if ( !index_array.is_valid() ) {
			return false;
		}
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
#if !T3D_INSTANCER_MINIMAL_RENDER_PATH
		const RID material_set = getForwardMaterialBinding( packet.m_nMaterial, packet.m_nOverlayMaterial, variant );
		if ( !material_set.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_uniform_set( draw_list, material_set, 1u );
#endif
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
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		t3dRdInstancePush_t push{ UINT16_MAX,
			UINT16_MAX,
			packet.m_nFirstVisible,
			0u,
			UINT32_MAX,
			1u,
			0u,
			0u };
#else
		t3dRdInstancePush_t push{ packet.m_nMaterial,
			packet.m_nOverlayMaterial,
#if T3D_INSTANCER_RD_PORT
			packet.m_nFirstVisible,
#else
			0u,
#endif
			draw_flags,
			packet.m_nVisibilityLayers,
			0u,
			0u,
			0u };
#endif
		memcpy( m_pushBytes.ptrw(), &push, sizeof( push ) );
		rd->draw_list_set_push_constant( draw_list, m_pushBytes, sizeof( push ) );
#if T3D_INSTANCER_RD_PORT || T3D_INSTANCER_MINIMAL_RENDER_PATH
		rd->draw_list_draw( draw_list, true, packet.m_nVisibleCount, 0u );
#else
		rd->draw_list_draw_indirect( draw_list,
		        true,
		        shadow_pass ? m_hShadowIndirectBuffer : m_hIndirectBuffer,
		        i * sizeof( t3dRdIndexedIndirectCommand_t ),
		        1u,
		        sizeof( t3dRdIndexedIndirectCommand_t ) );
#endif
		++m_nDraws;
		switch ( rContext.m_passKind ) {
			case T3D_RD_FORWARD_PASS_DEPTH:
				++m_nDepthSubmittedDraws;
				break;
			case T3D_RD_FORWARD_PASS_OPAQUE:
				++m_nOpaqueSubmittedDraws;
				break;
			case T3D_RD_FORWARD_PASS_SHADOW:
				++m_nShadowSubmittedDraws;
				break;
			case T3D_RD_FORWARD_PASS_MOTION:
				++m_nMotionSubmittedDraws;
				break;
			case T3D_RD_FORWARD_PASS_TRANSPARENT:
				++m_nTransparentSubmittedDraws;
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
	++m_nFrames;
	m_lastError = String();
	return true;
}

size_t Terrain3DRdInstanceRenderer::getRetainedCpuBytes( void ) const {
	size_t bytes = 0u;
	bytes += m_visibleIndices.capacity() * sizeof( uint32_t );
	bytes += m_shadowVisibleIndices.capacity() * sizeof( uint32_t );
	bytes += m_motionHistoryRecords.capacity() * sizeof( t3dRdInstanceRecord_t );
	bytes += m_motionHistoryCells.capacity() * sizeof( t3dRdInstanceCell_t );
	bytes += m_motionPreviousRecords.capacity() * sizeof( t3dRdInstanceRecord_t );
	bytes += m_motionCurrentOrder.capacity() * sizeof( uint32_t );
	bytes += m_motionPreviousOrder.capacity() * sizeof( uint32_t );
	bytes += m_motionDirtyRanges.capacity() * sizeof( t3dRdInstanceDirtyRange_t );
	bytes += m_drawPackets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_depthPackets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_shadowPackets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_alphaPackets.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_alphaSortScratch.capacity() * sizeof( t3dRdInstanceDrawPacket_t );
	bytes += m_packetBuildSlots.capacity() * sizeof( t3dRdPacketBuildSlot_t );
	bytes += m_packetBuildEntries.capacity() * sizeof( t3dRdPacketBuildEntry_t );
	bytes += m_materialBindings.capacity() * sizeof( t3dRdInstanceMaterialPairBinding_t );
	bytes += m_hlodDescriptors.capacity() * sizeof( t3dRdHlodCullDescriptor_t );
	bytes += m_instanceCullDescriptors.capacity() * sizeof( t3dRdInstanceCullDescriptor_t );
	bytes += m_shadowInstanceCullDescriptors.capacity() * sizeof( t3dRdInstanceCullDescriptor_t );
	bytes += m_instanceCullCellSpans.capacity() * sizeof( t3dRdInstanceDescriptorSpan_t );
	bytes += m_shadowInstanceCullCellSpans.capacity() * sizeof( t3dRdInstanceDescriptorSpan_t );
	bytes += m_frameBytes.size() + m_instanceBytes.size() + m_previousInstanceBytes.size() + m_dirtyInstanceBytes.size();
	bytes += m_visibleBytes.size() + m_shadowVisibleBytes.size() + m_materialBytes.size() + m_indirectBytes.size();
	bytes += m_shadowIndirectBytes.size() + m_pushBytes.size() + m_hlodDescriptorBytes.size() + m_hlodCullPushBytes.size();
	bytes += m_instanceCullDescriptorBytes.size() + m_instanceCullPushBytes.size();
	return bytes;
}

Dictionary Terrain3DRdInstanceRenderer::getDiagnostics( void ) const {
	Dictionary result;
	uint32_t alpha_packet_count = 0u;
	for ( size_t iPacket = 0u; iPacket < m_drawPackets.size(); ++iPacket ) {
		alpha_packet_count += ( m_drawPackets[iPacket].m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u ? 1u : 0u;
	}
	result["instance_frames"] = ( int64_t )( m_nFrames );
	result["instance_draws"] = ( int64_t )( m_nDraws );
#if T3D_INSTANCER_RD_PORT
	result["instance_indirect_draws"] = 0;
	result["instance_direct_draws"] = ( int64_t )( m_nDraws );
	result["instance_cpu_visibility_reference"] = true;
	result["instance_backend"] = "rd_direct_surface";
#else
	result["instance_indirect_draws"] = ( int64_t )( m_nDraws );
	result["instance_direct_draws"] = 0;
	result["instance_cpu_visibility_reference"] = m_bDirectDrawDebug;
	result["instance_backend"] = "rd_shared_arena_indirect";
#endif
	result["static_instances"] = ( int64_t )( m_nStaticInstances );
	result["visible_instances"] = ( int64_t )( m_nVisibleInstances );
	result["instance_scene_uploads"] = ( int64_t )( m_nSceneUploads );
	result["instance_dirty_upload_bytes"] = ( int64_t )( m_nDirtyUploadBytes );
	result["instance_dirty_upload_ranges"] = ( int64_t )( m_nDirtyUploadRanges );
	result["instance_buffer_capacity_bytes"] = ( int64_t )( m_nInstanceCapacityBytes );
	result["previous_instance_buffer_capacity_bytes"] = ( int64_t )( m_nPreviousInstanceCapacityBytes );
	result["motion_history_resets"] = ( int64_t )( m_nMotionHistoryResets );
	result["motion_history_matches"] = ( int64_t )( m_nMotionHistoryMatches );
	result["owner_buffer_capacity_misses"] = ( int64_t )( m_nBufferCapacityMissCount );
	result["owner_buffer_rebuilds"] = ( int64_t )( m_nBufferRebuildCount );
	result["owner_buffer_rebuild_failures"] = ( int64_t )( m_nBufferRebuildFailureCount );
	result["owner_cpu_capacity_rebuilds"] = ( int64_t )( m_nCpuCapacityRebuildCount );
	result["owner_cpu_capacity_rebuild_failures"] = ( int64_t )( m_nCpuCapacityRebuildFailureCount );
	result["mesh_owner_arena_capacity_misses"] = ( int64_t )( m_meshCache.getArenaCapacityMissCount() );
	result["mesh_owner_arena_growth_requests"] = ( int64_t )( m_meshCache.getArenaGrowthRequestCount() );
	result["mesh_owner_arena_rebuilds"] = ( int64_t )( m_meshCache.getArenaRebuildCount() );
	result["mesh_owner_arena_rebuild_failures"] = ( int64_t )( m_meshCache.getArenaRebuildFailureCount() );
	result["mesh_upload_attempts"] = ( int64_t )( m_meshCache.getUploadAttemptCount() );
	result["mesh_upload_failures"] = ( int64_t )( m_meshCache.getUploadFailureCount() );
	result["mesh_texture_fallbacks"] = ( int64_t )( m_meshCache.getTextureFallbackCount() );
	result["mesh_last_error"] = m_meshCache.getLastError();
	result["mesh_vertex_used_bytes"] = ( int64_t )( m_meshCache.getVertexUsedBytes() );
	result["mesh_vertex_capacity_bytes"] = ( int64_t )( m_meshCache.getVertexCapacityBytes() );
	result["mesh_index16_used_bytes"] = ( int64_t )( m_meshCache.getIndex16UsedBytes() );
	result["mesh_index16_capacity_bytes"] = ( int64_t )( m_meshCache.getIndex16CapacityBytes() );
	result["mesh_index32_used_bytes"] = ( int64_t )( m_meshCache.getIndex32UsedBytes() );
	result["mesh_index32_capacity_bytes"] = ( int64_t )( m_meshCache.getIndex32CapacityBytes() );
	result["motion_debug_enabled"] = m_bMotionDebug;
	result["visible_index_capacity_bytes"] = ( int64_t )( m_nVisibleCapacityBytes );
	result["shadow_visible_index_capacity_bytes"] = ( int64_t )( m_nShadowVisibleCapacityBytes );
	result["indirect_capacity_bytes"] = ( int64_t )( m_nIndirectCapacityBytes );
	result["shadow_indirect_capacity_bytes"] = ( int64_t )( m_nShadowIndirectCapacityBytes );
	result["instance_mesh_arena_bytes"] = ( int64_t )( m_meshCache.getGpuBytes() );
	result["instance_material_textures"] = ( int64_t )( m_meshCache.getTextureCount() );
	result["instance_material_bindings"] = ( int64_t )( m_materialBindings.size() );
	result["instance_mesh_lods"] = ( int64_t )( m_meshCache.getLodCount() );
	result["instance_mesh_surfaces"] = ( int64_t )( m_meshCache.getSurfaceCount() );
	result["instance_mesh_materials"] = ( int64_t )( m_meshCache.getMaterialCount() );
	result["instance_packet_build_input_cells"] = ( int64_t )( m_nPacketBuildInputCells );
	result["instance_resolved_mesh_cells"] = ( int64_t )( m_nResolvedMeshCells );
	result["instance_unresolved_mesh_cells"] = ( int64_t )( m_nUnresolvedMeshCells );
	result["instance_draw_packets"] = ( int64_t )( m_drawPackets.size() );
	result["instance_alpha_draw_packets"] = ( int64_t )( alpha_packet_count );
	result["depth_packets"] = ( int64_t )( m_depthPackets.size() );
	result["shadow_packets"] = ( int64_t )( m_shadowPackets.size() );
	result["instance_depth_submitted_draws"] = ( int64_t )( m_nDepthSubmittedDraws );
	result["instance_opaque_submitted_draws"] = ( int64_t )( m_nOpaqueSubmittedDraws );
	result["instance_shadow_submitted_draws"] = ( int64_t )( m_nShadowSubmittedDraws );
	result["instance_motion_submitted_draws"] = ( int64_t )( m_nMotionSubmittedDraws );
	result["instance_transparent_submitted_draws"] = ( int64_t )( m_nTransparentSubmittedDraws );
	result["hlod_gpu_cull_descriptors"] = ( int64_t )( m_nHlodCullDescriptors );
	result["hlod_gpu_cull_dispatches"] = ( int64_t )( m_nHlodCullDispatches );
	result["hlod_descriptor_capacity_bytes"] = ( int64_t )( m_nHlodDescriptorCapacityBytes );
	result["instance_gpu_cull_dispatches"] = ( int64_t )( m_nInstanceCullDispatches );
	result["instance_gpu_topology_builds"] = ( int64_t )( m_nGpuTopologyBuilds );
	result["instance_gpu_cull_reuses"] = ( int64_t )( m_nGpuCullReuses );
	result["instance_gpu_cull_descriptors"] = ( int64_t )( m_instanceCullDescriptors.size() );
	result["instance_gpu_shadow_cull_descriptors"] = ( int64_t )( m_shadowInstanceCullDescriptors.size() );
	result["instance_gpu_cull_descriptor_capacity_bytes"] = ( int64_t )( m_nInstanceCullDescriptorCapacityBytes );
	result["direct_draw_debug"] = m_bDirectDrawDebug;
	result["instance_forward_prepared_has_draws"] = m_bForwardPreparedHasDraws;
	result["instance_forward_prepared_pass_kind"] = ( int64_t )( m_forwardPreparedPacketPassKind );
	result["instance_forward_prepared_scene_pass"] = ( int64_t )( m_nForwardPreparedScenePass );
	result["instance_forward_pipeline_key_valid"] = m_bLastForwardPipelineKeyValid;
	uint32_t forward_shader_mask = 0u;
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX && variant < 32u; ++variant ) {
		if ( m_hszForwardShaders[variant].is_valid() ) {
			forward_shader_mask |= 1u << variant;
		}
	}
	result["instance_forward_shader_ready_mask"] = ( int64_t )forward_shader_mask;
	result["hlod_cull_shader_instantiated"] = m_hHlodCullShader.is_valid();
	result["forward_lighting"] = m_forwardLighting.getDiagnostics();
	result["last_error"] = m_lastError;
	return result;
}
