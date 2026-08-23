// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_texture_manager.h"

#include "core/logger.h"
#include "rendering/rd/terrain_3d_rd_context.h"

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/vector3.hpp>

Terrain3DRdTextureManager *Terrain3DRdTextureManager::s_pSingleton = nullptr;

void Terrain3DRdTextureManager::initializeSingleton( void ) {
	if ( s_pSingleton ) return;
	s_pSingleton = memnew( Terrain3DRdTextureManager );
}

void Terrain3DRdTextureManager::shutdownSingleton( void ) {
	Terrain3DRdTextureManager *const pManager = s_pSingleton;
	if ( !pManager ) return;
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && !pServer->is_on_render_thread() ) {
		pServer->call_on_render_thread( callable_mp( pManager, &Terrain3DRdTextureManager::renderShutdown ) );
		pServer->force_sync();
	} else {
		pManager->renderShutdown();
	}
	s_pSingleton = nullptr;
	memdelete( pManager );
}

bool Terrain3DRdTextureManager::imageFormatToDataFormat( const Image::Format format,
		RenderingDevice::DataFormat &rFormat ) {
	switch ( format ) {
		case Image::FORMAT_L8:
		case Image::FORMAT_R8:
			rFormat = RenderingDevice::DATA_FORMAT_R8_UNORM;
			return true;
		case Image::FORMAT_LA8:
		case Image::FORMAT_RG8:
			rFormat = RenderingDevice::DATA_FORMAT_R8G8_UNORM;
			return true;
		case Image::FORMAT_RGB8:
			rFormat = RenderingDevice::DATA_FORMAT_R8G8B8_UNORM;
			return true;
		case Image::FORMAT_RGBA8:
			rFormat = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
			return true;
		case Image::FORMAT_RGBA4444:
			rFormat = RenderingDevice::DATA_FORMAT_R4G4B4A4_UNORM_PACK16;
			return true;
		case Image::FORMAT_RGB565:
			rFormat = RenderingDevice::DATA_FORMAT_R5G6B5_UNORM_PACK16;
			return true;
		case Image::FORMAT_RF:
			rFormat = RenderingDevice::DATA_FORMAT_R32_SFLOAT;
			return true;
		case Image::FORMAT_RGF:
			rFormat = RenderingDevice::DATA_FORMAT_R32G32_SFLOAT;
			return true;
		case Image::FORMAT_RGBF:
			rFormat = RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT;
			return true;
		case Image::FORMAT_RGBAF:
			rFormat = RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT;
			return true;
		case Image::FORMAT_RH:
			rFormat = RenderingDevice::DATA_FORMAT_R16_SFLOAT;
			return true;
		case Image::FORMAT_RGH:
			rFormat = RenderingDevice::DATA_FORMAT_R16G16_SFLOAT;
			return true;
		case Image::FORMAT_RGBH:
			rFormat = RenderingDevice::DATA_FORMAT_R16G16B16_SFLOAT;
			return true;
		case Image::FORMAT_RGBAH:
			rFormat = RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT;
			return true;
		case Image::FORMAT_RGBE9995:
			rFormat = RenderingDevice::DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32;
			return true;
		case Image::FORMAT_DXT1:
			rFormat = RenderingDevice::DATA_FORMAT_BC1_RGB_UNORM_BLOCK;
			return true;
		case Image::FORMAT_DXT3:
			rFormat = RenderingDevice::DATA_FORMAT_BC2_UNORM_BLOCK;
			return true;
		case Image::FORMAT_DXT5:
		case Image::FORMAT_DXT5_RA_AS_RG:
			rFormat = RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK;
			return true;
		case Image::FORMAT_RGTC_R:
			rFormat = RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK;
			return true;
		case Image::FORMAT_RGTC_RG:
			rFormat = RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK;
			return true;
		case Image::FORMAT_BPTC_RGBA:
			rFormat = RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK;
			return true;
		case Image::FORMAT_BPTC_RGBF:
			rFormat = RenderingDevice::DATA_FORMAT_BC6H_SFLOAT_BLOCK;
			return true;
		case Image::FORMAT_BPTC_RGBFU:
			rFormat = RenderingDevice::DATA_FORMAT_BC6H_UFLOAT_BLOCK;
			return true;
		case Image::FORMAT_ETC:
		case Image::FORMAT_ETC2_RGB8:
			rFormat = RenderingDevice::DATA_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
			return true;
		case Image::FORMAT_ETC2_R11:
			rFormat = RenderingDevice::DATA_FORMAT_EAC_R11_UNORM_BLOCK;
			return true;
		case Image::FORMAT_ETC2_R11S:
			rFormat = RenderingDevice::DATA_FORMAT_EAC_R11_SNORM_BLOCK;
			return true;
		case Image::FORMAT_ETC2_RG11:
			rFormat = RenderingDevice::DATA_FORMAT_EAC_R11G11_UNORM_BLOCK;
			return true;
		case Image::FORMAT_ETC2_RG11S:
			rFormat = RenderingDevice::DATA_FORMAT_EAC_R11G11_SNORM_BLOCK;
			return true;
		case Image::FORMAT_ETC2_RGBA8:
		case Image::FORMAT_ETC2_RA_AS_RG:
			rFormat = RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
			return true;
		case Image::FORMAT_ETC2_RGB8A1:
			rFormat = RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
			return true;
		case Image::FORMAT_ASTC_4x4:
			rFormat = RenderingDevice::DATA_FORMAT_ASTC_4x4_UNORM_BLOCK;
			return true;
		case Image::FORMAT_ASTC_4x4_HDR:
			rFormat = RenderingDevice::DATA_FORMAT_ASTC_4x4_SFLOAT_BLOCK;
			return true;
		case Image::FORMAT_ASTC_8x8:
			rFormat = RenderingDevice::DATA_FORMAT_ASTC_8x8_UNORM_BLOCK;
			return true;
		case Image::FORMAT_ASTC_8x8_HDR:
			rFormat = RenderingDevice::DATA_FORMAT_ASTC_8x8_SFLOAT_BLOCK;
			return true;
		case Image::FORMAT_R16:
			rFormat = RenderingDevice::DATA_FORMAT_R16_UNORM;
			return true;
		case Image::FORMAT_RG16:
			rFormat = RenderingDevice::DATA_FORMAT_R16G16_UNORM;
			return true;
		case Image::FORMAT_RGB16:
			rFormat = RenderingDevice::DATA_FORMAT_R16G16B16_UNORM;
			return true;
		case Image::FORMAT_RGBA16:
			rFormat = RenderingDevice::DATA_FORMAT_R16G16B16A16_UNORM;
			return true;
		default:
			break;
	}
	rFormat = RenderingDevice::DATA_FORMAT_MAX;
	return false;
}

