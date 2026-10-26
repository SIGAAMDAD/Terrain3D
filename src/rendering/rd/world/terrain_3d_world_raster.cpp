// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/terrain_3d_world_raster.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_binding_builder.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"
#include "rendering/rd/terrain_3d_shader_pipeline_abi.h"
#include "rendering/rd/terrain_3d_rd_shader_compile_status.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <string.h>

using namespace godot;

static void t3dWorldMarkDynamicBuffers( const Ref<RDShaderSPIRV> &rShader, bool bPersistentFrameBuffer, bool bPersistentCpuBuffers, bool bTerrainInterface ) {
	if ( rShader.is_null() || !rShader->has_method( StringName( "set_dynamic_buffers" ) )
	        || ( !bPersistentFrameBuffer && !bPersistentCpuBuffers ) ) {
		return;
	}
	PackedInt64Array dynamic_buffers;
	if ( bPersistentFrameBuffer ) {
		dynamic_buffers.push_back( 0 );
	}
	if ( bPersistentCpuBuffers && bTerrainInterface ) {
		dynamic_buffers.push_back( 1 );
		dynamic_buffers.push_back( 8 );
		dynamic_buffers.push_back( 9 );
		dynamic_buffers.push_back( 13 );
		dynamic_buffers.push_back( 23 );
	}
	if ( bPersistentFrameBuffer ) {
		dynamic_buffers.push_back( ( int64_t )( 2ULL << 32u ) );
	}
	rShader->call( StringName( "set_dynamic_buffers" ), dynamic_buffers );
}

Terrain3DWorldRaster::Terrain3DWorldRaster( Terrain3DRdContext &rContext ) :
        m_context( rContext ), m_forwardLighting( rContext ), m_pipelines( rContext, Terrain3DRdContext::OWNER_WORLD ),
        m_set0Cache( rContext, Terrain3DRdContext::OWNER_WORLD ), m_set1Cache( rContext, Terrain3DRdContext::OWNER_WORLD ) {
	memset( m_nSubmitCount, 0, sizeof( m_nSubmitCount ) );
}
Terrain3DWorldRaster::~Terrain3DWorldRaster( void ) {
}

bool Terrain3DWorldRaster::initializeCold( const Ref<RDShaderSPIRV> *pTerrainStripVariants,
        const Ref<RDShaderSPIRV> &rTerrainStripShadow,
        const Ref<RDShaderSPIRV> *pMeshVariants,
        const Ref<RDShaderSPIRV> &rMeshShadow,
        bool bPersistentFrameBuffer,
        bool bPersistentCpuBuffers ) {
	if ( m_bInitialized ) {
		return true;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !pTerrainStripVariants || !pMeshVariants ) {
		return false;
	}
	m_forwardLighting.setPersistentFrameBufferEnabled( bPersistentFrameBuffer );
	const Ref<RDShaderSPIRV> *variant_sources[T3D_PRIMITIVE_CLASS_COUNT] = { pTerrainStripVariants, pMeshVariants };
	const Ref<RDShaderSPIRV> shadow_sources[T3D_PRIMITIVE_CLASS_COUNT] = { rTerrainStripShadow, rMeshShadow };
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		for ( uint32_t i = 0u; i < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++i ) {
			if ( variant_sources[primitive][i].is_null() ) {
				shutdownCold();
				return false;
			}
			t3dWorldMarkDynamicBuffers( variant_sources[primitive][i], bPersistentFrameBuffer, bPersistentCpuBuffers, primitive == T3D_PRIMITIVE_TERRAIN_STRIP );
			m_source[primitive][i] = variant_sources[primitive][i];
			const String class_name = primitive == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
			const String resource_name = String( "T3D.World.Raster." ) + class_name + String( ".Shader." ) + String::num_uint64( i );
			m_hShader[primitive][i] = t3dRdCreateShaderWithStatus( rd, variant_sources[primitive][i], resource_name );
			if ( !m_hShader[primitive][i].is_valid()
			        || !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_WORLD )
			                    .adopt( rd, m_hShader[primitive][i], resource_name, Terrain3DRdContext::RESOURCE_SHADER ) ) {
				shutdownCold();
				return false;
			}
		}
		if ( shadow_sources[primitive].is_null() ) {
			shutdownCold();
			return false;
		}
		t3dWorldMarkDynamicBuffers( shadow_sources[primitive], bPersistentFrameBuffer, bPersistentCpuBuffers, primitive == T3D_PRIMITIVE_TERRAIN_STRIP );
		m_shadowSource[primitive] = shadow_sources[primitive];
		const String class_name = primitive == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
		const String shadow_name = String( "T3D.World.Raster." ) + class_name + String( ".ShadowShader" );
		m_hShadowShader[primitive] = t3dRdCreateShaderWithStatus( rd, shadow_sources[primitive], shadow_name );
		if ( !m_hShadowShader[primitive].is_valid()
		        || !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_WORLD )
		                    .adopt( rd, m_hShadowShader[primitive], shadow_name, Terrain3DRdContext::RESOURCE_SHADER ) ) {
			shutdownCold();
			return false;
		}
	}
	m_bInitialized = true;
	return true;
}

