// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_forward_lighting.h"

#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstring>

namespace {
Ref<RDUniform> t3dRdForwardLightUniform( const int nBinding, const RenderingDevice::UniformType type, const RID &rRid ) {
	Ref<RDUniform> uniform;
	uniform.instantiate();
	uniform->set_binding( nBinding );
	uniform->set_uniform_type( type );
	uniform->add_id( rRid );
	return uniform;
}

bool t3dRdForwardLightAdopt( Terrain3DRdContext &rContext, RenderingDevice *const pRd, RID &rRid,
		const String &rName, const Terrain3DRdContext::ResourceType type,
		const Terrain3DRdContext::MemoryCategory memory, const uint64_t nBytes = 0u ) {
	if ( rRid.is_valid() && rContext.registerResource( rRid, rName, type, memory, nBytes ) ) return true;
	if ( pRd && rRid.is_valid() ) pRd->free_rid( rRid );
	rRid = RID();
	return false;
}
} // namespace

Terrain3DRdForwardLighting::Terrain3DRdForwardLighting( Terrain3DRdContext &rContext ) : m_context( rContext ) {
	m_sets.reserve( T3D_RD_FORWARD_SHADER_VARIANT_MAX * 3u );
	m_frameBytes.resize( sizeof( t3dRdForwardLightFrame_t ) );
}

Terrain3DRdForwardLighting::~Terrain3DRdForwardLighting( void ) { shutdown(); }

void Terrain3DRdForwardLighting::setError( const String &rError ) { m_lastError = rError; }

void Terrain3DRdForwardLighting::freeSets( void ) {
	for ( t3dRdForwardLightSet_t &entry : m_sets ) m_context.freeResource( entry.m_hSet );
	m_sets.clear();
}

