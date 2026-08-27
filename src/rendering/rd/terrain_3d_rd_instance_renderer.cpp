// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_instance_renderer.h"

#include "data/terrain_3d_half_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

typedef struct alignas( 16 ) t3dRdInstanceFrame_s
{
	float m_szViewProjection[s_nRdForwardMaxViews][16]{};
	float m_szView[16]{};
	float m_szPreviousViewProjection[s_nRdForwardMaxViews][16]{};
	float m_szCamera[s_nRdForwardMaxViews][4]{};
	float m_szMotionJitter[4]{};
	float m_szMotionParams[4]{};
} t3dRdInstanceFrame_t;
static_assert( sizeof( t3dRdInstanceFrame_t ) == 384u, "Instance frame ABI changed" );

typedef struct alignas( 16 ) t3dRdInstancePush_s
{
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

typedef struct t3dRdHlodCullPush_s
{
	uint32_t m_nDescriptorCount = 0u;
	uint32_t m_nVisibilityMask = UINT32_MAX;
	uint32_t m_nCommandCount = 0u;
	uint32_t m_nReserved0 = 0u;
} t3dRdHlodCullPush_t;
static_assert( sizeof( t3dRdHlodCullPush_t ) == 16u, "HLOD cull push ABI changed" );

static Ref<RDUniform>
t3dRdInstanceBufferUniform( const int nBinding, const RenderingDevice::UniformType type, const RID& rRid )
{
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_binding( nBinding );
	uniform->set_uniform_type( type );
	uniform->add_id( rRid );
	return uniform;
}

static Ref<RDUniform> t3dRdInstanceTextureUniform( const int nBinding, const RID& rSampler, const RID& rTexture )
{
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_binding( nBinding );
	uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE );
	uniform->add_id( rSampler );
	uniform->add_id( rTexture );
	return uniform;
}

static uint64_t t3dRdInstanceTextureId( const uint32_t nLow, const uint32_t nHigh )
{
	return (uint64_t)( nLow ) | ( (uint64_t)( nHigh ) << 32u );
}

typedef struct t3dRdInstanceFrustum_s {
	Vector4 m_szPlanes[6];
} t3dRdInstanceFrustum_t;

static Vector4 t3dRdInstanceNormalizePlane( const Vector4 &rPlane ) {
	const double length_sq = rPlane.x * rPlane.x + rPlane.y * rPlane.y + rPlane.z * rPlane.z;
	if ( length_sq <= 1e-20 ) {
		return rPlane;
	}
	const double inverse_length = 1.0 / std::sqrt( length_sq );
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
	for ( const Vector4 &plane : rFrustum.m_szPlanes ) {
		const double distance = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
		const double radius = std::abs( plane.x ) * extent.x + std::abs( plane.y ) * extent.y +
				std::abs( plane.z ) * extent.z;
		if ( distance + radius < 0.0 ) {
			return false;
		}
	}
	return true;
}

static uint32_t t3dRdNextCapacity( const uint32_t nRequired, const uint32_t nMinimum )
{
	if ( nRequired == 0u ) {
		return nMinimum;
	}
	uint64_t capacity = MAX( (uint64_t)( nMinimum ), 256ull );
	while ( capacity < nRequired ) {
		capacity <<= 1u;
	}
	return capacity > UINT32_MAX ? 0u : (uint32_t)( capacity );
}

