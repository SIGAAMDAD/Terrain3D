// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/visibility/terrain_3d_world_raster.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/forward/terrain_3d_rd_forward_variant.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"
#include "Terrain3DConstants.h"
#include "rendering/rd/vt/gpu/terrain_3d_vt_gpu.h"

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

static void t3dWorldMarkDynamicBuffers(
	const Ref< RDShaderSPIRV > &shader,
	bool,
	bool,
	bool ) {
	Terrain3DRdContext::getShared().markHotShader( shader );
}

Terrain3DWorldRaster::Terrain3DWorldRaster(
	Terrain3DRdContext &rContext ) :
	m_context( rContext ),
	m_forwardLighting( rContext ),
	m_pipelines( rContext, T3D_RD_CONTEXT_OWNER_WORLD ),
	m_set0Cache( rContext, T3D_RD_CONTEXT_OWNER_WORLD ),
	m_set1Cache( rContext, T3D_RD_CONTEXT_OWNER_WORLD ) {
	memset( m_nSubmitCount, 0, sizeof( m_nSubmitCount ) );
}
Terrain3DWorldRaster::~Terrain3DWorldRaster(
	void ) {
}

bool Terrain3DWorldRaster::configureComposedExperimentalCold( const bool bEnable ) {
	if ( m_bInitialized || ( bEnable && !Terrain3DRdShaderLibrary::isComposedShaderExperimentEnabledCold() ) ) return false;
	m_bExperimentalComposed = bEnable;
	return true;
}

bool Terrain3DWorldRaster::initializeCold(
	const bool bPersistentFrameBuffer,
	const bool bPersistentCpuBuffers ) {
	if ( m_bInitialized ) {
		return true;
	}
	if ( !m_context.getDevice() ) {
		return false;
	}
	m_bPersistentFrameBuffer = m_context.arena( Terrain3DRdContext::HOT ).isPersistent();
	m_hotPushBytes.resize( sizeof( t3dRdDrawPushConstants_t ) );
	m_bPersistentCpuBuffers = bPersistentCpuBuffers;
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		const t3dRdShaderId_t	   shadow_id = primitive == T3D_PRIMITIVE_TERRAIN_STRIP
												   ? T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW
												   : T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW;
		const Ref< RDShaderSPIRV > shadow_spirv = Terrain3DRdShaderLibrary::borrowSpirv( shadow_id );
		if ( shadow_spirv.is_null() ) {
			shutdownCold();
			return false;
		}
		t3dWorldMarkDynamicBuffers( shadow_spirv,
									bPersistentFrameBuffer,
									bPersistentCpuBuffers,
									primitive == T3D_PRIMITIVE_TERRAIN_STRIP );
		const String class_name =
			primitive == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
		const String shadow_name = String( "T3D.World.Raster." ) + class_name + String( ".ShadowShader" );
		if ( !m_hShadowShader[primitive].createTracked( m_context,
														T3D_RD_CONTEXT_OWNER_WORLD,
														shadow_name,
														shadow_id ) ) {
			shutdownCold();
			return false;
		}
	}
	m_bInitialized = true;
	return true;
}