bool Terrain3DRdForwardLighting::ensureOwnedResources( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) return false;
	const uint64_t generation = m_context.getDeviceGeneration();
	if ( m_nDeviceGeneration != 0u && m_nDeviceGeneration != generation ) {
		m_hFrameBuffer = RID();
		m_hFallbackShadowTexture = RID();
		m_hFallbackShadowSampler = RID();
		m_hFallbackStorageBuffer = RID();
		m_hFallbackColorTexture = RID();
		m_hFallbackColorArrayTexture = RID();
		m_hLinearSampler = RID();
		m_sets.clear();
		m_bKeyValid = false;
	}
	m_nDeviceGeneration = generation;
	if ( m_hFrameBuffer.is_valid() && m_hFallbackShadowTexture.is_valid() && m_hFallbackShadowSampler.is_valid() &&
			m_hFallbackStorageBuffer.is_valid() && m_hFallbackColorTexture.is_valid() &&
			m_hFallbackColorArrayTexture.is_valid() && m_hLinearSampler.is_valid() ) return true;

	m_context.beginResourceRebuild();
	if ( !m_hFrameBuffer.is_valid() ) {
		m_hFrameBuffer = rd->uniform_buffer_create( sizeof( t3dRdForwardLightFrame_t ) );
		if ( !t3dRdForwardLightAdopt( m_context, rd, m_hFrameBuffer, "T3D.ForwardLighting.Frame",
				Terrain3DRdContext::RESOURCE_UNIFORM_BUFFER, Terrain3DRdContext::MEMORY_UNIFORM,
				sizeof( t3dRdForwardLightFrame_t ) ) ) {
			m_context.endResourceRebuild(); setError( "Failed to create Forward+ lighting frame UBO" ); return false;
		}
	}
	if ( !m_hFallbackShadowTexture.is_valid() ) {
		Ref<RDTextureFormat> format;
		format.instantiate();
		format->set_width( 1u ); format->set_height( 1u ); format->set_depth( 1u );
		format->set_array_layers( 1u ); format->set_mipmaps( 1u );
		format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
		format->set_format( RenderingDevice::DATA_FORMAT_D32_SFLOAT );
		format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT );
		Ref<RDTextureView> view; view.instantiate();
		TypedArray<PackedByteArray> empty_data;
		m_hFallbackShadowTexture = rd->texture_create( format, view, empty_data );
		if ( !t3dRdForwardLightAdopt( m_context, rd, m_hFallbackShadowTexture, "T3D.ForwardLighting.FallbackShadow",
				Terrain3DRdContext::RESOURCE_TEXTURE, Terrain3DRdContext::MEMORY_SHADOW, sizeof( float ) ) ) {
			m_context.endResourceRebuild(); setError( "Failed to create Forward+ fallback shadow texture" ); return false;
		}
	}
	if ( !m_hFallbackShadowSampler.is_valid() ) {
		Ref<RDSamplerState> sampler; sampler.instantiate();
		sampler->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_enable_compare( true );
		sampler->set_compare_op( RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
		m_hFallbackShadowSampler = rd->sampler_create( sampler );
		if ( !t3dRdForwardLightAdopt( m_context, rd, m_hFallbackShadowSampler, "T3D.ForwardLighting.FallbackShadowSampler",
				Terrain3DRdContext::RESOURCE_SAMPLER, Terrain3DRdContext::MEMORY_NONE ) ) {
			m_context.endResourceRebuild(); setError( "Failed to create Forward+ fallback shadow sampler" ); return false;
		}
	}
	if ( !m_hFallbackStorageBuffer.is_valid() ) {
		m_hFallbackStorageBuffer = rd->storage_buffer_create( 256u );
		if ( !t3dRdForwardLightAdopt( m_context, rd, m_hFallbackStorageBuffer, "T3D.ForwardLighting.FallbackStorage",
				Terrain3DRdContext::RESOURCE_STORAGE_BUFFER, Terrain3DRdContext::MEMORY_UNIFORM, 256u ) ) {
			m_context.endResourceRebuild(); setError( "Failed to create Forward+ fallback scene storage buffer" ); return false;
		}
	}
	if ( !m_hFallbackColorTexture.is_valid() || !m_hFallbackColorArrayTexture.is_valid() ) {
		Ref<RDTextureView> view; view.instantiate();
		PackedByteArray pixel; pixel.resize( 4 );
		std::memset( pixel.ptrw(), 0, 4 );
		TypedArray<PackedByteArray> initial_data; initial_data.push_back( pixel );
		if ( !m_hFallbackColorTexture.is_valid() ) {
			Ref<RDTextureFormat> format; format.instantiate();
			format->set_width( 1u ); format->set_height( 1u ); format->set_depth( 1u );
			format->set_array_layers( 1u ); format->set_mipmaps( 1u );
			format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
			format->set_format( RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM );
			format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT );
			m_hFallbackColorTexture = rd->texture_create( format, view, initial_data );
			if ( !t3dRdForwardLightAdopt( m_context, rd, m_hFallbackColorTexture, "T3D.ForwardLighting.FallbackColor",
					Terrain3DRdContext::RESOURCE_TEXTURE, Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE, 4u ) ) {
				m_context.endResourceRebuild(); setError( "Failed to create Forward+ fallback color texture" ); return false;
			}
		}
		if ( !m_hFallbackColorArrayTexture.is_valid() ) {
			Ref<RDTextureFormat> format; format.instantiate();
			format->set_width( 1u ); format->set_height( 1u ); format->set_depth( 1u );
			format->set_array_layers( 1u ); format->set_mipmaps( 1u );
			format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D_ARRAY );
			format->set_format( RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM );
			format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT );
			m_hFallbackColorArrayTexture = rd->texture_create( format, view, initial_data );
			if ( !t3dRdForwardLightAdopt( m_context, rd, m_hFallbackColorArrayTexture, "T3D.ForwardLighting.FallbackColorArray",
					Terrain3DRdContext::RESOURCE_TEXTURE, Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE, 4u ) ) {
				m_context.endResourceRebuild(); setError( "Failed to create Forward+ fallback color-array texture" ); return false;
			}
		}
	}
	if ( !m_hLinearSampler.is_valid() ) {
		Ref<RDSamplerState> sampler; sampler.instantiate();
		sampler->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		sampler->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		sampler->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		sampler->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hLinearSampler = rd->sampler_create( sampler );
		if ( !t3dRdForwardLightAdopt( m_context, rd, m_hLinearSampler, "T3D.ForwardLighting.LinearSampler",
				Terrain3DRdContext::RESOURCE_SAMPLER, Terrain3DRdContext::MEMORY_NONE ) ) {
			m_context.endResourceRebuild(); setError( "Failed to create Forward+ scene integration sampler" ); return false;
		}
	}
	m_context.endResourceRebuild();
	return true;
}