static bool t3dRdMotionCellsMatch( const t3dRdInstanceCell_t& rCurrent, const t3dRdInstanceCell_t& rPrevious )
{
	const bool current_hlod = ( rCurrent.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u;
	const bool previous_hlod = ( rPrevious.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u;
	if ( current_hlod != previous_hlod || rCurrent.m_nRegionX != rPrevious.m_nRegionX ||
			rCurrent.m_nRegionZ != rPrevious.m_nRegionZ || rCurrent.m_nCellX != rPrevious.m_nCellX ||
			rCurrent.m_nCellZ != rPrevious.m_nCellZ || rCurrent.m_nInstanceCount != rPrevious.m_nInstanceCount ) {
		return false;
	}
	if ( current_hlod ) {
		return rCurrent.m_nMeshSourceKey == rPrevious.m_nMeshSourceKey && rCurrent.m_nRevision == rPrevious.m_nRevision;
	}
	return rCurrent.m_nMeshAssetId == rPrevious.m_nMeshAssetId;
}

static int t3dRdMotionCellCompare( const t3dRdInstanceCell_t &rA, const t3dRdInstanceCell_t &rB )
{
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

static bool t3dRdInstanceCellVisible( const t3dRdInstanceCell_t &rCell,
		const t3dRdInstanceFrustum_t &rFrustum,
		const Vector3 &rCamera,
		const bool bShadowPass )
{
	if ( rCell.m_nInstanceCount == 0u ||
			( !bShadowPass && ( rCell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) != 0u ) ) {
		return false;
	}
	if ( !bShadowPass ) {
		const float distance_sq = (float)( rCell.m_bounds.get_center().distance_squared_to( rCamera ) );
		if ( rCell.m_fLodBegin > 0.001f && distance_sq < rCell.m_fLodBegin * rCell.m_fLodBegin ) {
			return false;
		}
		if ( rCell.m_fLodEnd > 0.001f && distance_sq >= rCell.m_fLodEnd * rCell.m_fLodEnd ) {
			return false;
		}
	}
	return t3dRdInstanceAabbVisible( rCell.m_bounds, rFrustum );
}

static void t3dRdStableSortAlphaPackets( std::vector<t3dRdInstanceDrawPacket_t> &rPackets,
		std::vector<t3dRdInstanceDrawPacket_t> &rScratch )
{
	const size_t count = rPackets.size();
	if ( count < 2u ) {
		return;
	}
	rScratch.resize( count );
	for ( size_t width = 1u; width < count; width *= 2u ) {
		for ( size_t first = 0u; first < count; first += width * 2u ) {
			const size_t middle = MIN( first + width, count );
			const size_t last = MIN( first + width * 2u, count );
			size_t left = first;
			size_t right = middle;
			for ( size_t out = first; out < last; ++out ) {
				const bool take_left = right >= last ||
						( left < middle &&
								rPackets[left].m_fSortDistanceSq >= rPackets[right].m_fSortDistanceSq );
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

static uint32_t t3dRdPreviewPackSnormPair( const float x, const float y )
{
	const int16_t px = (int16_t)( std::lround( CLAMP( x, -1.0f, 1.0f ) * 32767.0f ) );
	const int16_t py = (int16_t)( std::lround( CLAMP( y, -1.0f, 1.0f ) * 32767.0f ) );
	return (uint32_t)( (uint16_t)( px ) ) | ( (uint32_t)( (uint16_t)( py ) ) << 16u );
}

static t3dRdInstanceRecord_t t3dRdPreviewInstanceRecord( const Transform3D& rTransform )
{
	t3dRdInstanceRecord_t record;
	record.m_szPosition[0] = (float)( rTransform.origin.x );
	record.m_szPosition[1] = (float)( rTransform.origin.y );
	record.m_szPosition[2] = (float)( rTransform.origin.z );
	const Vector3 scale = rTransform.basis.get_scale();
	const Quaternion rotation = rTransform.basis.orthonormalized().get_rotation_quaternion().normalized();
	record.m_nQuaternion01 = t3dRdPreviewPackSnormPair( (float)( rotation.x ), (float)( rotation.y ) );
	record.m_nQuaternion23 = t3dRdPreviewPackSnormPair( (float)( rotation.z ), (float)( rotation.w ) );
	record.m_nScale01 = (uint32_t)( t3dHalfEncode( (float)( scale.x ) ) ) |
			( (uint32_t)( t3dHalfEncode( (float)( scale.y ) ) ) << 16u );
	record.m_nScale2Flags = (uint32_t)( t3dHalfEncode( (float)( scale.z ) ) );
	record.m_nColorRgba8 = t3dRdPackGodotRgba32ForGpu( 0xffffffffu );
	return record;
}
Terrain3DRdInstanceRenderer::Terrain3DRdInstanceRenderer( void ) :
		m_context( Terrain3DRdContext::getShared() ), m_forwardLighting( m_context )
{
}
Terrain3DRdInstanceRenderer::~Terrain3DRdInstanceRenderer( void )
{
	shutdown();
}

void Terrain3DRdInstanceRenderer::setError( const String& rError )
{
	m_lastError = rError;
}
bool Terrain3DRdInstanceRenderer::ensureDevice( void )
{
	return m_context.initialize() && m_context.validateDevice() && m_context.getDevice();
}

bool Terrain3DRdInstanceRenderer::ensureShader( const Ref<RDShaderSPIRV>& rShader )
{
	if ( m_hShader.is_valid() ) {
		return true;
	}
	if ( rShader.is_null() ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	m_hShader = rd->shader_create_from_spirv( rShader, "T3D.InstanceShader" );
	if ( !m_hShader.is_valid() ||
			!m_context.registerResource( m_hShader,
					"T3D.Instance.Shader",
					Terrain3DRdContext::RESOURCE_SHADER,
					Terrain3DRdContext::MEMORY_NONE,
					0u ) ) {
		setError( "Failed to create Terrain3D instance shader" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureForwardShaders( const t3dRdForwardShaderBundle_t& rShaders )
{
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		if ( m_hszForwardShaders[variant].is_valid() ) {
			continue;
		}
		const Ref<RDShaderSPIRV> spirv = rShaders.m_szInstances[variant];
		if ( spirv.is_null() ) {
			setError( "Missing Terrain3D Forward+ instance shader variant" );
			return false;
		}
		RID shader = rd->shader_create_from_spirv(
				spirv, String( "T3D.Forward.InstanceShader." ) + String::num_int64( variant ) );
		const String name = String( "T3D.Forward.Shader.Instance." ) + String::num_int64( variant );
		if ( !shader.is_valid() ||
				!m_context.registerResource(
						shader, name, Terrain3DRdContext::RESOURCE_SHADER, Terrain3DRdContext::MEMORY_NONE, 0u ) ) {
			if ( shader.is_valid() ) {
				rd->free_rid( shader );
			}
			setError( "Failed to create Terrain3D Forward+ instance shader variant" );
			return false;
		}
		m_hszForwardShaders[variant] = shader;
	}
	m_forwardPipelines.reserve( 24u );
	return true;
}

void Terrain3DRdInstanceRenderer::freeForwardPipelines( void )
{
	for ( t3dRdInstanceForwardPipelineEntry_t& entry : m_forwardPipelines ) {
		for ( RID& pipeline : entry.m_hszPipelines ) {
			m_context.freeResource( pipeline );
		}
	}
	m_forwardPipelines.clear();
	m_bLastForwardPipelineKeyValid = false;
}

bool Terrain3DRdInstanceRenderer::selectForwardVariant( const t3dRdForwardPassContext_t& rContext,
		t3dRdForwardShaderVariant_t& rVariant ) const
{
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
	if ( ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION ) ||
			rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR ) {
		return false;
	}
	const bool specular = ( rContext.m_nColorPassFlags & ( 1u << 1u ) ) != 0u;
	const bool motion =
			rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || ( rContext.m_nColorPassFlags & ( 1u << 3u ) ) != 0u;
	const bool radiance_array = ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	if ( radiance_array ) {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY
									   : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY )
							: ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY
									   : T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY );
	} else {
		rVariant = specular
				? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR )
				: ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION : T3D_RD_FORWARD_SHADER_COLOR );
	}
	return true;
}

const Terrain3DRdInstanceRenderer::t3dRdInstanceForwardPipelineEntry_t *
Terrain3DRdInstanceRenderer::findForwardPipeline( const t3dRdInstanceForwardPipelineKey_t& rKey ) const
{
	for ( const t3dRdInstanceForwardPipelineEntry_t& entry : m_forwardPipelines ) {
		if ( entry.m_key == rKey ) {
			return &entry;
		}
	}
	return nullptr;
}

bool Terrain3DRdInstanceRenderer::ensureForwardPipelines( const t3dRdForwardPassContext_t& rContext,
		const t3dRdForwardShaderVariant_t variant )
{
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 ||
			m_meshCache.getVertexFormat() == RenderingDevice::INVALID_FORMAT_ID ) {
		return false;
	}
	t3dRdInstanceForwardPipelineKey_t key;
	key.m_nFramebufferFormat = rContext.m_nFramebufferFormat;
	key.m_variant = variant;
	key.m_passKind = rContext.m_passKind;
	key.m_samples = rd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u );
	if ( ( 1u << (uint32_t)( key.m_samples ) ) != rContext.m_nSampleCount ) {
		setError( "Terrain3D Forward+ sample-count metadata does not match native framebuffer format" );
		return false;
	}
	key.m_nViewCount = rContext.m_nViewCount;
	key.m_bReverseCull = rContext.m_bReverseCull;
	key.m_bDepthPrepassActive = ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
										rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) &&
			rContext.m_bDepthPrepassActive;
	if ( findForwardPipeline( key ) ) {
		m_lastForwardPipelineKey = key;
		m_bLastForwardPipelineKeyValid = true;
		return true;
	}
	if ( m_forwardPipelines.size() >= 64u ) {
		freeForwardPipelines();
	}

	t3dRdInstanceForwardPipelineEntry_t entry;
	entry.m_key = key;
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( key.m_samples );
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test( true );
	depth->set_enable_depth_write( !key.m_bDepthPrepassActive );
	depth->set_depth_compare_operator( key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL
																 : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	uint32_t blend_count = 0u;
	if ( variant <= T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY ) {
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
			a->set_enable_blend( false );
			attachments.push_back( a );
		}
		blend->set_attachments( attachments );
	}
	const RenderingDevice::PolygonCullMode cull_modes[3] = {
		key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK,
		key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_BACK : RenderingDevice::POLYGON_CULL_FRONT,
		RenderingDevice::POLYGON_CULL_DISABLED,
	};
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
		const String name = String( "T3D.Forward.Pipeline.Instance." ) + String::num_int64( variant ) + "." +
				String::num_int64( cull );
		if ( !entry.m_hszPipelines[cull].is_valid() ||
				!m_context.registerResource( entry.m_hszPipelines[cull],
						name,
						Terrain3DRdContext::RESOURCE_RASTER_PIPELINE,
						Terrain3DRdContext::MEMORY_NONE,
						0u ) ) {
			for ( RID& pipeline : entry.m_hszPipelines ) {
				m_context.freeResource( pipeline );
			}
			m_context.endResourceRebuild();
			setError( "Failed to create framebuffer-compatible Terrain3D Forward+ instance pipeline" );
			return false;
		}
	}
	m_context.endResourceRebuild();
	m_forwardPipelines.push_back( entry );
	m_lastForwardPipelineKey = key;
	m_bLastForwardPipelineKeyValid = true;
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureHlodCullShader( const Ref<RDShaderSPIRV>& rShader )
{
	if ( m_hHlodCullShader.is_valid() && m_hHlodCullPipeline.is_valid() ) {
		return true;
	}
	if ( rShader.is_null() ) {
		setError( "Missing Terrain3D HLOD cull shader" );
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	m_hHlodCullShader = rd->shader_create_from_spirv( rShader, "T3D.HlodCullShader" );
	if ( !m_hHlodCullShader.is_valid() ||
			!m_context.registerResource( m_hHlodCullShader,
					"T3D.HLOD.CullShader",
					Terrain3DRdContext::RESOURCE_SHADER,
					Terrain3DRdContext::MEMORY_NONE,
					0u ) ) {
		setError( "Failed to create Terrain3D HLOD cull shader" );
		return false;
	}
	m_hHlodCullPipeline = rd->compute_pipeline_create( m_hHlodCullShader );
	if ( !m_hHlodCullPipeline.is_valid() ||
			!m_context.registerResource( m_hHlodCullPipeline,
					"T3D.HLOD.CullPipeline",
					Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
					Terrain3DRdContext::MEMORY_NONE,
					0u ) ) {
		setError( "Failed to create Terrain3D HLOD cull pipeline" );
		return false;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureBuffer( RID& rRid,
		uint32_t& rCapacity,
		const uint32_t nRequired,
		const String& rName,
		const Terrain3DRdContext::ResourceType type,
		const Terrain3DRdContext::MemoryCategory memory )
{
	if ( rRid.is_valid() && rCapacity >= nRequired ) {
		return true;
	}
	const uint32_t minimum = type == Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER ? 256u : 4096u;
	const uint32_t capacity = t3dRdNextCapacity( nRequired, minimum );
	if ( capacity == 0u ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( rRid.is_valid() ) {
		m_context.freeResource( rRid, rCapacity );
	}
	if ( type == Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER ) {
		rRid = rd->uniform_buffer_create( capacity );
	} else if ( type == Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER ) {
		rRid = rd->storage_buffer_create( capacity );
	} else {
		rRid = rd->storage_buffer_create( capacity );
	}
	if ( !rRid.is_valid() || !m_context.registerResource( rRid, rName, type, memory, capacity ) ) {
		return false;
	}
	rCapacity = capacity;
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureBuffers( const uint32_t nInstanceBytes,
		const uint32_t nVisibleBytes,
		const uint32_t nMaterialBytes,
		const uint32_t nIndirectBytes )
{
	const bool replace_instance = !m_hInstanceBuffer.is_valid() || m_nInstanceCapacityBytes < nInstanceBytes;
	const bool replace_previous_instance =
			!m_hPreviousInstanceBuffer.is_valid() || m_nPreviousInstanceCapacityBytes < nInstanceBytes;
	const bool replace_visible = !m_hVisibleIndexBuffer.is_valid() || m_nVisibleCapacityBytes < nVisibleBytes;
	const bool replace_material = !m_hMaterialBuffer.is_valid() || m_nMaterialCapacityBytes < nMaterialBytes;
	const bool replace_indirect = !m_hIndirectBuffer.is_valid() || m_nIndirectCapacityBytes < nIndirectBytes;
	if ( ( replace_instance || replace_previous_instance || replace_visible || replace_material || replace_indirect ) &&
			m_hUniformSet.is_valid() ) {
		m_context.freeResource( m_hUniformSet );
	}
	if ( ( replace_instance || replace_previous_instance || replace_material ) && m_hShadowUniformSet.is_valid() ) {
		m_context.freeResource( m_hShadowUniformSet );
	}
	if ( replace_indirect && m_hHlodCullUniformSet.is_valid() ) {
		m_context.freeResource( m_hHlodCullUniformSet );
	}
	if ( !m_hFrameBuffer.is_valid() ) {
		RenderingDevice *const rd = m_context.getDevice();
		m_hFrameBuffer = rd->uniform_buffer_create( sizeof( t3dRdInstanceFrame_t ) );
		if ( !m_hFrameBuffer.is_valid() ||
				!m_context.registerResource( m_hFrameBuffer,
						"T3D.Instance.FrameUBO",
						Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER,
						Terrain3DRdContext::MEMORY_UNIFORM,
						sizeof( t3dRdInstanceFrame_t ) ) ) {
			return false;
		}
	}
	return ensureBuffer( m_hInstanceBuffer,
				   m_nInstanceCapacityBytes,
				   MAX( nInstanceBytes, (uint32_t)( sizeof( t3dRdInstanceRecord_t ) ) ),
				   "T3D.Instance.Records",
				   Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
				   Terrain3DRdContext::MEMORY_INSTANCE ) &&
			ensureBuffer( m_hPreviousInstanceBuffer,
					m_nPreviousInstanceCapacityBytes,
					MAX( nInstanceBytes, (uint32_t)( sizeof( t3dRdInstanceRecord_t ) ) ),
					"T3D.Instance.PreviousRecords",
					Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
					Terrain3DRdContext::MEMORY_INSTANCE ) &&
			ensureBuffer( m_hVisibleIndexBuffer,
					m_nVisibleCapacityBytes,
					MAX( nVisibleBytes, 4u ),
					"T3D.Instance.VisibleIndices",
					Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
					Terrain3DRdContext::MEMORY_INSTANCE ) &&
			ensureBuffer( m_hMaterialBuffer,
					m_nMaterialCapacityBytes,
					MAX( nMaterialBytes, (uint32_t)( sizeof( t3dRdInstanceMaterial_t ) ) ),
					"T3D.Instance.MaterialTable",
					Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
					Terrain3DRdContext::MEMORY_MATERIAL_TABLE ) &&
			ensureBuffer( m_hIndirectBuffer,
					m_nIndirectCapacityBytes,
					MAX( nIndirectBytes, (uint32_t)( sizeof( t3dRdIndexedIndirectCommand_t ) ) ),
					"T3D.Instance.Indirect",
					Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER,
					Terrain3DRdContext::MEMORY_INDIRECT );
}

bool Terrain3DRdInstanceRenderer::ensureShadowBuffers( const uint32_t nVisibleBytes, const uint32_t nIndirectBytes )
{
	const bool replace_visible =
			!m_hShadowVisibleIndexBuffer.is_valid() || m_nShadowVisibleCapacityBytes < nVisibleBytes;
	if ( replace_visible && m_hShadowUniformSet.is_valid() ) {
		m_context.freeResource( m_hShadowUniformSet );
	}
	return ensureBuffer( m_hShadowVisibleIndexBuffer,
				   m_nShadowVisibleCapacityBytes,
				   MAX( nVisibleBytes, 4u ),
				   "T3D.Instance.ShadowVisibleIndices",
				   Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
				   Terrain3DRdContext::MEMORY_INSTANCE ) &&
			ensureBuffer( m_hShadowIndirectBuffer,
					m_nShadowIndirectCapacityBytes,
					MAX( nIndirectBytes, (uint32_t)( sizeof( t3dRdIndexedIndirectCommand_t ) ) ),
					"T3D.Instance.ShadowIndirect",
					Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER,
					Terrain3DRdContext::MEMORY_INDIRECT );
}

bool Terrain3DRdInstanceRenderer::ensureUniformSet( void )
{
	if ( m_hUniformSet.is_valid() ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( t3dRdInstanceBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
	uniforms.push_back(
			t3dRdInstanceBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hInstanceBuffer ) );
	uniforms.push_back(
			t3dRdInstanceBufferUniform( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hVisibleIndexBuffer ) );
	uniforms.push_back(
			t3dRdInstanceBufferUniform( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hMaterialBuffer ) );
	uniforms.push_back(
			t3dRdInstanceBufferUniform( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPreviousInstanceBuffer ) );
	m_hUniformSet = rd->uniform_set_create( uniforms, m_hShader, 0u );
	return m_hUniformSet.is_valid() &&
			m_context.registerResource( m_hUniformSet,
					"T3D.Instance.UniformSet",
					Terrain3DRdContext::RESOURCE_UNIFORM_SET,
					Terrain3DRdContext::MEMORY_NONE,
					0u );
}

bool Terrain3DRdInstanceRenderer::ensureShadowUniformSet( void )
{
	if ( m_hShadowUniformSet.is_valid() ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( t3dRdInstanceBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
	uniforms.push_back(
			t3dRdInstanceBufferUniform( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hInstanceBuffer ) );
	uniforms.push_back( t3dRdInstanceBufferUniform(
			2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hShadowVisibleIndexBuffer ) );
	uniforms.push_back(
			t3dRdInstanceBufferUniform( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hMaterialBuffer ) );
	uniforms.push_back(
			t3dRdInstanceBufferUniform( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hPreviousInstanceBuffer ) );
	m_hShadowUniformSet = rd->uniform_set_create( uniforms, m_hShader, 0u );
	return m_hShadowUniformSet.is_valid() &&
			m_context.registerResource( m_hShadowUniformSet,
					"T3D.Instance.ShadowUniformSet",
					Terrain3DRdContext::RESOURCE_UNIFORM_SET,
					Terrain3DRdContext::MEMORY_NONE,
					0u );
}

bool Terrain3DRdInstanceRenderer::ensureMaterialSamplers( void )
{
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
		state->set_mag_filter(
				nearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_min_filter(
				nearest ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR );
		state->set_mip_filter( ( filter == 3u || filter == 5u ) ? RenderingDevice::SAMPLER_FILTER_LINEAR
																: RenderingDevice::SAMPLER_FILTER_NEAREST );
		state->set_min_lod( 0.0f );
		state->set_max_lod( mipmaps ? 1000.0f : 0.0f );
		state->set_use_anisotropy( anisotropic );
		state->set_anisotropy_max( anisotropic ? 16.0f : 1.0f );
		const RenderingDevice::SamplerRepeatMode repeat_mode = repeat
				? RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT
				: RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		state->set_repeat_u( repeat_mode );
		state->set_repeat_v( repeat_mode );
		state->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hMaterialSamplers[i] = rd->sampler_create( state );
		const String name = String( "T3D.Instance.MaterialSampler." ) + String::num_int64( i );
		if ( !m_hMaterialSamplers[i].is_valid() ||
				!m_context.registerResource( m_hMaterialSamplers[i],
						name,
						Terrain3DRdContext::RESOURCE_SAMPLER,
						Terrain3DRdContext::MEMORY_NONE,
						0u ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::ensureFallbackMaterialTextures( void )
{
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	auto create_fallback = [&]( RID& rTexture, const uint32_t nRgba, const String& rName ) -> bool {
		if ( rTexture.is_valid() ) {
			return true;
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
		format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT );
		Ref<RDTextureView> view;
		view.instantiate();
		PackedByteArray bytes;
		bytes.resize( sizeof( nRgba ) );
		std::memcpy( bytes.ptrw(), &nRgba, sizeof( nRgba ) );
		TypedArray<PackedByteArray> initial;
		initial.push_back( bytes );
		rTexture = rd->texture_create( format, view, initial );
		return rTexture.is_valid() &&
				m_context.registerResource( rTexture,
						rName,
						Terrain3DRdContext::RESOURCE_TEXTURE,
						Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
						sizeof( nRgba ) );
	};
	// RGBA byte order is low-to-high for R,G,B,A in the upload payload.
	return create_fallback( m_hFallbackAlbedoTexture, 0xFFFFFFFFu, "T3D.Instance.FallbackAlbedo" ) &&
			create_fallback( m_hFallbackNormalTexture, 0xFFFF8080u, "T3D.Instance.FallbackNormal" ) &&
			create_fallback( m_hFallbackOrmTexture, 0xFF00FFFFu, "T3D.Instance.FallbackOrm" );
}

void Terrain3DRdInstanceRenderer::freeMaterialBindings( void )
{
	for ( t3dRdInstanceMaterialPairBinding_t& binding : m_materialBindings ) {
		m_context.freeResource( binding.m_hUniformSet );
	}
	m_materialBindings.clear();
	m_nMaterialBindingGeneration = 0u;
}

RID Terrain3DRdInstanceRenderer::getMaterialBinding( const uint32_t nMaterial, const uint32_t nOverlayMaterial ) const
{
	for ( const t3dRdInstanceMaterialPairBinding_t& binding : m_materialBindings ) {
		if ( binding.m_nMaterial == nMaterial && binding.m_nOverlayMaterial == nOverlayMaterial ) {
			return binding.m_hUniformSet;
		}
	}
	return RID();
}

bool Terrain3DRdInstanceRenderer::rebuildMaterialBindings( void )
{
	if ( m_nMaterialBindingGeneration == m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
		return true;
	}
	m_context.beginResourceRebuild();
	freeMaterialBindings();
	if ( !ensureMaterialSamplers() || !ensureFallbackMaterialTextures() ) {
		m_context.endResourceRebuild();
		return false;
	}
	const std::vector<t3dRdInstanceMaterial_t>& materials = m_meshCache.getMaterials();
	auto material_ptr = [&]( const uint32_t nIndex ) -> const t3dRdInstanceMaterial_t * {
		return nIndex != UINT16_MAX && nIndex < materials.size() ? &materials[nIndex] : nullptr;
	};
	auto texture_for =
			[&]( const t3dRdInstanceMaterial_t *const pMaterial, const uint32_t nSlot, const RID& rFallback ) -> RID {
		if ( !pMaterial ) {
			return rFallback;
		}
		uint64_t id = 0u;
		if ( nSlot == 0u ) {
			id = t3dRdInstanceTextureId( pMaterial->m_nAlbedoTextureIdLow, pMaterial->m_nAlbedoTextureIdHigh );
		} else if ( nSlot == 1u ) {
			id = t3dRdInstanceTextureId( pMaterial->m_nNormalTextureIdLow, pMaterial->m_nNormalTextureIdHigh );
		} else {
			id = t3dRdInstanceTextureId( pMaterial->m_nOrmTextureIdLow, pMaterial->m_nOrmTextureIdHigh );
		}
		const RID texture = m_meshCache.getTextureRid( id );
		return texture.is_valid() ? texture : rFallback;
	};
	auto sampler_for = [&]( const t3dRdInstanceMaterial_t *const pMaterial ) -> RID {
		if ( !pMaterial ) {
			return m_hMaterialSamplers[7u]; // linear/no-mip, repeating
		}
		const uint32_t filter =
				( pMaterial->m_nFlags & T3D_RD_INSTANCE_MATERIAL_FILTER_MASK ) >> T3D_RD_INSTANCE_MATERIAL_FILTER_SHIFT;
		const uint32_t repeat = ( pMaterial->m_nFlags & T3D_RD_INSTANCE_MATERIAL_TEXTURE_REPEAT ) != 0u ? 6u : 0u;
		return m_hMaterialSamplers[MIN( filter, 5u ) + repeat];
	};
	auto add_binding = [&]( const uint32_t nMaterial, const uint32_t nOverlay ) -> bool {
		if ( getMaterialBinding( nMaterial, nOverlay ).is_valid() ) {
			return true;
		}
		const t3dRdInstanceMaterial_t *const base = material_ptr( nMaterial );
		const t3dRdInstanceMaterial_t *const overlay = material_ptr( nOverlay );
		const RID base_sampler = sampler_for( base );
		const RID overlay_sampler = sampler_for( overlay );
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back(
				t3dRdInstanceTextureUniform( 0, base_sampler, texture_for( base, 0u, m_hFallbackAlbedoTexture ) ) );
		uniforms.push_back(
				t3dRdInstanceTextureUniform( 1, base_sampler, texture_for( base, 1u, m_hFallbackNormalTexture ) ) );
		uniforms.push_back(
				t3dRdInstanceTextureUniform( 2, base_sampler, texture_for( base, 2u, m_hFallbackOrmTexture ) ) );
		uniforms.push_back( t3dRdInstanceTextureUniform(
				3, overlay_sampler, texture_for( overlay, 0u, m_hFallbackAlbedoTexture ) ) );
		uniforms.push_back( t3dRdInstanceTextureUniform(
				4, overlay_sampler, texture_for( overlay, 1u, m_hFallbackNormalTexture ) ) );
		uniforms.push_back(
				t3dRdInstanceTextureUniform( 5, overlay_sampler, texture_for( overlay, 2u, m_hFallbackOrmTexture ) ) );
		RID set = m_context.getDevice()->uniform_set_create( uniforms, m_hShader, 1u );
		const String name = String( "T3D.Instance.MaterialPair." ) + String::num_int64( nMaterial ) + "." +
				String::num_int64( nOverlay );
		if ( !set.is_valid() ||
				!m_context.registerResource(
						set, name, Terrain3DRdContext::RESOURCE_UNIFORM_SET, Terrain3DRdContext::MEMORY_NONE, 0u ) ) {
			return false;
		}
		m_materialBindings.push_back(
				t3dRdInstanceMaterialPairBinding_t{ (uint16_t)( nMaterial ), (uint16_t)( nOverlay ), set } );
		return true;
	};
	if ( !add_binding( UINT16_MAX, UINT16_MAX ) ) {
		m_context.endResourceRebuild();
		return false;
	}
	for ( const t3dRdMeshSourceSurface_t& surface : m_meshCache.getSurfaces() ) {
		if ( !add_binding( surface.m_nMaterial, surface.m_nOverlayMaterial ) ) {
			m_context.endResourceRebuild();
			return false;
		}
	}
	m_nMaterialBindingGeneration = m_meshCache.getSourceGeneration();
	m_context.endResourceRebuild();
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadScene( const t3dRdInstanceSceneSnapshot_t *const pScene )
{
	if ( !pScene ) {
		return false;
	}
	if ( pScene->m_nGeneration == m_nUploadedSceneGeneration &&
			m_nSceneCapacityMeshGeneration == m_meshCache.getSourceGeneration() ) {
		return true;
	}
	if ( pScene->m_instances.size() > UINT32_MAX / sizeof( t3dRdInstanceRecord_t ) ) {
		setError( "RD instance publication exceeds 32-bit buffer limits" );
		return false;
	}

	uint64_t packet_capacity64 = 0u;
	uint64_t visible_index_capacity64 = 0u;
	for ( const t3dRdInstanceCell_t &cell : pScene->m_cells ) {
		const t3dRdMeshHandle_t mesh = m_meshCache.getMeshHandleBySourceKey( cell.m_nMeshSourceKey != 0u
						? cell.m_nMeshSourceKey
						: t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = m_meshCache.resolveMesh( mesh );
		if ( pLod ) {
			packet_capacity64 += pLod->m_nSurfaceCount;
			visible_index_capacity64 +=
					(uint64_t)( cell.m_nInstanceCount ) * (uint64_t)( pLod->m_nSurfaceCount );
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

	m_nPacketCapacity = (uint32_t)( packet_capacity64 );
	m_nVisibleIndexCapacity = (uint32_t)( visible_index_capacity64 );
	const uint32_t instance_bytes = (uint32_t)( pScene->m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
	const uint32_t visible_bytes = m_nVisibleIndexCapacity * sizeof( uint32_t );
	const uint32_t material_bytes =
			(uint32_t)( m_meshCache.getMaterials().size() * sizeof( t3dRdInstanceMaterial_t ) );
	const uint32_t indirect_bytes = m_nPacketCapacity * sizeof( t3dRdIndexedIndirectCommand_t );
	const uint32_t required_instance_capacity =
			MAX( instance_bytes, (uint32_t)( sizeof( t3dRdInstanceRecord_t ) ) );
	const bool full_instance_upload = !m_hInstanceBuffer.is_valid() ||
			m_nInstanceCapacityBytes < required_instance_capacity || m_nInstanceUsedBytes != instance_bytes;

	// Scene publication is the only capacity-rebuild boundary for frame-side
	// instance storage. All render callbacks only change logical sizes.
	m_visibleIndices.reserve( m_nVisibleIndexCapacity );
	m_shadowVisibleIndices.reserve( m_nVisibleIndexCapacity );
	m_drawPackets.reserve( m_nPacketCapacity );
	m_depthPackets.reserve( m_nPacketCapacity );
	m_shadowPackets.reserve( m_nPacketCapacity );
	m_hlodDescriptors.reserve( m_nPacketCapacity );
	m_packetBuildSlots.reserve( m_nPacketCapacity );
	m_packetBuildEntries.reserve( m_nPacketCapacity );
	m_alphaPackets.reserve( m_nPacketCapacity );
	m_alphaSortScratch.reserve( m_nPacketCapacity );
	m_motionHistoryRecords.reserve( pScene->m_instances.size() );
	m_motionPreviousRecords.reserve( pScene->m_instances.size() );
	m_motionHistoryCells.reserve( pScene->m_cells.size() );
	m_motionCurrentOrder.reserve( pScene->m_cells.size() );
	m_motionPreviousOrder.reserve( pScene->m_cells.size() );
	m_packetIndex.beginColdRebuild();
	if ( !m_packetIndex.reserveAndSeal( m_nPacketCapacity ) ) {
		setError( "RD instance packet hash capacity could not be sealed" );
		return false;
	}

	const uint32_t hlod_descriptor_bytes =
			MAX( m_nPacketCapacity * (uint32_t)( sizeof( t3dRdHlodCullDescriptor_t ) ),
					(uint32_t)( sizeof( t3dRdHlodCullDescriptor_t ) ) );
	if ( ( !m_hHlodCullDescriptorBuffer.is_valid() || m_nHlodDescriptorCapacityBytes < hlod_descriptor_bytes ) &&
			m_hHlodCullUniformSet.is_valid() ) {
		m_context.freeResource( m_hHlodCullUniformSet );
	}
	m_context.beginResourceRebuild();
	const bool buffers_ok = ensureBuffers( instance_bytes,
			MAX( visible_bytes, 4u ),
			MAX( material_bytes, (uint32_t)( sizeof( t3dRdInstanceMaterial_t ) ) ),
			MAX( indirect_bytes, (uint32_t)( sizeof( t3dRdIndexedIndirectCommand_t ) ) ) ) &&
			ensureShadowBuffers( MAX( visible_bytes, 4u ),
					MAX( indirect_bytes, (uint32_t)( sizeof( t3dRdIndexedIndirectCommand_t ) ) ) ) &&
			ensureBuffer( m_hHlodCullDescriptorBuffer,
					m_nHlodDescriptorCapacityBytes,
					hlod_descriptor_bytes,
					"T3D.HLOD.CullDescriptors",
					Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
					Terrain3DRdContext::MEMORY_INSTANCE ) &&
			ensureUniformSet() && ensureShadowUniformSet();
	m_context.endResourceRebuild();
	if ( !buffers_ok ) {
		return false;
	}

	if ( m_instanceBytes.size() < (int64_t)( m_nInstanceCapacityBytes ) ) {
		m_instanceBytes.resize( m_nInstanceCapacityBytes );
	}
	if ( m_previousInstanceBytes.size() < (int64_t)( m_nPreviousInstanceCapacityBytes ) ) {
		m_previousInstanceBytes.resize( m_nPreviousInstanceCapacityBytes );
	}
	if ( m_visibleBytes.size() < (int64_t)( m_nVisibleCapacityBytes ) ) {
		m_visibleBytes.resize( m_nVisibleCapacityBytes );
	}
	if ( m_shadowVisibleBytes.size() < (int64_t)( m_nShadowVisibleCapacityBytes ) ) {
		m_shadowVisibleBytes.resize( m_nShadowVisibleCapacityBytes );
	}
	if ( m_indirectBytes.size() < (int64_t)( m_nIndirectCapacityBytes ) ) {
		m_indirectBytes.resize( m_nIndirectCapacityBytes );
	}
	if ( m_shadowIndirectBytes.size() < (int64_t)( m_nShadowIndirectCapacityBytes ) ) {
		m_shadowIndirectBytes.resize( m_nShadowIndirectCapacityBytes );
	}
	if ( m_hlodDescriptorBytes.size() < (int64_t)( m_nHlodDescriptorCapacityBytes ) ) {
		m_hlodDescriptorBytes.resize( m_nHlodDescriptorCapacityBytes );
	}
	constexpr uint32_t s_nDirtyChunkRecords = 64u;
	const uint32_t dirty_chunk_bytes = s_nDirtyChunkRecords * sizeof( t3dRdInstanceRecord_t );
	if ( m_dirtyInstanceBytes.size() < (int64_t)( dirty_chunk_bytes ) ) {
		m_dirtyInstanceBytes.resize( dirty_chunk_bytes );
	}
	const uint32_t dirty_chunk_count =
			( (uint32_t)( pScene->m_instances.size() ) + s_nDirtyChunkRecords - 1u ) / s_nDirtyChunkRecords;
	m_dirtyInstanceChunks.resize( dirty_chunk_count );

	uint32_t uploaded_bytes = 0u;
	if ( instance_bytes > 0u ) {
		const uint8_t *const pSource = reinterpret_cast<const uint8_t *>( pScene->m_instances.data() );
		RenderingDevice *const pRd = m_context.getDevice();
		if ( full_instance_upload ) {
			std::memcpy( m_instanceBytes.ptrw(), pSource, instance_bytes );
			if ( pRd->buffer_update( m_hInstanceBuffer, 0u, instance_bytes, m_instanceBytes ) != OK ) {
				return false;
			}
			uploaded_bytes = instance_bytes;
			++m_nDirtyUploadRanges;
		} else {
			std::memset( m_dirtyInstanceChunks.data(), 0, m_dirtyInstanceChunks.size() * sizeof( uint8_t ) );
			const uint8_t *const pPrevious = m_instanceBytes.ptr();
			const uint32_t record_count = (uint32_t)( pScene->m_instances.size() );
			for ( uint32_t i = 0u; i < record_count; ++i ) {
				const uint32_t offset = i * sizeof( t3dRdInstanceRecord_t );
				if ( std::memcmp( pPrevious + offset, pSource + offset, sizeof( t3dRdInstanceRecord_t ) ) != 0 ) {
					m_dirtyInstanceChunks[i / s_nDirtyChunkRecords] = 1u;
				}
			}
			for ( uint32_t chunk = 0u; chunk < dirty_chunk_count; ++chunk ) {
				if ( m_dirtyInstanceChunks[chunk] == 0u ) {
					continue;
				}
				const uint32_t first_record = chunk * s_nDirtyChunkRecords;
				const uint32_t record_count_in_chunk = MIN( s_nDirtyChunkRecords, record_count - first_record );
				const uint32_t first_byte = first_record * sizeof( t3dRdInstanceRecord_t );
				const uint32_t chunk_bytes = record_count_in_chunk * sizeof( t3dRdInstanceRecord_t );
				std::memcpy( m_dirtyInstanceBytes.ptrw(), pSource + first_byte, chunk_bytes );
				if ( pRd->buffer_update( m_hInstanceBuffer, first_byte, chunk_bytes, m_dirtyInstanceBytes ) != OK ) {
					return false;
				}
				uploaded_bytes += chunk_bytes;
				++m_nDirtyUploadRanges;
			}
			std::memcpy( m_instanceBytes.ptrw(), pSource, instance_bytes );
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

bool Terrain3DRdInstanceRenderer::prepareMotionHistory( const t3dRdInstanceSceneSnapshot_t &rScene,
		const uint64_t nRenderFrame )
{
	if ( m_nMotionHistoryRenderFrame == nRenderFrame && rScene.m_nGeneration == m_nMotionHistorySceneGeneration ) {
		return true;
	}
	if ( rScene.m_instances.empty() ) {
		m_motionHistoryRecords.clear();
		m_motionHistoryCells.clear();
		m_motionCurrentOrder.clear();
		m_motionPreviousOrder.clear();
		m_nMotionHistorySceneGeneration = rScene.m_nGeneration;
		m_bMotionHistoryNeedsAdvance = false;
		m_nMotionHistoryRenderFrame = nRenderFrame;
		return true;
	}

	bool upload_previous = false;
	const bool reset_history = m_motionHistoryRecords.empty() || m_motionHistoryCells.empty();
	const bool scene_changed = rScene.m_nGeneration != m_nMotionHistorySceneGeneration;
	m_motionPreviousRecords.resize( rScene.m_instances.size() );
	std::memcpy( m_motionPreviousRecords.data(),
			rScene.m_instances.data(),
			rScene.m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
	if ( reset_history ) {
		upload_previous = true;
		++m_nMotionHistoryResets;
		m_bMotionHistoryNeedsAdvance = false;
	} else if ( scene_changed ) {
		m_motionCurrentOrder.resize( rScene.m_cells.size() );
		m_motionPreviousOrder.resize( m_motionHistoryCells.size() );
		for ( uint32_t i = 0u; i < (uint32_t)( m_motionCurrentOrder.size() ); ++i ) {
			m_motionCurrentOrder[i] = i;
		}
		for ( uint32_t i = 0u; i < (uint32_t)( m_motionPreviousOrder.size() ); ++i ) {
			m_motionPreviousOrder[i] = i;
		}
		std::sort( m_motionCurrentOrder.begin(),
				m_motionCurrentOrder.end(),
				[&rScene]( const uint32_t a, const uint32_t b ) {
					return t3dRdMotionCellCompare( rScene.m_cells[a], rScene.m_cells[b] ) < 0;
				} );
		std::sort( m_motionPreviousOrder.begin(),
				m_motionPreviousOrder.end(),
				[this]( const uint32_t a, const uint32_t b ) {
					return t3dRdMotionCellCompare( m_motionHistoryCells[a], m_motionHistoryCells[b] ) < 0;
				} );

		size_t current = 0u;
		size_t previous = 0u;
		while ( current < m_motionCurrentOrder.size() && previous < m_motionPreviousOrder.size() ) {
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
			const uint64_t current_end =
					(uint64_t)( current_cell.m_nFirstInstance ) + current_cell.m_nInstanceCount;
			const uint64_t previous_end =
					(uint64_t)( previous_cell.m_nFirstInstance ) + previous_cell.m_nInstanceCount;
			if ( current_end <= rScene.m_instances.size() && previous_end <= m_motionHistoryRecords.size() &&
					t3dRdMotionCellsMatch( current_cell, previous_cell ) ) {
				std::memcpy( m_motionPreviousRecords.data() + current_cell.m_nFirstInstance,
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
		const uint32_t bytes =
				(uint32_t)( m_motionPreviousRecords.size() * sizeof( t3dRdInstanceRecord_t ) );
		if ( bytes > m_nPreviousInstanceCapacityBytes || m_previousInstanceBytes.size() < bytes ) {
			setError( "RD motion history exceeded publication-time capacity" );
			return false;
		}
		std::memcpy( m_previousInstanceBytes.ptrw(), m_motionPreviousRecords.data(), bytes );
		if ( m_context.getDevice()->buffer_update(
				m_hPreviousInstanceBuffer, 0u, bytes, m_previousInstanceBytes ) != OK ) {
			return false;
		}
		m_context.recordBufferUpdate( bytes );
	}

	if ( reset_history || scene_changed ) {
		m_motionHistoryRecords.resize( rScene.m_instances.size() );
		std::memcpy( m_motionHistoryRecords.data(),
				rScene.m_instances.data(),
				rScene.m_instances.size() * sizeof( t3dRdInstanceRecord_t ) );
		m_motionHistoryCells.resize( rScene.m_cells.size() );
		if ( !rScene.m_cells.empty() ) {
			std::memcpy( m_motionHistoryCells.data(),
					rScene.m_cells.data(),
					rScene.m_cells.size() * sizeof( t3dRdInstanceCell_t ) );
		}
		m_nMotionHistorySceneGeneration = rScene.m_nGeneration;
	}
	m_nMotionHistoryRenderFrame = nRenderFrame;
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadMaterials( void )
{
	if ( m_nUploadedMeshGeneration == m_meshCache.getSourceGeneration() ) {
		return true;
	}
	const std::vector<t3dRdInstanceMaterial_t>& materials = m_meshCache.getMaterials();
	const uint32_t bytes = (uint32_t)( materials.size() * sizeof( t3dRdInstanceMaterial_t ) );
	m_context.beginResourceRebuild();
	const bool buffers_ok = ensureBuffers( m_nInstanceCapacityBytes,
									m_nVisibleCapacityBytes,
									MAX( bytes, (uint32_t)( sizeof( t3dRdInstanceMaterial_t ) ) ),
									m_nIndirectCapacityBytes ) &&
			ensureUniformSet();
	m_context.endResourceRebuild();
	if ( !buffers_ok ) {
		return false;
	}
	if ( bytes > 0u ) {
		if ( m_materialBytes.size() < (int64_t)( m_nMaterialCapacityBytes ) ) {
			m_materialBytes.resize( m_nMaterialCapacityBytes );
		}
		std::memcpy( m_materialBytes.ptrw(), materials.data(), bytes );
		if ( m_context.getDevice()->buffer_update( m_hMaterialBuffer, 0u, bytes, m_materialBytes ) != OK ) {
			return false;
		}
		m_context.recordBufferUpdate( bytes );
		m_nDirtyUploadBytes += bytes;
	}
	m_nUploadedMeshGeneration = m_meshCache.getSourceGeneration();
	return rebuildMaterialBindings();
}

template <bool t_bShadowPass> bool Terrain3DRdInstanceRenderer::buildPacketsForPass(
		const t3dRdInstanceSceneSnapshot_t &rScene,
		const Projection *const pViewProjections,
		const uint32_t nViewCount,
		const Vector3 &rCamera,
		const uint32_t nVisibilityMask,
		const bool bSkipFrustum )
{
	t3dRdInstanceFrustum_t frustums[s_nRdForwardMaxViews];
	for ( uint32_t view = 0u; view < nViewCount; ++view ) {
		frustums[view] = t3dRdInstanceExtractFrustum( pViewProjections[view] );
	}

	m_packetIndex.clear();
	m_packetBuildSlots.clear();
	m_packetBuildEntries.clear();
	m_alphaPackets.clear();
	if constexpr ( t_bShadowPass ) {
		m_shadowVisibleIndices.clear();
		m_shadowPackets.clear();
	} else {
		m_visibleIndices.clear();
		m_drawPackets.clear();
		m_depthPackets.clear();
		m_hlodDescriptors.clear();
	}
	uint32_t unique_key = 1u;

	for ( const t3dRdInstanceCell_t &cell : rScene.m_cells ) {
		const bool hlod = ( cell.m_nFlags & T3D_RD_INSTANCE_HLOD ) != 0u;
		if constexpr ( t_bShadowPass ) {
			if ( ( cell.m_nVisibilityLayers & nVisibilityMask ) == 0u ||
					( cell.m_nFlags & T3D_RD_INSTANCE_CAST_SHADOWS ) == 0u ) {
				continue;
			}
		} else if ( !hlod && ( cell.m_nVisibilityLayers & nVisibilityMask ) == 0u ) {
			continue;
		}
		if ( (uint64_t)( cell.m_nFirstInstance ) + cell.m_nInstanceCount > rScene.m_instances.size() ) {
			setError( "RD instance cell range exceeds published instance table" );
			return false;
		}

		const t3dRdMeshHandle_t mesh = m_meshCache.getMeshHandleBySourceKey( cell.m_nMeshSourceKey != 0u
						? cell.m_nMeshSourceKey
						: t3dRdRegularMeshSourceKey( cell.m_nMeshAssetId, cell.m_nLod ) );
		const t3dRdMeshSourceLod_t *const pLod = m_meshCache.resolveMesh( mesh );
		if ( !pLod ) {
			continue;
		}

		bool visible = false;
		if ( bSkipFrustum ) {
			visible = cell.m_nInstanceCount > 0u;
			if constexpr ( !t_bShadowPass ) {
				visible = visible && ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u;
			}
		} else if constexpr ( !t_bShadowPass ) {
			if ( hlod && nViewCount == 1u ) {
				visible = cell.m_nInstanceCount > 0u &&
						( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u;
			} else {
				for ( uint32_t view = 0u; view < nViewCount && !visible; ++view ) {
					visible = hlod
							? ( cell.m_nFlags & T3D_RD_INSTANCE_SHADOWS_ONLY ) == 0u &&
									t3dRdInstanceCellVisible( cell, frustums[view], rCamera, true )
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
			if ( cell.m_fFadeMargin > 0.0f ) {
				const float distance_sq = (float)( cell.m_bounds.get_center().distance_squared_to( rCamera ) );
				const float begin_fade_end = cell.m_fLodBegin + cell.m_fFadeMargin;
				const float end_fade_begin = MAX( 0.0f, cell.m_fLodEnd - cell.m_fFadeMargin );
				const bool near_begin = cell.m_fLodBegin > 0.001f && distance_sq < begin_fade_end * begin_fade_end;
				const bool near_end = cell.m_fLodEnd > 0.001f && distance_sq > end_fade_begin * end_fade_begin;
				if ( near_begin || near_end ) {
					const float distance = std::sqrt( distance_sq );
					float fade = 1.0f;
					if ( near_begin ) {
						fade = MIN( fade, ( distance - cell.m_fLodBegin ) / cell.m_fFadeMargin );
					}
					if ( near_end ) {
						fade = MIN( fade, ( cell.m_fLodEnd - distance ) / cell.m_fFadeMargin );
					}
					const uint32_t fade_unorm =
							(uint32_t)( std::lround( CLAMP( fade, 0.0f, 1.0f ) * 65535.0f ) );
					cell_draw_flags |= T3D_RD_INSTANCE_FADE_ACTIVE |
							( fade_unorm << T3D_RD_INSTANCE_FADE_SHIFT ) & T3D_RD_INSTANCE_FADE_MASK;
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
			if ( const uint32_t *const pExisting = m_packetIndex.find( key ) ) {
				packet_index = *pExisting;
			} else {
				if ( m_packetBuildSlots.size() >= m_nPacketCapacity ) {
					setError( "RD instance packet count exceeded publication-time capacity" );
					return false;
				}
				packet_index = (uint32_t)( m_packetBuildSlots.size() );
				if ( !m_packetIndex.insertOrAssign( key, packet_index ) ) {
					setError( "RD instance packet hash overflowed sealed capacity" );
					return false;
				}
				t3dRdPacketBuildSlot_t slot;
				slot.m_packet.m_nSurfaceSlot = handle.m_nSlot;
				slot.m_packet.m_nMaterial = pSurface->m_nMaterial;
				slot.m_packet.m_nOverlayMaterial = pSurface->m_nOverlayMaterial;
				slot.m_packet.m_nFlags = draw_flags;
				slot.m_packet.m_fSortDistanceSq =
						(float)( cell.m_bounds.get_center().distance_squared_to( rCamera ) );
				slot.m_packet.m_indexFormat =
						(RenderingDevice::IndexBufferFormat)( pSurface->m_nIndexFormat );
				slot.m_packet.m_cullBounds = cell.m_bounds;
				slot.m_packet.m_nVisibilityLayers = cell.m_nVisibilityLayers;
				m_packetBuildSlots.push_back( slot );
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
			m_packetBuildEntries.push_back(
					t3dRdPacketBuildEntry_t{ packet_index, cell.m_nFirstInstance, cell.m_nInstanceCount } );
		}
	}

	uint64_t total_visible64 = 0u;
	for ( t3dRdPacketBuildSlot_t &slot : m_packetBuildSlots ) {
		slot.m_packet.m_nFirstVisible = (uint32_t)( total_visible64 );
		slot.m_packet.m_nVisibleCount = slot.m_nVisibleCount;
		slot.m_nWriteOffset = slot.m_packet.m_nFirstVisible;
		total_visible64 += slot.m_nVisibleCount;
	}
	if ( total_visible64 > m_nVisibleIndexCapacity ) {
		setError( "RD instance visible-index output exceeded publication-time capacity" );
		return false;
	}
	std::vector<uint32_t> &visible_indices = t_bShadowPass ? m_shadowVisibleIndices : m_visibleIndices;
	visible_indices.resize( (size_t)( total_visible64 ) );
	for ( const t3dRdPacketBuildEntry_t &entry : m_packetBuildEntries ) {
		t3dRdPacketBuildSlot_t &slot = m_packetBuildSlots[entry.m_nPacketIndex];
		uint32_t write = slot.m_nWriteOffset;
		for ( uint32_t instance = 0u; instance < entry.m_nInstanceCount; ++instance ) {
			visible_indices[write++] = entry.m_nFirstInstance + instance;
		}
		slot.m_nWriteOffset = write;
	}

	for ( t3dRdPacketBuildSlot_t &slot : m_packetBuildSlots ) {
		const t3dRdMeshSourceSurface_t *const pSurface = m_meshCache.resolveSurface(
				t3dRdSurfaceHandle_t{ slot.m_packet.m_nSurfaceSlot, m_meshCache.getGeneration() } );
		if ( !pSurface ) {
			return false;
		}
		slot.m_packet.m_command.m_nIndexCount = pSurface->m_nIndexCount;
		slot.m_packet.m_command.m_nInstanceCount = slot.m_packet.m_nVisibleCount;
		slot.m_packet.m_command.m_nFirstIndex = pSurface->m_nIndexOffset;
		slot.m_packet.m_command.m_nVertexOffset = (int32_t)( pSurface->m_nVertexOffset );
		slot.m_packet.m_command.m_nFirstInstance = slot.m_packet.m_nFirstVisible;
		if constexpr ( t_bShadowPass ) {
			m_shadowPackets.push_back( slot.m_packet );
		} else {
			const bool alpha = ( slot.m_packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u;
			if ( alpha ) {
				m_alphaPackets.push_back( slot.m_packet );
			} else {
				m_drawPackets.push_back( slot.m_packet );
			}
			if ( !alpha || ( slot.m_packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) != 0u ) {
				m_depthPackets.push_back( slot.m_packet );
			}
		}
	}

	if constexpr ( !t_bShadowPass ) {
		t3dRdStableSortAlphaPackets( m_alphaPackets, m_alphaSortScratch );
		if ( m_drawPackets.size() + m_alphaPackets.size() > m_nPacketCapacity ) {
			setError( "RD instance draw packet output exceeded publication-time capacity" );
			return false;
		}
		m_drawPackets.insert( m_drawPackets.end(), m_alphaPackets.begin(), m_alphaPackets.end() );
		for ( uint32_t command_index = 0u; command_index < (uint32_t)( m_drawPackets.size() ); ++command_index ) {
			const t3dRdInstanceDrawPacket_t &packet = m_drawPackets[command_index];
			if ( nViewCount != 1u || ( packet.m_nFlags & T3D_RD_INSTANCE_HLOD ) == 0u ) {
				continue;
			}
			t3dRdHlodCullDescriptor_t descriptor;
			const Vector3 center = packet.m_cullBounds.get_center();
			const Vector3 extent = packet.m_cullBounds.size * 0.5f;
			descriptor.m_szBoundsCenter[0] = (float)( center.x );
			descriptor.m_szBoundsCenter[1] = (float)( center.y );
			descriptor.m_szBoundsCenter[2] = (float)( center.z );
			descriptor.m_szBoundsExtent[0] = (float)( extent.x );
			descriptor.m_szBoundsExtent[1] = (float)( extent.y );
			descriptor.m_szBoundsExtent[2] = (float)( extent.z );
			descriptor.m_nVisibilityLayers = packet.m_nVisibilityLayers;
			descriptor.m_nCommandIndex = command_index;
			descriptor.m_nFirstVisible = packet.m_nFirstVisible;
			m_hlodDescriptors.push_back( descriptor );
		}
	}
	m_nHlodCullDescriptors = m_hlodDescriptors.size();
	m_nVisibleInstances = visible_indices.size();
	return true;
}

bool Terrain3DRdInstanceRenderer::buildPackets( const t3dRdInstanceSceneSnapshot_t &rScene,
		const Projection *const pViewProjections,
		const uint32_t nViewCount,
		const Vector3 &rCamera,
		const uint32_t nVisibilityMask,
		const bool bShadowPass,
		const bool bSkipFrustum )
{
	if ( !pViewProjections || nViewCount == 0u || nViewCount > s_nRdForwardMaxViews ) {
		return false;
	}
	return bShadowPass ? buildPacketsForPass<true>(
			rScene, pViewProjections, nViewCount, rCamera, nVisibilityMask, bSkipFrustum )
						 : buildPacketsForPass<false>(
								rScene, pViewProjections, nViewCount, rCamera, nVisibilityMask, bSkipFrustum );
}

bool Terrain3DRdInstanceRenderer::uploadVisibility( void )
{
	const uint64_t visible_bytes64 = m_visibleIndices.size() * sizeof( uint32_t );
	const uint64_t indirect_bytes64 = m_drawPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	if ( visible_bytes64 > m_nVisibleCapacityBytes || indirect_bytes64 > m_nIndirectCapacityBytes ) {
		setError( "RD instance visibility output exceeded publication-time capacity" );
		return false;
	}
	if ( !ensureUniformSet() ) {
		return false;
	}
	const uint32_t visible_bytes = (uint32_t)( visible_bytes64 );
	const uint32_t indirect_bytes = (uint32_t)( indirect_bytes64 );
	RenderingDevice *const rd = m_context.getDevice();
	if ( visible_bytes > 0u ) {
		std::memcpy( m_visibleBytes.ptrw(), m_visibleIndices.data(), visible_bytes );
		if ( rd->buffer_update( m_hVisibleIndexBuffer, 0u, visible_bytes, m_visibleBytes ) != OK ) {
			return false;
		}
	}
	if ( indirect_bytes > 0u ) {
		for ( uint32_t i = 0u; i < (uint32_t)( m_drawPackets.size() ); ++i ) {
			std::memcpy( m_indirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
					&m_drawPackets[i].m_command,
					sizeof( t3dRdIndexedIndirectCommand_t ) );
		}
		if ( rd->buffer_update( m_hIndirectBuffer, 0u, indirect_bytes, m_indirectBytes ) != OK ) {
			return false;
		}
	}
	m_context.recordBufferUpdate( visible_bytes + indirect_bytes );
	return uploadHlodDescriptors();
}

bool Terrain3DRdInstanceRenderer::uploadShadowVisibility( void )
{
	const uint64_t visible_bytes64 = m_shadowVisibleIndices.size() * sizeof( uint32_t );
	const uint64_t indirect_bytes64 = m_shadowPackets.size() * sizeof( t3dRdIndexedIndirectCommand_t );
	if ( visible_bytes64 > m_nShadowVisibleCapacityBytes || indirect_bytes64 > m_nShadowIndirectCapacityBytes ||
			!ensureShadowUniformSet() ) {
		return false;
	}
	const uint32_t visible_bytes = (uint32_t)( visible_bytes64 ), indirect_bytes = (uint32_t)( indirect_bytes64 );
	RenderingDevice *const rd = m_context.getDevice();
	if ( visible_bytes > 0u ) {
		std::memcpy( m_shadowVisibleBytes.ptrw(), m_shadowVisibleIndices.data(), visible_bytes );
		if ( rd->buffer_update( m_hShadowVisibleIndexBuffer, 0u, visible_bytes, m_shadowVisibleBytes ) != OK ) {
			return false;
		}
	}
	if ( indirect_bytes > 0u ) {
		for ( uint32_t i = 0u; i < (uint32_t)m_shadowPackets.size(); ++i ) {
			std::memcpy( m_shadowIndirectBytes.ptrw() + i * sizeof( t3dRdIndexedIndirectCommand_t ),
					&m_shadowPackets[i].m_command,
					sizeof( t3dRdIndexedIndirectCommand_t ) );
		}
		if ( rd->buffer_update( m_hShadowIndirectBuffer, 0u, indirect_bytes, m_shadowIndirectBytes ) != OK ) {
			return false;
		}
	}
	m_context.recordBufferUpdate( visible_bytes + indirect_bytes );
	return true;
}

bool Terrain3DRdInstanceRenderer::uploadHlodDescriptors( void )
{
	if ( m_hlodDescriptors.empty() ) {
		return true;
	}
	const uint64_t bytes64 = m_hlodDescriptors.size() * sizeof( t3dRdHlodCullDescriptor_t );
	if ( bytes64 > m_nHlodDescriptorCapacityBytes || !m_hHlodCullDescriptorBuffer.is_valid() ||
			m_hlodDescriptorBytes.size() < (int64_t)( bytes64 ) ) {
		setError( "RD HLOD descriptor output exceeded publication-time capacity" );
		return false;
	}
	const uint32_t bytes = (uint32_t)( bytes64 );
	std::memcpy( m_hlodDescriptorBytes.ptrw(), m_hlodDescriptors.data(), bytes );
	if ( m_context.getDevice()->buffer_update( m_hHlodCullDescriptorBuffer, 0u, bytes, m_hlodDescriptorBytes ) != OK ) {
		return false;
	}
	m_context.recordBufferUpdate( bytes );
	if ( !m_hHlodCullUniformSet.is_valid() ) {
		TypedArray<Ref<RDUniform>> uniforms;
		uniforms.push_back(
				t3dRdInstanceBufferUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
		uniforms.push_back( t3dRdInstanceBufferUniform(
				1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hHlodCullDescriptorBuffer ) );
		uniforms.push_back(
				t3dRdInstanceBufferUniform( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_hIndirectBuffer ) );
		m_hHlodCullUniformSet = m_context.getDevice()->uniform_set_create( uniforms, m_hHlodCullShader, 0u );
		if ( !m_hHlodCullUniformSet.is_valid() ||
				!m_context.registerResource( m_hHlodCullUniformSet,
						"T3D.HLOD.CullUniformSet",
						Terrain3DRdContext::RESOURCE_UNIFORM_SET,
						Terrain3DRdContext::MEMORY_NONE,
						0u ) ) {
			setError( "Failed to create Terrain3D HLOD cull uniform set" );
			return false;
		}
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::dispatchHlodCull( const uint32_t nVisibilityMask )
{
	if ( m_hlodDescriptors.empty() ) {
		return true;
	}
	if ( !m_hHlodCullPipeline.is_valid() || !m_hHlodCullUniformSet.is_valid() ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdHlodCullPush_t push;
	push.m_nDescriptorCount = (uint32_t)( m_hlodDescriptors.size() );
	push.m_nVisibilityMask = nVisibilityMask;
	push.m_nCommandCount = (uint32_t)( m_drawPackets.size() );
	std::memcpy( m_hlodCullPushBytes.ptrw(), &push, sizeof( push ) );
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

void Terrain3DRdInstanceRenderer::freeViewState( t3dRdInstanceViewState_t& rState )
{
	for ( RID& pipeline : rState.m_szPipeline ) {
		m_context.freeResource( pipeline );
	}
	m_context.freeResource( rState.m_hFramebuffer );
	rState = t3dRdInstanceViewState_t{};
}

bool Terrain3DRdInstanceRenderer::ensureViewState( const uint32_t nView,
		const RID& rColor,
		const RID& rDepth,
		const RenderingDevice::TextureSamples samples )
{
	if ( nView >= s_nMaxViews ) {
		return false;
	}
	t3dRdInstanceViewState_t& state = m_szViewStates[nView];
	bool pipelines_valid = true;
	for ( const RID& pipeline : state.m_szPipeline ) {
		pipelines_valid = pipelines_valid && pipeline.is_valid();
	}
	if ( state.m_hFramebuffer.is_valid() && pipelines_valid && state.m_hColor == rColor && state.m_hDepth == rDepth &&
			state.m_samples == samples ) {
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
	if ( !state.m_hFramebuffer.is_valid() ||
			!m_context.registerResource( state.m_hFramebuffer,
					framebuffer_name,
					Terrain3DRdContext::RESOURCE_FRAMEBUFFER,
					Terrain3DRdContext::MEMORY_NONE,
					0u ) ) {
		m_context.endResourceRebuild();
		return false;
	}
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( samples );
	const RenderingDevice::PolygonCullMode cull_modes[3] = {
		RenderingDevice::POLYGON_CULL_BACK,
		RenderingDevice::POLYGON_CULL_FRONT,
		RenderingDevice::POLYGON_CULL_DISABLED,
	};
	for ( uint32_t pipeline_index = 0u; pipeline_index < 9u; ++pipeline_index ) {
		const uint32_t cull_index = pipeline_index % 3u;
		const uint32_t opacity_mode = pipeline_index / 3u;
		Ref<RDPipelineRasterizationState> raster;
		raster.instantiate();
		raster->set_cull_mode( cull_modes[cull_index] );
		raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE );

		Ref<RDPipelineDepthStencilState> depth;
		depth.instantiate();
		depth->set_enable_depth_test( true );
		depth->set_enable_depth_write( opacity_mode != 1u );
		depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_LESS_OR_EQUAL );

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
		const String pipeline_name = String( "T3D.Instance.Pipeline." ) + String::num_int64( nView ) + "." +
				String::num_int64( pipeline_index );
		if ( !state.m_szPipeline[pipeline_index].is_valid() ||
				!m_context.registerResource( state.m_szPipeline[pipeline_index],
						pipeline_name,
						Terrain3DRdContext::RESOURCE_RASTER_PIPELINE,
						Terrain3DRdContext::MEMORY_NONE,
						0u ) ) {
			freeViewState( state );
			m_context.endResourceRebuild();
			return false;
		}
	}
	state.m_hColor = rColor;
	state.m_hDepth = rDepth;
	state.m_samples = samples;
	m_context.endResourceRebuild();
	return true;
}

bool Terrain3DRdInstanceRenderer::drawView( const uint32_t nView,
		const Projection& rViewProjection,
		const Vector3& rCamera,
		const uint32_t nVisibilityMask )
{
	if ( nView >= s_nMaxViews ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	t3dRdInstanceViewState_t& state = m_szViewStates[nView];
	t3dRdInstanceFrame_t frame{};
	for ( uint32_t view_index = 0u; view_index < s_nRdForwardMaxViews; ++view_index ) {
		for ( int column = 0; column < 4; ++column ) {
			const Vector4 c = rViewProjection[column];
			frame.m_szViewProjection[view_index][column * 4 + 0] = (float)( c.x );
			frame.m_szViewProjection[view_index][column * 4 + 1] = (float)( c.y );
			frame.m_szViewProjection[view_index][column * 4 + 2] = (float)( c.z );
			frame.m_szViewProjection[view_index][column * 4 + 3] = (float)( c.w );
		}
		frame.m_szCamera[view_index][0] = (float)( rCamera.x );
		frame.m_szCamera[view_index][1] = (float)( rCamera.y );
		frame.m_szCamera[view_index][2] = (float)( rCamera.z );
	}
	std::memcpy( m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	if ( rd->buffer_update( m_hFrameBuffer, 0u, sizeof( frame ), m_frameBytes ) != OK ) {
		return false;
	}
	m_context.recordBufferUpdate( sizeof( frame ) );
	if ( !dispatchHlodCull( nVisibilityMask ) ) {
		return false;
	}
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
	rd->draw_list_bind_vertex_array( list, m_meshCache.getVertexArray() );
	auto draw_packet = [&]( const uint32_t i, const bool depth_prepass ) -> bool {
		const t3dRdInstanceDrawPacket_t& packet = m_drawPackets[i];
		const RID index_array = m_meshCache.getIndexArray( packet.m_indexFormat );
		if ( !index_array.is_valid() || packet.m_nVisibleCount == 0u ) {
			return true;
		}
		uint32_t pipeline_index = 0u;
		if ( ( packet.m_nFlags & ( T3D_RD_INSTANCE_DOUBLE_SIDED | T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED ) ) != 0u ) {
			pipeline_index = 2u;
		} else if ( ( packet.m_nFlags & T3D_RD_INSTANCE_CULL_FRONT ) != 0u ) {
			pipeline_index = 1u;
		}
		if ( depth_prepass ) {
			pipeline_index += 6u;
		} else if ( ( packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u ) {
			pipeline_index += 3u;
		}
		if ( pipeline_index != bound_pipeline ) {
			rd->draw_list_bind_render_pipeline( list, state.m_szPipeline[pipeline_index] );
			bound_pipeline = pipeline_index;
		}
		rd->draw_list_bind_index_array( list, index_array );
		const RID material_set = getMaterialBinding( packet.m_nMaterial, packet.m_nOverlayMaterial );
		if ( !material_set.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_uniform_set( list, material_set, 1u );
		const uint32_t draw_flags =
				depth_prepass ? packet.m_nFlags | T3D_RD_INSTANCE_DEPTH_PREPASS_PASS : packet.m_nFlags;
		t3dRdInstancePush_t push{ packet.m_nMaterial,
			packet.m_nOverlayMaterial,
			m_bDirectDrawDebug ? packet.m_nFirstVisible : 0u,
			draw_flags,
			packet.m_nVisibilityLayers,
			0u,
			0u,
			0u };
		std::memcpy( m_pushBytes.ptrw(), &push, sizeof( push ) );
		rd->draw_list_set_push_constant( list, m_pushBytes, sizeof( push ) );
		if ( m_bDirectDrawDebug ) {
			rd->draw_list_draw( list, true, packet.m_nVisibleCount, 0u );
		} else {
			rd->draw_list_draw_indirect( list,
					true,
					m_hIndirectBuffer,
					i * sizeof( t3dRdIndexedIndirectCommand_t ),
					1u,
					sizeof( t3dRdIndexedIndirectCommand_t ) );
		}
		++m_nDraws;
		return true;
	};

	for ( uint32_t i = 0u; i < (uint32_t)( m_drawPackets.size() ); ++i ) {
		if ( ( m_drawPackets[i].m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) != 0u && !draw_packet( i, true ) ) {
			rd->draw_list_end();
			return false;
		}
	}
	bound_pipeline = UINT32_MAX;
	for ( uint32_t i = 0u; i < (uint32_t)( m_drawPackets.size() ); ++i ) {
		if ( !draw_packet( i, false ) ) {
			rd->draw_list_end();
			return false;
		}
	}
	rd->draw_list_end();
	return true;
}

void Terrain3DRdInstanceRenderer::freePreviewTargets( void )
{
	if ( m_szViewStates[0].m_hColor == m_hPreviewColorTexture ||
			m_szViewStates[0].m_hDepth == m_hPreviewDepthTexture ) {
		freeViewState( m_szViewStates[0] );
	}
	const uint64_t color_bytes = (uint64_t)( MAX( m_previewSize.x, 0 ) ) * (uint64_t)( MAX( m_previewSize.y, 0 ) ) * 4u;
	const uint64_t depth_bytes = (uint64_t)( MAX( m_previewSize.x, 0 ) ) * (uint64_t)( MAX( m_previewSize.y, 0 ) ) * 4u;
	m_context.freeResource( m_hPreviewColorTexture, color_bytes );
	m_context.freeResource( m_hPreviewDepthTexture, depth_bytes );
	m_hPreviewColorTexture = RID();
	m_hPreviewDepthTexture = RID();
	m_previewSize = Vector2i();
}

bool Terrain3DRdInstanceRenderer::ensurePreviewTargets( const Vector2i& rSize )
{
	if ( rSize.x < 2 || rSize.y < 2 ) {
		return false;
	}
	if ( m_hPreviewColorTexture.is_valid() && m_hPreviewDepthTexture.is_valid() && m_previewSize == rSize ) {
		return true;
	}
	freePreviewTargets();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	m_context.beginResourceRebuild();
	Ref<RDTextureView> view;
	view.instantiate();
	Ref<RDTextureFormat> color_format;
	color_format.instantiate();
	color_format->set_width( (uint32_t)( rSize.x ) );
	color_format->set_height( (uint32_t)( rSize.y ) );
	color_format->set_depth( 1u );
	color_format->set_array_layers( 1u );
	color_format->set_mipmaps( 1u );
	color_format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
	color_format->set_format( RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM );
	color_format->set_usage_bits(
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT );
	m_hPreviewColorTexture = rd->texture_create( color_format, view );
	const uint64_t bytes = (uint64_t)( rSize.x ) * (uint64_t)( rSize.y ) * 4u;
	if ( !m_hPreviewColorTexture.is_valid() ||
			!m_context.registerResource( m_hPreviewColorTexture,
					"T3D.EditorPreview.Color",
					Terrain3DRdContext::RESOURCE_TEXTURE,
					Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
					bytes ) ) {
		m_context.endResourceRebuild();
		freePreviewTargets();
		return false;
	}
	Ref<RDTextureFormat> depth_format;
	depth_format.instantiate();
	depth_format->set_width( (uint32_t)( rSize.x ) );
	depth_format->set_height( (uint32_t)( rSize.y ) );
	depth_format->set_depth( 1u );
	depth_format->set_array_layers( 1u );
	depth_format->set_mipmaps( 1u );
	depth_format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
	depth_format->set_format( RenderingDevice::DATA_FORMAT_D32_SFLOAT );
	depth_format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT );
	m_hPreviewDepthTexture = rd->texture_create( depth_format, view );
	if ( !m_hPreviewDepthTexture.is_valid() ||
			!m_context.registerResource( m_hPreviewDepthTexture,
					"T3D.EditorPreview.Depth",
					Terrain3DRdContext::RESOURCE_TEXTURE,
					Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
					bytes ) ) {
		m_context.endResourceRebuild();
		freePreviewTargets();
		return false;
	}
	m_previewSize = rSize;
	m_context.endResourceRebuild();
	return true;
}

bool Terrain3DRdInstanceRenderer::initialize( const Ref<RDShaderSPIRV>& rShader,
		const Ref<RDShaderSPIRV>& rHlodCullShader,
		const t3dRdForwardShaderBundle_t& rForwardShaders )
{
	if ( !ensureDevice() || !ensureShader( rShader ) || !ensureForwardShaders( rForwardShaders ) ||
			!ensureHlodCullShader( rHlodCullShader ) ) {
		return false;
	}
	m_frameBytes.resize( sizeof( t3dRdInstanceFrame_t ) );
	m_pushBytes.resize( sizeof( t3dRdInstancePush_t ) );
	m_hlodCullPushBytes.resize( sizeof( t3dRdHlodCullPush_t ) );
	return true;
}

void Terrain3DRdInstanceRenderer::shutdown( void )
{
	for ( t3dRdInstanceViewState_t& state : m_szViewStates ) {
		freeViewState( state );
	}
	freePreviewTargets();
	m_forwardLighting.shutdown();
	freeForwardPipelines();
	freeMaterialBindings();
	m_meshCache.shutdown();
	for ( RID& sampler : m_hMaterialSamplers ) {
		m_context.freeResource( sampler );
	}
	m_context.freeResource( m_hFallbackAlbedoTexture, 4u );
	m_context.freeResource( m_hFallbackNormalTexture, 4u );
	m_context.freeResource( m_hFallbackOrmTexture, 4u );
	m_context.freeResource( m_hUniformSet );
	m_context.freeResource( m_hFrameBuffer, sizeof( t3dRdInstanceFrame_t ) );
	m_context.freeResource( m_hInstanceBuffer, m_nInstanceCapacityBytes );
	m_context.freeResource( m_hPreviousInstanceBuffer, m_nPreviousInstanceCapacityBytes );
	m_context.freeResource( m_hVisibleIndexBuffer, m_nVisibleCapacityBytes );
	m_context.freeResource( m_hShadowVisibleIndexBuffer, m_nShadowVisibleCapacityBytes );
	m_context.freeResource( m_hShadowUniformSet );
	m_context.freeResource( m_hMaterialBuffer, m_nMaterialCapacityBytes );
	m_context.freeResource( m_hIndirectBuffer, m_nIndirectCapacityBytes );
	m_context.freeResource( m_hShadowIndirectBuffer, m_nShadowIndirectCapacityBytes );
	m_context.freeResource( m_hHlodCullUniformSet );
	m_context.freeResource( m_hHlodCullDescriptorBuffer, m_nHlodDescriptorCapacityBytes );
	m_context.freeResource( m_hHlodCullPipeline );
	m_context.freeResource( m_hHlodCullShader );
	for ( uint32_t variant = 0u; variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++variant ) {
		m_context.freeResource( m_hszForwardShaders[variant] );
	}
	m_context.freeResource( m_hShader );
	for ( RID& shader : m_hszForwardShaders ) {
		shader = RID();
	}
	m_nInstanceCapacityBytes = m_nPreviousInstanceCapacityBytes = m_nVisibleCapacityBytes =
			m_nShadowVisibleCapacityBytes = m_nMaterialCapacityBytes = m_nIndirectCapacityBytes =
					m_nShadowIndirectCapacityBytes = 0u;
	m_nHlodDescriptorCapacityBytes = 0u;
	m_nUploadedSceneGeneration = m_nUploadedMeshGeneration = 0u;
	m_nSceneCapacityMeshGeneration = 0u;
	m_visibleIndices.clear();
	m_motionHistoryRecords.clear();
	m_motionHistoryCells.clear();
	m_motionPreviousRecords.clear();
	m_previousInstanceBytes.clear();
	m_drawPackets.clear();
	m_depthPackets.clear();
	m_shadowPackets.clear();
	m_hlodDescriptors.clear();
	m_nStaticInstances = 0u;
	m_nVisibleInstances = 0u;
	m_nPreviewSceneGeneration = 0u;
	m_nHlodCullDescriptors = 0u;
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
}

bool Terrain3DRdInstanceRenderer::render( RenderData *const pRenderData,
		const t3dRdMeshSourceSnapshot_t *const pMeshes,
		const t3dRdInstanceSceneSnapshot_t *const pScene,
		const uint32_t nVisibilityMask )
{
	if ( !pRenderData || !pScene ) {
		return true;
	}
	if ( pScene->m_instances.empty() || pScene->m_cells.empty() ) {
		m_nStaticInstances = pScene->m_instances.size();
		m_nVisibleInstances = 0u;
		m_visibleIndices.clear();
		m_drawPackets.clear();
		m_depthPackets.clear();
		m_shadowPackets.clear();
		m_hlodDescriptors.clear();
		m_nHlodCullDescriptors = 0u;
		return true;
	}
	if ( pMeshes && pMeshes->m_nGeneration != m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
		m_context.beginResourceRebuild();
		freeMaterialBindings();
		m_context.endResourceRebuild();
	}
	if ( !pMeshes || !m_meshCache.upload( pMeshes ) || !uploadMaterials() || !uploadScene( pScene ) ) {
		setError( "Failed to upload Terrain3D RD instance scene" );
		return false;
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
		if ( !ensureViewState( view, color, depth, samples ) ||
				!drawView( view, view_projection, eye, nVisibilityMask ) ) {
			return false;
		}
	}
	++m_nFrames;
	m_nStaticInstances = pScene->m_instances.size();
	m_lastError = String();
	return true;
}

bool Terrain3DRdInstanceRenderer::renderPreview( const t3dRdMeshSourceSnapshot_t *const pMeshes,
		const uint16_t nAssetId,
		const Vector2i& rSize,
		Ref<Image>& rImage )
{
	rImage.unref();
	if ( !pMeshes || !ensureDevice() || !ensurePreviewTargets( rSize ) ) {
		setError( "RD editor preview resources are unavailable" );
		return false;
	}
	if ( pMeshes->m_nGeneration != m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
		m_context.beginResourceRebuild();
		freeMaterialBindings();
		m_context.endResourceRebuild();
	}
	if ( !m_meshCache.upload( pMeshes ) || !uploadMaterials() ) {
		setError( "Failed to upload RD editor preview mesh/material data" );
		return false;
	}
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
	float extent = (float)( MAX( rotated.size.x, rotated.size.y ) );
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
	scene.m_instances.push_back( t3dRdPreviewInstanceRecord( xform ) );
	t3dRdInstanceCell_t cell;
	cell.m_nMeshSourceKey = t3dRdRegularMeshSourceKey( nAssetId, 0 );
	cell.m_nMeshAssetId = nAssetId;
	cell.m_nFirstInstance = 0u;
	cell.m_nInstanceCount = 1u;
	cell.m_nVisibilityLayers = 1u;
	cell.m_bounds = xform.xform( source_bounds );
	scene.m_cells.push_back( cell );
	if ( !uploadScene( &scene ) ) {
		setError( "Failed to upload RD editor preview instance" );
		return false;
	}

	const float aspect = (float)( rSize.x ) / (float)( MAX( rSize.y, 1 ) );
	const Projection projection = Projection::create_orthogonal_aspect( 1.1f, aspect, 0.01f, 1000.0f );
	const Transform3D camera( Basis(), Vector3( 0.0f, 0.0f, 3.0f ) );
	const Projection view( camera.affine_inverse() );
	const Projection view_projection = projection * view;
	if ( !buildPackets( scene, &view_projection, 1u, camera.origin, 1u, false, true ) || !uploadVisibility() ||
			!ensureViewState(
					0u, m_hPreviewColorTexture, m_hPreviewDepthTexture, RenderingDevice::TEXTURE_SAMPLES_1 ) ) {
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
	if ( bytes.size() != (int64_t)( rSize.x ) * (int64_t)( rSize.y ) * 4 ) {
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

bool Terrain3DRdInstanceRenderer::prepareForwardPass( const t3dRdForwardPassContext_t& rContext,
		const t3dRdMeshSourceSnapshot_t *const pMeshes,
		const t3dRdInstanceSceneSnapshot_t *const pScene,
		const uint32_t nVisibilityMask,
		const bool bTemporalHistoryValid,
		const bool bViewportReconfigured )
{
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE || !pScene ) {
		return false;
	}
	if ( rContext.m_nViewCount == 0u || rContext.m_nViewCount > s_nRdForwardMaxViews ) {
		setError( "Terrain3D native instance view count exceeds bridge bounds" );
		return false;
	}
	t3dRdForwardShaderVariant_t variant;
	if ( !selectForwardVariant( rContext, variant ) ) {
		return false;
	}
	// Instance cells can be published before any referenced mesh has produced
	// renderable geometry.  This is an empty companion pass, not a terrain-wide
	// renderer failure.
	const bool has_renderable_meshes =
			pMeshes && !pMeshes->m_vertexBytes.is_empty() && !pMeshes->m_lods.empty() && !pMeshes->m_surfaces.empty();
	if ( pScene->m_instances.empty() || pScene->m_cells.empty() || !has_renderable_meshes ) {
		// Motion-history buffers are part of the renderable-mesh lane.  They do
		// not exist while this publication is being treated as an empty pass.
		if ( has_renderable_meshes && rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW &&
				!prepareMotionHistory( *pScene, rContext.m_nRenderFrame ) ) {
			setError( "Failed to reset Terrain3D instance/HLOD previous-transform history" );
			return false;
		}
		m_drawPackets.clear();
		m_depthPackets.clear();
		m_shadowPackets.clear();
		m_visibleIndices.clear();
		m_hlodDescriptors.clear();
		m_nVisibleInstances = 0u;
		m_nStaticInstances = pScene->m_instances.size();
		m_nForwardPreparedScenePass = rContext.m_nScenePass;
		m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
		m_nForwardPreparedSceneGeneration = pScene->m_nGeneration;
		m_bForwardPreparedHasDraws = false;
		m_bLastForwardPipelineKeyValid = false;
		m_lastError = String();
		return true;
	}
	if ( pMeshes && pMeshes->m_nGeneration != m_meshCache.getSourceGeneration() && !m_materialBindings.empty() ) {
		m_context.beginResourceRebuild();
		freeMaterialBindings();
		m_context.endResourceRebuild();
	}
	if ( !pMeshes || !m_meshCache.upload( pMeshes ) || !uploadMaterials() || !uploadScene( pScene ) ) {
		setError( "Failed to prepare Terrain3D native Forward+ instance scene" );
		return false;
	}
	const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	// Shadow passes may arrive before the scene motion pass. They must never advance or
	// reset temporal history shared by the opaque/motion scene submission.
	if ( !shadow_pass && !prepareMotionHistory( *pScene, rContext.m_nRenderFrame ) ) {
		setError( "Failed to prepare Terrain3D instance/HLOD previous-transform history" );
		return false;
	}

	const bool rebuild_packets = shadow_pass || m_nForwardPreparedScenePass != rContext.m_nScenePass ||
			m_nForwardPreparedViewportKey != rContext.m_nViewportKey ||
			m_nForwardPreparedSceneGeneration != pScene->m_nGeneration ||
			m_forwardPreparedPacketPassKind != rContext.m_passKind ||
			m_nForwardPreparedVisibilityMask != nVisibilityMask;
	if ( rebuild_packets ) {
		if ( !buildPackets( *pScene,
					 rContext.m_szViewProjection,
					 rContext.m_nViewCount,
					 rContext.m_cameraTransform.origin,
					 nVisibilityMask,
					 shadow_pass ) ||
				( shadow_pass ? !uploadShadowVisibility()
							  : ( !uploadVisibility() ||
										( rContext.m_nViewCount == 1u && !dispatchHlodCull( nVisibilityMask ) ) ) ) ) {
			setError( "Failed to prepare Terrain3D native Forward+ instance visibility" );
			return false;
		}
		m_nForwardPreparedScenePass = rContext.m_nScenePass;
		m_nForwardPreparedViewportKey = rContext.m_nViewportKey;
		m_nForwardPreparedSceneGeneration = pScene->m_nGeneration;
		m_forwardPreparedPacketPassKind = rContext.m_passKind;
		m_nForwardPreparedVisibilityMask = nVisibilityMask;
	}

	t3dRdInstanceFrame_t frame{};
	const Projection view_matrix = Projection( rContext.m_cameraTransform.affine_inverse() );
	for ( int column = 0; column < 4; ++column ) {
		const Vector4 vm = view_matrix[column];
		frame.m_szView[column * 4 + 0] = (float)( vm.x );
		frame.m_szView[column * 4 + 1] = (float)( vm.y );
		frame.m_szView[column * 4 + 2] = (float)( vm.z );
		frame.m_szView[column * 4 + 3] = (float)( vm.w );
	}
	const bool history_valid = rContext.m_bMotionVectorsEnabled && bTemporalHistoryValid;
	for ( uint32_t view_index = 0u; view_index < s_nRdForwardMaxViews; ++view_index ) {
		const uint32_t source = MIN( view_index, rContext.m_nViewCount - 1u );
		for ( int column = 0; column < 4; ++column ) {
			const Vector4 vp = rContext.m_szViewProjection[source][column];
			frame.m_szViewProjection[view_index][column * 4 + 0] = (float)( vp.x );
			frame.m_szViewProjection[view_index][column * 4 + 1] = (float)( vp.y );
			frame.m_szViewProjection[view_index][column * 4 + 2] = (float)( vp.z );
			frame.m_szViewProjection[view_index][column * 4 + 3] = (float)( vp.w );
			const Vector4 previous_vp = ( history_valid ? rContext.m_szPreviousViewProjection[source]
														: rContext.m_szViewProjection[source] )[column];
			frame.m_szPreviousViewProjection[view_index][column * 4 + 0] = (float)( previous_vp.x );
			frame.m_szPreviousViewProjection[view_index][column * 4 + 1] = (float)( previous_vp.y );
			frame.m_szPreviousViewProjection[view_index][column * 4 + 2] = (float)( previous_vp.z );
			frame.m_szPreviousViewProjection[view_index][column * 4 + 3] = (float)( previous_vp.w );
		}
		Vector3 eye = rContext.m_cameraTransform.origin;
		if ( rContext.m_nViewCount > 1u ) {
			eye += rContext.m_cameraTransform.basis.xform( rContext.m_szEyeOffset[source] );
		}
		frame.m_szCamera[view_index][0] = (float)( eye.x );
		frame.m_szCamera[view_index][1] = (float)( eye.y );
		frame.m_szCamera[view_index][2] = (float)( eye.z );
	}
	frame.m_szMotionJitter[0] = (float)( rContext.m_taaJitter.x );
	frame.m_szMotionJitter[1] = (float)( rContext.m_taaJitter.y );
	frame.m_szMotionJitter[2] = (float)( history_valid ? rContext.m_previousTaaJitter.x : rContext.m_taaJitter.x );
	frame.m_szMotionJitter[3] = (float)( history_valid ? rContext.m_previousTaaJitter.y : rContext.m_taaJitter.y );
	frame.m_szMotionParams[0] = history_valid ? 1.0f : 0.0f;
	frame.m_szMotionParams[1] = m_bMotionDebug ? 1.0f : 0.0f;
	frame.m_szMotionParams[2] = (float)( rContext.m_nViewCount );
	std::memcpy( m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	if ( m_context.getDevice()->buffer_update( m_hFrameBuffer, 0u, sizeof( frame ), m_frameBytes ) != OK ) {
		setError( "Failed to update Terrain3D native Forward+ instance frame buffer" );
		return false;
	}
	m_context.recordBufferUpdate( sizeof( frame ) );
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) {
		if ( bViewportReconfigured ) {
			m_forwardLighting.invalidateBorrowedState();
		}
		if ( !m_forwardLighting.prepare( rContext, nVisibilityMask ) ||
				!m_forwardLighting.ensureSet( m_hszForwardShaders[variant] ).is_valid() ) {
			setError( "Failed to prepare Terrain3D Forward+ instance/HLOD native lighting" );
			return false;
		}
	}
	if ( !ensureForwardPipelines( rContext, variant ) ) {
		return false;
	}
	m_bForwardPreparedHasDraws = shadow_pass ? !m_shadowPackets.empty() : !m_drawPackets.empty();
	m_lastError = String();
	return true;
}

bool Terrain3DRdInstanceRenderer::drawForwardView( const t3dRdForwardPassContext_t& rContext )
{
	if ( !m_bForwardPreparedHasDraws ) {
		return true;
	}
	if ( rContext.m_nDrawList < 0 || !m_bLastForwardPipelineKeyValid ) {
		return false;
	}
	t3dRdForwardShaderVariant_t variant;
	if ( !selectForwardVariant( rContext, variant ) || variant != m_lastForwardPipelineKey.m_variant ||
			rContext.m_nFramebufferFormat != m_lastForwardPipelineKey.m_nFramebufferFormat ||
			rContext.m_nViewCount != m_lastForwardPipelineKey.m_nViewCount ||
			rContext.m_passKind != m_lastForwardPipelineKey.m_passKind ||
			rContext.m_bReverseCull != m_lastForwardPipelineKey.m_bReverseCull ||
			( ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE ||
					  rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) &&
					rContext.m_bDepthPrepassActive ) != m_lastForwardPipelineKey.m_bDepthPrepassActive ) {
		return false;
	}
	const t3dRdInstanceForwardPipelineEntry_t *const entry = findForwardPipeline( m_lastForwardPipelineKey );
	if ( !entry ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const int64_t draw_list = rContext.m_nDrawList;
	const bool shadow_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	const std::vector<t3dRdInstanceDrawPacket_t>& packets = shadow_pass ? m_shadowPackets : m_drawPackets;
	rd->draw_list_bind_uniform_set( draw_list, shadow_pass ? m_hShadowUniformSet : m_hUniformSet, 0u );
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE || rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION ) {
		const RID lighting_set = m_forwardLighting.getSet( m_hszForwardShaders[variant] );
		if ( !lighting_set.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_uniform_set( draw_list, lighting_set, 2u );
	}
	rd->draw_list_bind_vertex_array( draw_list, m_meshCache.getVertexArray() );
	uint32_t bound_pipeline = UINT32_MAX;
	for ( uint32_t i = 0u; i < (uint32_t)( packets.size() ); ++i ) {
		const t3dRdInstanceDrawPacket_t& packet = packets[i];
		const bool alpha_blend = ( packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_BLEND ) != 0u;
		bool depth_prepass = false;
		if ( shadow_pass ) {
			if ( ( packet.m_nVisibilityLayers & rContext.m_nShadowCasterMask ) == 0u ||
					( alpha_blend && ( packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) == 0u ) ) {
				continue;
			}
		} else if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_DEPTH ) {
			if ( alpha_blend && ( packet.m_nFlags & T3D_RD_INSTANCE_ALPHA_DEPTH_PREPASS ) == 0u ) {
				continue;
			}
			depth_prepass = alpha_blend;
		} else if ( alpha_blend ) {
			continue;
		}
		if ( packet.m_nVisibleCount == 0u ) {
			continue;
		}
		const RID index_array = m_meshCache.getIndexArray( packet.m_indexFormat );
		if ( !index_array.is_valid() ) {
			return false;
		}
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
		const RID material_set = getMaterialBinding( packet.m_nMaterial, packet.m_nOverlayMaterial );
		if ( !material_set.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_uniform_set( draw_list, material_set, 1u );
		const uint32_t draw_flags =
				depth_prepass ? packet.m_nFlags | T3D_RD_INSTANCE_DEPTH_PREPASS_PASS : packet.m_nFlags;
		t3dRdInstancePush_t push{ packet.m_nMaterial,
			packet.m_nOverlayMaterial,
			m_bDirectDrawDebug ? packet.m_nFirstVisible : 0u,
			draw_flags,
			packet.m_nVisibilityLayers,
			0u,
			0u,
			0u };
		std::memcpy( m_pushBytes.ptrw(), &push, sizeof( push ) );
		rd->draw_list_set_push_constant( draw_list, m_pushBytes, sizeof( push ) );
		if ( m_bDirectDrawDebug ) {
			rd->draw_list_draw( draw_list, true, packet.m_nVisibleCount, 0u );
		} else {
			rd->draw_list_draw_indirect( draw_list,
					true,
					shadow_pass ? m_hShadowIndirectBuffer : m_hIndirectBuffer,
					i * sizeof( t3dRdIndexedIndirectCommand_t ),
					1u,
					sizeof( t3dRdIndexedIndirectCommand_t ) );
		}
		++m_nDraws;
	}
	return true;
}

bool Terrain3DRdInstanceRenderer::renderForwardPass( const t3dRdForwardPassContext_t& rContext )
{
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || m_nForwardPreparedScenePass != rContext.m_nScenePass ||
			m_nForwardPreparedViewportKey != rContext.m_nViewportKey ) {
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

Dictionary Terrain3DRdInstanceRenderer::getDiagnostics( void ) const
{
	Dictionary result;
	result["instance_frames"] = (int64_t)( m_nFrames );
	result["instance_draws"] = (int64_t)( m_nDraws );
	result["instance_indirect_draws"] = m_bDirectDrawDebug ? 0 : (int64_t)( m_nDraws );
	result["instance_direct_draws"] = m_bDirectDrawDebug ? (int64_t)( m_nDraws ) : 0;
	result["static_instances"] = (int64_t)( m_nStaticInstances );
	result["visible_instances"] = (int64_t)( m_nVisibleInstances );
	result["instance_scene_uploads"] = (int64_t)( m_nSceneUploads );
	result["instance_dirty_upload_bytes"] = (int64_t)( m_nDirtyUploadBytes );
	result["instance_dirty_upload_ranges"] = (int64_t)( m_nDirtyUploadRanges );
	result["instance_buffer_capacity_bytes"] = (int64_t)( m_nInstanceCapacityBytes );
	result["previous_instance_buffer_capacity_bytes"] = (int64_t)( m_nPreviousInstanceCapacityBytes );
	result["motion_history_resets"] = (int64_t)( m_nMotionHistoryResets );
	result["motion_history_matches"] = (int64_t)( m_nMotionHistoryMatches );
	result["motion_debug_enabled"] = m_bMotionDebug;
	result["visible_index_capacity_bytes"] = (int64_t)( m_nVisibleCapacityBytes );
	result["shadow_visible_index_capacity_bytes"] = (int64_t)( m_nShadowVisibleCapacityBytes );
	result["indirect_capacity_bytes"] = (int64_t)( m_nIndirectCapacityBytes );
	result["shadow_indirect_capacity_bytes"] = (int64_t)( m_nShadowIndirectCapacityBytes );
	result["instance_mesh_arena_bytes"] = (int64_t)( m_meshCache.getGpuBytes() );
	result["instance_material_textures"] = (int64_t)( m_meshCache.getTextureCount() );
	result["instance_material_bindings"] = (int64_t)( m_materialBindings.size() );
	result["instance_draw_packets"] = (int64_t)( m_drawPackets.size() );
	result["depth_packets"] = (int64_t)( m_depthPackets.size() );
	result["shadow_packets"] = (int64_t)( m_shadowPackets.size() );
	result["hlod_gpu_cull_descriptors"] = (int64_t)( m_nHlodCullDescriptors );
	result["hlod_gpu_cull_dispatches"] = (int64_t)( m_nHlodCullDispatches );
	result["hlod_descriptor_capacity_bytes"] = (int64_t)( m_nHlodDescriptorCapacityBytes );
	result["direct_draw_debug"] = m_bDirectDrawDebug;
	result["forward_lighting"] = m_forwardLighting.getDiagnostics();
	result["last_error"] = m_lastError;
	return result;
}