bool Terrain3DWorldRaster::ensureShaderCold(
	const t3dPrimitiveClass_t		  primitive,
	const t3dRdForwardShaderVariant_t variant ) {
	if ( primitive >= T3D_PRIMITIVE_CLASS_COUNT || variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ) {
		return false;
	}
	Terrain3DRdShader &shader = m_hShader[primitive][variant];
	if ( shader.isValid() ) {
		return true;
	}
	bool feedback = primitive == T3D_PRIMITIVE_MESH && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT &&
					m_pFeedbackOwner && m_pFeedbackOwner->is_readback_enabled();
	const bool composed_variant = m_bExperimentalComposed &&
		primitive == T3D_PRIMITIVE_TERRAIN_STRIP && ( uint32_t ) variant != 8u;
	const t3dRdShaderId_t fallback_id =
		( t3dRdShaderId_t ) ( ( primitive == T3D_PRIMITIVE_TERRAIN_STRIP ? T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE
																		 : T3D_RD_SHADER_WORLD_RASTER_MESH_BASE ) +
							  variant );
	t3dRdShaderId_t id = composed_variant
		? ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_CS_BASE + ( uint32_t ) variant - ( ( uint32_t ) variant > 8u ? 1u : 0u ) )
		: feedback ? ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE + variant ) : fallback_id;
	const bool compact = m_forwardLighting.compactRequested() &&
		variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT && !feedback && !composed_variant;
	if ( compact )
		id = ( t3dRdShaderId_t ) ( ( primitive == T3D_PRIMITIVE_TERRAIN_STRIP
			? T3D_RD_SHADER_WORLD_STRIP_COMPACT_BASE : T3D_RD_SHADER_WORLD_MESH_COMPACT_BASE ) + variant );
	if ( primitive == T3D_PRIMITIVE_MESH && !feedback ) {
		const uint32_t trial = Terrain3DRdShaderLibrary::getWorldAlphaTrialCold();
		if ( ( ( trial == 1u || trial == 2u ) && variant == T3D_RD_FORWARD_SHADER_DEPTH ) ||
			( ( trial == 3u || trial == 4u ) && variant == T3D_RD_FORWARD_SHADER_DEPTH_NORMAL_ROUGHNESS ) ) {
			id = t3dRdAlphaSamplingTrialId( true, trial );
		}
	}
	Ref< RDShaderSPIRV > spirv = Terrain3DRdShaderLibrary::borrowSpirv( id );
	if ( feedback && spirv.is_null() ) {
		feedback = false;
		id = fallback_id;
		spirv = Terrain3DRdShaderLibrary::borrowSpirv( id );
	}
	if ( spirv.is_null() ) {
		return false;
	}
	t3dWorldMarkDynamicBuffers( spirv,
								m_bPersistentFrameBuffer,
								m_bPersistentCpuBuffers,
								primitive == T3D_PRIMITIVE_TERRAIN_STRIP );
	const String class_name = primitive == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
	const String name =
		String( "T3D.World.Raster." ) + class_name + String( ".Shader." ) + String::num_uint64( variant );
	if ( feedback ) {
		m_context.beginRecoverableFailureScope();
	}
	const bool created = shader.createTracked( m_context, T3D_RD_CONTEXT_OWNER_WORLD, name, id );
	if ( feedback ) {
		m_context.endRecoverableFailureScope();
		m_bMeshFeedback[variant] = created;
		if ( !created ) {
			const Ref< RDShaderSPIRV > fallback_spirv = Terrain3DRdShaderLibrary::borrowSpirv( fallback_id );
			if ( fallback_spirv.is_null() ) {
				return false;
			}
			t3dWorldMarkDynamicBuffers( fallback_spirv, m_bPersistentFrameBuffer, m_bPersistentCpuBuffers, false );
			return shader.createTracked( m_context, T3D_RD_CONTEXT_OWNER_WORLD, name, fallback_id );
		}
	}
	if ( created ) m_bCompactShader[primitive][variant] = compact;
	return created;
}

void Terrain3DWorldRaster::freeSetsCold(
	void ) {
	m_set0Cache.clear();
	m_set1Cache.clear();
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		for ( uint32_t slot = 0u; slot < T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT; ++slot ) {
			m_hSet0[primitive][slot] = RID();
			m_hSet1[primitive][slot] = RID();
			m_set0Signature[primitive][slot] = t3dSetSignature_t{};
			m_set1Signature[primitive][slot] = t3dSetSignature_t{};
		}
		m_hCurrentSet0[primitive] = RID();
		m_hCurrentSet1[primitive] = RID();
		m_hLightingSet[primitive] = RID();
		m_hCompactSet[primitive] = RID();
	}
}

