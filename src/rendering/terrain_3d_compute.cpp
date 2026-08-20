// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "rendering/terrain_3d_compute.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/memory.hpp>

#include "core/logger.h"
#include "terrain_3d.h"
#include "assets/terrain_3d_assets.h"
#include "data/terrain_3d_data.h"
#include "rendering/terrain_3d_material.h"
#include "data/terrain_3d_region_codec.h"
#include "streaming/terrain_3d_upload_bridge.h"

namespace {
#define TERRAIN3D_CLAMP01( value ) std::clamp( ( value ), 0.0f, 1.0f )
#define TERRAIN3D_LERP( a, b, t ) ( ( a ) + ( ( b ) - ( a ) ) * ( t ) )
#define TERRAIN3D_UNPACK_COLOR( value, shift ) ( ( float ) ( ( ( value ) >> ( shift ) ) & 0xffu ) / TERRAIN3D_BYTE_MAX_FLOAT )
#define TERRAIN3DQUANTIZECOLOR( value ) ( uint32_t ) ( std::clamp( std::round( ( value ) * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) )

static float terrain3dBitsToFloat( const uint32_t nBits ) {
	float fValue = 0.0f;
	std::memcpy( &fValue, &nBits, sizeof( fValue ) );
	return fValue;
}

static uint32_t terrain3dFloatToBits( const float fValue ) {
	uint32_t bits = 0u;
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	return bits;
}

static uint32_t terrain3dPackColor( const float fR, const float fG, const float fB, const float fA ) {
	return TERRAIN3DQUANTIZECOLOR( fR ) | ( TERRAIN3DQUANTIZECOLOR( fG ) << 8u ) |
		( TERRAIN3DQUANTIZECOLOR( fB ) << 16u ) | ( TERRAIN3DQUANTIZECOLOR( fA ) << 24u );
}
static_assert( sizeof( T3DCompute::t3dbrushElement_t ) == 16u, "t3dbrushElement_t must match GLSL std430 uvec4 layout" );
static_assert( sizeof( T3DCompute::t3dbrushParams_t ) == 52u, "t3dbrushParams_t must match GLSL push-constant layout" );

Ref<RDUniform> makeStorageUniform( const int nBinding, const RID &rBuffer ) {
	Ref<RDUniform> pUniform;
	pUniform.instantiate();
	pUniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
	pUniform->set_binding( nBinding );
	pUniform->add_id( rBuffer );
	return pUniform;
}

Ref<RDUniform> makeSampledTextureUniform( const int nBinding, const RID &rSampler, const RID &rTexture ) {
	Ref<RDUniform> pUniform;
	pUniform.instantiate();
	pUniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE );
	pUniform->set_binding( nBinding );
	pUniform->add_id( rSampler );
	pUniform->add_id( rTexture );
	return pUniform;
}

Ref<RDUniform> makeImageUniform( const int nBinding, const RID &rTexture ) {
	Ref<RDUniform> pUniform;
	pUniform.instantiate();
	pUniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_IMAGE );
	pUniform->set_binding( nBinding );
	pUniform->add_id( rTexture );
	return pUniform;
}

uint64_t hashBytes( const uint8_t *pData, const size_t nSize ) {
	uint64_t hash = 1469598103934665603ULL;
	if ( !pData && nSize > 0u ) {
		return 0u;
	}
	for ( size_t i = 0; i < nSize; ++i ) {
		hash ^= ( uint64_t ) ( pData[i] );
		hash *= 1099511628211ULL;
	}
	return hash;
}
} // namespace

T3DCompute::~T3DCompute( void ) {
	shutdown();
}

uint32_t T3DCompute::growthCapacity( const uint32_t nRequired ) {
	if ( nRequired == 0u ) {
		return 0u;
	}
	uint64_t capacity = 4096u;
	while ( capacity < nRequired ) {
		capacity = capacity + capacity / 2u;
		if ( capacity > std::numeric_limits<uint32_t>::max() ) {
			return nRequired;
		}
	}
	return ( uint32_t ) ( capacity );
}

PackedByteArray T3DCompute::copyToBytes( const void *pData, const size_t nSize ) {
	PackedByteArray bytes;
	if ( !pData || nSize == 0u || nSize > ( size_t ) ( std::numeric_limits<int64_t>::max() ) ) {
		return bytes;
	}
	bytes.resize( ( int64_t ) ( nSize ) );
	std::memcpy( bytes.ptrw(), pData, nSize );
	return bytes;
}

bool T3DCompute::ensureDevice( void ) {
	if ( m_pRd ) {
		return true;
	}
	if ( m_bDeviceAttempted ) {
		return false;
	}
	m_bDeviceAttempted = true;
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer || !pServer->get_rendering_device() ) {
		setError( "RenderingDevice is unavailable (Compatibility renderer or headless runtime)" );
		return false;
	}
	m_pRd = pServer->create_local_rendering_device();
	if ( !m_pRd ) {
		setError( "Could not create a local RenderingDevice" );
		return false;
	}
	return true;
}

bool T3DCompute::ensureRenderDevice( void ) {
	if ( m_pRenderRd ) {
		return true;
	}
	if ( m_bRenderDeviceAttempted ) {
		return false;
	}
	m_bRenderDeviceAttempted = true;
	RenderingServer *const pServer = RenderingServer::get_singleton();
	m_pRenderRd = pServer ? pServer->get_rendering_device() : nullptr;
	if ( !m_pRenderRd ) {
		setError( "Global RenderingDevice is unavailable (Compatibility renderer or headless runtime)" );
		return false;
	}
	return true;
}

