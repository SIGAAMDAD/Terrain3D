// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypes.h"

#include "rendering/compute/terrain_3d_compute.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "core/utility/logger.h"
#include "core/config/terrain_3d_compiler.h"
#include "data/model/terrain_3d_data.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "rendering/material/terrain_3d_material.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_library.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_binding_builder.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
#include "terrain_3d.h"

#include <EASTL/numeric_limits.h>
#include <string.h>
#include <math.h>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

#define TERRAIN3D_CLAMP01( value ) CLAMP( ( value ), 0.0f, 1.0f )
#define TERRAIN3D_LERP( a, b, t ) ( ( a ) + ( ( b ) - ( a ) ) * ( t ) )
#define TERRAIN3D_UNPACK_COLOR( value, shift ) ( ( float )( ( ( value ) >> ( shift ) ) & 0xffu ) / s_fByteMaxFloat )
#define TERRAIN3DQUANTIZECOLOR( value ) ( uint32_t )( CLAMP( round( ( value ) * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) )

EA_FORCE_INLINE static float t3dComputeBitsToFloat( const uint32_t nBits ) {
	float fValue = 0.0f;
	memcpy( &fValue, &nBits, sizeof( fValue ) );
	return fValue;
}

EA_FORCE_INLINE static uint32_t t3dComputeFloatToBits( const float fValue ) {
	uint32_t bits = 0u;
	memcpy( &bits, &fValue, sizeof( bits ) );
	return bits;
}

EA_FORCE_INLINE static uint32_t t3dComputePackColor( const float fR, const float fG, const float fB, const float fA ) {
	return TERRAIN3DQUANTIZECOLOR( fR ) | ( TERRAIN3DQUANTIZECOLOR( fG ) << 8u ) | ( TERRAIN3DQUANTIZECOLOR( fB ) << 16u )
	        | ( TERRAIN3DQUANTIZECOLOR( fA ) << 24u );
}
static_assert( sizeof( t3dBrushElement_t ) == 16u, "t3dBrushElement_t must match GLSL std430 uvec4 layout" );
static_assert( sizeof( t3dBrushParams_t ) == 52u, "t3dBrushParams_t must match GLSL push-constant layout" );

static uint64_t t3dComputeHashBytes( const uint8_t *pData, const size_t nSize ) {
	uint64_t hash = 1469598103934665603ULL;
	if ( !pData && nSize > 0u ) {
		return 0u;
	}
	return t3dHashFnv1a64( pData, nSize, hash );
}

Terrain3DCompute::~Terrain3DCompute( void ) {
	shutdown();
}

PackedByteArray Terrain3DCompute::copyToBytes( const void *pData, const size_t nSize ) {
	PackedByteArray bytes;
	if ( !pData || nSize == 0u || nSize > ( size_t )( eastl::numeric_limits<int64_t>::max() ) ) {
		return bytes;
	}
	bytes.resize( ( int64_t )( nSize ) );
	memcpy( bytes.ptrw(), pData, nSize );
	return bytes;
}

bool Terrain3DCompute::ensureDevice( void ) {
	if ( m_pRd ) {
		return true;
	}
	if ( m_bDeviceAttempted ) {
		return false;
	}
	m_bDeviceAttempted = true;
	if ( !Terrain3DRdContext::getGlobalDevice() ) {
		setError( "RenderingDevice is unavailable (Compatibility renderer or headless runtime)" );
		return false;
	}
	m_pRd = Terrain3DRdContext::createLocalDevice();
	if ( !m_pRd ) {
		setError( "Could not create a local RenderingDevice" );
		return false;
	}
	m_localBuffers.setDevice( m_pRd );
	return true;
}

bool Terrain3DCompute::ensureRenderDevice( void ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	if ( m_pRenderRd ) {
		if ( context.validateDevice() && context.getDevice() == m_pRenderRd ) {
			return true;
		}
		m_pRenderRd = NULL;
		m_bRenderDeviceAttempted = false;
	}
	if ( m_bRenderDeviceAttempted ) {
		return false;
	}
	m_bRenderDeviceAttempted = true;
	if ( !context.isReady() && !context.initialize() ) {
		setError( "Global RenderingDevice is unavailable (Compatibility renderer or headless runtime)" );
		return false;
	}
	m_pRenderRd = context.getDevice();
	if ( !m_pRenderRd ) {
		setError( "Global RenderingDevice is unavailable (Compatibility renderer or headless runtime)" );
		return false;
	}
	return true;
}

bool Terrain3DCompute::ensurePipeline( t3dPipeline_t &rPipeline, const t3dRdShaderId_t id, const String &rName ) {
	if ( rPipeline.m_hPipeline.isValid() ) {
		return true;
	}
	if ( rPipeline.m_bAttempted || !ensureDevice() ) {
		return false;
	}
	const t3dRdShaderCatalogEntry_t entry = t3dRdShaderCatalogGet( id );
	const Ref<RDShaderSPIRV> pSpirv = Terrain3DRdShaderLibrary::resolveComputeShader( id );
	if ( pSpirv.is_null() ) {
		setError( String( "Compute shader cache is not ready: " ) + String( entry.m_path ? entry.m_path : "invalid ID" ) );
		return false;
	}
	rPipeline.m_bAttempted = true;
	if ( !m_localBuffers.createPipeline(
	             pSpirv, rName, rPipeline.m_hShader, rPipeline.m_hPipeline ) ) {
		setError( String( "Failed to create compute pipeline: " ) + String( entry.m_path ? entry.m_path : "invalid ID" ) );
		return false;
	}
	return true;
}