bool Terrain3DWorldRaster::ensureSetsCold(
	Terrain3DWorldResources					&rResources,
	const t3dRdWorldTerrainRasterBindings_t &rTerrain,
	const t3dSharedVTBindings_t				&rVT,
	const RID								&rShader,
	t3dPrimitiveClass_t						 primitiveClass,
	uint32_t								 nDescriptorSlot,
	const t3dComposedSurfaceGpuBindings_t *pComposed ) {
	const uint32_t primitive = ( uint32_t ) primitiveClass;
	const uint64_t world_generation = rResources.getResourceGeneration();
	if ( primitive >= T3D_PRIMITIVE_CLASS_COUNT || nDescriptorSlot >= T3D_WORLD_DESCRIPTOR_LAYOUT_COUNT ) {
		return false;
	}
	RID					   &hSet0 = m_hSet0[primitive][nDescriptorSlot];
	RID					   &hSet1 = m_hSet1[primitive][nDescriptorSlot];
	const t3dSetSignature_t set0_key{ rShader, world_generation, rTerrain.m_nGeneration };
	// Layout identity includes composed pool replacement even when source V3 is unchanged.
	const uint64_t composed_dependency = pComposed ?
		( uint64_t ) pComposed->m_hPageTable.get_id() ^
		( ( uint64_t ) pComposed->m_hChannels[0].get_id() << 1u ) ^
		( ( uint64_t ) pComposed->m_hChannels[1].get_id() << 2u ) ^
		( ( uint64_t ) pComposed->m_hChannels[2].get_id() << 3u ) ^
		( ( uint64_t ) pComposed->m_nDeviceGeneration << 32u ) : 0u;
	const t3dSetSignature_t set1_key{ rShader, world_generation,
		rVT.nBindingGeneration ^ composed_dependency };
	if ( m_set0Signature[primitive][nDescriptorSlot] == set0_key &&
		 m_set1Signature[primitive][nDescriptorSlot] == set1_key ) {
		const RID liveSet0 = m_set0Cache.resolve( hSet0 );
		const RID liveSet1 = m_set1Cache.resolve( hSet1 );
		if ( liveSet0.is_valid() && liveSet1.is_valid() ) {
			m_hCurrentSet0[primitive] = liveSet0;
			m_hCurrentSet1[primitive] = liveSet1;
			return true;
		}
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
	const RenderingDevice::UniformType frame_type = rTerrain.m_bPersistentFrameBuffer
														? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
														: RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
	const RenderingDevice::UniformType cpu_type = rTerrain.m_bPersistentCpuBuffers
													  ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
													  : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
	TypedArray< Ref< RDUniform > >	   set0;
	set0.push_back( Terrain3DRdBindingBuilder::single( 0, frame_type, rTerrain.m_hFrameBuffer ) );
	if ( primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ) {
		// Unified terrain-strip schema explicitly owns the standalone terrain resource surface.
		set0.push_back( Terrain3DRdBindingBuilder::single( 1, cpu_type, rTerrain.m_hRegionMapBuffer ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 2, rTerrain.m_hTerrainSampler, rTerrain.m_hHeightTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 3, rTerrain.m_hTerrainSampler, rTerrain.m_hControlTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 4, rTerrain.m_hMapSampler, rTerrain.m_hColorTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 5, rTerrain.m_hMaterialSampler, rTerrain.m_hAlbedoTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 6, rTerrain.m_hMaterialSampler, rTerrain.m_hNormalTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 7, rTerrain.m_hMaterialSampler, rTerrain.m_hSurfaceTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 8, cpu_type, rTerrain.m_hMaterialBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 10,
														   RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
														   rTerrain.m_hVisibleBuffer ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 11,
																   rTerrain.m_hMaterialSampler,
																   rTerrain.m_hMaterialDisplacementTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 12,
																   rTerrain.m_hTerrainSampler,
																   rTerrain.m_hDisplacementTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::single( 13, cpu_type, rTerrain.m_hMaterialConfigBuffer ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 14, rTerrain.m_hMapSampler, rTerrain.m_hDerivedTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 15,
																   rTerrain.m_hMapSampler,
																   rTerrain.m_hMiddleCompositeTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 16,
																   rTerrain.m_hTerrainSampler,
																   rTerrain.m_hMiddleHeightTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 17,
																   rTerrain.m_hTerrainSampler,
																   rTerrain.m_hMiddleHeightRegionMapTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 18, rTerrain.m_hTerrainSampler, rTerrain.m_hFarHeightTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 19, rTerrain.m_hTerrainSampler, rTerrain.m_hFarColorTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 20,
																   rTerrain.m_hMaterialSampler,
																   rTerrain.m_hFarAlbedoTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 21,
																   rTerrain.m_hMaterialSampler,
																   rTerrain.m_hWorldNoiseTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 22,
																   rTerrain.m_hMaterialSampler,
																   rTerrain.m_hMacroNoiseTexture ) );
		set0.push_back(
			Terrain3DRdBindingBuilder::samplerTexture( 24, rTerrain.m_hMapSampler, rTerrain.m_hEditorBrushTexture ) );
		set0.push_back( Terrain3DRdBindingBuilder::samplerTexture( 25,
																   rTerrain.m_hMapSampler,
																   rTerrain.m_hTerrainShadowMaskTexture ) );
	}
	static const struct
	{
		int				   binding;
		t3dWorldBufferId_t section;
		bool			   terrain;
	} worldBindings[] = { { 39, WORLD_BUFFER_VERTICES, false },
						  { 40, WORLD_BUFFER_CLUSTER_VERTEX_INDICES, false },
						  { 41, WORLD_BUFFER_TRIANGLES, false },
						  { 42, WORLD_BUFFER_CLUSTERS, false },
						  { 46, WORLD_BUFFER_TRANSFORMS_CURRENT, false },
						  { 47, WORLD_BUFFER_TRANSFORMS_PREVIOUS, false },
						  { 48, WORLD_BUFFER_INSTANCES, false },
						  { 50, WORLD_BUFFER_TERRAIN_STRIP_CLUSTERS, true } };
	set0.push_back( Terrain3DRdBindingBuilder::storage( 49, rResources.getBuffer( WORLD_BUFFER_VISIBLE_ITEMS ) ) );
	for ( const auto &binding : worldBindings ) {
		if ( binding.terrain == ( primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ) ) {
			set0.push_back(
				Terrain3DRdBindingBuilder::storage( binding.binding, rResources.getBuffer( binding.section ) ) );
		}
	}
	const String class_name =
		primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
	hSet0 = m_set0Cache.getOrCreate( set0_key,
									 String( "T3D.World.Raster." ) + class_name + String( ".Set0." ) +
										 String::num_uint64( nDescriptorSlot ),
									 set0,
									 rShader,
									 0u );
	if ( !hSet0.is_valid() ) {
		return false;
	}

	if ( rVT.nBankCount[0] != 1u || rVT.nBankCount[1] != 1u || rVT.nBankCount[2] != 1u || rVT.nBankCount[3] != 1u ) {
		return false;
	}
	TypedArray< Ref< RDUniform > > set1;
	if ( primitiveClass == T3D_PRIMITIVE_MESH ) {
		set1.push_back( Terrain3DRdBindingBuilder::single( 0,
														   RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
														   rResources.getBuffer( WORLD_BUFFER_MATERIALS ) ) );
	}
	set1.push_back(
		Terrain3DRdBindingBuilder::single( 1, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hAllocationTable ) );
	set1.push_back(
		Terrain3DRdBindingBuilder::single( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hPageTable ) );
	for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
		set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( 3 + ( int ) channel,
																   rTerrain.m_hMaterialSampler,
																   rVT.hPhysical[channel][0] ) );
	}
	set1.push_back(
		Terrain3DRdBindingBuilder::single( 7, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hTailMetadata ) );
	for ( uint32_t channel = 0u; channel < 4u; ++channel ) {
		set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( 8 + ( int ) channel,
																   rTerrain.m_hMapSampler,
																   rVT.hTail[channel] ) );
	}
	set1.push_back(
		Terrain3DRdBindingBuilder::single( 12, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, rVT.hMipLayoutTable ) );
