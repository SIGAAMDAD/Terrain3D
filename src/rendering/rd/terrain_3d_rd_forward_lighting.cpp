// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/infrastructure/terrain_3d_rd_binding_builder.h"
#include "rendering/rd/terrain_3d_rd_forward_lighting.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"
#include "rendering/rd/infrastructure/terrain_3d_gpu_upload_service.h"
#include "rendering/rd/terrain_3d_rd_persistent.h"

#include <string.h>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>

Terrain3DRdForwardLighting::Terrain3DRdForwardLighting( Terrain3DRdContext &rContext ) :
        m_context( rContext ), m_descriptors( rContext, Terrain3DRdContext::OWNER_FORWARD_LIGHTING ) {
	m_frameBytes.resize( sizeof( t3dRdForwardLightFrame_t ) );
}

Terrain3DRdForwardLighting::~Terrain3DRdForwardLighting( void ) {
	shutdown();
}

void Terrain3DRdForwardLighting::setError( const String &rError ) {
	m_lastError = rError;
}

void Terrain3DRdForwardLighting::freeSets( void ) {
	m_descriptors.clear();
}

bool Terrain3DRdForwardLighting::ensureOwnedResources( void ) {
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return false;
	}
	const uint64_t generation = m_context.getDeviceGeneration();
	if ( m_nDeviceGeneration != 0u && m_nDeviceGeneration != generation ) {
		m_hFrameBuffer = RID();
		m_hFallbackShadowTexture = RID();
		m_hFallbackShadowSampler = RID();
		m_hFallbackStorageBuffer = RID();
		m_hFallbackColorTexture = RID();
		m_hFallbackColorArrayTexture = RID();
		m_hFallbackRadianceTexture = RID();
		m_hFallbackRadianceArrayTexture = RID();
		m_hLinearSampler = RID();
		m_descriptors.abandon();
		m_bKeyValid = false;
		m_nFrameGeneration = 0u;
	}
	m_nDeviceGeneration = generation;
	if ( m_hFrameBuffer.is_valid() && m_hFallbackShadowTexture.is_valid() && m_hFallbackShadowSampler.is_valid()
	        && m_hFallbackStorageBuffer.is_valid() && m_hFallbackColorTexture.is_valid() && m_hFallbackColorArrayTexture.is_valid()
	        && m_hFallbackRadianceTexture.is_valid() && m_hFallbackRadianceArrayTexture.is_valid() && m_hLinearSampler.is_valid() ) {
		return true;
	}

	m_context.beginResourceRebuild();
	if ( !m_hFrameBuffer.is_valid() ) {
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING )
	                        .createUniformBuffer( m_hFrameBuffer,
	                                "T3D.ForwardLighting.Frame",
	                                sizeof( t3dRdForwardLightFrame_t ),
	                                Terrain3DRdContext::MEMORY_UNIFORM,
	                                m_bPersistentFrameBuffer ? t3dRdPersistentCreationBits()
	                                                         : BitField<RenderingDevice::BufferCreationBits>( 0 ) ) ) {
			m_context.endResourceRebuild();
			setError( "Failed to create Forward+ lighting frame UBO" );
			return false;
		}
	}
	if ( !m_hFallbackShadowTexture.is_valid() ) {
		Ref<RDTextureFormat> format;
		format.instantiate();
		format->set_width( 1u );
		format->set_height( 1u );
		format->set_depth( 1u );
		format->set_array_layers( 1u );
		format->set_mipmaps( 1u );
		format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
		format->set_format( RenderingDevice::DATA_FORMAT_D32_SFLOAT );
		format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT );
		Ref<RDTextureView> view;
		view.instantiate();
		TypedArray<PackedByteArray> empty_data;
		m_hFallbackShadowTexture = rd->texture_create( format, view, empty_data );
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING ).adopt( rd,
		             m_hFallbackShadowTexture,
		             "T3D.ForwardLighting.FallbackShadow",
		             Terrain3DRdContext::RESOURCE_TEXTURE,
		             Terrain3DRdContext::MEMORY_SHADOW,
		             sizeof( float ) ) ) {
			m_context.endResourceRebuild();
			setError( "Failed to create Forward+ fallback shadow texture" );
			return false;
		}
	}
	if ( !m_hFallbackShadowSampler.is_valid() ) {
		Ref<RDSamplerState> sampler;
		sampler.instantiate();
		sampler->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		sampler->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_enable_compare( true );
		sampler->set_compare_op( RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL );
		m_hFallbackShadowSampler = rd->sampler_create( sampler );
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING ).adopt( rd,
		             m_hFallbackShadowSampler,
		             "T3D.ForwardLighting.FallbackShadowSampler",
		             Terrain3DRdContext::RESOURCE_SAMPLER,
		             Terrain3DRdContext::MEMORY_NONE ) ) {
			m_context.endResourceRebuild();
			setError( "Failed to create Forward+ fallback shadow sampler" );
			return false;
		}
	}
	if ( !m_hFallbackStorageBuffer.is_valid() ) {
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING )
	                        .createStorageBuffer( m_hFallbackStorageBuffer,
	                                "T3D.ForwardLighting.FallbackStorage",
	                                256u,
	                                Terrain3DRdContext::MEMORY_UNIFORM ) ) {
			m_context.endResourceRebuild();
			setError( "Failed to create Forward+ fallback scene storage buffer" );
			return false;
		}
	}
	if ( !m_hFallbackColorTexture.is_valid() || !m_hFallbackColorArrayTexture.is_valid() ) {
		Ref<RDTextureView> view;
		view.instantiate();
		PackedByteArray pixel;
		pixel.resize( 4 );
		memset( pixel.ptrw(), 0, 4 );
		TypedArray<PackedByteArray> initial_data;
		initial_data.push_back( pixel );
		if ( !m_hFallbackColorTexture.is_valid() ) {
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
			m_hFallbackColorTexture = rd->texture_create( format, view, initial_data );
			if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING ).adopt( rd,
			             m_hFallbackColorTexture,
			             "T3D.ForwardLighting.FallbackColor",
			             Terrain3DRdContext::RESOURCE_TEXTURE,
			             Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
			             4u ) ) {
				m_context.endResourceRebuild();
				setError( "Failed to create Forward+ fallback color texture" );
				return false;
			}
		}
		if ( !m_hFallbackColorArrayTexture.is_valid() ) {
			Ref<RDTextureFormat> format;
			format.instantiate();
			format->set_width( 1u );
			format->set_height( 1u );
			format->set_depth( 1u );
			format->set_array_layers( 1u );
			format->set_mipmaps( 1u );
			format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D_ARRAY );
			format->set_format( RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM );
			format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT );
			m_hFallbackColorArrayTexture = rd->texture_create( format, view, initial_data );
			if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING ).adopt( rd,
			             m_hFallbackColorArrayTexture,
			             "T3D.ForwardLighting.FallbackColorArray",
			             Terrain3DRdContext::RESOURCE_TEXTURE,
			             Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
			             4u ) ) {
				m_context.endResourceRebuild();
				setError( "Failed to create Forward+ fallback color-array texture" );
				return false;
			}
		}
	}
	if ( !m_hFallbackRadianceTexture.is_valid() || !m_hFallbackRadianceArrayTexture.is_valid() ) {
		Ref<RDTextureView> view;
		view.instantiate();
		PackedByteArray pixel;
		pixel.resize( 4 );
		memset( pixel.ptrw(), 0, 4 );
		TypedArray<PackedByteArray> initial_data;
		initial_data.push_back( pixel );
		if ( !m_hFallbackRadianceTexture.is_valid() ) {
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
			m_hFallbackRadianceTexture = rd->texture_create( format, view, initial_data );
			if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING ).adopt( rd,
			             m_hFallbackRadianceTexture,
			             "T3D.ForwardLighting.FallbackRadiance",
			             Terrain3DRdContext::RESOURCE_TEXTURE,
			             Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
			             4u ) ) {
				m_context.endResourceRebuild();
				setError( "Failed to create Forward+ fallback 2D radiance texture" );
				return false;
			}
		}
		if ( !m_hFallbackRadianceArrayTexture.is_valid() ) {
			Ref<RDTextureFormat> format;
			format.instantiate();
			format->set_width( 1u );
			format->set_height( 1u );
			format->set_depth( 1u );
			format->set_array_layers( 1u );
			format->set_mipmaps( 1u );
			format->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D_ARRAY );
			format->set_format( RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM );
			format->set_usage_bits( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT );
			m_hFallbackRadianceArrayTexture = rd->texture_create( format, view, initial_data );
			if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING ).adopt( rd,
			             m_hFallbackRadianceArrayTexture,
			             "T3D.ForwardLighting.FallbackRadianceArray",
			             Terrain3DRdContext::RESOURCE_TEXTURE,
			             Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
			             4u ) ) {
				m_context.endResourceRebuild();
				setError( "Failed to create Forward+ fallback array radiance texture" );
				return false;
			}
		}
	}
	if ( !m_hLinearSampler.is_valid() ) {
		Ref<RDSamplerState> sampler;
		sampler.instantiate();
		sampler->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		sampler->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		sampler->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		sampler->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		sampler->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hLinearSampler = rd->sampler_create( sampler );
		if ( !Terrain3DRdResourceService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING ).adopt( rd,
		             m_hLinearSampler,
		             "T3D.ForwardLighting.LinearSampler",
		             Terrain3DRdContext::RESOURCE_SAMPLER,
		             Terrain3DRdContext::MEMORY_NONE ) ) {
			m_context.endResourceRebuild();
			setError( "Failed to create Forward+ scene integration sampler" );
			return false;
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
RID Terrain3DRdForwardLighting::resolveRadianceTexture( const RID &rNative, const bool bArray ) const {
	if ( rNative.is_valid() ) {
		return rNative;
	}
	return bArray ? m_hFallbackRadianceArrayTexture : m_hFallbackRadianceTexture;
}

bool Terrain3DRdForwardLighting::prepare(
        const t3dRdForwardPassContext_t &rContext, const uint32_t nDefaultSurfaceMask, const bool bOneSun ) {
	if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE
	        || ( rContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && rContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION ) ) {
		return true;
	}
	if ( !ensureOwnedResources() ) {
		return false;
	}
	if ( !rContext.m_hDirectionalLightBuffer.is_valid() ) {
		setError( "Forward+ lighting packet is missing the native directional-light buffer" );
		return false;
	}
	if ( !bOneSun
	        && ( !rContext.m_hOmniLightBuffer.is_valid() || !rContext.m_hSpotLightBuffer.is_valid()
	                || !rContext.m_hAreaLightBuffer.is_valid() || !rContext.m_hClusterBuffer.is_valid() ) ) {
		setError( "Forward+ lighting packet is missing a native local-light or cluster buffer" );
		return false;
	}
	if ( rContext.m_fCameraZFar <= 0.0f
	        || ( !bOneSun
	                && ( rContext.m_nClusterWidth == 0u || rContext.m_nClusterTypeSize == 0u
	                        || rContext.m_nClusterMaxElementsDiv32 == 0u ) ) ) {
		setError( "Forward+ lighting packet has invalid Godot 4.7.2 cluster/depth metadata" );
		return false;
	}

	t3dRdForwardLightKey_t key;
	key.m_hDirectional = rContext.m_hDirectionalLightBuffer;
	key.m_bOneSun = bOneSun;
	if ( !bOneSun ) {
		key.m_hOmni = rContext.m_hOmniLightBuffer;
		key.m_hSpot = rContext.m_hSpotLightBuffer;
		key.m_hArea = rContext.m_hAreaLightBuffer;
		key.m_hCluster = rContext.m_hClusterBuffer;
		key.m_hShadowAtlas = resolveShadowTexture( rContext.m_hShadowAtlasTexture );
		key.m_hReflectionProbeBuffer = resolveStorageBuffer( rContext.m_hReflectionProbeBuffer );
		key.m_hReflectionAtlas = resolveColorArrayTexture( rContext.m_hReflectionAtlasTexture );
		key.m_hDecalBuffer = resolveStorageBuffer( rContext.m_hDecalBuffer );
		key.m_hDecalAtlas = resolveColorTexture( rContext.m_hDecalAtlasTexture );
		key.m_hDecalAtlasSrgb = resolveColorTexture( rContext.m_hDecalAtlasSrgbTexture );
	}
	key.m_hDirectionalShadowAtlas = resolveShadowTexture( rContext.m_hDirectionalShadowAtlasTexture );
	key.m_hShadowSampler = resolveShadowSampler( rContext.m_hNativeShadowSampler );
	key.m_nBorrowedGeneration = bOneSun ? 0u : rContext.m_nResourceGeneration;
	const bool radiance_array = ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	const bool radiance_valid = rContext.m_hRadianceTexture.is_valid();
	key.m_hRadiance = resolveRadianceTexture( rContext.m_hRadianceTexture, radiance_array );
	const bool radiance_resource_changed = m_bKeyValid && key.m_hRadiance != m_key.m_hRadiance;
	const bool resources_changed = !m_bKeyValid || !( key == m_key );
	if ( resources_changed ) {
		m_context.beginResourceRebuild();
		freeSets();
		m_context.endResourceRebuild();
		m_key = key;
		m_bKeyValid = true;
		++m_nSceneResourceChanges;
		if ( radiance_resource_changed ) {
			++m_nEnvironmentResourceChanges;
		}
	}
	if ( !resources_changed && m_nFrameGeneration == rContext.m_nFrameGeneration && m_nFrameSurfaceMask == nDefaultSurfaceMask ) {
		return true;
	}

	t3dRdForwardLightFrame_t frame;
	initFrame( &frame );
	frame.m_nClusterShift = bOneSun ? 0u : rContext.m_nClusterShift;
	frame.m_nClusterWidth = bOneSun ? 0u : rContext.m_nClusterWidth;
	frame.m_nClusterTypeSize = bOneSun ? 0u : rContext.m_nClusterTypeSize;
	frame.m_nClusterMaxElementsDiv32 = bOneSun ? 0u : rContext.m_nClusterMaxElementsDiv32;
	frame.m_nDirectionalLightCount = MIN( rContext.m_nDirectionalLightCount, bOneSun ? 1u : 8u );
	frame.m_nDefaultSurfaceMask = nDefaultSurfaceMask;
	frame.m_nShadowFlags = ( !bOneSun && rContext.m_hShadowAtlasTexture.is_valid() ? 1u : 0u )
	        | ( rContext.m_hDirectionalShadowAtlasTexture.is_valid() ? 2u : 0u ) | ( rContext.m_hNativeShadowSampler.is_valid() ? 4u : 0u );
	frame.m_nDirectionalSoftShadows = rContext.m_bDirectionalLightSoftShadows ? 1u : 0u;
	frame.m_fCameraZNear = rContext.m_fCameraZNear;
	frame.m_fCameraZFar = rContext.m_fCameraZFar;
	frame.m_fInvCameraZFar = 1.0f / rContext.m_fCameraZFar;
	frame.m_fReflectionAtlasBorderSize = MAX( rContext.m_fReflectionAtlasBorderSize, 0.0f );
	frame.m_nRenderWidth = MAX( rContext.m_renderSize.x, 0 );
	frame.m_nRenderHeight = MAX( rContext.m_renderSize.y, 0 );
	frame.m_nClusterMaxElements = bOneSun ? 0u : rContext.m_nClusterMaxElements;
	frame.m_nSceneFeatureFlags =
	        ( !bOneSun && rContext.m_hReflectionProbeBuffer.is_valid() && rContext.m_hReflectionAtlasTexture.is_valid() ? 1u : 0u )
	        | ( !bOneSun && rContext.m_hDecalBuffer.is_valid() && rContext.m_hDecalAtlasTexture.is_valid()
	                                && rContext.m_hDecalAtlasSrgbTexture.is_valid()
	                        ? 2u
	                        : 0u )
	        | ( ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_USE_AMBIENT_LIGHT ) != 0u ? 4u : 0u )
	        | ( radiance_valid && ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_USE_AMBIENT_CUBEMAP ) != 0u ? 8u : 0u )
	        | ( radiance_valid && ( rContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_USE_REFLECTION_CUBEMAP ) != 0u ? 16u : 0u )
	        | ( radiance_array ? 32u : 0u );
	memcpy( frame.m_szAmbientLightColorEnergy, rContext.m_szAmbientLightColorEnergy, sizeof( frame.m_szAmbientLightColorEnergy ) );
	frame.m_szEnvironmentParams0[0] = rContext.m_fAmbientColorSkyMix;
	frame.m_szEnvironmentParams0[1] = rContext.m_fEmissiveExposureNormalization;
	frame.m_szEnvironmentParams0[2] = rContext.m_fIblExposureNormalization;
	frame.m_szEnvironmentParams0[3] = rContext.m_fRadiancePixelSize;
	frame.m_szEnvironmentParams1[0] = rContext.m_fRadianceBorderSize;
	memcpy( frame.m_szRadianceInverseXform, rContext.m_szRadianceInverseXform, sizeof( frame.m_szRadianceInverseXform ) );
	const bool missing_radiance = !radiance_valid
	        && ( rContext.m_nEnvironmentFlags & ( T3D_RD_FORWARD_ENV_USE_AMBIENT_CUBEMAP | T3D_RD_FORWARD_ENV_USE_REFLECTION_CUBEMAP ) )
	                != 0u;
	if ( missing_radiance && m_nLastInvalidEnvironmentFrameGeneration != rContext.m_nFrameGeneration ) {
		++m_nEnvironmentInvalidResourceCount;
		m_nLastInvalidEnvironmentFrameGeneration = rContext.m_nFrameGeneration;
	}
	m_nLastEnvironmentFlags = rContext.m_nEnvironmentFlags;
	memcpy( m_szLastAmbientLightColorEnergy, rContext.m_szAmbientLightColorEnergy, sizeof( m_szLastAmbientLightColorEnergy ) );
	m_fLastAmbientColorSkyMix = rContext.m_fAmbientColorSkyMix;
	m_fLastIblExposureNormalization = rContext.m_fIblExposureNormalization;
	m_fLastEmissiveExposureNormalization = rContext.m_fEmissiveExposureNormalization;
	m_bLastRadianceValid = radiance_valid;
	++m_nEnvironmentFrameUpdates;
	if ( !Terrain3DGpuUploadService( m_context, Terrain3DRdContext::OWNER_FORWARD_LIGHTING, T3D_GPU_HOT_FRAME )
	                        .writeWhole( m_hFrameBuffer,
	                                sizeof( frame ),
	                                &frame,
	                                sizeof( frame ),
	                                m_frameBytes,
	                                m_bPersistentFrameBuffer ) ) {
		setError( "Failed to update Forward+ lighting metadata UBO" );
		return false;
	}
	m_nFrameGeneration = rContext.m_nFrameGeneration;
	m_nFrameSurfaceMask = nDefaultSurfaceMask;
	m_lastError = String();
	return true;
}