bool Terrain3DCompute::ensureRenderPipeline( t3dPipeline_t &rPipeline, const Ref<RDShaderSPIRV> &rSpirv, const String &rName ) {
	if ( rPipeline.m_hPipeline.isValid() ) {
		return true;
	}
	if ( rPipeline.m_bAttempted || !ensureRenderDevice() ) {
		return false;
	}
	rPipeline.m_bAttempted = true;
	if ( rSpirv.is_null() ) {
		setError( "Displacement compute shader has no compiled SPIR-V" );
		return false;
	}
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	if ( !rPipeline.m_hShader.createTracked( context, T3D_RD_CONTEXT_OWNER_TERRAIN,
	             rName, T3D_RD_SHADER_DISPLACEMENT_COMPUTE, "T3D.Compute.Displacement.Shader" ) ) {
		setError( "Failed to create displacement compute shader" );
		return false;
	}
	if ( !rPipeline.m_hPipeline.createTrackedCompute( context, T3D_RD_CONTEXT_OWNER_TERRAIN,
	             "T3D.Compute.Displacement.Pipeline", rPipeline.m_hShader.rid() ) ) {
		setError( "Failed to create displacement compute pipeline" );
		rPipeline.m_hShader.releaseTracked( context );
		return false;
	}
	return true;
}

void Terrain3DCompute::freeRenderUniformSet( Terrain3DRdUniformSet &rUniformSet ) {
	rUniformSet.releaseTracked( Terrain3DRdContext::getShared() );
}

void Terrain3DCompute::freeRenderPipeline( t3dPipeline_t &rPipeline ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	rPipeline.m_hPipeline.releaseTracked( context );
	rPipeline.m_hShader.releaseTracked( context );
	rPipeline.m_bAttempted = false;
}

bool Terrain3DCompute::ensureBrushCapacity( const uint32_t nBytes ) {
	if ( !m_localBuffers.ensureStorageSet(
	             m_brushStorage, nBytes, m_brushPipeline.m_hShader.rid() ) ) {
		setError( "Failed to rebuild brush compute buffer and uniform set" );
		return false;
	}
	return true;
}

bool Terrain3DCompute::ensureDerivedCapacity( const uint32_t nSlot, const uint32_t nInputBytes, const uint32_t nOutputBytes ) {
	if ( nSlot >= T3D_DERIVED_READBACK_SLOT_COUNT ) {
		return false;
	}
	if ( !m_localBuffers.ensureStoragePairSet( m_derivedStorage[nSlot], nInputBytes,
	             nOutputBytes, m_derivedPipeline.m_hShader.rid() ) ) {
		setError( "Failed to rebuild derived-map compute buffers and uniform set" );
		return false;
	}
	return true;
}

void Terrain3DCompute::freeDisplacementTexture( void ) {
	freeRenderUniformSet( m_hDisplacementUniformSet );
	m_hDisplacementBoundHeight = RID();
	m_hDisplacementBoundControl = RID();
	m_hDisplacementBoundDisplacement = RID();
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	m_hDisplacementRegionBuffer.releaseTracked( context );
	m_hDisplacementMaterialBuffer.releaseTracked( context );
	m_hDisplacementOutputTexture.releaseTracked( Terrain3DRdContext::getShared() );
	m_nDisplacementRegionHash = 0u;
	m_nDisplacementMaterialHash = 0u;
	m_bDisplacementRegionUploaded = false;
	m_bDisplacementMaterialUploaded = false;
	m_nDisplacementWidth = 0u;
	m_nDisplacementHeight = 0u;
}

