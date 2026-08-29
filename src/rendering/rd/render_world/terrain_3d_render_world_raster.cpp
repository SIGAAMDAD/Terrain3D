// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_raster.h"
#include "rendering/rd/render_world/terrain_3d_render_world_rd_owner.h"

#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <string.h>

using namespace godot;

Terrain3DRenderWorldThreeTierRaster::Terrain3DRenderWorldThreeTierRaster() {
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	memset( m_hShader, 0, sizeof( m_hShader ) );
	memset( m_hUniformSet, 0, sizeof( m_hUniformSet ) );
	memset( m_PipelineCache, 0, sizeof( m_PipelineCache ) );
	memset( &m_CurrentPipelineKey, 0, sizeof( m_CurrentPipelineKey ) );
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	m_nPipelineCount = 0u;
	m_nPipelineEvictCursor = 0u;
	m_nHotCreateEpoch = 0u;
	m_nHotFreeEpoch = 0u;
	m_bCurrentPipelineValid = false;
	m_bInitialized = false;
	m_bHotSealed = false;
}

Terrain3DRenderWorldThreeTierRaster::~Terrain3DRenderWorldThreeTierRaster() {
	// Explicit cold shutdown owns all RIDs.
}

bool Terrain3DRenderWorldThreeTierRaster::initialize_cold(
        const Terrain3DRenderWorldResourceImage *pResources, const t3dRdRenderWorldShaderBundle_t &rShaders ) {
	Terrain3DRdContext *const pContext = pResources ? pResources->get_rd_context() : NULL;
	RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( m_bInitialized || pRenderingDevice == NULL || pResources == NULL || !pResources->is_initialized() ) {
		return false;
	}
	m_pRenderingDevice = pRenderingDevice;
	m_pResources = pResources;
	if ( !create_shaders_cold( rShaders ) || !create_sampler_cold() || !create_uniform_sets_cold() ) {
		shutdown_cold();
		return false;
	}
	m_bInitialized = true;
	return true;
}

void Terrain3DRenderWorldThreeTierRaster::shutdown_cold() {
	m_bHotSealed = false;
	m_bCurrentPipelineValid = false;
	m_hCurrentLightingSet = RID();
	if ( m_pRenderingDevice != NULL ) {
		for ( uint32_t i = 0u; i < m_nPipelineCount; ++i ) {
			free_pipeline_entry_cold( m_PipelineCache[i] );
		}
		for ( int32_t iVariant = ( int32_t )T3D_RW_FORWARD_COLOR_VARIANT_COUNT - 1; iVariant >= 0; --iVariant ) {
			for ( int32_t iTier = ( int32_t )T3D_RW_TIER_COUNT - 1; iTier >= 0; --iTier ) {
				free_rid_cold( m_hUniformSet[iVariant][iTier] );
			}
		}
		free_rid_cold( m_hVtSampler );
		for ( int32_t iVariant = ( int32_t )T3D_RW_FORWARD_COLOR_VARIANT_COUNT - 1; iVariant >= 0; --iVariant ) {
			free_rid_cold( m_hShader[iVariant] );
		}
	}
	memset( m_PipelineCache, 0, sizeof( m_PipelineCache ) );
	m_nPipelineCount = 0u;
	m_nPipelineEvictCursor = 0u;
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_bInitialized = false;
}

void Terrain3DRenderWorldThreeTierRaster::seal_hot_runtime() {
	if ( !m_bInitialized ) {
		return;
	}
	m_nHotCreateEpoch = m_Stats.nGpuCreateCount;
	m_nHotFreeEpoch = m_Stats.nGpuFreeCount;
	m_bHotSealed = true;
}

void Terrain3DRenderWorldThreeTierRaster::reseal_hot_runtime_cold() {
	seal_hot_runtime();
}