RID Terrain3DRdForwardLighting::getSet( const RID &rShader ) const {
	return m_bKeyValid ? m_descriptors.find( descriptorKey( rShader ) ) : RID();
}

Terrain3DRdForwardLighting::t3dRdForwardDescriptorKey_t Terrain3DRdForwardLighting::descriptorKey( const RID &rShader ) const {
	t3dRdForwardDescriptorKey_t key;
	key.shader = rShader;
	key.frame = m_hFrameBuffer;
	key.sampler = m_hLinearSampler;
	key.resources = m_key;
	key.deviceGeneration = m_nDeviceGeneration;
	return key;
}

RID Terrain3DRdForwardLighting::ensureSet( const RID &rShader ) {
	if ( !rShader.is_valid() || !m_bKeyValid ) {
		return RID();
	}
	const t3dRdForwardDescriptorKey_t key = descriptorKey( rShader );
	const RID existing = m_descriptors.find( key );
	if ( existing.is_valid() ) {
		return existing;
	}
	RenderingDevice *const rd = m_context.getDevice();
	if ( !rd ) {
		return RID();
	}
	TypedArray<Ref<RDUniform>> uniforms;
	uniforms.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_FORWARD_LIGHTING_FRAME_BINDING,
	        m_bPersistentFrameBuffer ? RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC : RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER,
	        m_hFrameBuffer ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single(
	        T3D_RD_FORWARD_LIGHTING_DIRECTIONAL_BINDING, RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER, m_key.m_hDirectional ) );
	if ( !m_key.m_bOneSun ) {
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_OMNI_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hOmni ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_SPOT_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hSpot ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_AREA_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hArea ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_CLUSTER_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hCluster ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_SHADOW_SAMPLER_BINDING, RenderingDevice::UNIFORM_TYPE_SAMPLER, m_key.m_hShadowSampler ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_SHADOW_ATLAS_BINDING, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hShadowAtlas ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_FORWARD_LIGHTING_DIRECTIONAL_SHADOW_BINDING,
		        RenderingDevice::UNIFORM_TYPE_TEXTURE,
		        m_key.m_hDirectionalShadowAtlas ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_FORWARD_LIGHTING_REFLECTION_BUFFER_BINDING,
		        RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
		        m_key.m_hReflectionProbeBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_REFLECTION_ATLAS_BINDING, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hReflectionAtlas ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_DECAL_BUFFER_BINDING, RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, m_key.m_hDecalBuffer ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_DECAL_ATLAS_BINDING, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hDecalAtlas ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_DECAL_ATLAS_SRGB_BINDING, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hDecalAtlasSrgb ) );
	} else {
		// P06 one-sun layout keeps directional shadows and environment radiance,
		// but does not bind local-light, cluster, decal, or reflection-probe resources.
		uniforms.push_back( Terrain3DRdBindingBuilder::single(
		        T3D_RD_FORWARD_LIGHTING_SHADOW_SAMPLER_BINDING, RenderingDevice::UNIFORM_TYPE_SAMPLER, m_key.m_hShadowSampler ) );
		uniforms.push_back( Terrain3DRdBindingBuilder::single( T3D_RD_FORWARD_LIGHTING_DIRECTIONAL_SHADOW_BINDING,
		        RenderingDevice::UNIFORM_TYPE_TEXTURE,
		        m_key.m_hDirectionalShadowAtlas ) );
	}
	uniforms.push_back( Terrain3DRdBindingBuilder::single(
	        T3D_RD_FORWARD_LIGHTING_SCENE_SAMPLER_BINDING, RenderingDevice::UNIFORM_TYPE_SAMPLER, m_hLinearSampler ) );
	uniforms.push_back( Terrain3DRdBindingBuilder::single(
	        T3D_RD_FORWARD_LIGHTING_RADIANCE_BINDING, RenderingDevice::UNIFORM_TYPE_TEXTURE, m_key.m_hRadiance ) );
	m_context.beginResourceRebuild();
	const RID set = m_descriptors.getOrCreate( key,
	        String( "T3D.ForwardLighting.Set." ) + String::num_uint64( rShader.get_id() ),
	        uniforms,
	        rShader,
	        T3D_RD_FORWARD_LIGHTING_DESCRIPTOR_SET );
	m_context.endResourceRebuild();
	if ( !set.is_valid() ) {
		setError( "Failed to create Terrain3D Forward+ lighting descriptor set" );
		return RID();
	}
	return set;
}

