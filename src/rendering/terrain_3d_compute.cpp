// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/terrain_3d_compute.h"

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_assets.h"
#include "core/logger.h"
#include "core/terrain_3d_compiler.h"
#include "data/terrain_3d_data.h"
#include "data/terrain_3d_region_codec.h"
#include "rendering/terrain_3d_material.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "terrain_3d.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
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

Terrain3DCompute::~Terrain3DCompute( void ) {
	shutdown();
}

uint32_t Terrain3DCompute::growthCapacity( const uint32_t nRequired ) {
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

PackedByteArray Terrain3DCompute::copyToBytes( const void *pData, const size_t nSize ) {
	PackedByteArray bytes;
	if ( !pData || nSize == 0u || nSize > ( size_t )( std::numeric_limits<int64_t>::max() ) ) {
		return bytes;
	}
	bytes.resize( ( int64_t )( nSize ) );
	std::memcpy( bytes.ptrw(), pData, nSize );
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
	return true;
}

bool Terrain3DCompute::ensureRenderDevice( void ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	if ( m_pRenderRd ) {
		if ( context.validateDevice() && context.getDevice() == m_pRenderRd ) return true;
		m_pRenderRd = nullptr;
		m_bRenderDeviceAttempted = false;
	}
	if ( m_bRenderDeviceAttempted ) return false;
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

bool Terrain3DCompute::ensurePipeline( t3dPipeline_t &rPipeline, const String &rPath, const String &rName ) {
	if ( rPipeline.m_hPipeline.is_valid() ) {
		return true;
	}
	if ( rPipeline.m_bAttempted || !ensureDevice() ) {
		return false;
	}
	rPipeline.m_bAttempted = true;
	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	if ( !pLoader ) {
		setError( String( "ResourceLoader unavailable while loading " ) + rPath );
		return false;
	}
	const Ref<RDShaderFile> pShaderFile = pLoader->load( rPath );
	if ( pShaderFile.is_null() ) {
		setError( String( "Compute shader resource is unavailable: " ) + rPath );
		return false;
	}
	const Ref<RDShaderSPIRV> pSpirv = pShaderFile->get_spirv();
	if ( pSpirv.is_null() ) {
		setError( String( "Compute shader has no compiled SPIR-V: " ) + rPath );
		return false;
	}
	rPipeline.m_hShader = m_pRd->shader_create_from_spirv( pSpirv, rName );
	if ( !rPipeline.m_hShader.is_valid() ) {
		setError( String( "Failed to create compute shader: " ) + rPath );
		return false;
	}
	rPipeline.m_hPipeline = m_pRd->compute_pipeline_create( rPipeline.m_hShader );
	if ( !rPipeline.m_hPipeline.is_valid() ) {
		setError( String( "Failed to create compute pipeline: " ) + rPath );
		m_pRd->free_rid( rPipeline.m_hShader );
		rPipeline.m_hShader = RID();
		return false;
	}
	return true;
}

bool Terrain3DCompute::ensureRenderPipeline( t3dPipeline_t &rPipeline,
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
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	rPipeline.m_hShader = m_pRenderRd->shader_create_from_spirv( rSpirv, rName );
	if ( !rPipeline.m_hShader.is_valid() ) {
		setError( "Failed to create displacement compute shader" );
		return false;
	}
	if ( !context.registerResource( rPipeline.m_hShader,
					String( "T3D.Compute.Displacement.Shader" ),
					Terrain3DRdContext::RESOURCE_SHADER,
					Terrain3DRdContext::MEMORY_NONE ) ) {
		setError( "Failed to create displacement compute shader" );
		m_pRenderRd->free_rid( rPipeline.m_hShader );
		rPipeline.m_hShader = RID();
		return false;
	}
	rPipeline.m_hPipeline = m_pRenderRd->compute_pipeline_create( rPipeline.m_hShader );
	if ( !rPipeline.m_hPipeline.is_valid() ||
			!context.registerResource( rPipeline.m_hPipeline,
					String( "T3D.Compute.Displacement.Pipeline" ),
					Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
					Terrain3DRdContext::MEMORY_NONE ) ) {
		setError( "Failed to create displacement compute pipeline" );
		if ( rPipeline.m_hPipeline.is_valid() ) m_pRenderRd->free_rid( rPipeline.m_hPipeline );
		rPipeline.m_hPipeline = RID();
		context.freeResource( rPipeline.m_hShader );
		return false;
	}
	return true;
}

void Terrain3DCompute::freeUniformSet( RID &rUniformSet ) {
	if ( m_pRd && rUniformSet.is_valid() ) {
		m_pRd->free_rid( rUniformSet );
	}
	rUniformSet = RID();
}

void Terrain3DCompute::freeBuffer( RID &rBuffer ) {
	if ( m_pRd && rBuffer.is_valid() ) {
		m_pRd->free_rid( rBuffer );
	}
	rBuffer = RID();
}

void Terrain3DCompute::freePipeline( t3dPipeline_t &rPipeline ) {
	if ( m_pRd && rPipeline.m_hPipeline.is_valid() ) {
		m_pRd->free_rid( rPipeline.m_hPipeline );
	}
	if ( m_pRd && rPipeline.m_hShader.is_valid() ) {
		m_pRd->free_rid( rPipeline.m_hShader );
	}
	rPipeline = t3dPipeline_t{};
}

void Terrain3DCompute::freeRenderUniformSet( RID &rUniformSet ) {
	Terrain3DRdContext::getShared().freeResource( rUniformSet );
}

void Terrain3DCompute::freeRenderRid( RID &rRid ) {
	Terrain3DRdContext::getShared().freeResource( rRid );
}

void Terrain3DCompute::freeRenderPipeline( t3dPipeline_t &rPipeline ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	context.freeResource( rPipeline.m_hPipeline );
	context.freeResource( rPipeline.m_hShader );
	rPipeline = t3dPipeline_t{};
}

bool Terrain3DCompute::ensureBrushCapacity( const uint32_t nBytes ) {
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

bool Terrain3DCompute::ensureDerivedCapacity( const uint32_t nSlot,
		const uint32_t nInputBytes,
		const uint32_t nOutputBytes ) {
	if ( nSlot >= s_nDerivedReadbackSlotCount ) {
		return false;
	}
	if ( nInputBytes <= m_nszDerivedInputCapacity[nSlot] && nOutputBytes <= m_nszDerivedOutputCapacity[nSlot] &&
			m_szDerivedInputBuffer[nSlot].is_valid() && m_szDerivedOutputBuffer[nSlot].is_valid() &&
			m_szDerivedUniformSet[nSlot].is_valid() ) {
		return true;
	}
	freeUniformSet( m_szDerivedUniformSet[nSlot] );
	freeBuffer( m_szDerivedInputBuffer[nSlot] );
	freeBuffer( m_szDerivedOutputBuffer[nSlot] );
	m_nszDerivedInputCapacity[nSlot] = growthCapacity( nInputBytes );
	m_nszDerivedOutputCapacity[nSlot] = growthCapacity( nOutputBytes );
	m_szDerivedInputBuffer[nSlot] = m_pRd->storage_buffer_create( m_nszDerivedInputCapacity[nSlot] );
	m_szDerivedOutputBuffer[nSlot] = m_pRd->storage_buffer_create( m_nszDerivedOutputCapacity[nSlot] );
	if ( !m_szDerivedInputBuffer[nSlot].is_valid() || !m_szDerivedOutputBuffer[nSlot].is_valid() ) {
		setError( "Failed to allocate derived-map compute buffers" );
		return false;
	}
	TypedArray<Ref<RDUniform>> pUniforms;
	pUniforms.push_back( t3dComputeMakeStorageUniform( 0, m_szDerivedInputBuffer[nSlot] ) );
	pUniforms.push_back( t3dComputeMakeStorageUniform( 1, m_szDerivedOutputBuffer[nSlot] ) );
	m_szDerivedUniformSet[nSlot] = m_pRd->uniform_set_create( pUniforms, m_derivedPipeline.m_hShader, 0u );
	if ( !m_szDerivedUniformSet[nSlot].is_valid() ) {
		setError( "Failed to create derived-map compute uniform set" );
		return false;
	}
	return true;
}

void Terrain3DCompute::freeDisplacementTexture( void ) {
	freeRenderUniformSet( m_hDisplacementUniformSet );
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

bool Terrain3DCompute::ensureDisplacementResources( const uint32_t nWidth,
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
		if ( m_hNearestSampler.is_valid() &&
				!Terrain3DRdContext::getShared().registerResource( m_hNearestSampler,
						String( "T3D.Compute.Displacement.NearestSampler" ),
						Terrain3DRdContext::RESOURCE_SAMPLER,
						Terrain3DRdContext::MEMORY_NONE ) ) {
			m_pRenderRd->free_rid( m_hNearestSampler );
			m_hNearestSampler = RID();
		}
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
		if ( m_hLinearRepeatSampler.is_valid() &&
				!Terrain3DRdContext::getShared().registerResource( m_hLinearRepeatSampler,
						String( "T3D.Compute.Displacement.LinearRepeatSampler" ),
						Terrain3DRdContext::RESOURCE_SAMPLER,
						Terrain3DRdContext::MEMORY_NONE ) ) {
			m_pRenderRd->free_rid( m_hLinearRepeatSampler );
			m_hLinearRepeatSampler = RID();
		}
	}
	if ( !m_hNearestSampler.is_valid() || !m_hLinearRepeatSampler.is_valid() ) {
		setError( "Failed to create displacement samplers" );
		return false;
	}
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	if ( !m_hDisplacementRegionBuffer.is_valid() ) {
		const uint64_t region_bytes = 4096u * sizeof( int32_t );
		m_hDisplacementRegionBuffer = m_pRenderRd->storage_buffer_create( region_bytes );
		if ( m_hDisplacementRegionBuffer.is_valid() &&
				!context.registerResource( m_hDisplacementRegionBuffer,
						String( "T3D.Compute.Displacement.RegionBuffer" ),
						Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
						Terrain3DRdContext::MEMORY_STORAGE,
						region_bytes ) ) {
			m_pRenderRd->free_rid( m_hDisplacementRegionBuffer );
			m_hDisplacementRegionBuffer = RID();
		}
		m_bDisplacementRegionUploaded = false;
	}
	if ( !m_hDisplacementMaterialBuffer.is_valid() ) {
		const uint64_t material_bytes = 32u * 4u * sizeof( float );
		m_hDisplacementMaterialBuffer = m_pRenderRd->storage_buffer_create( material_bytes );
		if ( m_hDisplacementMaterialBuffer.is_valid() &&
				!context.registerResource( m_hDisplacementMaterialBuffer,
						String( "T3D.Compute.Displacement.MaterialBuffer" ),
						Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
						Terrain3DRdContext::MEMORY_MATERIAL_TABLE,
						material_bytes ) ) {
			m_pRenderRd->free_rid( m_hDisplacementMaterialBuffer );
			m_hDisplacementMaterialBuffer = RID();
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
	const uint64_t output_bytes = ( uint64_t )( nWidth ) * ( uint64_t )( nHeight ) * 8u;
	if ( !m_hDisplacementOutputTexture.is_valid() ||
			!context.registerResource( m_hDisplacementOutputTexture,
					String( "T3D.Compute.Displacement.OutputTexture" ),
					Terrain3DRdContext::RESOURCE_TEXTURE,
					Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
					output_bytes ) ) {
		if ( m_hDisplacementOutputTexture.is_valid() ) m_pRenderRd->free_rid( m_hDisplacementOutputTexture );
		m_hDisplacementOutputTexture = RID();
		setError( "Failed to allocate displacement storage texture" );
		return false;
	}
	m_nDisplacementWidth = nWidth;
	m_nDisplacementHeight = nHeight;
	return true;
}

bool Terrain3DCompute::prepareDisplacementRequest( Terrain3D *const pTerrain,
		const Vector3 &rTargetPosition,
		t3dDisplacementRequest_t &rRequest ) {
	rRequest = t3dDisplacementRequest_t{};
	if ( !pTerrain || pTerrain->getTessellationLevel() <= 0 ) {
		return false;
	}
	const Ref<Terrain3DMaterial> pMaterial = pTerrain->getMaterial();
	const Ref<Terrain3DAssets> pAssets = pTerrain->getAssets();
	Terrain3DData *const data = pTerrain->getData();
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
	if ( !Terrain3DRdContext::getGlobalDevice() ) {
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
	rRequest.m_hHeightTexture = data->getHeightMapsRid();
	rRequest.m_hControlTexture = data->getControlMapsRid();
	rRequest.m_hDisplacementTexture = pAssets->getDisplacementArrayRid();
	rRequest.m_regionMap = data->getRegionMap();
	rRequest.m_nRegionMapSize = ( uint32_t )( data->getRegionMapSizeInternal() );
	if ( rRequest.m_nWidth == 0u || rRequest.m_nHeight == 0u || rRequest.m_nRegionMapSize == 0u ||
			rRequest.m_regionMap.size() !=
					( int64_t )( rRequest.m_nRegionMapSize ) * ( int64_t )( rRequest.m_nRegionMapSize ) ||
			!rRequest.m_hHeightTexture.is_valid() || !rRequest.m_hControlTexture.is_valid() ||
			!rRequest.m_hDisplacementTexture.is_valid() ) {
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

bool Terrain3DCompute::dispatchDisplacementRequest( const t3dDisplacementRequest_t &rRequest, RID &rTexture ) {
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

	const RID height_texture = rRequest.m_hHeightTexture;
	const RID control_texture = rRequest.m_hControlTexture;
	const RID displacement_texture = rRequest.m_hDisplacementTexture;
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
		if ( !m_hDisplacementUniformSet.is_valid() ||
				!Terrain3DRdContext::getShared().registerResource( m_hDisplacementUniformSet,
						String( "T3D.Compute.Displacement.UniformSet" ),
						Terrain3DRdContext::RESOURCE_UNIFORM_SET,
						Terrain3DRdContext::MEMORY_NONE ) ) {
			if ( m_hDisplacementUniformSet.is_valid() ) m_pRenderRd->free_rid( m_hDisplacementUniformSet );
			m_hDisplacementUniformSet = RID();
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
	rTexture = m_hDisplacementOutputTexture;
	return rTexture.is_valid();
}

void Terrain3DCompute::shutdownDisplacementResources( void ) {
	freeDisplacementTexture();
	freeRenderPipeline( m_displacementPipeline );
	freeRenderRid( m_hNearestSampler );
	freeRenderRid( m_hLinearRepeatSampler );
	m_pRenderRd = nullptr;
	m_bRenderDeviceAttempted = false;
}

bool Terrain3DCompute::isAvailable( void ) {
	return ensureDevice();
}

void Terrain3DCompute::processBrushCpu( eastl::vector<t3dBrushElement_t> &rElements, const t3dBrushParams_t &p ) {
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

bool Terrain3DCompute::processBrush( eastl::vector<t3dBrushElement_t> &rElements, const t3dBrushParams_t &rParams ) {
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

bool Terrain3DCompute::generateDerivedMaps( t3dDerivedMapComputeJob_t *const pJobs, const uint32_t nJobCount ) {
	if ( !pJobs || nJobCount == 0u || nJobCount > s_nDerivedReadbackSlotCount ) {
		setError( "Derived-map compute batch must contain one to three jobs" );
		return false;
	}
	if ( !ensurePipeline( m_derivedPipeline,
				 "res://addons/terrain_3d/compute/terrain_derived.glsl",
				 "Terrain3D derived map compute" ) ) {
		return false;
	}
	typedef struct t3dDerivedParams_s {
		uint32_t m_nWidth;
		uint32_t m_nHeight;
		uint32_t m_nHaloStride;
		float m_fVertexSpacing;
	} t3dDerivedParams_t;

	for ( uint32_t slot = 0u; slot < nJobCount; ++slot ) {
		t3dDerivedMapComputeJob_t &job = pJobs[slot];
		job.m_bSuccess = false;
		if ( !job.m_pHeightHalo || !job.m_pRgbaOutput || job.m_nWidth == 0u || job.m_nHeight == 0u ||
				job.m_fVertexSpacing <= 0.0f || job.m_nHaloElements != ( job.m_nWidth + 2u ) * ( job.m_nHeight + 2u ) ||
				job.m_nOutputBytes != job.m_nWidth * job.m_nHeight * sizeof( uint32_t ) ) {
			setError( "Invalid derived-map batch job" );
			return false;
		}
		const uint64_t input_bytes_64 = ( uint64_t )job.m_nHaloElements * sizeof( float );
		if ( input_bytes_64 > std::numeric_limits<uint32_t>::max() ) {
			setError( "Derived-map input exceeds 4 GiB" );
			return false;
		}
		const uint32_t input_bytes = ( uint32_t )input_bytes_64;
		if ( !ensureDerivedCapacity( slot, input_bytes, job.m_nOutputBytes ) ) {
			return false;
		}
		const PackedByteArray input = copyToBytes( job.m_pHeightHalo, input_bytes );
		if ( m_pRd->buffer_update( m_szDerivedInputBuffer[slot], 0u, input_bytes, input ) != OK ) {
			setError( "Failed to upload derived-map height halo" );
			return false;
		}
	}

	const int64_t list = m_pRd->compute_list_begin();
	m_pRd->compute_list_bind_compute_pipeline( list, m_derivedPipeline.m_hPipeline );
	for ( uint32_t slot = 0u; slot < nJobCount; ++slot ) {
		const t3dDerivedMapComputeJob_t &job = pJobs[slot];
		const t3dDerivedParams_t params{ job.m_nWidth, job.m_nHeight, job.m_nWidth + 2u, job.m_fVertexSpacing };
		const PackedByteArray push = copyToBytes( &params, sizeof( params ) );
		const uint32_t count = job.m_nWidth * job.m_nHeight;
		m_pRd->compute_list_bind_uniform_set( list, m_szDerivedUniformSet[slot], 0u );
		m_pRd->compute_list_set_push_constant( list, push, ( uint32_t )( sizeof( params ) ) );
		m_pRd->compute_list_dispatch( list, ( count + s_nComputeWorkgroupSize - 1u ) / s_nComputeWorkgroupSize, 1u, 1u );
	}
	m_pRd->compute_list_end();
	m_pRd->submit();
	// All outputs are consumed immediately by the edit transaction. This is the
	// single dependency point for the whole batch, rather than one sync per job.
	m_pRd->sync();
	for ( uint32_t slot = 0u; slot < nJobCount; ++slot ) {
		t3dDerivedMapComputeJob_t &job = pJobs[slot];
		const PackedByteArray output = m_pRd->buffer_get_data( m_szDerivedOutputBuffer[slot], 0u, job.m_nOutputBytes );
		if ( output.size() != job.m_nOutputBytes ) {
			setError( "Derived-map compute readback returned an unexpected byte count" );
			continue;
		}
		std::memcpy( job.m_pRgbaOutput, output.ptr(), job.m_nOutputBytes );
		job.m_bSuccess = true;
	}
	return true;
}

void Terrain3DCompute::shutdown( void ) {
	if ( !m_pRd ) {
		return;
	}
	freeUniformSet( m_hBrushUniformSet );
	freeBuffer( m_hBrushBuffer );
	for ( uint32_t slot = 0u; slot < s_nDerivedReadbackSlotCount; ++slot ) {
		freeUniformSet( m_szDerivedUniformSet[slot] );
		freeBuffer( m_szDerivedInputBuffer[slot] );
		freeBuffer( m_szDerivedOutputBuffer[slot] );
		m_nszDerivedInputCapacity[slot] = 0u;
		m_nszDerivedOutputCapacity[slot] = 0u;
	}
	m_nBrushCapacity = 0u;
	freePipeline( m_brushPipeline );
	freePipeline( m_derivedPipeline );
	memdelete( m_pRd );
	m_pRd = nullptr;
}