RID Terrain3DRdForwardLighting::resolveShadowTexture( const RID &rNative ) const {
	return rNative.is_valid() ? rNative : m_hFallbackShadowTexture;
}
RID Terrain3DRdForwardLighting::resolveShadowSampler( const RID &rNative ) const {
	return rNative.is_valid() ? rNative : m_hFallbackShadowSampler;
}
RID Terrain3DRdForwardLighting::resolveStorageBuffer( const RID &rNative ) const {
	return rNative.is_valid() ? rNative : m_hFallbackStorageBuffer;
}
RID Terrain3DRdForwardLighting::resolveColorTexture( const RID &rNative ) const {
	return rNative.is_valid() ? rNative : m_hFallbackColorTexture;
}
RID Terrain3DRdForwardLighting::resolveColorArrayTexture( const RID &rNative ) const {
	return rNative.is_valid() ? rNative : m_hFallbackColorArrayTexture;
}

bool Terrain3DRdForwardLighting::prepare( const t3dRdForwardPassContext_t &rContext, const uint32_t nDefaultSurfaceMask ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE ||
			( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION ) ) return true;
	if ( !ensureOwnedResources() ) return false;
	if ( !rContext.m_hDirectionalLightBuffer.is_valid() || !rContext.m_hOmniLightBuffer.is_valid() ||
			!rContext.m_hSpotLightBuffer.is_valid() || !rContext.m_hAreaLightBuffer.is_valid() || !rContext.m_hClusterBuffer.is_valid() ) {
		setError( "Forward+ lighting packet is missing a native light or cluster buffer" ); return false;
	}
	if ( rContext.m_nClusterWidth == 0u || rContext.m_nClusterTypeSize == 0u || rContext.m_nClusterMaxElementsDiv32 == 0u ||
			rContext.m_fCameraZFar <= 0.0f ) {
		setError( "Forward+ lighting packet has invalid Godot 4.7.1 cluster/depth metadata" ); return false;
	}

	t3dRdForwardLightKey_t key;
	key.m_hDirectional = rContext.m_hDirectionalLightBuffer;
	key.m_hOmni = rContext.m_hOmniLightBuffer;
	key.m_hSpot = rContext.m_hSpotLightBuffer;
	key.m_hArea = rContext.m_hAreaLightBuffer;
	key.m_hCluster = rContext.m_hClusterBuffer;
	key.m_hShadowAtlas = resolveShadowTexture( rContext.m_hShadowAtlasTexture );
	key.m_hDirectionalShadowAtlas = resolveShadowTexture( rContext.m_hDirectionalShadowAtlasTexture );
	key.m_hShadowSampler = resolveShadowSampler( rContext.m_hNativeShadowSampler );
	key.m_hReflectionProbeBuffer = resolveStorageBuffer( rContext.m_hReflectionProbeBuffer );
	key.m_hReflectionAtlas = resolveColorArrayTexture( rContext.m_hReflectionAtlasTexture );
	key.m_hDecalBuffer = resolveStorageBuffer( rContext.m_hDecalBuffer );
	key.m_hDecalAtlas = resolveColorTexture( rContext.m_hDecalAtlasTexture );
	key.m_hDecalAtlasSrgb = resolveColorTexture( rContext.m_hDecalAtlasSrgbTexture );
	if ( !m_bKeyValid || !( key == m_key ) ) {
		m_context.beginResourceRebuild(); freeSets(); m_context.endResourceRebuild();
		m_key = key; m_bKeyValid = true; ++m_nSetRebuilds; ++m_nSceneResourceChanges;
	}

	t3dRdForwardLightFrame_t frame;
	frame.m_nClusterShift = rContext.m_nClusterShift;
	frame.m_nClusterWidth = rContext.m_nClusterWidth;
	frame.m_nClusterTypeSize = rContext.m_nClusterTypeSize;
	frame.m_nClusterMaxElementsDiv32 = rContext.m_nClusterMaxElementsDiv32;
	frame.m_nDirectionalLightCount = MIN( rContext.m_nDirectionalLightCount, 8u );
	frame.m_nDefaultSurfaceMask = nDefaultSurfaceMask;
	frame.m_nShadowFlags = ( rContext.m_hShadowAtlasTexture.is_valid() ? 1u : 0u ) |
			( rContext.m_hDirectionalShadowAtlasTexture.is_valid() ? 2u : 0u ) |
			( rContext.m_hNativeShadowSampler.is_valid() ? 4u : 0u );
	frame.m_nDirectionalSoftShadows = rContext.m_bDirectionalLightSoftShadows ? 1u : 0u;
	frame.m_fCameraZNear = rContext.m_fCameraZNear;
	frame.m_fCameraZFar = rContext.m_fCameraZFar;
	frame.m_fInvCameraZFar = 1.0f / rContext.m_fCameraZFar;
	frame.m_fReflectionAtlasBorderSize = MAX( rContext.m_fReflectionAtlasBorderSize, 0.0f );
	frame.m_nRenderWidth = MAX( rContext.m_renderSize.x, 0 );
	frame.m_nRenderHeight = MAX( rContext.m_renderSize.y, 0 );
	frame.m_nClusterMaxElements = rContext.m_nClusterMaxElements;
	frame.m_nSceneFeatureFlags =
			( rContext.m_hReflectionProbeBuffer.is_valid() && rContext.m_hReflectionAtlasTexture.is_valid() ? 1u : 0u ) |
			( rContext.m_hDecalBuffer.is_valid() && rContext.m_hDecalAtlasTexture.is_valid() &&
					rContext.m_hDecalAtlasSrgbTexture.is_valid() ? 2u : 0u );
	std::memcpy( m_frameBytes.ptrw(), &frame, sizeof( frame ) );
	if ( m_context.getDevice()->buffer_update( m_hFrameBuffer, 0u, sizeof( frame ), m_frameBytes ) != OK ) {
		setError( "Failed to update Forward+ lighting metadata UBO" ); return false;
	}
	m_context.recordBufferUpdate( sizeof( frame ) );
	++m_nFrameUpdates;
	m_lastError = String();
	return true;
}