void Terrain3DRdForwardLighting::invalidateBorrowedState( void ) {
	m_context.beginResourceRebuild();
	freeSets();
	m_context.endResourceRebuild();
	m_key = t3dRdForwardLightKey_t{};
	m_bKeyValid = false;
	m_nFrameGeneration = 0u;
}

void Terrain3DRdForwardLighting::shutdown( void ) {
	freeSets();
	m_context.freeResource( m_hLinearSampler );
	m_context.freeResource( m_hFallbackRadianceArrayTexture, 4u );
	m_context.freeResource( m_hFallbackRadianceTexture, 4u );
	m_context.freeResource( m_hFallbackColorArrayTexture, 4u );
	m_context.freeResource( m_hFallbackColorTexture, 4u );
	m_context.freeResource( m_hFallbackStorageBuffer, 256u );
	m_context.freeResource( m_hFallbackShadowSampler );
	m_context.freeResource( m_hFallbackShadowTexture, sizeof( float ) );
	m_context.freeResource( m_hFrameBuffer, sizeof( t3dRdForwardLightFrame_t ) );
	m_bKeyValid = false;
	m_key = t3dRdForwardLightKey_t{};
	m_nDeviceGeneration = 0u;
	m_nFrameGeneration = 0u;
}

Dictionary Terrain3DRdForwardLighting::getDiagnostics( void ) const {
	Dictionary result;
	result["forward_light_sets"] = ( int64_t )( m_descriptors.size() );
	result["forward_light_set_rebuilds"] = ( int64_t )( m_nSceneResourceChanges );
	result["forward_light_frame_updates"] = ( int64_t )m_context.getRenderUploadCalls(
	        Terrain3DRdContext::OWNER_FORWARD_LIGHTING, T3D_GPU_HOT_FRAME );
	result["forward_scene_resource_changes"] = ( int64_t )( m_nSceneResourceChanges );
	result["forward_environment_flags"] = ( int64_t )( m_nLastEnvironmentFlags );
	result["forward_environment_ambient_rgb"] = Color( m_szLastAmbientLightColorEnergy[0],
	        m_szLastAmbientLightColorEnergy[1],
	        m_szLastAmbientLightColorEnergy[2],
	        m_szLastAmbientLightColorEnergy[3] );
	result["forward_environment_sky_mix"] = m_fLastAmbientColorSkyMix;
	result["forward_environment_ibl_exposure"] = m_fLastIblExposureNormalization;
	result["forward_environment_emissive_exposure"] = m_fLastEmissiveExposureNormalization;
	result["forward_environment_radiance_valid"] = m_bLastRadianceValid;
	result["forward_environment_radiance_array"] = ( m_nLastEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
	result["forward_environment_frame_updates"] = ( int64_t )( m_nEnvironmentFrameUpdates );
	result["forward_environment_resource_changes"] = ( int64_t )( m_nEnvironmentResourceChanges );
	result["forward_environment_invalid_resource_count"] = ( int64_t )( m_nEnvironmentInvalidResourceCount );
	result["forward_reflection_resources"] =
	        m_bKeyValid && m_key.m_hReflectionProbeBuffer.is_valid() && m_key.m_hReflectionAtlas.is_valid();
	result["forward_decal_resources"] =
	        m_bKeyValid && m_key.m_hDecalBuffer.is_valid() && m_key.m_hDecalAtlas.is_valid() && m_key.m_hDecalAtlasSrgb.is_valid();
	result["forward_one_sun"] = m_bKeyValid && m_key.m_bOneSun;
	result["forward_light_resources_ready"] = m_bKeyValid;
	result["forward_light_last_error"] = m_lastError;
	return result;
}