bool T3DCompute::ensurePipeline( t3dpipeline_t &rPipeline, const String &rPath, const String &rName ) {
	if ( rPipeline.pipeline.is_valid() ) {
		return true;
	}
	if ( rPipeline.bAttempted || !ensureDevice() ) {
		return false;
	}
	rPipeline.bAttempted = true;
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		setError( "ResourceLoader unavailable while loading " + rPath );
		return false;
	}
	const Ref<RDShaderFile> pShaderFile = pLoader->load( rPath );
	if ( pShaderFile.is_null() ) {
		setError( "Compute shader resource is unavailable: " + rPath );
		return false;
	}
	const Ref<RDShaderSPIRV> pSpirv = pShaderFile->get_spirv();
	if ( pSpirv.is_null() ) {
		setError( "Compute shader has no compiled SPIR-V: " + rPath );
		return false;
	}
	rPipeline.pShader = m_pRd->shader_create_from_spirv( pSpirv, rName );
	if ( !rPipeline.pShader.is_valid() ) {
		setError( "Failed to create compute shader: " + rPath );
		return false;
	}
	rPipeline.pipeline = m_pRd->compute_pipeline_create( rPipeline.pShader );
	if ( !rPipeline.pipeline.is_valid() ) {
		setError( "Failed to create compute pipeline: " + rPath );
		m_pRd->free_rid( rPipeline.pShader );
		rPipeline.pShader = RID();
		return false;
	}
	return true;
}

bool T3DCompute::ensureRenderPipeline(
	t3dpipeline_t &rPipeline, const Ref<RDShaderSPIRV> &rSpirv,
	const String &rName
) {
	if ( rPipeline.pipeline.is_valid() ) {
		return true;
	}
	if ( rPipeline.bAttempted || !ensureRenderDevice() ) {
		return false;
	}
	rPipeline.bAttempted = true;
	if ( rSpirv.is_null() ) {
		setError( "Displacement compute shader has no compiled SPIR-V" );
		return false;
	}
	rPipeline.pShader = m_pRenderRd->shader_create_from_spirv( rSpirv, rName );
	if ( !rPipeline.pShader.is_valid() ) {
		setError( "Failed to create displacement compute shader" );
		return false;
	}
	rPipeline.pipeline = m_pRenderRd->compute_pipeline_create( rPipeline.pShader );
	if ( !rPipeline.pipeline.is_valid() ) {
		setError( "Failed to create displacement compute pipeline" );
		m_pRenderRd->free_rid( rPipeline.pShader );
		rPipeline.pShader = RID();
		return false;
	}
	return true;
}

void T3DCompute::freeUniformSet( RID &rUniformSet ) {
	if ( m_pRd && rUniformSet.is_valid() ) {
		m_pRd->free_rid( rUniformSet );
	}
	rUniformSet = RID();
}

void T3DCompute::freeBuffer( RID &rBuffer ) {
	if ( m_pRd && rBuffer.is_valid() ) {
		m_pRd->free_rid( rBuffer );
	}
	rBuffer = RID();
}

void T3DCompute::freePipeline( t3dpipeline_t &rPipeline ) {
	if ( m_pRd && rPipeline.pipeline.is_valid() ) {
		m_pRd->free_rid( rPipeline.pipeline );
	}
	if ( m_pRd && rPipeline.pShader.is_valid() ) {
		m_pRd->free_rid( rPipeline.pShader );
	}
	rPipeline = t3dpipeline_t{};
}

void T3DCompute::freeRenderUniformSet( RID &rUniformSet ) {
	if ( m_pRenderRd && rUniformSet.is_valid() ) {
		m_pRenderRd->free_rid( rUniformSet );
	}
	rUniformSet = RID();
}

void T3DCompute::freeRenderRid( RID &rRid ) {
	if ( m_pRenderRd && rRid.is_valid() ) {
		m_pRenderRd->free_rid( rRid );
	}
	rRid = RID();
}

void T3DCompute::freeRenderPipeline( t3dpipeline_t &rPipeline ) {
	if ( m_pRenderRd && rPipeline.pipeline.is_valid() ) {
		m_pRenderRd->free_rid( rPipeline.pipeline );
	}
	if ( m_pRenderRd && rPipeline.pShader.is_valid() ) {
		m_pRenderRd->free_rid( rPipeline.pShader );
	}
	rPipeline = t3dpipeline_t{};
}

bool T3DCompute::ensureBrushCapacity( const uint32_t nBytes ) {
	if ( nBytes <= m_nBrushCapacity && m_brushBuffer.is_valid() && m_brushUniformSet.is_valid() ) {
		return true;
	}
	freeUniformSet( m_brushUniformSet );
	freeBuffer( m_brushBuffer );
	m_nBrushCapacity = growthCapacity( nBytes );
	m_brushBuffer = m_pRd->storage_buffer_create( m_nBrushCapacity );
	if ( !m_brushBuffer.is_valid() ) {
		setError( "Failed to allocate brush compute buffer" );
		m_nBrushCapacity = 0u;
		return false;
	}
	TypedArray<Ref<RDUniform>> pUniforms;
	pUniforms.push_back( makeStorageUniform( 0, m_brushBuffer ) );
	m_brushUniformSet = m_pRd->uniform_set_create( pUniforms, m_brushPipeline.pShader, 0u );
	if ( !m_brushUniformSet.is_valid() ) {
		setError( "Failed to create brush compute uniform set" );
		return false;
	}
	return true;
}

bool T3DCompute::ensureDerivedCapacity( const uint32_t nInputBytes, const uint32_t nOutputBytes ) {
	if ( nInputBytes <= m_nDerivedInputCapacity && nOutputBytes <= m_nDerivedOutputCapacity &&
		m_derivedInputBuffer.is_valid() && m_derivedOutputBuffer.is_valid() && m_derivedUniformSet.is_valid() ) {
		return true;
	}
	freeUniformSet( m_derivedUniformSet );
	freeBuffer( m_derivedInputBuffer );
	freeBuffer( m_derivedOutputBuffer );
	m_nDerivedInputCapacity = growthCapacity( nInputBytes );
	m_nDerivedOutputCapacity = growthCapacity( nOutputBytes );
	m_derivedInputBuffer = m_pRd->storage_buffer_create( m_nDerivedInputCapacity );
	m_derivedOutputBuffer = m_pRd->storage_buffer_create( m_nDerivedOutputCapacity );
	if ( !m_derivedInputBuffer.is_valid() || !m_derivedOutputBuffer.is_valid() ) {
		setError( "Failed to allocate derived-map compute buffers" );
		return false;
	}
	TypedArray<Ref<RDUniform>> pUniforms;
	pUniforms.push_back( makeStorageUniform( 0, m_derivedInputBuffer ) );
	pUniforms.push_back( makeStorageUniform( 1, m_derivedOutputBuffer ) );
	m_derivedUniformSet = m_pRd->uniform_set_create( pUniforms, m_derivedPipeline.pShader, 0u );
	if ( !m_derivedUniformSet.is_valid() ) {
		setError( "Failed to create derived-map compute uniform set" );
		return false;
	}
	return true;
}