RID Terrain3DRdForwardLighting::getSet( const RID &rShader ) const {
	for ( const t3dRdForwardLightSet_t &entry : m_sets ) if ( entry.m_hShader == rShader ) return entry.m_hSet;
	return RID();
}

RID Terrain3DRdForwardLighting::ensureSet( const RID &rShader ) {
	if ( !rShader.is_valid() || !m_bKeyValid ) return RID();
	for ( const t3dRdForwardLightSet_t &entry : m_sets ) if ( entry.m_hShader == rShader ) return entry.m_hSet;
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) return RID();
	TypedArray<RDUniform> uniforms;
	uniforms.push_back( t3dRdForwardLightUniform( 0, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_hFrameBuffer ) );
	uniforms.push_back( t3dRdForwardLightUniform( 1, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_key.m_hDirectional ) );
	uniforms.push_back( t3dRdForwardLightUniform( 2, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hOmni ) );
	uniforms.push_back( t3dRdForwardLightUniform( 3, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hSpot ) );
	uniforms.push_back( t3dRdForwardLightUniform( 4, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hArea ) );
	uniforms.push_back( t3dRdForwardLightUniform( 5, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hCluster ) );
	uniforms.push_back( t3dRdForwardLightUniform( 6, RenderingDevice::UNIFORM_TYPE_SAMPLER, m_key.m_hShadowSampler ) );
	uniforms.push_back( t3dRdForwardLightUniform( 7, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hShadowAtlas ) );
	uniforms.push_back( t3dRdForwardLightUniform( 8, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hDirectionalShadowAtlas ) );
	uniforms.push_back( t3dRdForwardLightUniform( 9, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hReflectionProbeBuffer ) );
	uniforms.push_back( t3dRdForwardLightUniform( 10, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hReflectionAtlas ) );
	uniforms.push_back( t3dRdForwardLightUniform( 11, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hDecalBuffer ) );
	uniforms.push_back( t3dRdForwardLightUniform( 12, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hDecalAtlas ) );
	uniforms.push_back( t3dRdForwardLightUniform( 13, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hDecalAtlasSrgb ) );
	uniforms.push_back( t3dRdForwardLightUniform( 14, RenderingDevice::UNIFORM_TYPE_SAMPLER, m_hLinearSampler ) );
	m_context.beginResourceRebuild();
	RID set = rd->uniform_set_create( uniforms, rShader, 2u );
	const bool adopted = set.is_valid() && m_context.registerResource( set,
			String( "T3D.ForwardLighting.Set." ) + String::num_uint64( rShader.get_id() ),
			Terrain3DRdContext::RESOURCE_UNIFORM_SET, Terrain3DRdContext::MEMORY_NONE, 0u );
	m_context.endResourceRebuild();
	if ( !adopted ) {
		if ( set.is_valid() ) rd->free_rid( set );
		setError( "Failed to create Terrain3D Forward+ lighting descriptor set" );
		return RID();
	}
	m_sets.push_back( { rShader, set } );
	return set;
}

