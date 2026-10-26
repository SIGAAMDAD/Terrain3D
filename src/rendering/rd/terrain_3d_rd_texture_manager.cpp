// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_texture_manager.h"
#include "core/terrain_3d_sync.h"

#include "core/logger.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/vector3.hpp>

Terrain3DRdTextureManager *Terrain3DRdTextureManager::s_pSingleton = NULL;

void Terrain3DRdTextureManager::initializeSingleton( void ) {
	if ( s_pSingleton ) {
		return;
	}
	s_pSingleton = memnew( Terrain3DRdTextureManager );
}

void Terrain3DRdTextureManager::shutdownSingleton( void ) {
	Terrain3DRdTextureManager *const pManager = s_pSingleton;
	if ( !pManager ) {
		return;
	}
	if ( !Terrain3DRdContext::isOnRenderThread() ) {
		if ( Terrain3DRdContext::enqueueOnRenderThread( callable_mp( pManager, &Terrain3DRdTextureManager::renderShutdown ) ) ) {
			Terrain3DRdContext::forceSync();
		} else {
			pManager->renderShutdown();
		}
	} else {
		pManager->renderShutdown();
	}
	s_pSingleton = NULL;
	memdelete( pManager );
}

bool Terrain3DRdTextureManager::imageFormatToDataFormat(
        const Image::Format format, const t3dRdTextureSemantic_t semantic, RenderingDevice::DataFormat &rFormat ) {
	if ( semantic == T3D_RD_TEXTURE_SOURCE_COLOR ) {
		switch ( format ) {
			case Image::FORMAT_RGB8:
				rFormat = RenderingDevice::DATA_FORMAT_R8G8B8_SRGB;
				return true;
			case Image::FORMAT_RGBA8:
				rFormat = RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB;
				return true;
			case Image::FORMAT_DXT1:
				rFormat = RenderingDevice::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK;
				return true;
			case Image::FORMAT_DXT3:
				rFormat = RenderingDevice::DATA_FORMAT_BC2_SRGB_BLOCK;
				return true;
			case Image::FORMAT_DXT5:
				rFormat = RenderingDevice::DATA_FORMAT_BC3_SRGB_BLOCK;
				return true;
			case Image::FORMAT_BPTC_RGBA:
				rFormat = RenderingDevice::DATA_FORMAT_BC7_SRGB_BLOCK;
				return true;
			case Image::FORMAT_ETC:
			case Image::FORMAT_ETC2_RGB8:
				rFormat = RenderingDevice::DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
				return true;
			case Image::FORMAT_ETC2_RGBA8:
				rFormat = RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
				return true;
			case Image::FORMAT_ETC2_RGB8A1:
				rFormat = RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
				return true;
			case Image::FORMAT_ASTC_4x4:
				rFormat = RenderingDevice::DATA_FORMAT_ASTC_4x4_SRGB_BLOCK;
				return true;
			case Image::FORMAT_ASTC_8x8:
				rFormat = RenderingDevice::DATA_FORMAT_ASTC_8x8_SRGB_BLOCK;
				return true;
			default:
				break;
		}
	}
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
			rFormat = RenderingDevice::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK;
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

static uint32_t t3dRdMaximumMipCount( const uint32_t nWidth, const uint32_t nHeight ) {
	if ( nWidth == 0u || nHeight == 0u ) {
		return 0u;
	}
	uint32_t dimension = MAX( nWidth, nHeight );
	uint32_t count = 1u;
	while ( dimension > 1u ) {
		dimension >>= 1u;
		++count;
	}
	return count;
}

uint32_t Terrain3DRdTextureManager::getImageMipmapCount( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		return 0u;
	}
	if ( !rImage->has_mipmaps() ) {
		return 1u;
	}
	const int32_t encoded_mipmaps = rImage->get_mipmap_count();
	if ( encoded_mipmaps < 0 ) {
		return 0u;
	}
	const uint64_t total_mipmaps = ( uint64_t )( encoded_mipmaps ) + 1u;
	const uint32_t maximum_mipmaps = t3dRdMaximumMipCount( ( uint32_t )( rImage->get_width() ), ( uint32_t )( rImage->get_height() ) );
	if ( total_mipmaps == 0u || total_mipmaps > maximum_mipmaps ) {
		return 0u;
	}
	return ( uint32_t )( total_mipmaps );
}