bool Terrain3DRenderWorldThreeTierRaster::select_forward_variant(
        const t3dRdForwardPassContext_t &pContext, t3dRdForwardShaderVariant_t &rVariant ) const {
	if ( ( pContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && pContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION )
	        || pContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR || pContext.m_nViewCount != 1u ) {
		return false;
	}
	const bool bSpecular = ( pContext.m_nColorPassFlags & ( 1u << 1u ) ) != 0u;
	const bool bMotion = pContext.m_passKind == T3D_RD_FORWARD_PASS_MOTION || ( pContext.m_nColorPassFlags & ( 1u << 3u ) ) != 0u;
	const bool bRadianceArray = ( pContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	if ( bRadianceArray ) {
		rVariant = bSpecular ? ( bMotion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION_RADIANCE_ARRAY
		                                 : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_RADIANCE_ARRAY )
		                     : ( bMotion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION_RADIANCE_ARRAY : T3D_RD_FORWARD_SHADER_COLOR_RADIANCE_ARRAY );
	} else {
		rVariant = bSpecular ? ( bMotion ? T3D_RD_FORWARD_SHADER_COLOR_SPECULAR_MOTION : T3D_RD_FORWARD_SHADER_COLOR_SPECULAR )
		                     : ( bMotion ? T3D_RD_FORWARD_SHADER_COLOR_MOTION : T3D_RD_FORWARD_SHADER_COLOR );
	}
	return true;
}

bool Terrain3DRenderWorldThreeTierRaster::prepare_forward_cold(
        const t3dRdForwardPassContext_t &pContext, Terrain3DRdForwardLighting &rLighting ) {
	if ( !m_bInitialized || pContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE ) {
		return false;
	}
	t3dRdForwardShaderVariant_t eVariant = T3D_RD_FORWARD_SHADER_COLOR;
	if ( !select_forward_variant( pContext, eVariant ) ) {
		return false;
	}
	if ( !ensure_forward_pipelines_cold( pContext, eVariant ) ) {
		return false;
	}
	if ( !rLighting.prepare( pContext, UINT32_MAX ) ) {
		return false;
	}
	m_hCurrentLightingSet = rLighting.ensureSet( m_hShader[( uint32_t )eVariant] );
	if ( !m_hCurrentLightingSet.is_valid() ) {
		return false;
	}
	m_bCurrentPipelineValid = true;
	reseal_hot_runtime_cold();
	return true;
}

bool Terrain3DRenderWorldThreeTierRaster::get_bindings_hot( T3DThreeTierRasterBindings &rBindings ) {
	if ( !m_bInitialized || !m_bHotSealed || !m_bCurrentPipelineValid || !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	const T3DPipelineEntry *const pEntry = find_pipeline_entry_hot( m_CurrentPipelineKey );
	if ( pEntry == NULL || !m_hCurrentLightingSet.is_valid() ) {
		return false;
	}
	const uint32_t nVariant = ( uint32_t )m_CurrentPipelineKey.eVariant;
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		rBindings.hPipeline[iTier] = pEntry->hPipeline[iTier];
		rBindings.hUniformSet[iTier] = m_hUniformSet[nVariant][iTier];
	}
	rBindings.hLightingSet = m_hCurrentLightingSet;
	rBindings.nUniformSetIndex = 0u;
	rBindings.nLightingSetIndex = 2u;
	rBindings.nFlags = 0u;
	return true;
}

bool Terrain3DRenderWorldThreeTierRaster::is_initialized() const {
	return m_bInitialized;
}
const T3DRasterResourceStats &Terrain3DRenderWorldThreeTierRaster::get_stats() const {
	return m_Stats;
}

bool Terrain3DRenderWorldThreeTierRaster::create_shaders_cold( const t3dRdRenderWorldShaderBundle_t &rShaders ) {
	static_assert(
	        T3D_RW_FORWARD_COLOR_VARIANT_COUNT == T3D_RD_RW_FORWARD_COLOR_VARIANT_COUNT, "RenderWorld raster shader bundle mismatch" );
	for ( uint32_t iVariant = 0u; iVariant < T3D_RW_FORWARD_COLOR_VARIANT_COUNT; ++iVariant ) {
		const Ref<RDShaderSPIRV> &Spirv = rShaders.m_szRaster[iVariant];
		if ( Spirv.is_null() ) {
			return false;
		}
		m_hShader[iVariant] = m_pRenderingDevice->shader_create_from_spirv(
		        Spirv, String( "T3D.RenderWorld.ForwardShader." ) + String::num_uint64( iVariant ) );
		if ( !m_hShader[iVariant].is_valid() ) {
			return false;
		}
		if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
		             m_hShader[iVariant],
		             String( "RenderWorld Forward+ raster shader " ) + String::num_uint64( iVariant ),
		             Terrain3DRdContext::RESOURCE_SHADER,
		             Terrain3DRdContext::OWNER_RENDER_WORLD_RASTER ) ) {
			t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hShader[iVariant] );
			return false;
		}
		++m_Stats.nGpuCreateCount;
	}
	return true;
}