void Terrain3DRdForwardLighting::invalidateBorrowedState( void ) {
	m_context.beginResourceRebuild();
	freeSets();
	m_context.endResourceRebuild();
	m_key = t3dRdForwardLightKey_t{};
	m_bKeyValid = false;
}

void Terrain3DRdForwardLighting::shutdown( void ) {
	freeSets();
	m_context.freeResource( m_hLinearSampler );
	m_context.freeResource( m_hFallbackColorArrayTexture, 4u );
	m_context.freeResource( m_hFallbackColorTexture, 4u );
	m_context.freeResource( m_hFallbackStorageBuffer, 256u );
	m_context.freeResource( m_hFallbackShadowSampler );
	m_context.freeResource( m_hFallbackShadowTexture, sizeof( float ) );
	m_context.freeResource( m_hFrameBuffer, sizeof( t3dRdForwardLightFrame_t ) );
	m_bKeyValid = false;
	m_key = t3dRdForwardLightKey_t{};
	m_nDeviceGeneration = 0u;
}

Dictionary Terrain3DRdForwardLighting::getDiagnostics( void ) const {
	Dictionary result;
	result["forward_light_sets"] = ( int64_t )( m_sets.size() );
	result["forward_light_set_rebuilds"] = ( int64_t )( m_nSetRebuilds );
	result["forward_light_frame_updates"] = ( int64_t )( m_nFrameUpdates );
	result["forward_scene_resource_changes"] = ( int64_t )( m_nSceneResourceChanges );
	result["forward_reflection_resources"] = m_bKeyValid && m_key.m_hReflectionProbeBuffer.is_valid() && m_key.m_hReflectionAtlas.is_valid();
	result["forward_decal_resources"] = m_bKeyValid && m_key.m_hDecalBuffer.is_valid() && m_key.m_hDecalAtlas.is_valid() && m_key.m_hDecalAtlasSrgb.is_valid();
	result["forward_light_resources_ready"] = m_bKeyValid;
	result["forward_light_last_error"] = m_lastError;
	return result;
}