bool Terrain3DCompute::ensureDisplacementResources( const uint32_t nWidth, const uint32_t nHeight, const Ref<RDShaderSPIRV> &rSpirv ) {
	if ( !m_displacementPipeline.m_hPipeline.isValid() ) {
		m_bPersistentDisplacementBuffers = ensureRenderDevice() && Terrain3DRdGpuBuffer::supportsPersistentMapping( m_pRenderRd ) && rSpirv.is_valid()
		        && rSpirv->has_method( StringName( "set_dynamic_buffers" ) );
		if ( m_bPersistentDisplacementBuffers ) {
			PackedInt64Array dynamic_buffers;
			dynamic_buffers.push_back( 4 ); // set 0, binding 4: region page table
			dynamic_buffers.push_back( 5 ); // set 0, binding 5: material displacement table
			rSpirv->call( StringName( "set_dynamic_buffers" ), dynamic_buffers );
		}
	}
	if ( !ensureRenderPipeline( m_displacementPipeline, rSpirv, "Terrain3D displacement compute" ) ) {
		return false;
	}
	if ( !m_hNearestSampler.isValid() ) {
		Ref<RDSamplerState> pState;
		pState.instantiate();
		pState->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		pState->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		pState->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hNearestSampler.createTracked( Terrain3DRdContext::getShared(), T3D_RD_CONTEXT_OWNER_TERRAIN,
		        "T3D.Compute.Displacement.NearestSampler", pState );
	}
	if ( !m_hLinearRepeatSampler.isValid() ) {
		Ref<RDSamplerState> pState;
		pState.instantiate();
		pState->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		pState->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		pState->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hLinearRepeatSampler.createTracked( Terrain3DRdContext::getShared(), T3D_RD_CONTEXT_OWNER_TERRAIN,
		        "T3D.Compute.Displacement.LinearRepeatSampler", pState );
	}
	if ( !m_hNearestSampler.isValid() || !m_hLinearRepeatSampler.isValid() ) {
		setError( "Failed to create displacement samplers" );
		return false;
	}
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	if ( !m_hDisplacementRegionBuffer.isValid() ) {
		const uint32_t region_bytes = 4096u * sizeof( int32_t );
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Compute.Displacement.RegionBuffer";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
		spec.m_creationBits = m_bPersistentDisplacementBuffers ? Terrain3DRdGpuBuffer::persistentCreationBits()
		        : BitField<RenderingDevice::BufferCreationBits>( 0 );
		m_hDisplacementRegionBuffer.configure( spec );
		m_hDisplacementRegionBuffer.createTracked( context, region_bytes );
		m_bDisplacementRegionUploaded = false;
	}
	if ( !m_hDisplacementMaterialBuffer.isValid() ) {
		const uint32_t material_bytes = 32u * 4u * sizeof( float );
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "T3D.Compute.Displacement.MaterialBuffer";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_MATERIAL_TABLE;
		spec.m_creationBits = m_bPersistentDisplacementBuffers ? Terrain3DRdGpuBuffer::persistentCreationBits()
		        : BitField<RenderingDevice::BufferCreationBits>( 0 );
		m_hDisplacementMaterialBuffer.configure( spec );
		m_hDisplacementMaterialBuffer.createTracked( context, material_bytes );
		m_bDisplacementMaterialUploaded = false;
	}
	if ( !m_hDisplacementRegionBuffer.isValid() || !m_hDisplacementMaterialBuffer.isValid() ) {
		setError( "Failed to allocate displacement parameter buffers" );
		return false;
	}
	if ( m_hDisplacementOutputTexture.isValid() && m_nDisplacementWidth == nWidth && m_nDisplacementHeight == nHeight ) {
		return true;
	}

	const uint64_t output_bytes = ( uint64_t )( nWidth ) * ( uint64_t )( nHeight ) * 8u;
	Terrain3DRdTexture replacement;
	t3dRdTextureSpec_t textureSpec;
	textureSpec.m_name = "T3D.Compute.Displacement.OutputTexture";
	textureSpec.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
	textureSpec.m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
	textureSpec.m_layout.m_format = RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT;
	textureSpec.m_layout.m_nWidth = nWidth; textureSpec.m_layout.m_nHeight = nHeight;
	textureSpec.m_layout.m_nLayers = 1u; textureSpec.m_layout.m_nMipmaps = 1u;
	textureSpec.m_layout.m_nUsageBits = RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT
	        | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	textureSpec.m_layout.m_nBytes = output_bytes;
	if ( !replacement.createTracked( context, textureSpec ) ) {
		setError( "Failed to allocate displacement storage texture" );
		return false;
	}
	freeRenderUniformSet( m_hDisplacementUniformSet );
	m_hDisplacementOutputTexture.releaseTracked( context );
	m_hDisplacementOutputTexture.swap( replacement );
	m_nDisplacementWidth = nWidth;
	m_nDisplacementHeight = nHeight;
	return true;
}