bool Terrain3DRenderWorldThreeTierRaster::create_sampler_cold() {
	Ref<RDSamplerState> Sampler;
	Sampler.instantiate();
	if ( Sampler.is_null() ) {
		return false;
	}
	Sampler->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
	Sampler->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
	Sampler->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
	Sampler->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	Sampler->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	Sampler->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
	m_hVtSampler = m_pRenderingDevice->sampler_create( Sampler );
	if ( !m_hVtSampler.is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hVtSampler,
	             "RenderWorld VT sampler",
	             Terrain3DRdContext::RESOURCE_SAMPLER,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_RASTER ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hVtSampler );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	return true;
}

bool Terrain3DRenderWorldThreeTierRaster::create_uniform_sets_cold() {
	const T3DRenderWorldGpuBufferId aCommon[] = { T3D_RW_GPU_BUFFER_VISIBILITY_FRAME,
		T3D_RW_GPU_BUFFER_GEOMETRY_VERTEX,
		T3D_RW_GPU_BUFFER_CLUSTER_VERTEX_INDEX,
		T3D_RW_GPU_BUFFER_CLUSTER_TRIANGLE,
		T3D_RW_GPU_BUFFER_CLUSTER_RECORDS,
		T3D_RW_GPU_BUFFER_INSTANCE_RECORDS };
	const T3DRenderWorldGpuBufferId aVisible[] = {
		T3D_RW_GPU_BUFFER_VISIBLE_WORLD_CLUSTERS, T3D_RW_GPU_BUFFER_VISIBLE_CLUTTER_CLUSTERS, T3D_RW_GPU_BUFFER_VISIBLE_FOLIAGE_CLUSTERS
	};
	const T3DRenderWorldGpuBufferId aMaterialVt[] = {
		T3D_RW_GPU_BUFFER_MATERIAL_RECORDS, T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE, T3D_RW_GPU_BUFFER_VT_PAGE_TABLE
	};
	if ( !m_hVtSampler.is_valid() ) {
		return false;
	}
	for ( uint32_t iVariant = 0u; iVariant < T3D_RW_FORWARD_COLOR_VARIANT_COUNT; ++iVariant ) {
		for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
			TypedArray<Ref<RDUniform>> Uniforms;
			for ( uint32_t i = 0u; i < sizeof( aCommon ) / sizeof( aCommon[0] ); ++i ) {
				Ref<RDUniform> Uniform;
				Uniform.instantiate();
				if ( Uniform.is_null() ) {
					return false;
				}
				Uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
				Uniform->set_binding( ( int32_t )i );
				Uniform->add_id( m_pResources->get_buffer_rid( aCommon[i] ) );
				Uniforms.push_back( Uniform );
			}
			Ref<RDUniform> Visible;
			Visible.instantiate();
			if ( Visible.is_null() ) {
				return false;
			}
			Visible->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
			Visible->set_binding( 6 );
			Visible->add_id( m_pResources->get_buffer_rid( aVisible[iTier] ) );
			Uniforms.push_back( Visible );
			for ( uint32_t i = 0u; i < 3u; ++i ) {
				Ref<RDUniform> Uniform;
				Uniform.instantiate();
				if ( Uniform.is_null() ) {
					return false;
				}
				Uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
				Uniform->set_binding( ( int32_t )( 7u + i ) );
				Uniform->add_id( m_pResources->get_buffer_rid( aMaterialVt[i] ) );
				Uniforms.push_back( Uniform );
			}
			const T3DRenderWorldVTChannel aChannels[3] = { T3D_RW_VT_ALBEDO, T3D_RW_VT_NORMAL, T3D_RW_VT_SURFACE };
			const uint32_t aBindingBase[3] = { 10u, 26u, 42u };
			for ( uint32_t iChannel = 0u; iChannel < 3u; ++iChannel ) {
				const T3DRenderWorldVTChannel eChannel = aChannels[iChannel];
				const uint32_t nBankCount = m_pResources->get_vt_bank_count( eChannel );
				if ( nBankCount == 0u ) {
					return false;
				}
				const RID &hFallback = m_pResources->get_vt_bank_rid( eChannel, 0u );
				if ( !hFallback.is_valid() ) {
					return false;
				}
				for ( uint32_t iBank = 0u; iBank < T3D_RW_MAX_VT_TEXTURE_BANKS; ++iBank ) {
					const RID &hTexture = iBank < nBankCount ? m_pResources->get_vt_bank_rid( eChannel, iBank ) : hFallback;
					if ( !hTexture.is_valid() ) {
						return false;
					}
					Ref<RDUniform> Uniform;
					Uniform.instantiate();
					if ( Uniform.is_null() ) {
						return false;
					}
					Uniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE );
					Uniform->set_binding( ( int32_t )( aBindingBase[iChannel] + iBank ) );
					Uniform->add_id( m_hVtSampler );
					Uniform->add_id( hTexture );
					Uniforms.push_back( Uniform );
				}
			}
			m_hUniformSet[iVariant][iTier] = m_pRenderingDevice->uniform_set_create( Uniforms, m_hShader[iVariant], 0u );
			if ( !m_hUniformSet[iVariant][iTier].is_valid() ) {
				return false;
			}
			if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
			             m_hUniformSet[iVariant][iTier],
			             String( "RenderWorld Forward+ raster set " ) + String::num_uint64( iVariant ) + "." + String::num_uint64( iTier ),
			             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
			             Terrain3DRdContext::OWNER_RENDER_WORLD_RASTER ) ) {
				t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hUniformSet[iVariant][iTier] );
				return false;
			}
			++m_Stats.nGpuCreateCount;
		}
	}
	return true;
}