uint32_t Terrain3DRdTextureManager::getImageMipmapCount( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) return 0u;
	return rImage->has_mipmaps() ? ( uint32_t )( rImage->get_mipmap_count() + 1 ) : 1u;
}

bool Terrain3DRdTextureManager::buildLayout( const Ref<Image> &rImage,
		const uint32_t nLayers,
		const bool bLayered,
		const uint64_t nPayloadBytes,
		t3dRdTextureLayout_t &rLayout ) {
	rLayout = t3dRdTextureLayout_t{};
	if ( rImage.is_null() || rImage->is_empty() || nLayers == 0u || nPayloadBytes == 0u ||
			!imageFormatToDataFormat( rImage->get_format(), rLayout.m_format ) ) {
		return false;
	}
	rLayout.m_nWidth = ( uint32_t )( rImage->get_width() );
	rLayout.m_nHeight = ( uint32_t )( rImage->get_height() );
	rLayout.m_nLayers = nLayers;
	rLayout.m_nMipmaps = getImageMipmapCount( rImage );
	rLayout.m_type = bLayered ? RenderingDevice::TEXTURE_TYPE_2D_ARRAY : RenderingDevice::TEXTURE_TYPE_2D;
	rLayout.m_nUsageBits = ( uint64_t )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT ) |
			( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT ) |
			( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT ) |
			( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );
	rLayout.m_nBytes = nPayloadBytes;
	return rLayout.valid();
}