bool Terrain3DRdTextureManager::buildLayout( const Ref<Image> &rImage,
        const uint32_t nLayers,
        const bool bLayered,
        const uint64_t nPayloadBytes,
        const t3dRdTextureSemantic_t semantic,
        t3dRdTextureLayout_t &rLayout ) {
	rLayout = t3dRdTextureLayout_t{};
	if ( rImage.is_null() || rImage->is_empty() || nLayers == 0u || nPayloadBytes == 0u
	        || !imageFormatToDataFormat( rImage->get_format(), semantic, rLayout.m_format ) ) {
		return false;
	}
	rLayout.m_nWidth = ( uint32_t )( rImage->get_width() );
	rLayout.m_nHeight = ( uint32_t )( rImage->get_height() );
	rLayout.m_nLayers = nLayers;
	rLayout.m_nMipmaps = getImageMipmapCount( rImage );
	if ( rLayout.m_nMipmaps == 0u ) {
		return false;
	}
	rLayout.m_type = bLayered ? RenderingDevice::TEXTURE_TYPE_2D_ARRAY : RenderingDevice::TEXTURE_TYPE_2D;
	rLayout.m_nUsageBits = ( uint64_t )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT )
	        | ( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT )
	        | ( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT )
	        | ( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );
	rLayout.m_nBytes = nPayloadBytes;
	return rLayout.valid();
}

RID Terrain3DRdTextureManager::createTexture( const t3dRdTextureLayout_t &rLayout,
        const TypedArray<PackedByteArray> &rInitialData,
        const String &rName,
        const int nMemoryCategory,
        const bool bFailureFatal ) {
	if ( !rLayout.valid() || rInitialData.size() != ( int64_t )( rLayout.m_nLayers ) ) {
		return RID();
	}
	if ( !rInitialData.is_empty() && ( rLayout.m_nUsageBits & ( uint64_t )( RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT ) ) == 0u ) {
		LOG( ERROR, "Rejected RD texture initial upload without TEXTURE_USAGE_CAN_UPDATE_BIT: ", rName, " usage=", rLayout.m_nUsageBits );
		return RID();
	}
	const uint32_t maximum_mipmaps = t3dRdMaximumMipCount( rLayout.m_nWidth, rLayout.m_nHeight );
	if ( rLayout.m_nMipmaps > maximum_mipmaps ) {
		LOG( ERROR,
		        "Rejected invalid RD texture mip chain before RenderingDevice creation: ",
		        rName,
		        " requested=",
		        rLayout.m_nMipmaps,
		        " maximum=",
		        maximum_mipmaps,
		        " size=",
		        rLayout.m_nWidth,
		        "x",
		        rLayout.m_nHeight );
		return RID();
	}
	if ( !Terrain3DRdContext::getGlobalDevice() ) {
		return RID();
	}
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

	t3dScopedLock_t createLock( m_createMutex );
	{
		t3dScopedLock_t resultLock( m_resultMutex );
		m_hCreateResult = RID();
		m_bCreateSucceeded = false;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		renderCreateTexture( pFormat, rInitialData, rName, rLayout.m_nBytes, nMemoryCategory, bFailureFatal );
	} else {
		if ( !Terrain3DRdContext::enqueueOnRenderThread( callable_mp( this, &Terrain3DRdTextureManager::renderCreateTexture )
		                     .bind( pFormat, rInitialData, rName, rLayout.m_nBytes, nMemoryCategory, bFailureFatal ) ) ) {
			return RID();
		}
		// Creation/recreation is a cold path and requires the RID before publication.
		Terrain3DRdContext::forceSync();
	}
	t3dScopedLock_t resultLock( m_resultMutex );
	return m_bCreateSucceeded ? m_hCreateResult : RID();
}

