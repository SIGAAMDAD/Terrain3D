// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/terrain_3d_compute.h"

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_assets.h"
#include "core/logger.h"
#include "core/terrain_3d_compiler.h"
#include "data/terrain_3d_data.h"
#include "data/terrain_3d_region_codec.h"
#include "rendering/terrain_3d_material.h"
#include "streaming/terrain_3d_upload_bridge.h"
#include "terrain_3d.h"

#include <algorithm>
#include <cstring>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/memory.hpp>
#include <limits>

#define TERRAIN3D_CLAMP01( value ) std::clamp( ( value ), 0.0f, 1.0f )
#define TERRAIN3D_LERP( a, b, t ) ( ( a ) + ( ( b ) - ( a ) ) * ( t ) )
#define TERRAIN3D_UNPACK_COLOR( value, shift ) ( ( float )( ( ( value ) >> ( shift ) ) & 0xffu ) / s_fByteMaxFloat )
#define TERRAIN3DQUANTIZECOLOR( value ) \
	( uint32_t )( std::clamp( std::round( ( value ) * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) )

EA_FORCE_INLINE static float t3dComputeBitsToFloat( const uint32_t nBits ) {
	float fValue = 0.0f;
	std::memcpy( &fValue, &nBits, sizeof( fValue ) );
	return fValue;
}

EA_FORCE_INLINE static uint32_t t3dComputeFloatToBits( const float fValue ) {
	uint32_t bits = 0u;
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	return bits;
}

EA_FORCE_INLINE static uint32_t t3dComputePackColor( const float fR, const float fG, const float fB, const float fA ) {
	return TERRAIN3DQUANTIZECOLOR( fR ) | ( TERRAIN3DQUANTIZECOLOR( fG ) << 8u ) |
			( TERRAIN3DQUANTIZECOLOR( fB ) << 16u ) | ( TERRAIN3DQUANTIZECOLOR( fA ) << 24u );
}
static_assert( sizeof( t3dBrushElement_t ) == 16u, "t3dBrushElement_t must match GLSL std430 uvec4 layout" );
static_assert( sizeof( t3dBrushParams_t ) == 52u, "t3dBrushParams_t must match GLSL push-constant layout" );

EA_FORCE_INLINE static Ref<RDUniform> t3dComputeMakeStorageUniform( const int nBinding, const RID &rBuffer ) {
	Ref<RDUniform> pUniform;
	pUniform.instantiate();
	pUniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
	pUniform->set_binding( nBinding );
	pUniform->add_id( rBuffer );
	return pUniform;
}

static Ref<RDUniform>
t3dComputeMakeSampledTextureUniform( const int nBinding, const RID &rSampler, const RID &rTexture ) {
	Ref<RDUniform> pUniform;
	pUniform.instantiate();
	pUniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE );
	pUniform->set_binding( nBinding );
	pUniform->add_id( rSampler );
	pUniform->add_id( rTexture );
	return pUniform;
}

EA_FORCE_INLINE static Ref<RDUniform> t3dComputeMakeImageUniform( const int nBinding, const RID &rTexture ) {
	Ref<RDUniform> pUniform;
	pUniform.instantiate();
	pUniform->set_uniform_type( RenderingDevice::UNIFORM_TYPE_IMAGE );
	pUniform->set_binding( nBinding );
	pUniform->add_id( rTexture );
	return pUniform;
}

static uint64_t t3dComputeHashBytes( const uint8_t *pData, const size_t nSize ) {
	uint64_t hash = 1469598103934665603ULL;
	if ( !pData && nSize > 0u ) {
		return 0u;
	}
	for ( size_t i = 0; i < nSize; ++i ) {
		hash ^= ( uint64_t )( pData[i] );
		hash *= 1099511628211ULL;
	}
	return hash;
}

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
	return ( uint32_t )( capacity );
}