bool Terrain3DWorldRaster::selectVariant( const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t &rVariant ) const {
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ) {
		rVariant = T3D_RD_FORWARD_SHADER_DEPTH;
		return m_hShadowShader[T3D_PRIMITIVE_TERRAIN_STRIP].is_valid() && m_hShadowShader[T3D_PRIMITIVE_MESH].is_valid();
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
	if ( ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION )
	        || rContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR ) {
		return false;
	}
	const bool specular = ( rContext.m_nColorPassFlags & ( 1u << 1u ) ) != 0u;
	const bool motion = rContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || ( rContext.m_nColorPassFlags & ( 1u << 3u ) ) != 0u;
	const bool radiance = ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	if ( radiance ) {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY
		                               : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY )
		                    : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY : T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY );
	} else {
		rVariant = specular ? ( motion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR )
		                    : ( motion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION : T3D_RD_FORWARD_SHADER_COLOR );
	}
	return true;
}

void Terrain3DWorldRaster::freeSetsCold( void ) {
	m_set0Cache.clear();
	m_set1Cache.clear();
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		for ( uint32_t slot = 0u; slot < s_nWorldDescriptorLayoutCount; ++slot ) {
			m_hSet0[primitive][slot] = RID();
			m_hSet1[primitive][slot] = RID();
			m_set0Signature[primitive][slot] = t3dSetSignature_t{};
			m_set1Signature[primitive][slot] = t3dSetSignature_t{};
		}
		m_hCurrentSet0[primitive] = RID();
		m_hCurrentSet1[primitive] = RID();
		m_hLightingSet[primitive] = RID();
	}
}