bool Terrain3DRdTextureManager::updateTexture( const RID hTexture, const uint32_t nLayer, const PackedByteArray &rData ) {
	if ( !hTexture.is_valid() || rData.is_empty() ) {
		return false;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		return renderUpdateTexture( hTexture, nLayer, rData );
	}
	return Terrain3DRdContext::enqueueOnRenderThread(
	        callable_mp( this, &Terrain3DRdTextureManager::renderUpdateTexture ).bind( hTexture, nLayer, rData ) );
}

uint64_t Terrain3DRdTextureManager::nextTrackedTextureBatchGeneration( void ) {
	uint64_t generation = m_nTrackedBatchGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	if ( generation == 0u ) {
		generation = m_nTrackedBatchGeneration.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	}
	return generation;
}

bool Terrain3DRdTextureManager::submitTrackedTextureLayerBatch( const uint32_t nSlot,
        const uint64_t nGeneration,
        const uint32_t nLayer,
        const t3dRdTextureBatchUpdate_t *const pUpdates,
        const uint32_t nUpdateCount ) {
	if ( nSlot >= s_nTrackedBatchSlotCount || nGeneration == 0u || !pUpdates || nUpdateCount == 0u
	        || nUpdateCount > s_nTrackedBatchUpdateCount || Terrain3DRdContext::isOnRenderThread() ) {
		return false;
	}
	for ( uint32_t i = 0u; i < nUpdateCount; ++i ) {
		if ( !pUpdates[i].m_hTexture.is_valid() || pUpdates[i].m_data.is_empty() ) {
			return false;
		}
	}
	t3dTrackedTextureBatchSlot_t &slot = m_szTrackedBatches[nSlot];
	uint8_t state = slot.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE );
	for ( ;; ) {
		if ( state == T3D_RD_TEXTURE_BATCH_PENDING || state == T3D_RD_TEXTURE_BATCH_WRITING ) {
			return false;
		}
		if ( slot.m_nState.compare_exchange_weak(
		             state, T3D_RD_TEXTURE_BATCH_WRITING, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
			break;
		}
	}
	clearTrackedTextureBatchPayload( slot );
	for ( uint32_t i = 0u; i < nUpdateCount; ++i ) {
		slot.m_szTextures[i] = pUpdates[i].m_hTexture;
		slot.m_szData[i] = pUpdates[i].m_data;
	}
	slot.m_nLayer = nLayer;
	slot.m_nCount = nUpdateCount;
	slot.m_nGeneration.store( nGeneration, T3D_MEMORY_ORDER_RELAXED );
	slot.m_nState.store( T3D_RD_TEXTURE_BATCH_PENDING, T3D_MEMORY_ORDER_RELEASE );
	if ( Terrain3DRdContext::enqueueOnRenderThread(
	             callable_mp( this, &Terrain3DRdTextureManager::renderTrackedTextureLayerBatch ).bind( nSlot, nGeneration ) ) ) {
		return true;
	}
	clearTrackedTextureBatchPayload( slot );
	slot.m_nState.store( T3D_RD_TEXTURE_BATCH_FAILED, T3D_MEMORY_ORDER_RELEASE );
	return false;
}

t3dRdTextureBatchState_t Terrain3DRdTextureManager::getTrackedTextureLayerBatchState(
        const uint32_t nSlot, const uint64_t nGeneration ) const {
	if ( nSlot >= s_nTrackedBatchSlotCount || nGeneration == 0u ) {
		return T3D_RD_TEXTURE_BATCH_UNUSED;
	}
	const t3dTrackedTextureBatchSlot_t &slot = m_szTrackedBatches[nSlot];
	const uint8_t state = slot.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( slot.m_nGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) != nGeneration ) {
		return T3D_RD_TEXTURE_BATCH_UNUSED;
	}
	return ( t3dRdTextureBatchState_t )( state );
}