void T3DCompute::freeDisplacementTexture( void ) {
	freeRenderUniformSet( m_displacementUniformSet );
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && m_displacementRsTexture.is_valid() ) {
		pServer->free_rid( m_displacementRsTexture );
	}
	m_displacementRsTexture = RID();
	m_displacementBoundHeight = RID();
	m_displacementBoundControl = RID();
	m_displacementBoundDisplacement = RID();
	freeRenderRid( m_displacementRegionBuffer );
	freeRenderRid( m_displacementMaterialBuffer );
	freeRenderRid( m_displacementOutputTexture );
	m_nDisplacementRegionHash = 0u;
	m_displacementMaterialHash = 0u;
	m_bDisplacementRegionUploaded = false;
	m_bDisplacementMaterialUploaded = false;
	m_nDisplacementWidth = 0u;
	m_nDisplacementHeight = 0u;
}

bool T3DCompute::ensureDisplacementResources(
	const uint32_t nWidth, const uint32_t nHeight,
	const Ref<RDShaderSPIRV> &rSpirv
) {
	if ( !ensureRenderPipeline( m_displacementPipeline, rSpirv, "Terrain3D displacement compute" ) ) {
		return false;
	}
	if ( !m_nearestSampler.is_valid() ) {
		Ref<RDSamplerState> pState;
		pState.instantiate();
		pState->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		pState->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		pState->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_nearestSampler = m_pRenderRd->sampler_create( pState );
	}
	if ( !m_linearRepeatSampler.is_valid() ) {
		Ref<RDSamplerState> pState;
		pState.instantiate();
		pState->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		pState->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		pState->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_linearRepeatSampler = m_pRenderRd->sampler_create( pState );
	}
	if ( !m_nearestSampler.is_valid() || !m_linearRepeatSampler.is_valid() ) {
		setError( "Failed to create displacement samplers" );
		return false;
	}
	if ( !m_displacementRegionBuffer.is_valid() ) {
		m_displacementRegionBuffer = T3DUploadBridge::createPersistentStorageBuffer( 4096u * sizeof( int32_t ) );
		if ( !m_displacementRegionBuffer.is_valid() ) {
			m_displacementRegionBuffer = m_pRenderRd->storage_buffer_create( 4096u * sizeof( int32_t ) );
		}
		m_bDisplacementRegionUploaded = false;
	}
	if ( !m_displacementMaterialBuffer.is_valid() ) {
		m_displacementMaterialBuffer = T3DUploadBridge::createPersistentStorageBuffer( 32u * 4u * sizeof( float ) );
		if ( !m_displacementMaterialBuffer.is_valid() ) {
			m_displacementMaterialBuffer = m_pRenderRd->storage_buffer_create( 32u * 4u * sizeof( float ) );
		}
		m_bDisplacementMaterialUploaded = false;
	}
	if ( !m_displacementRegionBuffer.is_valid() || !m_displacementMaterialBuffer.is_valid() ) {
		setError( "Failed to allocate displacement parameter buffers" );
		return false;
	}
	if ( m_displacementOutputTexture.is_valid() && m_nDisplacementWidth == nWidth && m_nDisplacementHeight == nHeight ) {
		return true;
	}
	freeRenderUniformSet( m_displacementUniformSet );
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && m_displacementRsTexture.is_valid() ) {
		pServer->free_rid( m_displacementRsTexture );
	}
	m_displacementRsTexture = RID();
	freeRenderRid( m_displacementOutputTexture );

	Ref<RDTextureFormat> pFormat;
	pFormat.instantiate();
	pFormat->set_width( nWidth );
	pFormat->set_height( nHeight );
	pFormat->set_depth( 1u );
	pFormat->set_array_layers( 1u );
	pFormat->set_mipmaps( 1u );
	pFormat->set_texture_type( RenderingDevice::TEXTURE_TYPE_2D );
	pFormat->set_format( RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT );
	pFormat->set_usage_bits( RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
						   RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
						   RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );
	Ref<RDTextureView> pView;
	pView.instantiate();
	m_displacementOutputTexture = m_pRenderRd->texture_create( pFormat, pView );
	if ( !m_displacementOutputTexture.is_valid() ) {
		setError( "Failed to allocate displacement storage texture" );
		return false;
	}
	if ( !pServer ) {
		setError( "RenderingServer unavailable while exposing displacement texture" );
		return false;
	}
	m_displacementRsTexture = pServer->texture_rd_create( m_displacementOutputTexture );
	if ( !m_displacementRsTexture.is_valid() ) {
		setError( "Failed to expose displacement texture to RenderingServer" );
		return false;
	}
	m_nDisplacementWidth = nWidth;
	m_nDisplacementHeight = nHeight;
	return true;
}