bool Terrain3DWorldRaster::ensureSetsCold( Terrain3DWorldResources &rResources,
        const t3dRdWorldTerrainRasterBindings_t &rTerrain,
        const T3DTerrainStageASharedVTBindings &rVT,
        const RID &rShader,
        t3dPrimitiveClass_t primitiveClass,
        uint32_t nDescriptorSlot ) {
	const uint32_t primitive = ( uint32_t )primitiveClass;
	const uint64_t world_generation = rResources.getResourceGeneration();
	if ( primitive >= T3D_PRIMITIVE_CLASS_COUNT || nDescriptorSlot >= s_nWorldDescriptorLayoutCount ) {
		return false;
	}
	RID &hSet0 = m_hSet0[primitive][nDescriptorSlot];
	RID &hSet1 = m_hSet1[primitive][nDescriptorSlot];
	const t3dSetSignature_t set0_key { rShader, world_generation, rTerrain.m_nGeneration };
	const t3dSetSignature_t set1_key { rShader, world_generation, rVT.nBindingGeneration };
	if ( hSet0.is_valid() && hSet1.is_valid() && m_set0Signature[primitive][nDescriptorSlot] == set0_key
	        && m_set1Signature[primitive][nDescriptorSlot] == set1_key ) {
		m_hCurrentSet0[primitive] = hSet0;
		m_hCurrentSet1[primitive] = hSet1;
		return true;
	}
	m_set0Cache.erase( hSet0 );
	m_set1Cache.erase( hSet1 );
	hSet0 = RID();
	hSet1 = RID();
	m_hLightingSet[primitive] = RID();
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || !rShader.is_valid() ) {
		return false;
	}
	const RenderingDevice::UniformType frame_type = rTerrain.m_bPersistentFrameBuffer ? RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC
	                                                                                  : RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER;
	const RenderingDevice::UniformType cpu_type = rTerrain.m_bPersistentCpuBuffers ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
	                                                                               : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
	TypedArray<Ref<RDUniform>> set0;
	set0.push_back( Terrain3DRdBindingBuilder::single( 0, frame_type, rTerrain.m_hFrameBuffer ) );
	if ( primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ) {
		// Unified terrain-strip schema explicitly owns the standalone terrain resource surface.
		set0.push_back( Terrain3DRdBindingBuilder::single( 1, cpu_type, rTerrain.m_hRegionMapBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 2, rTerrain.m_hTerrainSampler, rTerrain.m_hHeightTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 3, rTerrain.m_hTerrainSampler, rTerrain.m_hControlTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 4, rTerrain.m_hMapSampler, rTerrain.m_hColorTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 5, rTerrain.m_hMaterialSampler, rTerrain.m_hAlbedoTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 6, rTerrain.m_hMaterialSampler, rTerrain.m_hNormalTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 7, rTerrain.m_hMaterialSampler, rTerrain.m_hSurfaceTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 8, cpu_type, rTerrain.m_hMaterialBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 9, cpu_type, rTerrain.m_hPatchBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 10, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rTerrain.m_hVisibleBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 11, rTerrain.m_hMaterialSampler, rTerrain.m_hMaterialDisplacementTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 12, rTerrain.m_hTerrainSampler, rTerrain.m_hDisplacementTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 13, cpu_type, rTerrain.m_hMaterialConfigBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 14, rTerrain.m_hMapSampler, rTerrain.m_hDerivedTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 15, rTerrain.m_hMapSampler, rTerrain.m_hMiddleCompositeTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 16, rTerrain.m_hTerrainSampler, rTerrain.m_hMiddleHeightTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 17, rTerrain.m_hTerrainSampler, rTerrain.m_hMiddleHeightRegionMapTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 18, rTerrain.m_hTerrainSampler, rTerrain.m_hFarHeightTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 19, rTerrain.m_hTerrainSampler, rTerrain.m_hFarColorTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 20, rTerrain.m_hMaterialSampler, rTerrain.m_hFarAlbedoTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 21, rTerrain.m_hMaterialSampler, rTerrain.m_hWorldNoiseTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 22, rTerrain.m_hMaterialSampler, rTerrain.m_hMacroNoiseTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 23, cpu_type, rTerrain.m_hEditorOverlayBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 24, rTerrain.m_hMapSampler, rTerrain.m_hEditorBrushTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 25, rTerrain.m_hMapSampler, rTerrain.m_hTerrainShadowMaskTexture ) );
	}
	if ( primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ) {
		set0.push_back( Terrain3DRdBindingBuilder::single( 49,
		        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
		        rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_VISIBLE_ITEMS ) ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 50,
		        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
		        rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS ) ) );
	} else {
		for ( uint32_t i = Terrain3DWorldResources::WORLD_BUFFER_VERTICES; i <= Terrain3DWorldResources::WORLD_BUFFER_CLUSTERS; ++i ) {
			set0.push_back( Terrain3DRdBindingBuilder::single( 39 + ( int )i,
			        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
			        rResources.getBuffer( ( Terrain3DWorldResources::t3dWorldBufferId_t )i ) ) );
		}
		set0.push_back( Terrain3DRdBindingBuilder::single( 46,
		        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
		        rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_TRANSFORMS_CURRENT ) ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 47,
		        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
		        rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_TRANSFORMS_PREVIOUS ) ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 48,
		        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
		        rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_INSTANCES ) ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 49,
		        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
		        rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_VISIBLE_ITEMS ) ) );
	}
	const String class_name = primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
	hSet0 = m_set0Cache.getOrCreate( set0_key,
	        String( "T3D.World.Raster." ) + class_name + String( ".Set0." ) + String::num_uint64( nDescriptorSlot ),
	        set0, rShader, 0u );
	if ( !hSet0.is_valid() ) {
		return false;
	}

	if ( rVT.nBankCount[0] != 1u || rVT.nBankCount[1] != 1u || rVT.nBankCount[2] != 1u || rVT.nBankCount[3] != 1u ) {
		return false;
	}
	TypedArray<Ref<RDUniform>> set1;
	if ( primitiveClass == T3D_PRIMITIVE_MESH ) {
		set1.push_back( Terrain3DRdBindingBuilder::single(
		        0, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_MATERIALS ) ) );
	}
	set1.push_back( Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hAllocationTable ) );
	set1.push_back( Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hPageTable ) );
	for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
		set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( 3 + ( int )channel, rTerrain.m_hMaterialSampler, rVT.hPhysical[channel][0] ) );
	}
	set1.push_back( Terrain3DRdBindingBuilder::single( 7, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hTailMetadata ) );
	for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
		set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( 8 + ( int )channel, rTerrain.m_hMapSampler, rVT.hTail[channel] ) );
	}
	set1.push_back( Terrain3DRdBindingBuilder::single( 12, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hMipLayoutTable ) );
	hSet1 = m_set1Cache.getOrCreate( set1_key,
	        String( "T3D.World.Raster." ) + class_name + String( ".Set1." ) + String::num_uint64( nDescriptorSlot ),
	        set1, rShader, 1u );
	if ( !hSet1.is_valid() ) {
		return false;
	}
	m_set0Signature[primitive][nDescriptorSlot] = set0_key;
	m_set1Signature[primitive][nDescriptorSlot] = set1_key;
	m_hCurrentSet0[primitive] = hSet0;
	m_hCurrentSet1[primitive] = hSet1;
	return true;
}