bool Terrain3DRdTextureManager::updateTextureRegion( const RID hTexture,
        const RID hStaging,
        const uint32_t nDestinationX,
        const uint32_t nDestinationY,
        const uint32_t nWidth,
        const uint32_t nHeight,
        const PackedByteArray &rData ) {
	if ( !hTexture.is_valid() || !hStaging.is_valid() || nWidth == 0u || nHeight == 0u || rData.is_empty() ) {
		return false;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		return renderUpdateTextureRegion( hTexture, hStaging, nDestinationX, nDestinationY, nWidth, nHeight, rData );
	}
	return Terrain3DRdContext::enqueueOnRenderThread( callable_mp( this, &Terrain3DRdTextureManager::renderUpdateTextureRegion )
	                .bind( hTexture, hStaging, nDestinationX, nDestinationY, nWidth, nHeight, rData ) );
}

void Terrain3DRdTextureManager::freeTexture( const RID hTexture, const uint64_t nBytes ) {
	if ( !hTexture.is_valid() ) {
		return;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		renderFreeTexture( hTexture, nBytes );
	} else {
		Terrain3DRdContext::enqueueOnRenderThread(
		        callable_mp( this, &Terrain3DRdTextureManager::renderFreeTexture ).bind( hTexture, nBytes ) );
	}
}

void Terrain3DRdTextureManager::freeResource( const RID hRid, const uint64_t nBytes ) {
	if ( !hRid.is_valid() ) {
		return;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		renderFreeResource( hRid, nBytes );
	} else {
		Terrain3DRdContext::enqueueOnRenderThread(
		        callable_mp( this, &Terrain3DRdTextureManager::renderFreeResource ).bind( hRid, nBytes ) );
	}
}

void Terrain3DRdTextureManager::renderCreateTexture( const Ref<RDTextureFormat> &rFormat,
        const TypedArray<PackedByteArray> &rInitialData,
        const String &rName,
        const uint64_t nBytes,
        const int nMemoryCategory,
        const bool bFailureFatal ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	if ( !context.isReady() && !context.initialize() ) {
		return;
	}
	RenderingDevice *const pRd = context.getDevice();
	if ( !pRd ) {
		if ( bFailureFatal ) {
			context.recordResourceCreationFailure(
			        rName, Terrain3DRdContext::RESOURCE_TEXTURE, "RenderingDevice unavailable for texture creation" );
		}
		return;
	}
	if ( !pRd->texture_is_format_supported_for_usage( rFormat->get_format(), rFormat->get_usage_bits() ) ) {
		if ( bFailureFatal ) {
			context.recordResourceCreationFailure(
			        rName, Terrain3DRdContext::RESOURCE_TEXTURE, "Unsupported generated texture format/usage" );
		}
		return;
	}
	Ref<RDTextureView> pView;
	pView.instantiate();
	RID texture = pRd->texture_create( rFormat, pView, rInitialData );
	if ( !texture.is_valid() ) {
		if ( bFailureFatal ) {
			context.recordResourceCreationFailure( rName, Terrain3DRdContext::RESOURCE_TEXTURE );
		}
		return;
	}
	const Terrain3DRdContext::MemoryCategory category = nMemoryCategory >= 0 && nMemoryCategory < Terrain3DRdContext::MEMORY_CATEGORY_COUNT
	        ? ( Terrain3DRdContext::MemoryCategory )( nMemoryCategory )
	        : Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE;
	if ( !Terrain3DRdResourceService( context, Terrain3DRdContext::OWNER_UNSPECIFIED )
	                    .adopt( pRd, texture, rName, Terrain3DRdContext::RESOURCE_TEXTURE, category, nBytes ) ) {
		return;
	}
	{
		t3dScopedLock_t resultLock( m_resultMutex );
		m_hCreateResult = texture;
		m_bCreateSucceeded = true;
	}
	m_nCreateCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nUploadedBytes.fetch_add( nBytes, T3D_MEMORY_ORDER_RELAXED );
}

bool Terrain3DRdTextureManager::renderUpdateTexture( const RID hTexture, const uint32_t nLayer, const PackedByteArray &rData ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	RenderingDevice *const pRd = context.getDevice();
	if ( !pRd || pRd->texture_update( hTexture, nLayer, rData ) != OK ) {
		context.recordFailure( Terrain3DRdContext::FAILURE_RESOURCE_CREATION, "Failed Terrain3D generated texture update" );
		return false;
	}
	m_nUpdateCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nUploadedBytes.fetch_add( ( uint64_t )( rData.size() ), T3D_MEMORY_ORDER_RELAXED );
	return true;
}