// BR20: this consumer keeps its original descriptor ABI until a hint-specific shader version is selected.
	if ( pComposed && primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ) {
		if ( !pComposed->m_hPageTable.is_valid() || !pComposed->m_nTableCapacity ||
			!pComposed->m_nDeviceGeneration ) return false;
		for ( uint32_t i = 0u; i < T3D_CS_CHANNEL_COUNT; ++i )
			if ( !pComposed->m_hChannels[i].is_valid() ) return false;
		set1.push_back( Terrain3DRdBindingBuilder::single( 13,
			RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, pComposed->m_hPageTable ) );
		for ( uint32_t i = 0u; i < T3D_CS_CHANNEL_COUNT; ++i )
			set1.push_back( Terrain3DRdBindingBuilder::samplerTexture( 14 + ( int ) i,
				rTerrain.m_hMaterialSampler, pComposed->m_hChannels[i] ) );
	}
	hSet1 = m_set1Cache.getOrCreate( set1_key,
									 String( "T3D.World.Raster." ) + class_name + String( ".Set1." ) +
										 String::num_uint64( nDescriptorSlot ),
									 set1,
									 rShader,
									 1u );
	if ( !hSet1.is_valid() ) {
		return false;
	}
	m_set0Signature[primitive][nDescriptorSlot] = set0_key;
	m_set1Signature[primitive][nDescriptorSlot] = set1_key;
	m_hCurrentSet0[primitive] = hSet0;
	m_hCurrentSet1[primitive] = hSet1;
	return true;
}