bool Terrain3DCompute::prepareDisplacementRequest(
        Terrain3DOwner *const pTerrain, const Vector3 &rTargetPosition, t3dDisplacementRequest_t &rRequest ) {
	rRequest = t3dDisplacementRequest_t{};
	if ( !pTerrain || pTerrain->getTessellationLevel() <= 0 ) {
		return false;
	}
	const Ref<Terrain3DMaterial> pMaterial = pTerrain->materialResource();
	const Ref<Terrain3DAssets> pAssets = pTerrain->assetsResource();
	Terrain3DData *const data = pTerrain->dataObject();
	if ( pMaterial.is_null() || pAssets.is_null() || !data || !pAssets->usesPackedPbr() ) {
		setError( "Compute displacement requires the packed-PBR/R8 texture path" );
		return false;
	}
	const t3dTextureFiltering_t filtering = pMaterial->getTextureFiltering();
	if ( pMaterial->getProjectionEnabled() || pMaterial->getDetilingMode() != DETILING_DISABLED
	        || pMaterial->getWorldBackground() == NOISE || filtering == NEAREST || filtering == NEAREST_ANISOTROPIC ) {
		setError( "Compute displacement falls back for projection, detiling, world-noise backgrounds, or nearest filtering" );
		return false;
	}
	if ( !Terrain3DRdContext::getGlobalDevice() ) {
		setError( "Global RenderingDevice is unavailable" );
		return false;
	}
	rRequest.m_shaderSpirv = Terrain3DRdShaderLibrary::resolveComputeShader( T3D_RD_SHADER_DISPLACEMENT_COMPUTE );
	if ( rRequest.m_shaderSpirv.is_null() ) {
		setError( "Compute displacement shader is unavailable or failed to compile" );
		return false;
	}

	rRequest.m_nWidth = ( uint32_t )( pTerrain->getMeshSize() * 4 * pTerrain->getTessellationLevel() );
	rRequest.m_nHeight = ( uint32_t )( pTerrain->getMeshSize() * 4 );
	rRequest.m_hHeightTexture = data->getHeightMapsRid();
	rRequest.m_hControlTexture = data->getControlMapsRid();
	rRequest.m_hDisplacementTexture = pAssets->getDisplacementArrayRid();
	rRequest.m_regionMap = data->getRegionMap();
	if ( rRequest.m_nWidth == 0u || rRequest.m_nHeight == 0u || rRequest.m_regionMap.size() != T3D_REGION_SLOTS
	        || !rRequest.m_hHeightTexture.is_valid() || !rRequest.m_hControlTexture.is_valid()
	        || !rRequest.m_hDisplacementTexture.is_valid() ) {
		setError( "Displacement compute inputs are incomplete" );
		return false;
	}
eastl::array<t3dMaterialData_t, 32> material_data{};
	const PackedFloat32Array uv_scales = pAssets->getTextureUvScales();
	const PackedVector2Array displacements = pAssets->getTextureDisplacements();
	for ( int i = 0; i < 32; ++i ) {
		material_data[( size_t )( i )].m_fUvScale = i < uv_scales.size() ? uv_scales[i] : 1.0f;
		const Vector2 displacement = i < displacements.size() ? displacements[i] : Vector2( 0.0f, 1.0f );
		material_data[( size_t )( i )].m_fOffset = displacement.x;
		material_data[( size_t )( i )].m_fScale = displacement.y;
	}
	rRequest.m_materialData = copyToBytes( material_data.data(), sizeof( material_data ) );
	rRequest.m_nRegionMapHash = t3dComputeHashBytes(
	        ( const uint8_t * )( rRequest.m_regionMap.ptr() ), ( size_t )( rRequest.m_regionMap.size() ) * sizeof( int32_t ) );
	rRequest.m_nMaterialDataHash = t3dComputeHashBytes( rRequest.m_materialData.ptr(), ( size_t )( rRequest.m_materialData.size() ) );
t3dDisplacementParams_t params;
	memset( &params, 0, sizeof( params ) );
	static_assert( sizeof( t3dDisplacementParams_t ) == 104u, "Displacement push constants must match GLSL" );
	params.m_nWidth = rRequest.m_nWidth;
	params.m_nHeight = rRequest.m_nHeight;
	params.m_fVertexSpacing = ( float )( s_fRegionVertexSpacing );
	params.m_fVertexDensity = 1.0f / params.m_fVertexSpacing;
	params.m_fMeshSize = ( float )( pTerrain->getMeshSize() );
	params.m_fTessellationLevel = ( float )( pTerrain->getTessellationLevel() );
	params.m_fTargetX = ( float )( rTargetPosition.x );
	params.m_fTargetZ = ( float )( rTargetPosition.z );
	params.m_fBlendSharpness = ( float )( ( real_t )( pMaterial->getShaderParam( "blend_sharpness" ) ) );
	params.m_fDisplacementSharpness = ( float )( pMaterial->getDisplacementSharpness() );
	params.m_fAutoSlope = ( float )( ( real_t )( pMaterial->getShaderParam( "auto_slope" ) ) );
	params.m_fAutoHeightReduction = ( float )( ( real_t )( pMaterial->getShaderParam( "auto_height_reduction" ) ) );
	params.m_nTextureLayerCount = ( uint32_t )( MAX( pAssets->getGeneratedArraySize(), 1 ) );
	const int auto_layer_max = ( int )( params.m_nTextureLayerCount ) - 1;
	params.m_nAutoBaseTexture = ( uint32_t )( CLAMP( ( int )( pMaterial->getShaderParam( "auto_base_texture" ) ), 0, auto_layer_max ) );
	params.m_nAutoOverlayTexture =
	        ( uint32_t )( CLAMP( ( int )( pMaterial->getShaderParam( "auto_overlay_texture" ) ), 0, auto_layer_max ) );
	params.m_bAutoEnabled = pMaterial->getAutoShaderEnabled() ? 1u : 0u;
	params.m_nSamplingMode = ( uint32_t )( pMaterial->getTerrainSamplingMode() );
	const t3dShaderPerformancePreset_t preset = pMaterial->getShaderPerformancePreset();
	params.m_bFastBlendCurve = preset == SHADER_PRESET_RUNTIME_360 || preset == SHADER_PRESET_MINIMUM ? 1u : 0u;
	params.m_nBackgroundMode = ( uint32_t )( pMaterial->getWorldBackground() );
	params.m_fGroundLevel = ( float )( ( real_t )( pMaterial->getShaderParam( "ground_level" ) ) );
	params.m_fRegionBlend = ( float )( ( real_t )( pMaterial->getShaderParam( "region_blend" ) ) );
	params.m_fHeightMin = s_fRegionHeightMin;
	params.m_fHeightRange = s_fRegionHeightRange;
	params.m_bControlMapsImplicit = data->areControlMapsImplicit() ? 1u : 0u;
	rRequest.m_pushConstants = copyToBytes( &params, sizeof( params ) );
	return rRequest.valid();
}