bool T3DCompute::prepareDisplacementRequest(
	T3DTerrain *const pTerrain, const Vector3 &rTargetPosition,
	t3ddisplacementRequest_t &rRequest
) {
	rRequest = t3ddisplacementRequest_t{};
	if ( !pTerrain || pTerrain->getTessellationLevel() <= 0 ) {
		return false;
	}
	const Ref<T3DMaterial> pMaterial = pTerrain->getMaterial();
	const Ref<T3DAssets> pAssets = pTerrain->getAssets();
	T3DData *const data = pTerrain->getData();
	if ( pMaterial.is_null() || pAssets.is_null() || !data || !pAssets->usesPackedPbr() ) {
		setError( "Compute displacement requires the packed-PBR/R8 texture path" );
		return false;
	}
	const T3DMaterial::t3dtextureFiltering_t filtering = pMaterial->getTextureFiltering();
	if ( pMaterial->getProjectionEnabled() || pMaterial->getDetilingMode() != T3DMaterial::DETILING_DISABLED ||
		pMaterial->getBufferShaderOverride().is_valid() ||
		pMaterial->getWorldBackground() == T3DMaterial::NOISE || filtering == T3DMaterial::NEAREST ||
		filtering == T3DMaterial::NEAREST_ANISOTROPIC ) {
		setError( "Compute displacement falls back for projection, detiling, world-noise backgrounds, nearest "
				   "filtering, or buffer shader overrides" );
		return false;
	}
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer || !pServer->get_rendering_device() ) {
		setError( "Global RenderingDevice is unavailable" );
		return false;
	}
	if ( m_pDisplacementSpirv.is_null() ) {
		ResourceLoader *const pLoader = ResourceLoader::get_singleton();
		Ref<RDShaderFile> pShaderFile;
		if ( pLoader ) {
			pShaderFile = pLoader->load( "res://addons/terrain_3d/compute/terrain_displacement.glsl" );
		}
		m_pDisplacementSpirv = pShaderFile.is_valid() ? pShaderFile->get_spirv() : Ref<RDShaderSPIRV>();
	}
	rRequest.pShaderSpirv = m_pDisplacementSpirv;
	if ( rRequest.pShaderSpirv.is_null() ) {
		setError( "Compute displacement shader is unavailable or failed to compile" );
		return false;
	}

	rRequest.nWidth = ( uint32_t ) ( pTerrain->getMeshSize() * 4 * pTerrain->getTessellationLevel() );
	rRequest.nHeight = ( uint32_t ) ( pTerrain->getMeshSize() * 4 );
	rRequest.materialRid = pMaterial->getMaterialRid();
	rRequest.heightTexture = data->getHeightMapsRid();
	rRequest.controlTexture = data->getControlMapsRid();
	rRequest.displacementTexture = pAssets->getDisplacementArrayRid();
	rRequest.regionMap = data->getRegionMap();
	rRequest.nRegionMapSize = ( uint32_t ) ( data->getRegionMapSizeInternal() );
	if ( rRequest.nWidth == 0u || rRequest.nHeight == 0u || rRequest.nRegionMapSize == 0u ||
		rRequest.regionMap.size() != ( int64_t ) ( rRequest.nRegionMapSize ) * ( int64_t ) ( rRequest.nRegionMapSize ) ||
		!rRequest.materialRid.is_valid() || !rRequest.heightTexture.is_valid() ||
		!rRequest.controlTexture.is_valid() || !rRequest.displacementTexture.is_valid() ) {
		setError( "Displacement compute inputs are incomplete" );
		return false;
	}

	typedef struct alignas( 16 ) t3dmaterialData_s {
		float fUvScale;
		float fOffset;
		float fScale;
		float fPad;
	} t3dmaterialData_t;
	eastl::array<t3dmaterialData_t, 32> material_data{};
	const PackedFloat32Array uv_scales = pAssets->getTextureUvScales();
	const PackedVector2Array displacements = pAssets->getTextureDisplacements();
	for ( int i = 0; i < 32; ++i ) {
		material_data[( size_t ) ( i )].fUvScale = i < uv_scales.size() ? uv_scales[i] : 1.0f;
		const Vector2 displacement = i < displacements.size() ? displacements[i] : Vector2( 0.0f, 1.0f );
		material_data[( size_t ) ( i )].fOffset = displacement.x;
		material_data[( size_t ) ( i )].fScale = displacement.y;
	}
	rRequest.materialData = copyToBytes( material_data.data(), sizeof( material_data ) );
	rRequest.regionMapHash = hashBytes( ( const uint8_t * ) ( rRequest.regionMap.ptr() ),
										 ( size_t ) ( rRequest.regionMap.size() ) * sizeof( int32_t ) );
	rRequest.materialDataHash =
		hashBytes( rRequest.materialData.ptr(), ( size_t ) ( rRequest.materialData.size() ) );

	typedef struct t3ddisplacementParams_s {
		uint32_t nWidth, nHeight, nRegionSize, nRegionMapSize;
		float fVertexSpacing, vertex_density, mesh_size, fTessellationLevel;
		float fTargetX, fTargetZ, blend_sharpness, fDisplacementSharpness;
		float fAutoSlope, fAutoHeightReduction;
		uint32_t autoBaseTexture, autoOverlayTexture, auto_enabled, sampling_mode;
		uint32_t bFastBlendCurve, background_mode;
		float ground_level, fRegionBlend;
		float height_min, fHeightRange;
		uint32_t bControlMapsImplicit, texture_layer_count, padding1, padding2;
	} t3ddisplacementParams_t;
	t3ddisplacementParams_t params{};
	static_assert( sizeof( t3ddisplacementParams_t ) == 112u, "Displacement push constants must match GLSL" );
	params.nWidth = rRequest.nWidth;
	params.nHeight = rRequest.nHeight;
	params.nRegionSize = ( uint32_t ) ( pTerrain->getRegionSize() );
	params.nRegionMapSize = rRequest.nRegionMapSize;
	params.fVertexSpacing = ( float ) ( pTerrain->getVertexSpacing() );
	params.vertex_density = 1.0f / params.fVertexSpacing;
	params.mesh_size = ( float ) ( pTerrain->getMeshSize() );
	params.fTessellationLevel = ( float ) ( pTerrain->getTessellationLevel() );
	params.fTargetX = ( float ) ( rTargetPosition.x );
	params.fTargetZ = ( float ) ( rTargetPosition.z );
	params.blend_sharpness = ( float ) ( ( real_t ) ( pMaterial->getShaderParam( "blend_sharpness" ) ) );
	params.fDisplacementSharpness = ( float ) ( pMaterial->getDisplacementSharpness() );
	params.fAutoSlope = ( float ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_slope" ) ) );
	params.fAutoHeightReduction = ( float ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_height_reduction" ) ) );
	params.texture_layer_count = ( uint32_t ) ( MAX( pAssets->getGeneratedArraySize(), 1 ) );
	const int auto_layer_max = ( int ) ( params.texture_layer_count ) - 1;
	params.autoBaseTexture = ( uint32_t ) ( CLAMP( ( int ) ( pMaterial->getShaderParam( "auto_base_texture" ) ), 0, auto_layer_max ) );
	params.autoOverlayTexture = ( uint32_t ) ( CLAMP( ( int ) ( pMaterial->getShaderParam( "auto_overlay_texture" ) ), 0, auto_layer_max ) );
	params.auto_enabled = pMaterial->getAutoShaderEnabled() ? 1u : 0u;
	params.sampling_mode = ( uint32_t ) ( pMaterial->getTerrainSamplingMode() );
	const T3DMaterial::t3dshaderPerformancePreset_t preset = pMaterial->getShaderPerformancePreset();
	params.bFastBlendCurve =
		preset == T3DMaterial::SHADER_PRESET_XBOX_360 || preset == T3DMaterial::SHADER_PRESET_FASTEST ? 1u
																												  : 0u;
	params.background_mode = ( uint32_t ) ( pMaterial->getWorldBackground() );
	params.ground_level = ( float ) ( ( real_t ) ( pMaterial->getShaderParam( "ground_level" ) ) );
	params.fRegionBlend = ( float ) ( ( real_t ) ( pMaterial->getShaderParam( "region_blend" ) ) );
	params.height_min = TERRAIN3D_REGION_HEIGHT_MIN;
	params.fHeightRange = TERRAIN3D_REGION_HEIGHT_RANGE;
	params.bControlMapsImplicit = data->areControlMapsImplicit() ? 1u : 0u;
	rRequest.pushConstants = copyToBytes( &params, sizeof( params ) );
	return rRequest.valid();
}

bool T3DCompute::dispatchDisplacementRequest( const t3ddisplacementRequest_t &rRequest, RID &rTexture ) {
	rTexture = RID();
	if ( !rRequest.valid() || !ensureRenderDevice() ||
		!ensureDisplacementResources( rRequest.nWidth, rRequest.nHeight, rRequest.pShaderSpirv ) ) {
		return false;
	}
	if ( !m_bDisplacementRegionUploaded || m_nDisplacementRegionHash != rRequest.regionMapHash ) {
		const PackedByteArray region_bytes =
			copyToBytes( rRequest.regionMap.ptr(), ( size_t ) ( rRequest.regionMap.size() ) * sizeof( int32_t ) );
		const int64_t expected_region_bytes = ( int64_t ) ( rRequest.regionMap.size() ) * ( int64_t ) ( sizeof( int32_t ) );
		if ( region_bytes.size() != expected_region_bytes || expected_region_bytes > 4096 * ( int64_t ) ( sizeof( int32_t ) ) ||
			m_pRenderRd->buffer_update( m_displacementRegionBuffer, 0u, ( uint32_t ) ( region_bytes.size() ), region_bytes ) != OK ) {
			setError( "Failed to upload displacement region topology" );
			m_bDisplacementRegionUploaded = false;
			return false;
		}
		m_nDisplacementRegionHash = rRequest.regionMapHash;
		m_bDisplacementRegionUploaded = true;
	}
	if ( !m_bDisplacementMaterialUploaded || m_displacementMaterialHash != rRequest.materialDataHash ) {
		if ( rRequest.materialData.size() != 32 * 4 * ( int64_t ) ( sizeof( float ) ) ||
			m_pRenderRd->buffer_update( m_displacementMaterialBuffer, 0u, ( uint32_t ) ( rRequest.materialData.size() ),
									  rRequest.materialData ) != OK ) {
			setError( "Failed to upload displacement material data" );
			m_bDisplacementMaterialUploaded = false;
			return false;
		}
		m_displacementMaterialHash = rRequest.materialDataHash;
		m_bDisplacementMaterialUploaded = true;
	}

	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) {
		setError( "RenderingServer unavailable during displacement dispatch" );
		return false;
	}
	const RID height_texture = pServer->texture_get_rd_texture( rRequest.heightTexture );
	const RID control_texture = pServer->texture_get_rd_texture( rRequest.controlTexture );
	const RID displacement_texture = pServer->texture_get_rd_texture( rRequest.displacementTexture );
	if ( !height_texture.is_valid() || !control_texture.is_valid() || !displacement_texture.is_valid() ) {
		setError( "Displacement texture arrays are unavailable on the global RenderingDevice" );
		return false;
	}

	const bool bBindingsChanged =
		!m_displacementUniformSet.is_valid() || m_displacementBoundHeight != height_texture ||
		m_displacementBoundControl != control_texture || m_displacementBoundDisplacement != displacement_texture;
	if ( bBindingsChanged ) {
		freeRenderUniformSet( m_displacementUniformSet );
		TypedArray<Ref<RDUniform>> pUniforms;
		pUniforms.push_back( makeSampledTextureUniform( 0, m_nearestSampler, height_texture ) );
		pUniforms.push_back( makeSampledTextureUniform( 1, m_nearestSampler, control_texture ) );
		pUniforms.push_back( makeSampledTextureUniform( 2, m_linearRepeatSampler, displacement_texture ) );
		pUniforms.push_back( makeImageUniform( 3, m_displacementOutputTexture ) );
		pUniforms.push_back( makeStorageUniform( 4, m_displacementRegionBuffer ) );
		pUniforms.push_back( makeStorageUniform( 5, m_displacementMaterialBuffer ) );
		m_displacementUniformSet = m_pRenderRd->uniform_set_create( pUniforms, m_displacementPipeline.pShader, 0u );
		if ( !m_displacementUniformSet.is_valid() ) {
			setError( "Failed to bind displacement compute resources" );
			return false;
		}
		m_displacementBoundHeight = height_texture;
		m_displacementBoundControl = control_texture;
		m_displacementBoundDisplacement = displacement_texture;
	}

	const int64_t list = m_pRenderRd->compute_list_begin();
	m_pRenderRd->compute_list_bind_compute_pipeline( list, m_displacementPipeline.pipeline );
	m_pRenderRd->compute_list_bind_uniform_set( list, m_displacementUniformSet, 0u );
	m_pRenderRd->compute_list_set_push_constant( list, rRequest.pushConstants,
											   ( uint32_t ) ( rRequest.pushConstants.size() ) );
	m_pRenderRd->compute_list_dispatch( list, ( rRequest.nWidth + 7u ) / 8u, ( rRequest.nHeight + 7u ) / 8u, 1u );
	m_pRenderRd->compute_list_end();
	m_pRenderRd->barrier( RenderingDevice::BARRIER_MASK_COMPUTE, RenderingDevice::BARRIER_MASK_RASTER );
	rTexture = m_displacementRsTexture;
	return rTexture.is_valid();
}