RID Terrain3DRdTextureManager::createTexture( const t3dRdTextureLayout_t &rLayout,
		const TypedArray<PackedByteArray> &rInitialData,
		const String &rName,
		const int nMemoryCategory ) {
	if ( !rLayout.valid() || rInitialData.size() != ( int64_t )( rLayout.m_nLayers ) ) return RID();
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) return RID();
	Ref<RDTextureFormat> pFormat;
	pFormat.instantiate();
	pFormat->set_width( rLayout.m_nWidth );
	pFormat->set_height( rLayout.m_nHeight );
	pFormat->set_depth( 1u );
	pFormat->set_array_layers( rLayout.m_nLayers );
	pFormat->set_mipmaps( rLayout.m_nMipmaps );
	pFormat->set_texture_type( rLayout.m_type );
	pFormat->set_format( rLayout.m_format );
	pFormat->set_usage_bits( rLayout.m_nUsageBits );

	std::lock_guard<std::mutex> createLock( m_createMutex );
	{
		std::lock_guard<std::mutex> resultLock( m_resultMutex );
		m_hCreateResult = RID();
		m_bCreateSucceeded = false;
	}
	if ( pServer->is_on_render_thread() ) {
		renderCreateTexture( pFormat, rInitialData, rName, rLayout.m_nBytes, nMemoryCategory );
	} else {
		pServer->call_on_render_thread( callable_mp( this, &Terrain3DRdTextureManager::renderCreateTexture )
								 .bind( pFormat, rInitialData, rName, rLayout.m_nBytes, nMemoryCategory ) );
		// Creation/recreation is a cold path and requires the RID before publication.
		pServer->force_sync();
	}
	std::lock_guard<std::mutex> resultLock( m_resultMutex );
	return m_bCreateSucceeded ? m_hCreateResult : RID();
}

bool Terrain3DRdTextureManager::updateTexture( const RID hTexture,
		const uint32_t nLayer,
		const PackedByteArray &rData ) {
	if ( !hTexture.is_valid() || rData.is_empty() ) return false;
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) return false;
	if ( pServer->is_on_render_thread() ) {
		renderUpdateTexture( hTexture, nLayer, rData );
	} else {
		pServer->call_on_render_thread(
				callable_mp( this, &Terrain3DRdTextureManager::renderUpdateTexture ).bind( hTexture, nLayer, rData ) );
	}
	return true;
}

bool Terrain3DRdTextureManager::updateTextureRegion( const RID hTexture,
		const RID hStaging,
		const uint32_t nDestinationX,
		const uint32_t nDestinationY,
		const uint32_t nWidth,
		const uint32_t nHeight,
		const PackedByteArray &rData ) {
	if ( !hTexture.is_valid() || !hStaging.is_valid() || nWidth == 0u || nHeight == 0u || rData.is_empty() ) return false;
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) return false;
	if ( pServer->is_on_render_thread() ) {
		renderUpdateTextureRegion( hTexture, hStaging, nDestinationX, nDestinationY, nWidth, nHeight, rData );
	} else {
		pServer->call_on_render_thread( callable_mp( this, &Terrain3DRdTextureManager::renderUpdateTextureRegion )
								 .bind( hTexture, hStaging, nDestinationX, nDestinationY, nWidth, nHeight, rData ) );
	}
	return true;
}

void Terrain3DRdTextureManager::freeTexture( const RID hTexture, const uint64_t nBytes ) {
	if ( !hTexture.is_valid() ) return;
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) return;
	if ( pServer->is_on_render_thread() ) {
		renderFreeTexture( hTexture, nBytes );
	} else {
		pServer->call_on_render_thread(
				callable_mp( this, &Terrain3DRdTextureManager::renderFreeTexture ).bind( hTexture, nBytes ) );
	}
}

void Terrain3DRdTextureManager::freeResource( const RID hRid, const uint64_t nBytes ) {
	if ( !hRid.is_valid() ) return;
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) return;
	if ( pServer->is_on_render_thread() ) {
		renderFreeResource( hRid, nBytes );
	} else {
		pServer->call_on_render_thread(
				callable_mp( this, &Terrain3DRdTextureManager::renderFreeResource ).bind( hRid, nBytes ) );
	}
}