bool Terrain3DCompute::dispatchDisplacementRequest( const t3dDisplacementRequest_t &rRequest, RID &rTexture ) {
	rTexture = RID();
	if ( !rRequest.valid() || !ensureRenderDevice()
	        || !ensureDisplacementResources( rRequest.m_nWidth, rRequest.m_nHeight, rRequest.m_shaderSpirv ) ) {
		return false;
	}
	if ( !m_bDisplacementRegionUploaded || m_nDisplacementRegionHash != rRequest.m_nRegionMapHash ) {
		const uint64_t nRegionBytes64 = ( uint64_t )( rRequest.m_regionMap.size() ) * sizeof( int32_t );
		const bool bSizeValid = nRegionBytes64 <= 4096u * sizeof( int32_t ) && nRegionBytes64 <= eastl::numeric_limits<uint32_t>::max();
		const uint32_t nRegionBytes = bSizeValid ? ( uint32_t )( nRegionBytes64 ) : 0u;
		bool bUploaded = false;
		if ( bSizeValid ) {
			if ( m_hDisplacementRegionBuffer.isPersistent() ) {
				PackedByteArray unused_scratch;
				bUploaded = m_hDisplacementRegionBuffer.writeTracked( Terrain3DRdContext::getShared(), 0u,
				        rRequest.m_regionMap.ptr(), nRegionBytes, unused_scratch, T3D_GPU_DIRTY_UPLOAD );
			} else {
				const PackedByteArray region_bytes = copyToBytes( rRequest.m_regionMap.ptr(), nRegionBytes );
				bUploaded = region_bytes.size() == nRegionBytes
				        && m_hDisplacementRegionBuffer.writePackedTracked( Terrain3DRdContext::getShared(), 0u,
				                region_bytes, nRegionBytes, T3D_GPU_DIRTY_UPLOAD );
			}
		}
		if ( !bUploaded ) {
			setError( "Failed to upload displacement region topology" );
			m_bDisplacementRegionUploaded = false;
			return false;
		}
		m_nDisplacementRegionHash = rRequest.m_nRegionMapHash;
		m_bDisplacementRegionUploaded = true;
	}
	if ( !m_bDisplacementMaterialUploaded || m_nDisplacementMaterialHash != rRequest.m_nMaterialDataHash ) {
		const uint32_t nMaterialBytes = ( uint32_t )( rRequest.m_materialData.size() );
		const bool bSizeValid = rRequest.m_materialData.size() == 32 * 4 * ( int64_t )( sizeof( float ) );
		const bool bUploaded = bSizeValid
		        && m_hDisplacementMaterialBuffer.writePackedTracked( Terrain3DRdContext::getShared(), 0u,
		                rRequest.m_materialData, nMaterialBytes, T3D_GPU_PUBLICATION );
		if ( !bUploaded ) {
			setError( "Failed to upload displacement material data" );
			m_bDisplacementMaterialUploaded = false;
			return false;
		}
		m_nDisplacementMaterialHash = rRequest.m_nMaterialDataHash;
		m_bDisplacementMaterialUploaded = true;
	}

	const RID height_texture = rRequest.m_hHeightTexture;
	const RID control_texture = rRequest.m_hControlTexture;
	const RID displacement_texture = rRequest.m_hDisplacementTexture;
	if ( !height_texture.is_valid() || !control_texture.is_valid() || !displacement_texture.is_valid() ) {
		setError( "Displacement texture arrays are unavailable on the global RenderingDevice" );
		return false;
	}

	const bool bBindingsChanged = !m_hDisplacementUniformSet.isValid() || m_hDisplacementBoundHeight != height_texture
	        || m_hDisplacementBoundControl != control_texture || m_hDisplacementBoundDisplacement != displacement_texture;
	if ( bBindingsChanged ) {
		TypedArray<Ref<RDUniform>> pUniforms;
		pUniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 0, m_hNearestSampler.rid(), height_texture ) );
		pUniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 1, m_hNearestSampler.rid(), control_texture ) );
		pUniforms.push_back( Terrain3DRdBindingBuilder::samplerTexture( 2, m_hLinearRepeatSampler.rid(), displacement_texture ) );
		pUniforms.push_back( Terrain3DRdBindingBuilder::image( 3, m_hDisplacementOutputTexture.rid() ) );
		const RenderingDevice::UniformType displacement_storage_type = m_bPersistentDisplacementBuffers
		        ? RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC
		        : RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
		pUniforms.push_back( Terrain3DRdBindingBuilder::single( 4, displacement_storage_type, m_hDisplacementRegionBuffer.rid() ) );
		pUniforms.push_back( Terrain3DRdBindingBuilder::single( 5, displacement_storage_type, m_hDisplacementMaterialBuffer.rid() ) );
		Terrain3DRdUniformSet replacement;
		if ( !replacement.createTracked( Terrain3DRdContext::getShared(), T3D_RD_CONTEXT_OWNER_TERRAIN,
		             "T3D.Compute.Displacement.UniformSet", pUniforms, m_displacementPipeline.m_hShader.rid(), 0u ) ) {
			setError( "Failed to bind displacement compute resources" );
			return false;
		}
		freeRenderUniformSet( m_hDisplacementUniformSet );
		m_hDisplacementUniformSet.swap( replacement );
		m_hDisplacementBoundHeight = height_texture;
		m_hDisplacementBoundControl = control_texture;
		m_hDisplacementBoundDisplacement = displacement_texture;
	}

	const int64_t list = m_pRenderRd->compute_list_begin();
	m_pRenderRd->compute_list_bind_compute_pipeline( list, m_displacementPipeline.m_hPipeline.rid() );
	m_pRenderRd->compute_list_bind_uniform_set( list, m_hDisplacementUniformSet.rid(), 0u );
	m_pRenderRd->compute_list_set_push_constant( list, rRequest.m_pushConstants, ( uint32_t )( rRequest.m_pushConstants.size() ) );
	m_pRenderRd->compute_list_dispatch( list, ( rRequest.m_nWidth + 7u ) / 8u, ( rRequest.m_nHeight + 7u ) / 8u, 1u );
	m_pRenderRd->compute_list_end();
	// Godot 4.7.2 automatically synchronizes the completed compute list before later raster consumers.
	rTexture = m_hDisplacementOutputTexture.rid();
	return rTexture.is_valid();
}

void Terrain3DCompute::shutdownDisplacementResources( void ) {
	freeDisplacementTexture();
	freeRenderPipeline( m_displacementPipeline );
	m_hNearestSampler.releaseTracked( Terrain3DRdContext::getShared() );
	m_hLinearRepeatSampler.releaseTracked( Terrain3DRdContext::getShared() );
	m_pRenderRd = NULL;
	m_bRenderDeviceAttempted = false;
}

bool Terrain3DCompute::isAvailable( void ) {
	return ensureDevice();
}