const Terrain3DWorldRaster::t3dPipelineEntry_t *Terrain3DWorldRaster::findPipeline( const t3dPipelineKey_t &rKey ) const {
	return m_pipelines.find( rKey );
}

bool Terrain3DWorldRaster::ensurePipelineCold(
        const t3dRdForwardPassContext_t &rContext, t3dRdForwardShaderVariant_t variant, t3dPrimitiveClass_t primitiveClass ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 ) {
		return false;
	}
	t3dPipelineKey_t key;
	key.m_nFramebufferFormat = rContext.m_nFramebufferFormat;
	key.m_variant = variant;
	key.m_samples = rd->framebuffer_format_get_texture_samples( rContext.m_nFramebufferFormat, 0u );
	key.m_nViewCount = rContext.m_nViewCount;
	key.m_nPrimitiveClass = ( uint32_t )primitiveClass;
	key.m_bReverseCull = rContext.m_bReverseCull;
	key.m_bDepthPrepassActive = rContext.m_passKind == T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_bDepthPrepassActive;
	key.m_bShadow = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW;
	if ( ( 1u << ( uint32_t )key.m_samples ) != rContext.m_nSampleCount ) {
		return false;
	}
	if ( findPipeline( key ) ) {
		m_currentKey[( uint32_t )primitiveClass] = key;
		return true;
	}
	Ref<RDPipelineRasterizationState> raster;
	raster.instantiate();
	raster->set_front_face( primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP
	                ? RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE
	                : RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE );
	raster->set_cull_mode( key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK );
	Ref<RDPipelineMultisampleState> multisample;
	multisample.instantiate();
	multisample->set_sample_count( key.m_samples );
	Ref<RDPipelineDepthStencilState> depth;
	depth.instantiate();
	depth->set_enable_depth_test( true );
	depth->set_enable_depth_write( !key.m_bDepthPrepassActive );
	depth->set_depth_compare_operator(
	        key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
	Ref<RDPipelineColorBlendState> blend;
	blend.instantiate();
	uint32_t blend_count = variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT
	        ? 3u
	        : ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS
	                          ? 1u
	                          : ( variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI ? 2u : 0u ) );
	if ( blend_count ) {
		TypedArray<Ref<RDPipelineColorBlendStateAttachment>> attachments;
		for ( uint32_t i = 0u; i < blend_count; ++i ) {
			Ref<RDPipelineColorBlendStateAttachment> a;
			a.instantiate();
			a->set_enable_blend( false );
			attachments.push_back( a );
		}
		blend->set_attachments( attachments );
	}
	const uint32_t primitive = ( uint32_t )primitiveClass;
	const RID shader = key.m_bShadow ? m_hShadowShader[primitive] : m_hShader[primitive][( uint32_t )variant];
	if ( !shader.is_valid() ) {
		return false;
	}
	const RenderingDevice::RenderPrimitive render_primitive = primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP
	        ? RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS
	        : RenderingDevice::RENDER_PRIMITIVE_TRIANGLES;
	const String class_name = primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
	const RID pipeline = m_pipelines.getOrCreateRaster( key,
	        String( "T3D.World.Raster." ) + class_name + String( ".Pipeline" ),
	        shader,
	        rContext.m_nFramebufferFormat,
	        -1,
	        render_primitive,
	        raster,
	        multisample,
	        depth,
	        blend,
	        0u );
	if ( !pipeline.is_valid() ) {
		return false;
	}
	m_currentKey[primitive] = key;
	return true;
}