const Terrain3DWorldRaster::t3dPipelineEntry_t *Terrain3DWorldRaster::findPipeline(
	const t3dPipelineKey_t &rKey ) const {
	return m_pipelines.find( rKey );
}

bool Terrain3DWorldRaster::ensurePipelineCold(
	const t3dRdForwardPassContext_t &rContext,
	t3dRdForwardShaderVariant_t		 variant,
	t3dPrimitiveClass_t				 primitiveClass ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd || rContext.m_nFramebufferFormat < 0 ) {
		return false;
	}
	t3dPipelineKey_t key;
	if ( !t3dRdCompileForwardPipelineBaseKey( rd, rContext, variant, false, key ) ) {
		return false;
	}
	key.m_nPrimitiveClass = ( uint32_t ) primitiveClass;
	if ( findPipeline( key ) ) {
		m_currentKey[( uint32_t ) primitiveClass] = key;
		return true;
	}
	t3dRdForwardPipelineStateOptions_t state_options;
	state_options.m_frontFace = primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP
									? RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE
									: RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE;
	state_options.m_cullMode =
		key.m_bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK;
	state_options.m_nBlendAttachmentCount = t3dRdForwardBlendAttachmentCount( variant, false );
	state_options.m_bEnableDepthWrite = !key.m_bDepthPrepassActive;
	state_options.m_depthCompare =
		key.m_bDepthPrepassActive ? RenderingDevice::COMPARE_OP_EQUAL : RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	t3dRdForwardPipelineStates_t pipeline_states;
	if ( !t3dRdBuildForwardPipelineStates( key, state_options, pipeline_states ) ) {
		return false;
	}
	Ref< RDPipelineRasterizationState > raster = pipeline_states.m_raster;
	Ref< RDPipelineMultisampleState >	multisample = pipeline_states.m_multisample;
	Ref< RDPipelineDepthStencilState >	depth = pipeline_states.m_depth;
	Ref< RDPipelineColorBlendState >	blend = pipeline_states.m_blend;
	const uint32_t						primitive = ( uint32_t ) primitiveClass;
	const RID shader = key.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? m_hShadowShader[primitive].rid()
																	: m_hShader[primitive][( uint32_t ) variant].rid();
	if ( !shader.is_valid() ) {
		return false;
	}
	const RenderingDevice::RenderPrimitive render_primitive = primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP
																  ? RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS
																  : RenderingDevice::RENDER_PRIMITIVE_TRIANGLES;
	const String						   class_name =
		  primitiveClass == T3D_PRIMITIVE_TERRAIN_STRIP ? String( "TerrainStrip" ) : String( "Mesh" );
	const RID pipeline =
		m_pipelines.getOrCreateRaster( key,
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

bool Terrain3DWorldRaster::prepareCold(
	const t3dRdForwardPassContext_t			&rContext,
	Terrain3DWorldResources					&rResources,
	const t3dRdWorldTerrainRasterBindings_t &rTerrain,
	const t3dSharedVTBindings_t				&rVT,
	Terrain3DVTGpu							*pFeedbackOwner,
	const t3dComposedSurfaceGpuBindings_t *pComposed ) {
	m_pFeedbackOwner = pFeedbackOwner;
	if ( m_bExperimentalComposed && !pComposed ) return false;
	m_hCurrentFeedbackSet = RID();
	if ( !m_bInitialized || rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE || rContext.m_nViewCount == 0u ) {
		return false;
	}
	m_bCurrentValid = false;
	++m_nPrepareCount;
	t3dRdForwardShaderVariant_t variant;
	if ( !t3dRdSelectForwardVariant( rContext, T3D_RD_FORWARD_VARIANT_SHADOW_ANY_NATIVE_MODE, variant ) ) {
		return false;
	}
	if ( rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW &&
		 ( !m_hShadowShader[T3D_PRIMITIVE_TERRAIN_STRIP].isValid() ||
		   !m_hShadowShader[T3D_PRIMITIVE_MESH].isValid() ) ) {
		return false;
	}
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		m_hLightingSet[primitive] = RID();
		m_hCompactSet[primitive] = RID();
	}
	const t3dPrimitiveClass_t active_classes[2] = { T3D_PRIMITIVE_TERRAIN_STRIP, T3D_PRIMITIVE_MESH };
	const uint32_t			  descriptor_slot =
		   rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW ? T3D_WORLD_SHADOW_DESCRIPTOR_SLOT : ( uint32_t ) variant;
	for ( uint32_t active = 0u; active < 2u; ++active ) {
		const t3dPrimitiveClass_t primitive_class = active_classes[active];
		const uint32_t			  primitive = ( uint32_t ) primitive_class;
		if ( rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW && !ensureShaderCold( primitive_class, variant ) ) {
			return false;
		}
		if ( primitive_class == T3D_PRIMITIVE_MESH && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT &&
			 m_bMeshFeedback[variant] ) {
			m_hCurrentFeedbackSet = m_pFeedbackOwner->get_fragment_feedback_set_cold(
				m_hShader[primitive][variant].rid(),
				T3D_RD_FORWARD_COLOR_VARIANT_COUNT + ( uint32_t ) variant );
			if ( !m_hCurrentFeedbackSet.is_valid() ) {
				return false;
			}
		}
		const RID shader = rContext.m_passKind == T3D_RD_FORWARD_PASS_SHADOW
							   ? m_hShadowShader[primitive].rid()
							   : m_hShader[primitive][( uint32_t ) variant].rid();
		const t3dComposedSurfaceGpuBindings_t *pActiveComposed =
			m_bExperimentalComposed && primitive_class == T3D_PRIMITIVE_TERRAIN_STRIP &&
			rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW && ( uint32_t ) variant != 8u
				? pComposed : NULL;
		if ( !ensureSetsCold( rResources, rTerrain, rVT, shader, primitive_class, descriptor_slot, pActiveComposed ) ||
			 !ensurePipelineCold( rContext, variant, primitive_class ) ) {
			return false;
		}
	}
	if ( variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) {
		if ( !m_forwardLighting.prepare( rContext, UINT32_MAX ) ) {
			return false;
		}
		for ( uint32_t active = 0u; active < 2u; ++active ) {
			const uint32_t primitive = ( uint32_t ) active_classes[active];
			const RID	   shader = m_hShader[primitive][( uint32_t ) variant].rid();
			m_hLightingSet[primitive] = m_forwardLighting.ensureSet( shader );
			if ( !m_hLightingSet[primitive].is_valid() ) return false;
			if ( m_bCompactShader[primitive][variant] ) {
				m_hCompactSet[primitive] = m_forwardLighting.ensureCompactSet( shader );
				if ( !m_hCompactSet[primitive].is_valid() ) return false;
			}
		}
	}
	m_bCurrentValid = true;
	return true;
}