void Terrain3DCompute::processBrushCpu( eastl::vector<t3dBrushElement_t, Terrain3DEastlAllocator> &rElements, const t3dBrushParams_t &p ) {
	for ( size_t iElement = 0u; iElement < ( size_t )( ( rElements ).size() ); iElement++ ) {
		t3dBrushElement_t &e = ( rElements )[iElement];
		if ( ( e.m_nFlags & 1u ) == 0u ) {
			continue;
		}
		const float fAlpha = t3dComputeBitsToFloat( e.m_nAlpha );
		if ( p.m_nMapType == T3D_TYPE_HEIGHT_VALUE ) {
			float fSource = t3dComputeBitsToFloat( e.m_nValue );
			if ( !isfinite( fSource ) ) {
				fSource = 0.0f;
			}
			float fResult = fSource;
			const float fAmount = fAlpha * p.m_fStrength;
			if ( p.m_nOperation == T3D_OP_ADD_VALUE ) {
				if ( p.m_nTool == T3D_TOOL_HEIGHT_VALUE ) {
					fResult = TERRAIN3D_LERP( fSource, p.m_fSelectedHeight, TERRAIN3D_CLAMP01( fAmount ) );
				} else if ( ( p.m_nFeatureFlags & T3D_FEATURE_ALT_VALUE ) && ( p.m_nFeatureFlags & T3D_FEATURE_CENTER_VALID_VALUE ) ) {
					fResult = CLAMP( p.m_fBrushCenterHeight + fAmount, fSource, fSource + fAmount );
				} else {
					fResult = fSource + fAmount;
				}
			} else if ( p.m_nOperation == T3D_OP_SUBTRACT_VALUE ) {
				if ( p.m_nTool == T3D_TOOL_HEIGHT_VALUE ) {
					fResult = TERRAIN3D_LERP( fSource, p.m_fSelectedHeight, TERRAIN3D_CLAMP01( fAmount ) );
				} else if ( ( p.m_nFeatureFlags & T3D_FEATURE_ALT_VALUE ) && ( p.m_nFeatureFlags & T3D_FEATURE_CENTER_VALID_VALUE ) ) {
					fResult = CLAMP( p.m_fBrushCenterHeight - fAmount, fSource - fAmount, fSource );
				} else {
					fResult = fSource - fAmount;
				}
			} else if ( p.m_nOperation == T3D_OP_GRADIENT_VALUE ) {
				fResult = TERRAIN3D_LERP( fSource, t3dComputeBitsToFloat( e.m_nTarget ), TERRAIN3D_CLAMP01( fAmount ) );
			}
			e.m_nValue = t3dComputeFloatToBits( fResult );
		} else if ( p.m_nMapType == T3D_TYPE_CONTROL_VALUE ) {
			uint32_t base = ( e.m_nValue >> 27u ) & 31u;
			uint32_t overlay = ( e.m_nValue >> 22u ) & 31u;
			float fBlend = ( float )( ( e.m_nValue >> 14u ) & 255u ) / s_fByteMaxFloat;
			uint32_t rotation = ( e.m_nValue >> 10u ) & 15u;
			uint32_t scale = ( e.m_nValue >> 7u ) & 7u;
			bool bHole = ( ( e.m_nValue >> 2u ) & 1u ) != 0u;
			bool bNav = ( ( e.m_nValue >> 1u ) & 1u ) != 0u;
			bool bAutomatic = ( e.m_nValue & 1u ) != 0u;
			if ( p.m_nTool == T3D_TOOL_TEXTURE_VALUE ) {
				if ( p.m_nOperation == T3D_OP_REPLACE_VALUE && fAlpha > 0.5f ) {
					if ( p.m_nFeatureFlags & T3D_FEATURE_TEXTURE_VALUE ) {
						base = p.m_nAssetId;
						overlay = p.m_nAssetId;
						fBlend = 0.0f;
						bAutomatic = false;
					}
					if ( base == p.m_nAssetId && !bAutomatic ) {
						if ( p.m_nFeatureFlags & T3D_FEATURE_ANGLE_VALUE ) {
							rotation = p.m_nUvRotation;
						}
						if ( p.m_nFeatureFlags & T3D_FEATURE_SCALE_VALUE ) {
							scale = p.m_nUvScale;
						}
					}
				} else if ( p.m_nOperation == T3D_OP_ADD_VALUE ) {
					const float fSpray = CLAMP( p.m_fStrength * 0.05f, 0.004f, 0.25f );
					const float fBrushValue = TERRAIN3D_CLAMP01( fAlpha * fSpray );
					if ( ( p.m_nFeatureFlags & T3D_FEATURE_TEXTURE_VALUE ) && fAlpha * p.m_fStrength * 11.0f > 0.1f ) {
						bAutomatic = false;
						if ( p.m_nAssetId != base && p.m_nAssetId != overlay ) {
							if ( p.m_nFeatureFlags & T3D_FEATURE_ALT_VALUE ) {
								if ( fBlend < 0.5f ) {
									overlay = p.m_nAssetId;
								} else {
									base = p.m_nAssetId;
								}
							} else {
								fBlend = fBlend >= 0.5f ? TERRAIN3D_CLAMP01( fBlend + fBrushValue )
								                        : TERRAIN3D_CLAMP01( fBlend - fBrushValue );
								if ( fBlend <= 1.0f / 254.0f ) {
									overlay = p.m_nAssetId;
								} else if ( fBlend >= 1.0f - 1.0f / 254.0f ) {
									base = p.m_nAssetId;
								}
							}
						}
						if ( base == p.m_nAssetId ) {
							fBlend = TERRAIN3D_CLAMP01( fBlend - fBrushValue );
							if ( fAlpha > 0.5f && fBlend < 0.5f ) {
								bAutomatic = false;
							}
						}
						if ( overlay == p.m_nAssetId ) {
							fBlend = TERRAIN3D_CLAMP01( fBlend + fBrushValue );
							if ( fAlpha > 0.5f && fBlend >= 0.5f ) {
								bAutomatic = false;
							}
						}
					}
					if ( ( ( base == p.m_nAssetId && fBlend < 0.5f ) || ( overlay == p.m_nAssetId && fBlend >= 0.5f ) ) && !bAutomatic
					        && fAlpha > 0.5f ) {
						if ( p.m_nFeatureFlags & T3D_FEATURE_ANGLE_VALUE ) {
							rotation = p.m_nUvRotation;
						}
						if ( p.m_nFeatureFlags & T3D_FEATURE_SCALE_VALUE ) {
							scale = p.m_nUvScale;
						}
					}
				} else if ( p.m_nOperation == T3D_OP_SUBTRACT_VALUE ) {
					const float fBrushValue = TERRAIN3D_CLAMP01( fAlpha * CLAMP( p.m_fStrength * 0.05f, 0.004f, 0.25f ) );
					if ( base == p.m_nAssetId ) {
						fBlend = TERRAIN3D_CLAMP01( fBlend + fBrushValue );
					}
					if ( overlay == p.m_nAssetId ) {
						fBlend = TERRAIN3D_CLAMP01( fBlend - fBrushValue );
					}
				}
			} else if ( p.m_nTool == T3D_TOOL_AUTOSHADER_VALUE && fAlpha > 0.5f ) {
				bAutomatic = p.m_nOperation == T3D_OP_ADD_VALUE;
				scale = 0u;
				rotation = 0u;
			} else if ( p.m_nTool == T3D_TOOL_HOLES_VALUE && fAlpha > 0.5f ) {
				bHole = p.m_nOperation == T3D_OP_ADD_VALUE;
			} else if ( p.m_nTool == T3D_TOOL_NAVIGATION_VALUE && fAlpha > 0.5f ) {
				bNav = p.m_nOperation == T3D_OP_ADD_VALUE;
			}
			const uint32_t blend_u8 = ( uint32_t )( CLAMP( round( fBlend * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
			e.m_nValue = ( ( base & 31u ) << 27u ) | ( ( overlay & 31u ) << 22u ) | ( ( blend_u8 & 255u ) << 14u )
			        | ( ( rotation & 15u ) << 10u ) | ( ( scale & 7u ) << 7u ) | ( bHole ? 4u : 0u ) | ( bNav ? 2u : 0u )
			        | ( bAutomatic ? 1u : 0u );
		} else if ( p.m_nMapType == T3D_TYPE_COLOR_VALUE ) {
			float fR = TERRAIN3D_UNPACK_COLOR( e.m_nValue, 0 ), g = TERRAIN3D_UNPACK_COLOR( e.m_nValue, 8 ),
			      b = TERRAIN3D_UNPACK_COLOR( e.m_nValue, 16 ), a = TERRAIN3D_UNPACK_COLOR( e.m_nValue, 24 );
			const float fWeight = TERRAIN3D_CLAMP01( fAlpha * p.m_fStrength );
			if ( p.m_nTool == T3D_TOOL_COLOR_VALUE ) {
				const uint32_t fTarget = p.m_nOperation == T3D_OP_ADD_VALUE ? p.m_nColorRgba8 : 0xffffffffu;
				fR = TERRAIN3D_LERP( fR, TERRAIN3D_UNPACK_COLOR( fTarget, 0 ), fWeight );
				g = TERRAIN3D_LERP( g, TERRAIN3D_UNPACK_COLOR( fTarget, 8 ), fWeight );
				b = TERRAIN3D_LERP( b, TERRAIN3D_UNPACK_COLOR( fTarget, 16 ), fWeight );
			} else if ( p.m_nTool == T3D_TOOL_ROUGHNESS_VALUE ) {
				const float fTarget = p.m_nOperation == T3D_OP_ADD_VALUE ? 0.5f + 0.5f * p.m_fRoughness : 0.5f;
				a = TERRAIN3D_LERP( a, fTarget, fWeight );
				a = ( float )( ( int )( a * s_fByteMaxFloat ) ) / s_fByteMaxFloat;
			}
			e.m_nValue = t3dComputePackColor( fR, g, b, a );
		}
	}
}

bool Terrain3DCompute::processBrush(
        eastl::vector<t3dBrushElement_t, Terrain3DEastlAllocator> &rElements, const t3dBrushParams_t &rParams ) {
	if ( rElements.empty() ) {
		return true;
	}
	if ( rElements.size() > ( size_t )( eastl::numeric_limits<uint32_t>::max() ) ) {
		setError( "Brush batch exceeds RenderingDevice dispatch limits" );
		return false;
	}
	if ( !ensurePipeline( m_brushPipeline, T3D_RD_SHADER_BRUSH_COMPUTE, "Terrain3D brush compute" ) ) {
		return false;
	}
	const uint64_t byte_count_64 = ( uint64_t )( rElements.size() ) * sizeof( t3dBrushElement_t );
	if ( byte_count_64 > eastl::numeric_limits<uint32_t>::max() ) {
		setError( "Brush batch exceeds 4 GiB" );
		return false;
	}
	const uint32_t byte_count = ( uint32_t )( byte_count_64 );
	if ( !ensureBrushCapacity( byte_count ) ) {
		return false;
	}
	const PackedByteArray input = copyToBytes( rElements.data(), byte_count );
	if ( !m_localBuffers.upload( m_brushStorage, input, byte_count ) ) {
		setError( "Failed to upload brush compute batch" );
		return false;
	}
	t3dBrushParams_t params = rParams;
	params.m_nCount = ( uint32_t )( rElements.size() );
	const PackedByteArray push = copyToBytes( &params, sizeof( params ) );
	const int64_t list = m_pRd->compute_list_begin();
	m_pRd->compute_list_bind_compute_pipeline( list, m_brushPipeline.m_hPipeline.rid() );
	m_pRd->compute_list_bind_uniform_set( list, m_brushStorage.uniformSet.rid(), 0u );
	m_pRd->compute_list_set_push_constant( list, push, ( uint32_t )( sizeof( params ) ) );
	m_pRd->compute_list_dispatch( list, ( params.m_nCount + T3D_COMPUTE_WORKGROUP_SIZE - 1u ) / T3D_COMPUTE_WORKGROUP_SIZE, 1u, 1u );
	m_pRd->compute_list_end();
	m_pRd->submit();
	m_pRd->sync();
	const PackedByteArray output = m_localBuffers.readback( m_brushStorage, byte_count );
	if ( output.size() != byte_count ) {
		setError( "Brush compute readback returned an unexpected byte count" );
		return false;
	}
	memcpy( rElements.data(), output.ptr(), byte_count );
	return true;
}

bool Terrain3DCompute::generateDerivedMaps( t3dDerivedMapComputeJob_t *const pJobs, const uint32_t nJobCount ) {
	if ( !pJobs || nJobCount == 0u || nJobCount > T3D_DERIVED_READBACK_SLOT_COUNT ) {
		setError( "Derived-map compute batch must contain one to three jobs" );
		return false;
	}
	if ( !ensurePipeline( m_derivedPipeline, T3D_RD_SHADER_DERIVED_COMPUTE, "Terrain3D derived map compute" ) ) {
		return false;
	}
for ( uint32_t slot = 0u; slot < nJobCount; ++slot ) {
		t3dDerivedMapComputeJob_t &job = pJobs[slot];
		job.m_bSuccess = false;
		if ( !job.m_pHeightHalo || !job.m_pRgbaOutput || job.m_nWidth == 0u || job.m_nHeight == 0u || job.m_fVertexSpacing <= 0.0f
		        || job.m_nHaloElements != ( job.m_nWidth + 2u ) * ( job.m_nHeight + 2u )
		        || job.m_nOutputBytes != job.m_nWidth * job.m_nHeight * sizeof( uint32_t ) ) {
			setError( "Invalid derived-map batch job" );
			return false;
		}
		const uint64_t input_bytes_64 = ( uint64_t )job.m_nHaloElements * sizeof( float );
		if ( input_bytes_64 > eastl::numeric_limits<uint32_t>::max() ) {
			setError( "Derived-map input exceeds 4 GiB" );
			return false;
		}
		const uint32_t input_bytes = ( uint32_t )input_bytes_64;
		if ( !ensureDerivedCapacity( slot, input_bytes, job.m_nOutputBytes ) ) {
			return false;
		}
		const PackedByteArray input = copyToBytes( job.m_pHeightHalo, input_bytes );
		if ( !m_localBuffers.uploadInput( m_derivedStorage[slot], input, input_bytes ) ) {
			setError( "Failed to upload derived-map height halo" );
			return false;
		}
	}

	const int64_t list = m_pRd->compute_list_begin();
	m_pRd->compute_list_bind_compute_pipeline( list, m_derivedPipeline.m_hPipeline.rid() );
	for ( uint32_t slot = 0u; slot < nJobCount; ++slot ) {
		const t3dDerivedMapComputeJob_t &job = pJobs[slot];
		const t3dDerivedParams_t params{ job.m_nWidth, job.m_nHeight, job.m_nWidth + 2u, job.m_fVertexSpacing };
		const PackedByteArray push = copyToBytes( &params, sizeof( params ) );
		const uint32_t count = job.m_nWidth * job.m_nHeight;
		m_pRd->compute_list_bind_uniform_set( list, m_derivedStorage[slot].uniformSet.rid(), 0u );
		m_pRd->compute_list_set_push_constant( list, push, ( uint32_t )( sizeof( params ) ) );
		m_pRd->compute_list_dispatch( list, ( count + T3D_COMPUTE_WORKGROUP_SIZE - 1u ) / T3D_COMPUTE_WORKGROUP_SIZE, 1u, 1u );
	}
	m_pRd->compute_list_end();
	m_pRd->submit();
	// All outputs are consumed immediately by the edit transaction. This is the
	// single dependency point for the whole batch, rather than one sync per job.
	m_pRd->sync();
	for ( uint32_t slot = 0u; slot < nJobCount; ++slot ) {
		t3dDerivedMapComputeJob_t &job = pJobs[slot];
		const PackedByteArray output = m_localBuffers.readbackOutput( m_derivedStorage[slot], job.m_nOutputBytes );
		if ( output.size() != job.m_nOutputBytes ) {
			setError( "Derived-map compute readback returned an unexpected byte count" );
			continue;
		}
		memcpy( job.m_pRgbaOutput, output.ptr(), job.m_nOutputBytes );
		job.m_bSuccess = true;
	}
	return true;
}

void Terrain3DCompute::shutdown( void ) {
	if ( !m_pRd ) {
		return;
	}
	m_localBuffers.release( m_brushStorage );
	for ( uint32_t slot = 0u; slot < T3D_DERIVED_READBACK_SLOT_COUNT; ++slot ) {
		m_localBuffers.release( m_derivedStorage[slot] );
	}
	m_localBuffers.releasePipeline( m_brushPipeline.m_hPipeline, m_brushPipeline.m_hShader );
	m_localBuffers.releasePipeline( m_derivedPipeline.m_hPipeline, m_derivedPipeline.m_hShader );
	m_brushPipeline.m_bAttempted = false;
	m_derivedPipeline.m_bAttempted = false;
	memdelete( m_pRd );
	m_pRd = NULL;
	m_localBuffers.setDevice( NULL );
}