PackedByteArray T3DCompute::copyToBytes( const void *pData, const size_t nSize ) {
	PackedByteArray bytes;
	if ( !pData || nSize == 0u || nSize > ( size_t )( std::numeric_limits<int64_t>::max() ) ) {
		return bytes;
	}
	bytes.resize( ( int64_t )( nSize ) );
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

bool T3DCompute::ensurePipeline( t3dPipeline_t &rPipeline, const String &rPath, const String &rName ) {
	if ( rPipeline.m_hPipeline.is_valid() ) {
		return true;
	}
	if ( rPipeline.m_bAttempted || !ensureDevice() ) {
		return false;
	}
	rPipeline.m_bAttempted = true;
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
	rPipeline.m_hShader = m_pRd->shader_create_from_spirv( pSpirv, rName );
	if ( !rPipeline.m_hShader.is_valid() ) {
		setError( "Failed to create compute shader: " + rPath );
		return false;
	}
	rPipeline.m_hPipeline = m_pRd->compute_pipeline_create( rPipeline.m_hShader );
	if ( !rPipeline.m_hPipeline.is_valid() ) {
		setError( "Failed to create compute pipeline: " + rPath );
		m_pRd->free_rid( rPipeline.m_hShader );
		rPipeline.m_hShader = RID();
		return false;
	}
	return true;
}

bool T3DCompute::ensureRenderPipeline( t3dPipeline_t &rPipeline,
		const Ref<RDShaderSPIRV> &rSpirv,
		const String &rName ) {
	if ( rPipeline.m_hPipeline.is_valid() ) {
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
	rPipeline.m_hShader = m_pRenderRd->shader_create_from_spirv( rSpirv, rName );
	if ( !rPipeline.m_hShader.is_valid() ) {
		setError( "Failed to create displacement compute shader" );
		return false;
	}
	rPipeline.m_hPipeline = m_pRenderRd->compute_pipeline_create( rPipeline.m_hShader );
	if ( !rPipeline.m_hPipeline.is_valid() ) {
		setError( "Failed to create displacement compute pipeline" );
		m_pRenderRd->free_rid( rPipeline.m_hShader );
		rPipeline.m_hShader = RID();
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

void T3DCompute::freePipeline( t3dPipeline_t &rPipeline ) {
	if ( m_pRd && rPipeline.m_hPipeline.is_valid() ) {
		m_pRd->free_rid( rPipeline.m_hPipeline );
	}
	if ( m_pRd && rPipeline.m_hShader.is_valid() ) {
		m_pRd->free_rid( rPipeline.m_hShader );
	}
	rPipeline = t3dPipeline_t{};
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

void T3DCompute::freeRenderPipeline( t3dPipeline_t &rPipeline ) {
	if ( m_pRenderRd && rPipeline.m_hPipeline.is_valid() ) {
		m_pRenderRd->free_rid( rPipeline.m_hPipeline );
	}
	if ( m_pRenderRd && rPipeline.m_hShader.is_valid() ) {
		m_pRenderRd->free_rid( rPipeline.m_hShader );
	}
	rPipeline = t3dPipeline_t{};
}

bool T3DCompute::ensureBrushCapacity( const uint32_t nBytes ) {
	if ( nBytes <= m_nBrushCapacity && m_hBrushBuffer.is_valid() && m_hBrushUniformSet.is_valid() ) {
		return true;
	}
	freeUniformSet( m_hBrushUniformSet );
	freeBuffer( m_hBrushBuffer );
	m_nBrushCapacity = growthCapacity( nBytes );
	m_hBrushBuffer = m_pRd->storage_buffer_create( m_nBrushCapacity );
	if ( !m_hBrushBuffer.is_valid() ) {
		setError( "Failed to allocate brush compute buffer" );
		m_nBrushCapacity = 0u;
		return false;
	}
	TypedArray<Ref<RDUniform>> pUniforms;
	pUniforms.push_back( t3dComputeMakeStorageUniform( 0, m_hBrushBuffer ) );
	m_hBrushUniformSet = m_pRd->uniform_set_create( pUniforms, m_brushPipeline.m_hShader, 0u );
	if ( !m_hBrushUniformSet.is_valid() ) {
		setError( "Failed to create brush compute uniform set" );
		return false;
	}
	return true;
}

bool T3DCompute::ensureDerivedCapacity( const uint32_t nInputBytes, const uint32_t nOutputBytes ) {
	if ( nInputBytes <= m_nDerivedInputCapacity && nOutputBytes <= m_nDerivedOutputCapacity &&
			m_hDerivedInputBuffer.is_valid() && m_hDerivedOutputBuffer.is_valid() && m_hDerivedUniformSet.is_valid() ) {
		return true;
	}
	freeUniformSet( m_hDerivedUniformSet );
	freeBuffer( m_hDerivedInputBuffer );
	freeBuffer( m_hDerivedOutputBuffer );
	m_nDerivedInputCapacity = growthCapacity( nInputBytes );
	m_nDerivedOutputCapacity = growthCapacity( nOutputBytes );
	m_hDerivedInputBuffer = m_pRd->storage_buffer_create( m_nDerivedInputCapacity );
	m_hDerivedOutputBuffer = m_pRd->storage_buffer_create( m_nDerivedOutputCapacity );
	if ( !m_hDerivedInputBuffer.is_valid() || !m_hDerivedOutputBuffer.is_valid() ) {
		setError( "Failed to allocate derived-map compute buffers" );
		return false;
	}
	TypedArray<Ref<RDUniform>> pUniforms;
	pUniforms.push_back( t3dComputeMakeStorageUniform( 0, m_hDerivedInputBuffer ) );
	pUniforms.push_back( t3dComputeMakeStorageUniform( 1, m_hDerivedOutputBuffer ) );
	m_hDerivedUniformSet = m_pRd->uniform_set_create( pUniforms, m_derivedPipeline.m_hShader, 0u );
	if ( !m_hDerivedUniformSet.is_valid() ) {
		setError( "Failed to create derived-map compute uniform set" );
		return false;
	}
	return true;
}

void T3DCompute::freeDisplacementTexture( void ) {
	freeRenderUniformSet( m_hDisplacementUniformSet );
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && m_hDisplacementRsTexture.is_valid() ) {
		pServer->free_rid( m_hDisplacementRsTexture );
	}
	m_hDisplacementRsTexture = RID();
	m_hDisplacementBoundHeight = RID();
	m_hDisplacementBoundControl = RID();
	m_hDisplacementBoundDisplacement = RID();
	freeRenderRid( m_hDisplacementRegionBuffer );
	freeRenderRid( m_hDisplacementMaterialBuffer );
	freeRenderRid( m_hDisplacementOutputTexture );
	m_nDisplacementRegionHash = 0u;
	m_nDisplacementMaterialHash = 0u;
	m_bDisplacementRegionUploaded = false;
	m_bDisplacementMaterialUploaded = false;
	m_nDisplacementWidth = 0u;
	m_nDisplacementHeight = 0u;
}

bool T3DCompute::ensureDisplacementResources( const uint32_t nWidth,
		const uint32_t nHeight,
		const Ref<RDShaderSPIRV> &rSpirv ) {
	if ( !ensureRenderPipeline( m_displacementPipeline, rSpirv, "Terrain3D displacement compute" ) ) {
		return false;
	}
	if ( !m_hNearestSampler.is_valid() ) {
		Ref<RDSamplerState> pState;
		pState.instantiate();
		pState->set_mag_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_min_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_mip_filter( RenderingDevice::SAMPLER_FILTER_NEAREST );
		pState->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		pState->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		pState->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hNearestSampler = m_pRenderRd->sampler_create( pState );
	}
	if ( !m_hLinearRepeatSampler.is_valid() ) {
		Ref<RDSamplerState> pState;
		pState.instantiate();
		pState->set_mag_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_min_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_mip_filter( RenderingDevice::SAMPLER_FILTER_LINEAR );
		pState->set_repeat_u( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		pState->set_repeat_v( RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT );
		pState->set_repeat_w( RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE );
		m_hLinearRepeatSampler = m_pRenderRd->sampler_create( pState );
	}
	if ( !m_hNearestSampler.is_valid() || !m_hLinearRepeatSampler.is_valid() ) {
		setError( "Failed to create displacement samplers" );
		return false;
	}
	if ( !m_hDisplacementRegionBuffer.is_valid() ) {
		m_hDisplacementRegionBuffer = t3dUploadBridgeCreatePersistentStorageBuffer( 4096u * sizeof( int32_t ) );
		if ( !m_hDisplacementRegionBuffer.is_valid() ) {
			m_hDisplacementRegionBuffer = m_pRenderRd->storage_buffer_create( 4096u * sizeof( int32_t ) );
		}
		m_bDisplacementRegionUploaded = false;
	}
	if ( !m_hDisplacementMaterialBuffer.is_valid() ) {
		m_hDisplacementMaterialBuffer = t3dUploadBridgeCreatePersistentStorageBuffer( 32u * 4u * sizeof( float ) );
		if ( !m_hDisplacementMaterialBuffer.is_valid() ) {
			m_hDisplacementMaterialBuffer = m_pRenderRd->storage_buffer_create( 32u * 4u * sizeof( float ) );
		}
		m_bDisplacementMaterialUploaded = false;
	}
	if ( !m_hDisplacementRegionBuffer.is_valid() || !m_hDisplacementMaterialBuffer.is_valid() ) {
		setError( "Failed to allocate displacement parameter buffers" );
		return false;
	}
	if ( m_hDisplacementOutputTexture.is_valid() && m_nDisplacementWidth == nWidth &&
			m_nDisplacementHeight == nHeight ) {
		return true;
	}
	freeRenderUniformSet( m_hDisplacementUniformSet );
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && m_hDisplacementRsTexture.is_valid() ) {
		pServer->free_rid( m_hDisplacementRsTexture );
	}
	m_hDisplacementRsTexture = RID();
	freeRenderRid( m_hDisplacementOutputTexture );

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
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );
	Ref<RDTextureView> pView;
	pView.instantiate();
	m_hDisplacementOutputTexture = m_pRenderRd->texture_create( pFormat, pView );
	if ( !m_hDisplacementOutputTexture.is_valid() ) {
		setError( "Failed to allocate displacement storage texture" );
		return false;
	}
	if ( !pServer ) {
		setError( "RenderingServer unavailable while exposing displacement texture" );
		return false;
	}
	m_hDisplacementRsTexture = pServer->texture_rd_create( m_hDisplacementOutputTexture );
	if ( !m_hDisplacementRsTexture.is_valid() ) {
		setError( "Failed to expose displacement texture to RenderingServer" );
		return false;
	}
	m_nDisplacementWidth = nWidth;
	m_nDisplacementHeight = nHeight;
	return true;
}

bool T3DCompute::prepareDisplacementRequest( T3DTerrain *const pTerrain,
		const Vector3 &rTargetPosition,
		t3dDisplacementRequest_t &rRequest ) {
	rRequest = t3dDisplacementRequest_t{};
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
	const t3dTextureFiltering_t filtering = pMaterial->getTextureFiltering();
	if ( pMaterial->getProjectionEnabled() || pMaterial->getDetilingMode() != DETILING_DISABLED ||
			pMaterial->getBufferShaderOverride().is_valid() || pMaterial->getWorldBackground() == NOISE ||
			filtering == NEAREST || filtering == NEAREST_ANISOTROPIC ) {
		setError( "Compute displacement falls back for projection, detiling, world-noise backgrounds, nearest "
				  "filtering, or buffer shader overrides" );
		return false;
	}
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer || !pServer->get_rendering_device() ) {
		setError( "Global RenderingDevice is unavailable" );
		return false;
	}
	if ( m_displacementSpirv.is_null() ) {
		ResourceLoader *const pLoader = ResourceLoader::get_singleton();
		Ref<RDShaderFile> pShaderFile;
		if ( pLoader ) {
			pShaderFile = pLoader->load( "res://addons/terrain_3d/compute/terrain_displacement.glsl" );
		}
		m_displacementSpirv = pShaderFile.is_valid() ? pShaderFile->get_spirv() : Ref<RDShaderSPIRV>();
	}
	rRequest.m_shaderSpirv = m_displacementSpirv;
	if ( rRequest.m_shaderSpirv.is_null() ) {
		setError( "Compute displacement shader is unavailable or failed to compile" );
		return false;
	}

	rRequest.m_nWidth = ( uint32_t )( pTerrain->getMeshSize() * 4 * pTerrain->getTessellationLevel() );
	rRequest.m_nHeight = ( uint32_t )( pTerrain->getMeshSize() * 4 );
	rRequest.m_hMaterialRid = pMaterial->getMaterialRid();
	rRequest.m_hHeightTexture = data->getHeightMapsRid();
	rRequest.m_hControlTexture = data->getControlMapsRid();
	rRequest.m_hDisplacementTexture = pAssets->getDisplacementArrayRid();
	rRequest.m_regionMap = data->getRegionMap();
	rRequest.m_nRegionMapSize = ( uint32_t )( data->getRegionMapSizeInternal() );
	if ( rRequest.m_nWidth == 0u || rRequest.m_nHeight == 0u || rRequest.m_nRegionMapSize == 0u ||
			rRequest.m_regionMap.size() !=
					( int64_t )( rRequest.m_nRegionMapSize ) * ( int64_t )( rRequest.m_nRegionMapSize ) ||
			!rRequest.m_hMaterialRid.is_valid() || !rRequest.m_hHeightTexture.is_valid() ||
			!rRequest.m_hControlTexture.is_valid() || !rRequest.m_hDisplacementTexture.is_valid() ) {
		setError( "Displacement compute inputs are incomplete" );
		return false;
	}

	typedef struct alignas( 16 ) t3dMaterialData_s {
		float m_fUvScale;
		float m_fOffset;
		float m_fScale;
		float m_fPad;
	} t3dMaterialData_t;
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
	rRequest.m_nRegionMapHash = t3dComputeHashBytes( ( const uint8_t * )( rRequest.m_regionMap.ptr() ),
			( size_t )( rRequest.m_regionMap.size() ) * sizeof( int32_t ) );
	rRequest.m_nMaterialDataHash =
			t3dComputeHashBytes( rRequest.m_materialData.ptr(), ( size_t )( rRequest.m_materialData.size() ) );

	typedef struct t3dDisplacementParams_s {
		uint32_t m_nWidth, m_nHeight, m_nRegionSize, m_nRegionMapSize;
		float m_fVertexSpacing, m_fVertexDensity, m_fMeshSize, m_fTessellationLevel;
		float m_fTargetX, m_fTargetZ, m_fBlendSharpness, m_fDisplacementSharpness;
		float m_fAutoSlope, m_fAutoHeightReduction;
		uint32_t m_nAutoBaseTexture, m_nAutoOverlayTexture, m_bAutoEnabled, m_nSamplingMode;
		uint32_t m_bFastBlendCurve, m_nBackgroundMode;
		float m_fGroundLevel, m_fRegionBlend;
		float m_fHeightMin, m_fHeightRange;
		uint32_t m_bControlMapsImplicit, m_nTextureLayerCount, m_nPadding1, m_nPadding2;
	} t3dDisplacementParams_t;
	t3dDisplacementParams_t params{};
	static_assert( sizeof( t3dDisplacementParams_t ) == 112u, "Displacement push constants must match GLSL" );
	params.m_nWidth = rRequest.m_nWidth;
	params.m_nHeight = rRequest.m_nHeight;
	params.m_nRegionSize = ( uint32_t )( pTerrain->getRegionSize() );
	params.m_nRegionMapSize = rRequest.m_nRegionMapSize;
	params.m_fVertexSpacing = ( float )( pTerrain->getVertexSpacing() );
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
	params.m_nAutoBaseTexture =
			( uint32_t )( CLAMP( ( int )( pMaterial->getShaderParam( "auto_base_texture" ) ), 0, auto_layer_max ) );
	params.m_nAutoOverlayTexture =
			( uint32_t )( CLAMP( ( int )( pMaterial->getShaderParam( "auto_overlay_texture" ) ), 0, auto_layer_max ) );
	params.m_bAutoEnabled = pMaterial->getAutoShaderEnabled() ? 1u : 0u;
	params.m_nSamplingMode = ( uint32_t )( pMaterial->getTerrainSamplingMode() );
	const t3dShaderPerformancePreset_t preset = pMaterial->getShaderPerformancePreset();
	params.m_bFastBlendCurve = preset == SHADER_PRESET_XBOX_360 || preset == SHADER_PRESET_FASTEST ? 1u : 0u;
	params.m_nBackgroundMode = ( uint32_t )( pMaterial->getWorldBackground() );
	params.m_fGroundLevel = ( float )( ( real_t )( pMaterial->getShaderParam( "ground_level" ) ) );
	params.m_fRegionBlend = ( float )( ( real_t )( pMaterial->getShaderParam( "region_blend" ) ) );
	params.m_fHeightMin = s_fRegionHeightMin;
	params.m_fHeightRange = s_fRegionHeightRange;
	params.m_bControlMapsImplicit = data->areControlMapsImplicit() ? 1u : 0u;
	rRequest.m_pushConstants = copyToBytes( &params, sizeof( params ) );
	return rRequest.valid();
}

bool T3DCompute::dispatchDisplacementRequest( const t3dDisplacementRequest_t &rRequest, RID &rTexture ) {
	rTexture = RID();
	if ( !rRequest.valid() || !ensureRenderDevice() ||
			!ensureDisplacementResources( rRequest.m_nWidth, rRequest.m_nHeight, rRequest.m_shaderSpirv ) ) {
		return false;
	}
	if ( !m_bDisplacementRegionUploaded || m_nDisplacementRegionHash != rRequest.m_nRegionMapHash ) {
		const PackedByteArray region_bytes = copyToBytes(
				rRequest.m_regionMap.ptr(), ( size_t )( rRequest.m_regionMap.size() ) * sizeof( int32_t ) );
		const int64_t expected_region_bytes =
				( int64_t )( rRequest.m_regionMap.size() ) * ( int64_t )( sizeof( int32_t ) );
		if ( region_bytes.size() != expected_region_bytes ||
				expected_region_bytes > 4096 * ( int64_t )( sizeof( int32_t ) ) ||
				m_pRenderRd->buffer_update(
						m_hDisplacementRegionBuffer, 0u, ( uint32_t )( region_bytes.size() ), region_bytes ) != OK ) {
			setError( "Failed to upload displacement region topology" );
			m_bDisplacementRegionUploaded = false;
			return false;
		}
		m_nDisplacementRegionHash = rRequest.m_nRegionMapHash;
		m_bDisplacementRegionUploaded = true;
	}
	if ( !m_bDisplacementMaterialUploaded || m_nDisplacementMaterialHash != rRequest.m_nMaterialDataHash ) {
		if ( rRequest.m_materialData.size() != 32 * 4 * ( int64_t )( sizeof( float ) ) ||
				m_pRenderRd->buffer_update( m_hDisplacementMaterialBuffer,
						0u,
						( uint32_t )( rRequest.m_materialData.size() ),
						rRequest.m_materialData ) != OK ) {
			setError( "Failed to upload displacement material data" );
			m_bDisplacementMaterialUploaded = false;
			return false;
		}
		m_nDisplacementMaterialHash = rRequest.m_nMaterialDataHash;
		m_bDisplacementMaterialUploaded = true;
	}

	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) {
		setError( "RenderingServer unavailable during displacement dispatch" );
		return false;
	}
	const RID height_texture = pServer->texture_get_rd_texture( rRequest.m_hHeightTexture );
	const RID control_texture = pServer->texture_get_rd_texture( rRequest.m_hControlTexture );
	const RID displacement_texture = pServer->texture_get_rd_texture( rRequest.m_hDisplacementTexture );
	if ( !height_texture.is_valid() || !control_texture.is_valid() || !displacement_texture.is_valid() ) {
		setError( "Displacement texture arrays are unavailable on the global RenderingDevice" );
		return false;
	}

	const bool bBindingsChanged = !m_hDisplacementUniformSet.is_valid() ||
			m_hDisplacementBoundHeight != height_texture || m_hDisplacementBoundControl != control_texture ||
			m_hDisplacementBoundDisplacement != displacement_texture;
	if ( bBindingsChanged ) {
		freeRenderUniformSet( m_hDisplacementUniformSet );
		TypedArray<Ref<RDUniform>> pUniforms;
		pUniforms.push_back( t3dComputeMakeSampledTextureUniform( 0, m_hNearestSampler, height_texture ) );
		pUniforms.push_back( t3dComputeMakeSampledTextureUniform( 1, m_hNearestSampler, control_texture ) );
		pUniforms.push_back( t3dComputeMakeSampledTextureUniform( 2, m_hLinearRepeatSampler, displacement_texture ) );
		pUniforms.push_back( t3dComputeMakeImageUniform( 3, m_hDisplacementOutputTexture ) );
		pUniforms.push_back( t3dComputeMakeStorageUniform( 4, m_hDisplacementRegionBuffer ) );
		pUniforms.push_back( t3dComputeMakeStorageUniform( 5, m_hDisplacementMaterialBuffer ) );
		m_hDisplacementUniformSet = m_pRenderRd->uniform_set_create( pUniforms, m_displacementPipeline.m_hShader, 0u );
		if ( !m_hDisplacementUniformSet.is_valid() ) {
			setError( "Failed to bind displacement compute resources" );
			return false;
		}
		m_hDisplacementBoundHeight = height_texture;
		m_hDisplacementBoundControl = control_texture;
		m_hDisplacementBoundDisplacement = displacement_texture;
	}

	const int64_t list = m_pRenderRd->compute_list_begin();
	m_pRenderRd->compute_list_bind_compute_pipeline( list, m_displacementPipeline.m_hPipeline );
	m_pRenderRd->compute_list_bind_uniform_set( list, m_hDisplacementUniformSet, 0u );
	m_pRenderRd->compute_list_set_push_constant(
			list, rRequest.m_pushConstants, ( uint32_t )( rRequest.m_pushConstants.size() ) );
	m_pRenderRd->compute_list_dispatch( list, ( rRequest.m_nWidth + 7u ) / 8u, ( rRequest.m_nHeight + 7u ) / 8u, 1u );
	m_pRenderRd->compute_list_end();
	m_pRenderRd->barrier( RenderingDevice::BARRIER_MASK_COMPUTE, RenderingDevice::BARRIER_MASK_RASTER );
	rTexture = m_hDisplacementRsTexture;
	return rTexture.is_valid();
}

void T3DCompute::shutdownDisplacementResources( void ) {
	freeDisplacementTexture();
	freeRenderPipeline( m_displacementPipeline );
	freeRenderRid( m_hNearestSampler );
	freeRenderRid( m_hLinearRepeatSampler );
	m_pRenderRd = nullptr;
	m_bRenderDeviceAttempted = false;
}

bool T3DCompute::isAvailable( void ) {
	return ensureDevice();
}

void T3DCompute::processBrushCpu( eastl::vector<t3dBrushElement_t> &rElements, const t3dBrushParams_t &p ) {
	for ( t3dBrushElement_t &e : rElements ) {
		if ( ( e.m_nFlags & 1u ) == 0u ) {
			continue;
		}
		const float fAlpha = t3dComputeBitsToFloat( e.m_nAlpha );
		if ( p.m_nMapType == s_nTypeHeightValue ) {
			float fSource = t3dComputeBitsToFloat( e.m_nValue );
			if ( !std::isfinite( fSource ) ) {
				fSource = 0.0f;
			}
			float fResult = fSource;
			const float fAmount = fAlpha * p.m_fStrength;
			if ( p.m_nOperation == s_nOpAddValue ) {
				if ( p.m_nTool == s_nToolHeightValue ) {
					fResult = TERRAIN3D_LERP( fSource, p.m_fSelectedHeight, TERRAIN3D_CLAMP01( fAmount ) );
				} else if ( ( p.m_nFeatureFlags & s_nFeatureAltValue ) &&
						( p.m_nFeatureFlags & s_nFeatureCenterValidValue ) ) {
					fResult = std::clamp( p.m_fBrushCenterHeight + fAmount, fSource, fSource + fAmount );
				} else {
					fResult = fSource + fAmount;
				}
			} else if ( p.m_nOperation == s_nOpSubtractValue ) {
				if ( p.m_nTool == s_nToolHeightValue ) {
					fResult = TERRAIN3D_LERP( fSource, p.m_fSelectedHeight, TERRAIN3D_CLAMP01( fAmount ) );
				} else if ( ( p.m_nFeatureFlags & s_nFeatureAltValue ) &&
						( p.m_nFeatureFlags & s_nFeatureCenterValidValue ) ) {
					fResult = std::clamp( p.m_fBrushCenterHeight - fAmount, fSource - fAmount, fSource );
				} else {
					fResult = fSource - fAmount;
				}
			} else if ( p.m_nOperation == s_nOpGradientValue ) {
				fResult = TERRAIN3D_LERP( fSource, t3dComputeBitsToFloat( e.m_nTarget ), TERRAIN3D_CLAMP01( fAmount ) );
			}
			e.m_nValue = t3dComputeFloatToBits( fResult );
		} else if ( p.m_nMapType == s_nTypeControlValue ) {
			uint32_t base = ( e.m_nValue >> 27u ) & 31u;
			uint32_t overlay = ( e.m_nValue >> 22u ) & 31u;
			float fBlend = ( float )( ( e.m_nValue >> 14u ) & 255u ) / s_fByteMaxFloat;
			uint32_t rotation = ( e.m_nValue >> 10u ) & 15u;
			uint32_t scale = ( e.m_nValue >> 7u ) & 7u;
			bool bHole = ( ( e.m_nValue >> 2u ) & 1u ) != 0u;
			bool bNav = ( ( e.m_nValue >> 1u ) & 1u ) != 0u;
			bool bAutomatic = ( e.m_nValue & 1u ) != 0u;
			if ( p.m_nTool == s_nToolTextureValue ) {
				if ( p.m_nOperation == s_nOpReplaceValue && fAlpha > 0.5f ) {
					if ( p.m_nFeatureFlags & s_nFeatureTextureValue ) {
						base = p.m_nAssetId;
						overlay = p.m_nAssetId;
						fBlend = 0.0f;
						bAutomatic = false;
					}
					if ( base == p.m_nAssetId && !bAutomatic ) {
						if ( p.m_nFeatureFlags & s_nFeatureAngleValue ) {
							rotation = p.m_nUvRotation;
						}
						if ( p.m_nFeatureFlags & s_nFeatureScaleValue ) {
							scale = p.m_nUvScale;
						}
					}
				} else if ( p.m_nOperation == s_nOpAddValue ) {
					const float fSpray = std::clamp( p.m_fStrength * 0.05f, 0.004f, 0.25f );
					const float fBrushValue = TERRAIN3D_CLAMP01( fAlpha * fSpray );
					if ( ( p.m_nFeatureFlags & s_nFeatureTextureValue ) && fAlpha * p.m_fStrength * 11.0f > 0.1f ) {
						bAutomatic = false;
						if ( p.m_nAssetId != base && p.m_nAssetId != overlay ) {
							if ( p.m_nFeatureFlags & s_nFeatureAltValue ) {
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
					if ( ( ( base == p.m_nAssetId && fBlend < 0.5f ) ||
								 ( overlay == p.m_nAssetId && fBlend >= 0.5f ) ) &&
							!bAutomatic && fAlpha > 0.5f ) {
						if ( p.m_nFeatureFlags & s_nFeatureAngleValue ) {
							rotation = p.m_nUvRotation;
						}
						if ( p.m_nFeatureFlags & s_nFeatureScaleValue ) {
							scale = p.m_nUvScale;
						}
					}
				} else if ( p.m_nOperation == s_nOpSubtractValue ) {
					const float fBrushValue =
							TERRAIN3D_CLAMP01( fAlpha * std::clamp( p.m_fStrength * 0.05f, 0.004f, 0.25f ) );
					if ( base == p.m_nAssetId ) {
						fBlend = TERRAIN3D_CLAMP01( fBlend + fBrushValue );
					}
					if ( overlay == p.m_nAssetId ) {
						fBlend = TERRAIN3D_CLAMP01( fBlend - fBrushValue );
					}
				}
			} else if ( p.m_nTool == s_nToolAutoshaderValue && fAlpha > 0.5f ) {
				bAutomatic = p.m_nOperation == s_nOpAddValue;
				scale = 0u;
				rotation = 0u;
			} else if ( p.m_nTool == s_nToolHolesValue && fAlpha > 0.5f ) {
				bHole = p.m_nOperation == s_nOpAddValue;
			} else if ( p.m_nTool == s_nToolNavigationValue && fAlpha > 0.5f ) {
				bNav = p.m_nOperation == s_nOpAddValue;
			}
			const uint32_t blend_u8 =
					( uint32_t )( std::clamp( std::round( fBlend * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
			e.m_nValue = ( ( base & 31u ) << 27u ) | ( ( overlay & 31u ) << 22u ) | ( ( blend_u8 & 255u ) << 14u ) |
					( ( rotation & 15u ) << 10u ) | ( ( scale & 7u ) << 7u ) | ( bHole ? 4u : 0u ) |
					( bNav ? 2u : 0u ) | ( bAutomatic ? 1u : 0u );
		} else if ( p.m_nMapType == s_nTypeColorValue ) {
			float fR = TERRAIN3D_UNPACK_COLOR( e.m_nValue, 0 ), g = TERRAIN3D_UNPACK_COLOR( e.m_nValue, 8 ),
				  b = TERRAIN3D_UNPACK_COLOR( e.m_nValue, 16 ), a = TERRAIN3D_UNPACK_COLOR( e.m_nValue, 24 );
			const float fWeight = TERRAIN3D_CLAMP01( fAlpha * p.m_fStrength );
			if ( p.m_nTool == s_nToolColorValue ) {
				const uint32_t fTarget = p.m_nOperation == s_nOpAddValue ? p.m_nColorRgba8 : 0xffffffffu;
				fR = TERRAIN3D_LERP( fR, TERRAIN3D_UNPACK_COLOR( fTarget, 0 ), fWeight );
				g = TERRAIN3D_LERP( g, TERRAIN3D_UNPACK_COLOR( fTarget, 8 ), fWeight );
				b = TERRAIN3D_LERP( b, TERRAIN3D_UNPACK_COLOR( fTarget, 16 ), fWeight );
			} else if ( p.m_nTool == s_nToolRoughnessValue ) {
				const float fTarget = p.m_nOperation == s_nOpAddValue ? 0.5f + 0.5f * p.m_fRoughness : 0.5f;
				a = TERRAIN3D_LERP( a, fTarget, fWeight );
				a = ( float )( ( int )( a * s_fByteMaxFloat ) ) / s_fByteMaxFloat;
			}
			e.m_nValue = t3dComputePackColor( fR, g, b, a );
		}
	}
}

bool T3DCompute::processBrush( eastl::vector<t3dBrushElement_t> &rElements, const t3dBrushParams_t &rParams ) {
	if ( rElements.empty() ) {
		return true;
	}
	if ( rElements.size() > ( size_t )( std::numeric_limits<uint32_t>::max() ) ) {
		setError( "Brush batch exceeds RenderingDevice dispatch limits" );
		return false;
	}
	if ( !ensurePipeline(
				 m_brushPipeline, "res://addons/terrain_3d/compute/terrain_brush.glsl", "Terrain3D brush compute" ) ) {
		return false;
	}
	const uint64_t byte_count_64 = ( uint64_t )( rElements.size() ) * sizeof( t3dBrushElement_t );
	if ( byte_count_64 > std::numeric_limits<uint32_t>::max() ) {
		setError( "Brush batch exceeds 4 GiB" );
		return false;
	}
	const uint32_t byte_count = ( uint32_t )( byte_count_64 );
	if ( !ensureBrushCapacity( byte_count ) ) {
		return false;
	}
	const PackedByteArray input = copyToBytes( rElements.data(), byte_count );
	if ( m_pRd->buffer_update( m_hBrushBuffer, 0u, byte_count, input ) != OK ) {
		setError( "Failed to upload brush compute batch" );
		return false;
	}
	t3dBrushParams_t params = rParams;
	params.m_nCount = ( uint32_t )( rElements.size() );
	const PackedByteArray push = copyToBytes( &params, sizeof( params ) );
	const int64_t list = m_pRd->compute_list_begin();
	m_pRd->compute_list_bind_compute_pipeline( list, m_brushPipeline.m_hPipeline );
	m_pRd->compute_list_bind_uniform_set( list, m_hBrushUniformSet, 0u );
	m_pRd->compute_list_set_push_constant( list, push, ( uint32_t )( sizeof( params ) ) );
	m_pRd->compute_list_dispatch(
			list, ( params.m_nCount + s_nComputeWorkgroupSize - 1u ) / s_nComputeWorkgroupSize, 1u, 1u );
	m_pRd->compute_list_end();
	m_pRd->submit();
	m_pRd->sync();
	const PackedByteArray output = m_pRd->buffer_get_data( m_hBrushBuffer, 0u, byte_count );
	if ( output.size() != byte_count ) {
		setError( "Brush compute readback returned an unexpected byte count" );
		return false;
	}
	std::memcpy( rElements.data(), output.ptr(), byte_count );
	return true;
}

bool T3DCompute::generateDerivedMap( const eastl::vector<float> &fHeightHalo,
		const int nWidth,
		const int nHeight,
		const float fVertexSpacing,
		PackedByteArray &rRgba8 ) {
	if ( nWidth <= 0 || nHeight <= 0 || fVertexSpacing <= 0.0f ) {
		setError( "Invalid derived-map dimensions or vertex spacing" );
		return false;
	}
	const size_t halo_size = ( size_t )( nWidth + 2 ) * ( size_t )( nHeight + 2 );
	if ( fHeightHalo.size() != halo_size ) {
		setError( "Derived-map halo does not match tile dimensions" );
		return false;
	}
	if ( !ensurePipeline( m_derivedPipeline,
				 "res://addons/terrain_3d/compute/terrain_derived.glsl",
				 "Terrain3D derived map compute" ) ) {
		return false;
	}
	const uint64_t input_bytes_64 = ( uint64_t )( fHeightHalo.size() ) * sizeof( float );
	const uint64_t output_bytes_64 = ( uint64_t )( nWidth ) * ( uint64_t )( nHeight ) * sizeof( uint32_t );
	if ( input_bytes_64 > std::numeric_limits<uint32_t>::max() ||
			output_bytes_64 > std::numeric_limits<uint32_t>::max() ) {
		setError( "Derived-map buffer exceeds 4 GiB" );
		return false;
	}
	const uint32_t input_bytes = ( uint32_t )( input_bytes_64 );
	const uint32_t output_bytes = ( uint32_t )( output_bytes_64 );
	if ( !ensureDerivedCapacity( input_bytes, output_bytes ) ) {
		return false;
	}
	const PackedByteArray input = copyToBytes( fHeightHalo.data(), input_bytes );
	if ( m_pRd->buffer_update( m_hDerivedInputBuffer, 0u, input_bytes, input ) != OK ) {
		setError( "Failed to upload derived-map height halo" );
		return false;
	}
	typedef struct t3dDerivedParams_s {
		uint32_t m_nWidth;
		uint32_t m_nHeight;
		uint32_t m_nHaloStride;
		float m_fVertexSpacing;
	} t3dDerivedParams_t;
	t3dDerivedParams_t params{
		( uint32_t )( nWidth ), ( uint32_t )( nHeight ), ( uint32_t )( nWidth + 2 ), fVertexSpacing
	};
	const PackedByteArray push = copyToBytes( &params, sizeof( params ) );
	const uint32_t count = ( uint32_t )( nWidth * nHeight );
	const int64_t list = m_pRd->compute_list_begin();
	m_pRd->compute_list_bind_compute_pipeline( list, m_derivedPipeline.m_hPipeline );
	m_pRd->compute_list_bind_uniform_set( list, m_hDerivedUniformSet, 0u );
	m_pRd->compute_list_set_push_constant( list, push, ( uint32_t )( sizeof( params ) ) );
	m_pRd->compute_list_dispatch( list, ( count + s_nComputeWorkgroupSize - 1u ) / s_nComputeWorkgroupSize, 1u, 1u );
	m_pRd->compute_list_end();
	m_pRd->submit();
	m_pRd->sync();
	rRgba8 = m_pRd->buffer_get_data( m_hDerivedOutputBuffer, 0u, output_bytes );
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
	freeUniformSet( m_hBrushUniformSet );
	freeUniformSet( m_hDerivedUniformSet );
	freeBuffer( m_hBrushBuffer );
	freeBuffer( m_hDerivedInputBuffer );
	freeBuffer( m_hDerivedOutputBuffer );
	freePipeline( m_brushPipeline );
	freePipeline( m_derivedPipeline );
	memdelete( m_pRd );
	m_pRd = nullptr;
}