Terrain3DRenderWorldThreeTierRaster::T3DPipelineEntry *Terrain3DRenderWorldThreeTierRaster::find_pipeline_entry_cold(
        const T3DPipelineKey &pKey ) {
	for ( uint32_t i = 0u; i < m_nPipelineCount; ++i ) {
		if ( m_PipelineCache[i].Key == pKey ) {
			return &m_PipelineCache[i];
		}
	}
	return NULL;
}

const Terrain3DRenderWorldThreeTierRaster::T3DPipelineEntry *Terrain3DRenderWorldThreeTierRaster::find_pipeline_entry_hot(
        const T3DPipelineKey &pKey ) const {
	for ( uint32_t i = 0u; i < m_nPipelineCount; ++i ) {
		if ( m_PipelineCache[i].Key == pKey ) {
			return &m_PipelineCache[i];
		}
	}
	return NULL;
}

bool Terrain3DRenderWorldThreeTierRaster::ensure_forward_pipelines_cold(
        const t3dRdForwardPassContext_t &pContext, const t3dRdForwardShaderVariant_t pVariant ) {
	if ( m_pRenderingDevice == NULL || pContext.m_nFramebufferFormat < 0 || pContext.m_nViewCount != 1u ) {
		return false;
	}
	T3DPipelineKey Key = {};
	Key.nFramebufferFormat = pContext.m_nFramebufferFormat;
	Key.eVariant = pVariant;
	Key.eSamples = m_pRenderingDevice->framebuffer_format_get_texture_samples( pContext.m_nFramebufferFormat, 0u );
	if ( ( 1u << ( uint32_t )Key.eSamples ) != pContext.m_nSampleCount ) {
		return false;
	}
	Key.nViewCount = pContext.m_nViewCount;
	Key.bReverseCull = pContext.m_bReverseCull;
	// RenderWorld does not yet submit its own geometry into Godot's depth prepass.
	// Treating the native prepass as authoritative here would select EQUAL/no-write
	// against depth that does not contain RenderWorld geometry and can reject the
	// whole color pass. Keep the bit in the key for ABI/debug visibility, but the
	// color pipeline owns its depth write until a real RenderWorld depth pass exists.
	Key.bDepthPrepassActive = pContext.m_bDepthPrepassActive;
	if ( find_pipeline_entry_cold( Key ) != NULL ) {
		++m_Stats.nPipelineCacheHitCount;
		m_CurrentPipelineKey = Key;
		return true;
	}
	++m_Stats.nPipelineCacheMissCount;
	const bool bReplace = m_nPipelineCount >= T3D_RW_PIPELINE_CACHE_CAPACITY;
	const uint32_t nIndex = bReplace ? ( m_nPipelineEvictCursor++ % T3D_RW_PIPELINE_CACHE_CAPACITY ) : m_nPipelineCount++;
	T3DPipelineEntry &Entry = m_PipelineCache[nIndex];
	if ( bReplace ) {
		free_pipeline_entry_cold( Entry );
	}
	memset( &Entry, 0, sizeof( Entry ) );
	Entry.Key = Key;

	Ref<RDPipelineMultisampleState> Multisample;
	Multisample.instantiate();
	Ref<RDPipelineDepthStencilState> Depth;
	Depth.instantiate();
	Ref<RDPipelineColorBlendState> Blend;
	Blend.instantiate();
	if ( Multisample.is_null() || Depth.is_null() || Blend.is_null() ) {
		return false;
	}
	Multisample->set_sample_count( Key.eSamples );
	Depth->set_enable_depth_test( true );
	Depth->set_enable_depth_write( true );
	Depth->set_depth_compare_operator( RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
	TypedArray<Ref<RDPipelineColorBlendStateAttachment>> Attachments;
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		Ref<RDPipelineColorBlendStateAttachment> Attachment;
		Attachment.instantiate();
		if ( Attachment.is_null() ) {
			return false;
		}
		Attachment->set_enable_blend( false );
		Attachments.push_back( Attachment );
	}
	Blend->set_attachments( Attachments );
	for ( uint32_t iTier = 0u; iTier < T3D_RW_TIER_COUNT; ++iTier ) {
		Ref<RDPipelineRasterizationState> Raster;
		Raster.instantiate();
		if ( Raster.is_null() ) {
			return false;
		}
		Raster->set_front_face( RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE );
		if ( iTier == T3D_RW_TIER_FOLIAGE ) {
			Raster->set_cull_mode( RenderingDevice::POLYGON_CULL_DISABLED );
		} else {
			Raster->set_cull_mode( Key.bReverseCull ? RenderingDevice::POLYGON_CULL_FRONT : RenderingDevice::POLYGON_CULL_BACK );
		}
		Entry.hPipeline[iTier] = m_pRenderingDevice->render_pipeline_create( m_hShader[( uint32_t )pVariant],
		        Key.nFramebufferFormat,
		        -1,
		        RenderingDevice::RENDER_PRIMITIVE_TRIANGLES,
		        Raster,
		        Multisample,
		        Depth,
		        Blend,
		        0u );
		if ( !Entry.hPipeline[iTier].is_valid() ) {
			free_pipeline_entry_cold( Entry );
			return false;
		}
		if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
		             Entry.hPipeline[iTier],
		             String( "RenderWorld Forward+ pipeline " ) + String::num_uint64( ( uint32_t )pVariant ) + "."
		                     + String::num_uint64( iTier ),
		             Terrain3DRdContext::RESOURCE_RASTER_PIPELINE,
		             Terrain3DRdContext::OWNER_RENDER_WORLD_RASTER ) ) {
			t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, Entry.hPipeline[iTier] );
			free_pipeline_entry_cold( Entry );
			return false;
		}
		++m_Stats.nGpuCreateCount;
	}
	m_CurrentPipelineKey = Key;
	return true;
}

void Terrain3DRenderWorldThreeTierRaster::free_pipeline_entry_cold( T3DPipelineEntry &rEntry ) {
	for ( int32_t iTier = ( int32_t )T3D_RW_TIER_COUNT - 1; iTier >= 0; --iTier ) {
		free_rid_cold( rEntry.hPipeline[iTier] );
	}
	memset( &rEntry, 0, sizeof( rEntry ) );
}

void Terrain3DRenderWorldThreeTierRaster::free_rid_cold( RID &rRid ) {
	if ( m_pRenderingDevice == NULL || !rRid.is_valid() ) {
		return;
	}
	t3dRenderWorldFreeRid( m_pResources ? m_pResources->get_rd_context() : NULL, rRid );
	++m_Stats.nGpuFreeCount;
}

bool Terrain3DRenderWorldThreeTierRaster::hot_epoch_unchanged() const {
	return m_Stats.nGpuCreateCount == m_nHotCreateEpoch && m_Stats.nGpuFreeCount == m_nHotFreeEpoch;
}