void Terrain3DRdTextureManager::clearTrackedTextureBatchPayload( t3dTrackedTextureBatchSlot_t &rSlot ) {
	for ( uint32_t i = 0u; i < s_nTrackedBatchUpdateCount; ++i ) {
		rSlot.m_szTextures[i] = RID();
		rSlot.m_szData[i].clear();
	}
	rSlot.m_nLayer = 0u;
	rSlot.m_nCount = 0u;
}

void Terrain3DRdTextureManager::renderTrackedTextureLayerBatch( const uint32_t nSlot, const uint64_t nGeneration ) {
	if ( nSlot >= s_nTrackedBatchSlotCount || nGeneration == 0u ) {
		return;
	}
	t3dTrackedTextureBatchSlot_t &slot = m_szTrackedBatches[nSlot];
	if ( slot.m_nState.load( T3D_MEMORY_ORDER_ACQUIRE ) != T3D_RD_TEXTURE_BATCH_PENDING
	        || slot.m_nGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) != nGeneration ) {
		return;
	}
	bool bSucceeded = true;
	for ( uint32_t i = 0u; i < slot.m_nCount; ++i ) {
		if ( !renderUpdateTexture( slot.m_szTextures[i], slot.m_nLayer, slot.m_szData[i] ) ) {
			bSucceeded = false;
			break;
		}
	}
	clearTrackedTextureBatchPayload( slot );
	slot.m_nState.store( bSucceeded ? T3D_RD_TEXTURE_BATCH_SUCCEEDED : T3D_RD_TEXTURE_BATCH_FAILED, T3D_MEMORY_ORDER_RELEASE );
}

bool Terrain3DRdTextureManager::renderUpdateTextureRegion( const RID hTexture,
        const RID hStaging,
        const uint32_t nDestinationX,
        const uint32_t nDestinationY,
        const uint32_t nWidth,
        const uint32_t nHeight,
        const PackedByteArray &rData ) {
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	RenderingDevice *const pRd = context.getDevice();
	if ( !pRd || pRd->texture_update( hStaging, 0u, rData ) != OK
	        || pRd->texture_copy( hStaging,
	                   hTexture,
	                   Vector3( 0.0, 0.0, 0.0 ),
	                   Vector3( ( real_t )( nDestinationX ), ( real_t )( nDestinationY ), 0.0 ),
	                   Vector3( ( real_t )( nWidth ), ( real_t )( nHeight ), 1.0 ),
	                   0u,
	                   0u,
	                   0u,
	                   0u )
	                != OK ) {
		context.recordFailure( Terrain3DRdContext::FAILURE_RESOURCE_CREATION, "Failed Terrain3D generated texture subregion update" );
		return false;
	}
	// Godot 4.7.2 automatically synchronizes the staging upload/copy before later raster or compute consumers.
	m_nRegionUpdateCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
	m_nUploadedBytes.fetch_add( ( uint64_t )( rData.size() ), T3D_MEMORY_ORDER_RELAXED );
	return true;
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
	result["create_count"] = ( int64_t )( m_nCreateCount.load( T3D_MEMORY_ORDER_RELAXED ) );
	result["update_count"] = ( int64_t )( m_nUpdateCount.load( T3D_MEMORY_ORDER_RELAXED ) );
	result["region_update_count"] = ( int64_t )( m_nRegionUpdateCount.load( T3D_MEMORY_ORDER_RELAXED ) );
	result["uploaded_bytes"] = ( int64_t )( m_nUploadedBytes.load( T3D_MEMORY_ORDER_RELAXED ) );
	return result;
}

void Terrain3DRdTextureManager::renderShutdown( void ) {
	// The render-thread callback is a queue drain point. Shared RD ownership is
	// released by module teardown after this manager can no longer enqueue work.
}