bool Terrain3DWorldRaster::submitHot(
	const t3dRdForwardPassContext_t &rContext,
	Terrain3DWorldResources			&rResources ) {
	if ( !m_bCurrentValid || rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT || rContext.m_nDrawList < 0 ) {
		return false;
	}
	RenderingDevice *const rd = m_context.getDevice();
	const RID			   indirect = rResources.getBuffer( WORLD_BUFFER_INDIRECT );
	if ( !rd || !indirect.is_valid() ) {
		return false;
	}
	const t3dPrimitiveClass_t active_classes[2] = { T3D_PRIMITIVE_TERRAIN_STRIP, T3D_PRIMITIVE_MESH };
	for ( uint32_t active = 0u; active < 2u; ++active ) {
		const uint32_t					primitive = ( uint32_t ) active_classes[active];
		const t3dPipelineEntry_t *const entry = findPipeline( m_currentKey[primitive] );
		const RID						currentSet0 = m_set0Cache.resolve( m_hCurrentSet0[primitive] );
		const RID						currentSet1 = m_set1Cache.resolve( m_hCurrentSet1[primitive] );
		if ( !entry || !currentSet0.is_valid() || !currentSet1.is_valid() ) {
			return false;
		}
		rd->draw_list_bind_render_pipeline( rContext.m_nDrawList, entry->m_pipeline.rid() );
		t3dRdDrawPushConstants_t selection{};
		selection.m_nHotIndex = rResources.hotIndex();
		memcpy( m_hotPushBytes.ptrw(), &selection, sizeof( selection ) );
		rd->draw_list_set_push_constant( rContext.m_nDrawList, m_hotPushBytes, sizeof( selection ) );
		rd->draw_list_bind_uniform_set( rContext.m_nDrawList, currentSet0, 0u );
		rd->draw_list_bind_uniform_set( rContext.m_nDrawList, currentSet1, 1u );
		if ( m_hLightingSet[primitive].is_valid() ) {
			const RID lightingSet = m_forwardLighting.resolveSet( m_hLightingSet[primitive] );
			if ( !lightingSet.is_valid() ) {
				return false;
			}
			rd->draw_list_bind_uniform_set( rContext.m_nDrawList, lightingSet, 2u );
			if ( m_hCompactSet[primitive].is_valid() )
				rd->draw_list_bind_uniform_set( rContext.m_nDrawList, m_hCompactSet[primitive], 4u );
		}
		if ( primitive == T3D_PRIMITIVE_MESH && m_hCurrentFeedbackSet.is_valid() ) {
			const RID feedbackSet =
				m_pFeedbackOwner ? m_pFeedbackOwner->resolve_fragment_feedback_set_hot( m_hCurrentFeedbackSet ) : RID();
			if ( !feedbackSet.is_valid() ) {
				return false;
			}
			rd->draw_list_bind_uniform_set( rContext.m_nDrawList, feedbackSet, 3u );
		}
		if ( active == 0u ) {
			rd->draw_list_draw_indirect( rContext.m_nDrawList,
										 false,
										 indirect,
										 rResources.bufferOffset( WORLD_BUFFER_INDIRECT ) +
											 t3dWorldIndirectOffsetBytes( T3D_WORLD_INDIRECT_TERRAIN_STRIP_INDEX ),
										 T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT,
										 T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES );
			m_context.recordRenderIndirectCommandsSubmitted( T3D_RD_CONTEXT_OWNER_WORLD,
															 T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT );
		} else {
			rd->draw_list_draw_indirect( rContext.m_nDrawList,
										 false,
										 indirect,
										 rResources.bufferOffset( WORLD_BUFFER_INDIRECT ) +
											 t3dWorldIndirectOffsetBytes( T3D_WORLD_INDIRECT_NONTERRAIN_INDEX ),
										 1u,
										 0u );
			m_context.recordRenderIndirectCommandsSubmitted( T3D_RD_CONTEXT_OWNER_WORLD, 1u );
		}
	}
	if ( rContext.m_passKind > T3D_RD_FORWARD_PASS_INVALID && rContext.m_passKind < T3D_RD_FORWARD_PASS_MAX ) {
		++m_nSubmitCount[( uint32_t ) rContext.m_passKind];
	}
	return true;
}