void Terrain3DRdTextureManager::renderCreateTexture( const Ref<RDTextureFormat> &rFormat,
		const TypedArray<PackedByteArray> &rInitialData,
		const String &rName,
		const uint64_t nBytes,
		const int nMemoryCategory ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	if ( !context.isReady() && !context.initialize() ) return;
	RenderingDevice *const pRd = context.getDevice();
	if ( !pRd || !pRd->texture_is_format_supported_for_usage( rFormat->get_format(), rFormat->get_usage_bits() ) ) {
		context.recordResourceCreationFailure( rName, Terrain3DRdContext::RESOURCE_TEXTURE, "Unsupported generated texture format/usage" );
		return;
	}
	Ref<RDTextureView> pView;
	pView.instantiate();
	RID texture = pRd->texture_create( rFormat, pView, rInitialData );
	if ( !texture.is_valid() ) {
		context.recordResourceCreationFailure( rName, Terrain3DRdContext::RESOURCE_TEXTURE );
		return;
	}
	const Terrain3DRdContext::MemoryCategory category = nMemoryCategory >= 0 && nMemoryCategory < Terrain3DRdContext::MEMORY_CATEGORY_COUNT
			? ( Terrain3DRdContext::MemoryCategory )( nMemoryCategory )
			: Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE;
	if ( !context.registerResource( texture, rName, Terrain3DRdContext::RESOURCE_TEXTURE, category, nBytes ) ) {
		pRd->free_rid( texture );
		return;
	}
	{
		std::lock_guard<std::mutex> resultLock( m_resultMutex );
		m_hCreateResult = texture;
		m_bCreateSucceeded = true;
	}
	m_nCreateCount.fetch_add( 1u, std::memory_order_relaxed );
	m_nUploadedBytes.fetch_add( nBytes, std::memory_order_relaxed );
}

void Terrain3DRdTextureManager::renderUpdateTexture( const RID hTexture,
		const uint32_t nLayer,
		const PackedByteArray &rData ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	RenderingDevice *const pRd = context.getDevice();
	if ( !pRd || pRd->texture_update( hTexture, nLayer, rData ) != OK ) {
		context.recordFailure( Terrain3DRdContext::FAILURE_RESOURCE_CREATION, "Failed Terrain3D generated texture update" );
		return;
	}
	m_nUpdateCount.fetch_add( 1u, std::memory_order_relaxed );
	m_nUploadedBytes.fetch_add( ( uint64_t )( rData.size() ), std::memory_order_relaxed );
}

void Terrain3DRdTextureManager::renderUpdateTextureRegion( const RID hTexture,
		const RID hStaging,
		const uint32_t nDestinationX,
		const uint32_t nDestinationY,
		const uint32_t nWidth,
		const uint32_t nHeight,
		const PackedByteArray &rData ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	RenderingDevice *const pRd = context.getDevice();
	if ( !pRd || pRd->texture_update( hStaging, 0u, rData ) != OK ||
			pRd->texture_copy( hStaging,
					hTexture,
					Vector3( 0.0, 0.0, 0.0 ),
					Vector3( ( real_t )( nDestinationX ), ( real_t )( nDestinationY ), 0.0 ),
					Vector3( ( real_t )( nWidth ), ( real_t )( nHeight ), 1.0 ),
					0u,
					0u,
					0u,
					0u ) != OK ) {
		context.recordFailure( Terrain3DRdContext::FAILURE_RESOURCE_CREATION, "Failed Terrain3D generated texture subregion update" );
		return;
	}
	pRd->barrier( RenderingDevice::BARRIER_MASK_TRANSFER, RenderingDevice::BARRIER_MASK_RASTER | RenderingDevice::BARRIER_MASK_COMPUTE );
	m_nRegionUpdateCount.fetch_add( 1u, std::memory_order_relaxed );
	m_nUploadedBytes.fetch_add( ( uint64_t )( rData.size() ), std::memory_order_relaxed );
}

void Terrain3DRdTextureManager::renderFreeTexture( RID hTexture, const uint64_t nBytes ) {
	Terrain3DRdContext::getShared().freeTextureResource( hTexture, nBytes );
}

void Terrain3DRdTextureManager::renderFreeResource( RID hRid, const uint64_t nBytes ) {
	Terrain3DRdContext::getShared().freeResource( hRid, nBytes );
}

Dictionary Terrain3DRdTextureManager::getStats( void ) const {
	Dictionary result;
	result["backend"] = "rendering_device";
	result["available"] = Terrain3DRdContext::getShared().isReady();
	result["create_count"] = ( int64_t )( m_nCreateCount.load( std::memory_order_relaxed ) );
	result["update_count"] = ( int64_t )( m_nUpdateCount.load( std::memory_order_relaxed ) );
	result["region_update_count"] = ( int64_t )( m_nRegionUpdateCount.load( std::memory_order_relaxed ) );
	result["uploaded_bytes"] = ( int64_t )( m_nUploadedBytes.load( std::memory_order_relaxed ) );
	return result;
}

void Terrain3DRdTextureManager::renderShutdown( void ) {
	// The render-thread callback is a queue drain point. Shared RD ownership is
	// released by module teardown after this manager can no longer enqueue work.
}