bool Terrain3DWorldRaster::prepareCold( const t3dRdForwardPassContext_t &rContext,
        Terrain3DWorldResources &rResources,
        const t3dRdWorldTerrainRasterBindings_t &rTerrain,
        const T3DTerrainStageASharedVTBindings &rVT ) {
	if ( !m_bInitialized || rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE || rContext.m_nViewCount == 0u ) {
		return false;
	}
	m_bCurrentValid = false;
	++m_nPrepareCount;
	t3dRdForwardShaderVariant_t variant;
	if ( !selectVariant( rContext, variant ) ) {
		return false;
	}
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		m_hLightingSet[primitive] = RID();
	}
	const t3dPrimitiveClass_t active_classes[2] = { T3D_PRIMITIVE_TERRAIN_STRIP, T3D_PRIMITIVE_MESH };
	const uint32_t descriptor_slot = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
	        ? s_nWorldShadowDescriptorSlot
	        : ( uint32_t )variant;
	for ( uint32_t active = 0u; active < 2u; ++active ) {
		const t3dPrimitiveClass_t primitive_class = active_classes[active];
		const uint32_t primitive = ( uint32_t )primitive_class;
		const RID shader = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
		        ? m_hShadowShader[primitive]
		        : m_hShader[primitive][( uint32_t )variant];
		if ( !ensureSetsCold( rResources, rTerrain, rVT, shader, primitive_class, descriptor_slot )
		        || !ensurePipelineCold( rContext, variant, primitive_class ) ) {
			return false;
		}
	}
	if ( variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
		if ( !m_forwardLighting.prepare( rContext, UINT32_MAX ) ) {
			return false;
		}
		for ( uint32_t active = 0u; active < 2u; ++active ) {
			const uint32_t primitive = ( uint32_t )active_classes[active];
			const RID shader = m_hShader[primitive][( uint32_t )variant];
			m_hLightingSet[primitive] = m_forwardLighting.ensureSet( shader );
			if ( !m_hLightingSet[primitive].is_valid() ) {
				return false;
			}
		}
	}
	m_bCurrentValid = true;
	return true;
}