void Terrain3DWorldRaster::shutdownCold(
	void ) {
	m_bCurrentValid = false;
	freeSetsCold();
	m_forwardLighting.shutdown();
	m_pipelines.clear();
	for ( uint32_t primitive = 0u; primitive < T3D_PRIMITIVE_CLASS_COUNT; ++primitive ) {
		m_hShadowShader[primitive].releaseTracked( m_context );
		for ( uint32_t i = 0u; i < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++i ) {
			m_hShader[primitive][i].releaseTracked( m_context );
		}
	}
	memset( m_bMeshFeedback, 0, sizeof( m_bMeshFeedback ) );
	memset( m_bCompactShader, 0, sizeof( m_bCompactShader ) );
	m_hCurrentFeedbackSet = RID();
	m_pFeedbackOwner = NULL;
	m_bInitialized = false;
}

bool Terrain3DWorldRaster::isReady(
	void ) const {
	return m_bInitialized && m_bCurrentValid;
}
uint64_t Terrain3DWorldRaster::getSubmitCount(
	t3dRdForwardPassKind_t pass ) const {
	return pass > T3D_RD_FORWARD_PASS_INVALID && pass < T3D_RD_FORWARD_PASS_MAX ? m_nSubmitCount[( uint32_t ) pass]
																				: 0u;
}
uint64_t Terrain3DWorldRaster::getPrepareCount(
	void ) const {
	return m_nPrepareCount;
}