void T3DCompute::shutdownDisplacementResources( void ) {
	freeDisplacementTexture();
	freeRenderPipeline( m_displacementPipeline );
	freeRenderRid( m_nearestSampler );
	freeRenderRid( m_linearRepeatSampler );
	m_pRenderRd = nullptr;
	m_bRenderDeviceAttempted = false;
}

bool T3DCompute::isAvailable( void ) {
	return ensureDevice();
}

void T3DCompute::processBrushCpu( eastl::vector<t3dbrushElement_t> &rElements, const t3dbrushParams_t &p ) {
	for ( t3dbrushElement_t &e : rElements ) {
		if ( ( e.flags & 1u ) == 0u ) {
			continue;
		}
		const float fAlpha = terrain3dBitsToFloat( e.alpha );
		if ( p.mapType == TERRAIN3D_TYPE_HEIGHT_VALUE ) {
			float fSource = terrain3dBitsToFloat( e.value );
			if ( !std::isfinite( fSource ) ) {
				fSource = 0.0f;
			}
			float fResult = fSource;
			const float fAmount = fAlpha * p.fStrength;
			if ( p.operation == TERRAIN3D_OP_ADD_VALUE ) {
				if ( p.tool == TERRAIN3D_TOOL_HEIGHT_VALUE ) {
					fResult = TERRAIN3D_LERP( fSource, p.fSelectedHeight, TERRAIN3D_CLAMP01( fAmount ) );
				} else if ( ( p.featureFlags & TERRAIN3D_FEATURE_ALT_VALUE ) && ( p.featureFlags & TERRAIN3D_FEATURE_CENTER_VALID_VALUE ) ) {
					fResult = std::clamp( p.fBrushCenterHeight + fAmount, fSource, fSource + fAmount );
				} else {
					fResult = fSource + fAmount;
				}
			} else if ( p.operation == TERRAIN3D_OP_SUBTRACT_VALUE ) {
				if ( p.tool == TERRAIN3D_TOOL_HEIGHT_VALUE ) {
					fResult = TERRAIN3D_LERP( fSource, p.fSelectedHeight, TERRAIN3D_CLAMP01( fAmount ) );
				} else if ( ( p.featureFlags & TERRAIN3D_FEATURE_ALT_VALUE ) && ( p.featureFlags & TERRAIN3D_FEATURE_CENTER_VALID_VALUE ) ) {
					fResult = std::clamp( p.fBrushCenterHeight - fAmount, fSource - fAmount, fSource );
				} else {
					fResult = fSource - fAmount;
				}
			} else if ( p.operation == TERRAIN3D_OP_GRADIENT_VALUE ) {
				fResult = TERRAIN3D_LERP( fSource, terrain3dBitsToFloat( e.target ), TERRAIN3D_CLAMP01( fAmount ) );
			}
			e.value = terrain3dFloatToBits( fResult );
		} else if ( p.mapType == TERRAIN3D_TYPE_CONTROL_VALUE ) {
			uint32_t base = ( e.value >> 27u ) & 31u;
			uint32_t overlay = ( e.value >> 22u ) & 31u;
			float fBlend = ( float ) ( ( e.value >> 14u ) & 255u ) / TERRAIN3D_BYTE_MAX_FLOAT;
			uint32_t rotation = ( e.value >> 10u ) & 15u;
			uint32_t scale = ( e.value >> 7u ) & 7u;
			bool bHole = ( ( e.value >> 2u ) & 1u ) != 0u;
			bool bNav = ( ( e.value >> 1u ) & 1u ) != 0u;
			bool bAutomatic = ( e.value & 1u ) != 0u;
			if ( p.tool == TERRAIN3D_TOOL_TEXTURE_VALUE ) {
				if ( p.operation == TERRAIN3D_OP_REPLACE_VALUE && fAlpha > 0.5f ) {
					if ( p.featureFlags & TERRAIN3D_FEATURE_TEXTURE_VALUE ) {
						base = p.assetId;
						overlay = p.assetId;
						fBlend = 0.0f;
						bAutomatic = false;
					}
					if ( base == p.assetId && !bAutomatic ) {
						if ( p.featureFlags & TERRAIN3D_FEATURE_ANGLE_VALUE ) {
							rotation = p.uvRotation;
						}
						if ( p.featureFlags & TERRAIN3D_FEATURE_SCALE_VALUE ) {
							scale = p.uvScale;
						}
					}
				} else if ( p.operation == TERRAIN3D_OP_ADD_VALUE ) {
					const float fSpray = std::clamp( p.fStrength * 0.05f, 0.004f, 0.25f );
					const float fBrushValue = TERRAIN3D_CLAMP01( fAlpha * fSpray );
					if ( ( p.featureFlags & TERRAIN3D_FEATURE_TEXTURE_VALUE ) && fAlpha * p.fStrength * 11.0f > 0.1f ) {
						bAutomatic = false;
						if ( p.assetId != base && p.assetId != overlay ) {
							if ( p.featureFlags & TERRAIN3D_FEATURE_ALT_VALUE ) {
								if ( fBlend < 0.5f ) {
									overlay = p.assetId;
								} else {
									base = p.assetId;
								}
							} else {
								fBlend = fBlend >= 0.5f ? TERRAIN3D_CLAMP01( fBlend + fBrushValue ) : TERRAIN3D_CLAMP01( fBlend - fBrushValue );
								if ( fBlend <= 1.0f / 254.0f ) {
									overlay = p.assetId;
								} else if ( fBlend >= 1.0f - 1.0f / 254.0f ) {
									base = p.assetId;
								}
							}
						}
						if ( base == p.assetId ) {
							fBlend = TERRAIN3D_CLAMP01( fBlend - fBrushValue );
							if ( fAlpha > 0.5f && fBlend < 0.5f ) {
								bAutomatic = false;
							}
						}
						if ( overlay == p.assetId ) {
							fBlend = TERRAIN3D_CLAMP01( fBlend + fBrushValue );
							if ( fAlpha > 0.5f && fBlend >= 0.5f ) {
								bAutomatic = false;
							}
						}
					}
					if ( ( ( base == p.assetId && fBlend < 0.5f ) || ( overlay == p.assetId && fBlend >= 0.5f ) ) &&
						!bAutomatic && fAlpha > 0.5f ) {
						if ( p.featureFlags & TERRAIN3D_FEATURE_ANGLE_VALUE ) {
							rotation = p.uvRotation;
						}
						if ( p.featureFlags & TERRAIN3D_FEATURE_SCALE_VALUE ) {
							scale = p.uvScale;
						}
					}
				} else if ( p.operation == TERRAIN3D_OP_SUBTRACT_VALUE ) {
					const float fBrushValue = TERRAIN3D_CLAMP01( fAlpha * std::clamp( p.fStrength * 0.05f, 0.004f, 0.25f ) );
					if ( base == p.assetId ) {
						fBlend = TERRAIN3D_CLAMP01( fBlend + fBrushValue );
					}
					if ( overlay == p.assetId ) {
						fBlend = TERRAIN3D_CLAMP01( fBlend - fBrushValue );
					}
				}
			} else if ( p.tool == TERRAIN3D_TOOL_AUTOSHADER_VALUE && fAlpha > 0.5f ) {
				bAutomatic = p.operation == TERRAIN3D_OP_ADD_VALUE;
				scale = 0u;
				rotation = 0u;
			} else if ( p.tool == TERRAIN3D_TOOL_HOLES_VALUE && fAlpha > 0.5f ) {
				bHole = p.operation == TERRAIN3D_OP_ADD_VALUE;
			} else if ( p.tool == TERRAIN3D_TOOL_NAVIGATION_VALUE && fAlpha > 0.5f ) {
				bNav = p.operation == TERRAIN3D_OP_ADD_VALUE;
			}
			const uint32_t blend_u8 = ( uint32_t ) ( std::clamp( std::round( fBlend * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
			e.value = ( ( base & 31u ) << 27u ) | ( ( overlay & 31u ) << 22u ) | ( ( blend_u8 & 255u ) << 14u ) |
					  ( ( rotation & 15u ) << 10u ) | ( ( scale & 7u ) << 7u ) | ( bHole ? 4u : 0u ) | ( bNav ? 2u : 0u ) |
					  ( bAutomatic ? 1u : 0u );
		} else if ( p.mapType == TERRAIN3D_TYPE_COLOR_VALUE ) {
			float fR = TERRAIN3D_UNPACK_COLOR( e.value, 0 ), g = TERRAIN3D_UNPACK_COLOR( e.value, 8 ), b = TERRAIN3D_UNPACK_COLOR( e.value, 16 ),
				  a = TERRAIN3D_UNPACK_COLOR( e.value, 24 );
			const float fWeight = TERRAIN3D_CLAMP01( fAlpha * p.fStrength );
			if ( p.tool == TERRAIN3D_TOOL_COLOR_VALUE ) {
				const uint32_t fTarget = p.operation == TERRAIN3D_OP_ADD_VALUE ? p.colorRgba8 : 0xffffffffu;
				fR = TERRAIN3D_LERP( fR, TERRAIN3D_UNPACK_COLOR( fTarget, 0 ), fWeight );
				g = TERRAIN3D_LERP( g, TERRAIN3D_UNPACK_COLOR( fTarget, 8 ), fWeight );
				b = TERRAIN3D_LERP( b, TERRAIN3D_UNPACK_COLOR( fTarget, 16 ), fWeight );
			} else if ( p.tool == TERRAIN3D_TOOL_ROUGHNESS_VALUE ) {
				const float fTarget = p.operation == TERRAIN3D_OP_ADD_VALUE ? 0.5f + 0.5f * p.fRoughness : 0.5f;
				a = TERRAIN3D_LERP( a, fTarget, fWeight );
				a = ( float ) ( ( int ) ( a * TERRAIN3D_BYTE_MAX_FLOAT ) ) / TERRAIN3D_BYTE_MAX_FLOAT;
			}
			e.value = terrain3dPackColor( fR, g, b, a );
		}
	}
}

bool T3DCompute::processBrush( eastl::vector<t3dbrushElement_t> &rElements, const t3dbrushParams_t &rParams ) {
	if ( rElements.empty() ) {
		return true;
	}
	if ( rElements.size() > ( size_t ) ( std::numeric_limits<uint32_t>::max() ) ) {
		setError( "Brush batch exceeds RenderingDevice dispatch limits" );
		return false;
	}
	if ( !ensurePipeline( m_brushPipeline, "res://addons/terrain_3d/compute/terrain_brush.glsl",
						  "Terrain3D brush compute" ) ) {
		return false;
	}
	const uint64_t byte_count_64 = ( uint64_t ) ( rElements.size() ) * sizeof( t3dbrushElement_t );
	if ( byte_count_64 > std::numeric_limits<uint32_t>::max() ) {
		setError( "Brush batch exceeds 4 GiB" );
		return false;
	}
	const uint32_t byte_count = ( uint32_t ) ( byte_count_64 );
	if ( !ensureBrushCapacity( byte_count ) ) {
		return false;
	}
	const PackedByteArray input = copyToBytes( rElements.data(), byte_count );
	if ( m_pRd->buffer_update( m_brushBuffer, 0u, byte_count, input ) != OK ) {
		setError( "Failed to upload brush compute batch" );
		return false;
	}
	t3dbrushParams_t params = rParams;
	params.nCount = ( uint32_t ) ( rElements.size() );
	const PackedByteArray push = copyToBytes( &params, sizeof( params ) );
	const int64_t list = m_pRd->compute_list_begin();
	m_pRd->compute_list_bind_compute_pipeline( list, m_brushPipeline.pipeline );
	m_pRd->compute_list_bind_uniform_set( list, m_brushUniformSet, 0u );
	m_pRd->compute_list_set_push_constant( list, push, ( uint32_t ) ( sizeof( params ) ) );
	m_pRd->compute_list_dispatch( list, ( params.nCount + TERRAIN3D_COMPUTE_WORKGROUP_SIZE - 1u ) / TERRAIN3D_COMPUTE_WORKGROUP_SIZE, 1u, 1u );
	m_pRd->compute_list_end();
	m_pRd->submit();
	m_pRd->sync();
	const PackedByteArray output = m_pRd->buffer_get_data( m_brushBuffer, 0u, byte_count );
	if ( output.size() != byte_count ) {
		setError( "Brush compute readback returned an unexpected byte count" );
		return false;
	}
	std::memcpy( rElements.data(), output.ptr(), byte_count );
	return true;
}

bool T3DCompute::generateDerivedMap(
	const eastl::vector<float> &fHeightHalo, const int nWidth,
	const int nHeight, const float fVertexSpacing,
	PackedByteArray &rRgba8
) {
	if ( nWidth <= 0 || nHeight <= 0 || fVertexSpacing <= 0.0f ) {
		setError( "Invalid derived-map dimensions or vertex spacing" );
		return false;
	}
	const size_t halo_size = ( size_t ) ( nWidth + 2 ) * ( size_t ) ( nHeight + 2 );
	if ( fHeightHalo.size() != halo_size ) {
		setError( "Derived-map halo does not match tile dimensions" );
		return false;
	}
	if ( !ensurePipeline( m_derivedPipeline, "res://addons/terrain_3d/compute/terrain_derived.glsl",
						  "Terrain3D derived map compute" ) ) {
		return false;
	}
	const uint64_t input_bytes_64 = ( uint64_t ) ( fHeightHalo.size() ) * sizeof( float );
	const uint64_t output_bytes_64 = ( uint64_t ) ( nWidth ) * ( uint64_t ) ( nHeight ) * sizeof( uint32_t );
	if ( input_bytes_64 > std::numeric_limits<uint32_t>::max() ||
		output_bytes_64 > std::numeric_limits<uint32_t>::max() ) {
		setError( "Derived-map buffer exceeds 4 GiB" );
		return false;
	}
	const uint32_t input_bytes = ( uint32_t ) ( input_bytes_64 );
	const uint32_t output_bytes = ( uint32_t ) ( output_bytes_64 );
	if ( !ensureDerivedCapacity( input_bytes, output_bytes ) ) {
		return false;
	}
	const PackedByteArray input = copyToBytes( fHeightHalo.data(), input_bytes );
	if ( m_pRd->buffer_update( m_derivedInputBuffer, 0u, input_bytes, input ) != OK ) {
		setError( "Failed to upload derived-map height halo" );
		return false;
	}
	typedef struct t3dderivedParams_s {
		uint32_t nWidth;
		uint32_t height;
		uint32_t halo_stride;
		float fVertexSpacing;
	} t3dderivedParams_t;
	t3dderivedParams_t params{( uint32_t ) ( nWidth ), ( uint32_t ) ( nHeight ), ( uint32_t ) ( nWidth + 2 ), fVertexSpacing};
	const PackedByteArray push = copyToBytes( &params, sizeof( params ) );
	const uint32_t count = ( uint32_t ) ( nWidth * nHeight );
	const int64_t list = m_pRd->compute_list_begin();
	m_pRd->compute_list_bind_compute_pipeline( list, m_derivedPipeline.pipeline );
	m_pRd->compute_list_bind_uniform_set( list, m_derivedUniformSet, 0u );
	m_pRd->compute_list_set_push_constant( list, push, ( uint32_t ) ( sizeof( params ) ) );
	m_pRd->compute_list_dispatch( list, ( count + TERRAIN3D_COMPUTE_WORKGROUP_SIZE - 1u ) / TERRAIN3D_COMPUTE_WORKGROUP_SIZE, 1u, 1u );
	m_pRd->compute_list_end();
	m_pRd->submit();
	m_pRd->sync();
	rRgba8 = m_pRd->buffer_get_data( m_derivedOutputBuffer, 0u, output_bytes );
	if ( rRgba8.size() != output_bytes ) {
		setError( "Derived-map compute readback returned an unexpected byte count" );
		rRgba8.clear();
		return false;
	}
	return true;
}

void T3DCompute::shutdown( void ) {
	if ( !m_pRd ) {
		return;
	}
	freeUniformSet( m_brushUniformSet );
	freeUniformSet( m_derivedUniformSet );
	freeBuffer( m_brushBuffer );
	freeBuffer( m_derivedInputBuffer );
	freeBuffer( m_derivedOutputBuffer );
	freePipeline( m_brushPipeline );
	freePipeline( m_derivedPipeline );
	memdelete( m_pRd );
	m_pRd = nullptr;
}