bool Terrain3DWorldRaster::submitHot( const t3dRdForwardPassContext_t &rContext, Terrain3DWorldResources &rResources ) {
	if ( !m_bCurrentValid || rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || rContext.m_nDrawList < 0 ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const RID indirect = rResources.getBuffer( Terrain3DWorldResources::WORLD_BUFFER_INDIRECT );
	if ( !rd || !indirect.is_valid() ) {
		return false;
	}
	const t3dPrimitiveClass_t active_classes[2] = { T3D_PRIMITIVE_TERRAIN_STRIP, T3D_PRIMITIVE_MESH };
	for ( uint32_t active = 0u; active < 2u; ++active ) {
		const uint32_t primitive = ( uint32_t )active_classes[active];
		const t3dPipelineEntry_t *const entry = findPipeline( m_currentKey[primitive] );
		if ( !entry || !m_hCurrentSet0[primitive].is_valid() || !m_hCurrentSet1[primitive].is_valid() ) {
			return false;
		}
		rd->draw_list_bind_render_pipeline( rContext.m_nDrawList, entry->m_hPipeline );
		rd->draw_list_bind_uniform_set( rContext.m_nDrawList, m_hCurrentSet0[primitive], 0u );
		rd->draw_list_bind_uniform_set( rContext.m_nDrawList, m_hCurrentSet1[primitive], 1u );
		if ( m_hLightingSet[primitive].is_valid() ) {
			rd->draw_list_bind_uniform_set( rContext.m_nDrawList, m_hLightingSet[primitive], 2u );
		}
		if ( active == 0u ) {
			rd->draw_list_draw_indirect( rContext.m_nDrawList,
			        false,
			        indirect,
			        t3dWorldIndirectOffsetBytes( T3D_WORLD_INDIRECT_TERRAIN_STRIP_INDEX ),
			        T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT,
			        T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES );
			m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_WORLD, T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT );
		} else {
			rd->draw_list_draw_indirect( rContext.m_nDrawList,
			        false,
			        indirect,
			        t3dWorldIndirectOffsetBytes( T3D_WORLD_INDIRECT_NONTERRAIN_INDEX ),
			        1u,
			        0u );
			m_context.recordRenderIndirectCommandsSubmitted( Terrain3DRdContext::OWNER_WORLD, 1u );
		}
	}
	if ( rContext.m_passKind > T3D_RD_FORWARD_PASS_INVALID && rContext.m_passKind < T3D_RD_FORWARD_PASS_MAX ) {
		++m_nSubmitCount[( uint32_t )rContext.m_passKind];
	}
	return true;
}


void Terrain3DWorldRaster::shutdownCold( void ) {
	m_bCurrentValid = false;
	freeSetsCold();
	m_forwardLighting.shutdown();
	m_pipelines.clear();
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		m_context.freeResource( m_hShadowShader[primitive] );
		for ( uint32_t i = 0u; i < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++i ) {
			m_context.freeResource( m_hShader[primitive][i] );
			m_source[primitive][i].unref();
		}
		m_shadowSource[primitive].unref();
	}
	m_bInitialized = false;
}

bool Terrain3DWorldRaster::isReady( void ) const {
	return m_bInitialized && m_bCurrentValid;
}
uint64_t Terrain3DWorldRaster::getSubmitCount( t3dRdForwardPassKind_t pass ) const {
	return pass > T3D_RD_FORWARD_PASS_INVALID && pass < T3D_RD_FORWARD_PASS_MAX ? m_nSubmitCount[( uint32_t )pass] : 0u;
}
uint64_t Terrain3DWorldRaster::getPrepareCount( void ) const {
	return m_nPrepareCount;
}
