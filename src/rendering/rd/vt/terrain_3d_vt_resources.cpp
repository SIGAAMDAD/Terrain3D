// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/vt/terrain_3d_vt_resources.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <limits.h>
#include <string.h>

using namespace godot;

static uint32_t t3d_rw_channel_page_count( const T3DRenderWorldLimits &pLimits, T3DVtChannel pChannel ) {
	switch ( pChannel ) {
		case T3D_RW_VT_ALBEDO:
			return pLimits.nMaxVtPhysicalPageSets;
		case T3D_RW_VT_NORMAL:
			return pLimits.nMaxVtPhysicalPageSets;
		case T3D_RW_VT_SURFACE:
			return pLimits.nMaxVtPhysicalPageSets;
		case T3D_RW_VT_HEIGHT:
			return pLimits.nMaxVtPhysicalPageSets;
		default:
			return 0u;
	}
}

static RenderingDevice::DataFormat t3d_rw_channel_format( T3DVtChannel pChannel ) {
	switch ( pChannel ) {
		case T3D_RW_VT_ALBEDO:
			// VT V3 physical family 0 is shared by ALBEDO (sRGB semantic) and ORM (linear semantic).
			// Keep the cache itself linear and perform ALBEDO sRGB decode explicitly in the shader.
			return RenderingDevice::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK;
		case T3D_RW_VT_NORMAL:
			return RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK;
		case T3D_RW_VT_SURFACE:
			return RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK;
		case T3D_RW_VT_HEIGHT:
			return RenderingDevice::DATA_FORMAT_R8_UNORM;
		default:
			return RenderingDevice::DATA_FORMAT_MAX;
	}
}

static const char *t3d_rw_buffer_name( const T3DVtGpuBufferId pBuffer ) {
	switch ( pBuffer ) {
		case T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE: return "VT allocations";
		case T3D_RW_GPU_BUFFER_VT_PAGE_TABLE: return "VT page table";
		case T3D_RW_GPU_BUFFER_VT_MIP_LAYOUT_TABLE: return "VT mip layout";
		case T3D_RW_GPU_BUFFER_VT_TAIL_METADATA: return "VT tail metadata";
		default: return "VT buffer";
	}
}

Terrain3DVtResourceImage::Terrain3DVtResourceImage() {
	m_pContext = NULL;
	m_pRenderingDevice = NULL;
	memset( m_nVtBankCount, 0, sizeof( m_nVtBankCount ) );
	memset( m_nVtBankLayers, 0, sizeof( m_nVtBankLayers ) );
	memset( m_nVtBankBytes, 0, sizeof( m_nVtBankBytes ) );
	memset( &m_Plan, 0, sizeof( m_Plan ) );
	memset( &m_Limits, 0, sizeof( m_Limits ) );
	memset( &m_InitializeProfile, 0, sizeof( m_InitializeProfile ) );
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	m_nInitializeBufferCursor = 0u;
	m_nInitializeChannelCursor = 0u;
	m_nInitializeStage = 0u;
	m_bInitializePending = false;
	m_nHotSealCreateEpoch = 0u;
	m_nHotSealFreeEpoch = 0u;
	m_bInitialized = false;
	m_bHotSealed = false;
}

Terrain3DVtResourceImage::~Terrain3DVtResourceImage() {
	// Ownership is intentionally explicit. The composition root must call shutdown_cold().
}

bool Terrain3DVtResourceImage::initialize_cold(
        Terrain3DRdContext &rContext, const T3DRenderWorldLimits &pLimits, const T3DVtGpuProfile &pProfile ) {
	return initialize_internal_cold( rContext, pLimits, pProfile );
}

bool Terrain3DVtResourceImage::initialize_internal_cold(
        Terrain3DRdContext &rContext, const T3DRenderWorldLimits &pLimits, const T3DVtGpuProfile &pProfile ) {
	if ( !begin_initialize_cold( rContext, pLimits, pProfile ) ) {
		return false;
	}
	while ( is_initialize_pending_cold() ) {
		if ( !service_initialize_cold( UINT32_MAX ) ) {
			return false;
		}
	}
	return m_bInitialized;
}

bool Terrain3DVtResourceImage::begin_initialize_cold(
        Terrain3DRdContext &rContext, const T3DRenderWorldLimits &pLimits, const T3DVtGpuProfile &pProfile ) {
	RenderingDevice *const pRenderingDevice = rContext.getDevice();
	if ( m_bInitialized || m_bInitializePending || pRenderingDevice == NULL ) {
		return false;
	}
	if ( !t3d_vt_compute_resource_plan( pLimits, pProfile, m_Plan ) ) {
		return false;
	}
	m_pContext = &rContext;
	m_pRenderingDevice = pRenderingDevice;
	m_Limits = pLimits;
	m_InitializeProfile = pProfile;
	if ( !validate_device_cold( pLimits, pProfile ) ) {
		m_pRenderingDevice = NULL;
		m_pContext = NULL;
		memset( &m_Plan, 0, sizeof( m_Plan ) );
		memset( &m_Limits, 0, sizeof( m_Limits ) );
		memset( &m_InitializeProfile, 0, sizeof( m_InitializeProfile ) );
		return false;
	}
	m_nInitializeBufferCursor = 0u;
	m_nInitializeChannelCursor = 0u;
	m_nInitializeStage = 0u;
	m_bInitializePending = true;
	return true;
}

bool Terrain3DVtResourceImage::service_initialize_cold( const uint32_t nMaxWorkUnits ) {
	if ( !m_bInitializePending || m_bInitialized || m_pRenderingDevice == NULL || nMaxWorkUnits == 0u ) {
		return false;
	}
	uint32_t nUnits = 0u;
	while ( m_bInitializePending && nUnits < nMaxWorkUnits ) {
		bool bReady = true;
		if ( m_nInitializeStage == 0u ) {
			if ( m_nInitializeBufferCursor < T3D_RW_GPU_BUFFER_COUNT ) {
				const uint32_t iBuffer = m_nInitializeBufferCursor++;
				if ( m_Plan.nBufferBytes[iBuffer] != 0u ) {
					bReady = create_storage_buffer_cold( ( T3DVtGpuBufferId )iBuffer,
					        m_Plan.nBufferBytes[iBuffer] );
				}
				++nUnits;
			} else {
				m_nInitializeStage = 1u;
				continue;
			}
		} else if ( m_nInitializeStage == 1u ) {
			if ( m_nInitializeChannelCursor < T3D_RW_VT_CHANNEL_COUNT ) {
				const T3DVtChannel Channel = ( T3DVtChannel )m_nInitializeChannelCursor++;
				bReady = create_vt_channel_cold( Channel,
				        t3d_rw_channel_page_count( m_Limits, Channel ),
				        m_InitializeProfile.nVtPhysicalTileSize,
				        t3d_rw_channel_format( Channel ) )
				        && create_vt_tail_channel_cold( Channel, t3d_rw_channel_format( Channel ) );
				++nUnits;
			} else {
				m_nInitializeStage = 2u;
				continue;
			}
		} else if ( m_nInitializeStage == 2u ) {
			bReady = true;
			++nUnits;
			if ( bReady ) {
				m_nInitializeStage = 3u;
				m_bInitializePending = false;
				m_bInitialized = true;
				++m_Stats.nResourceGeneration;
			}
		}
		if ( !bReady ) {
			rollback_failed_initialize_cold();
			return false;
		}
	}
	return true;
}

bool Terrain3DVtResourceImage::is_initialize_pending_cold() const {
	return m_bInitializePending;
}

void Terrain3DVtResourceImage::shutdown_cold() {
	m_bHotSealed = false;

	if ( m_pRenderingDevice == NULL ) {
		m_bInitializePending = false;
		m_bInitialized = false;
		return;
	}


	for ( int32_t iChannel = ( int32_t )T3D_RW_VT_CHANNEL_COUNT - 1; iChannel >= 0; --iChannel ) {
		free_rid_cold( m_hVtTail[iChannel], m_Plan.nVtTailBytes[iChannel], true );
	}

	for ( int32_t iChannel = ( int32_t )T3D_RW_VT_CHANNEL_COUNT - 1; iChannel >= 0; --iChannel ) {
		for ( int32_t iBank = ( int32_t )m_nVtBankCount[iChannel] - 1; iBank >= 0; --iBank ) {
			free_rid_cold( m_hVtPhysical[iChannel][iBank], m_nVtBankBytes[iChannel][iBank], true );
		}
	}

	for ( int32_t iBuffer = ( int32_t )T3D_RW_GPU_BUFFER_COUNT - 1; iBuffer >= 0; --iBuffer ) {
		free_rid_cold( m_hBuffers[iBuffer], m_Plan.nBufferBytes[iBuffer], false );
	}

	memset( m_nVtBankCount, 0, sizeof( m_nVtBankCount ) );
	memset( m_nVtBankLayers, 0, sizeof( m_nVtBankLayers ) );
	memset( m_nVtBankBytes, 0, sizeof( m_nVtBankBytes ) );
	memset( &m_Plan, 0, sizeof( m_Plan ) );
	memset( &m_Limits, 0, sizeof( m_Limits ) );
	memset( &m_InitializeProfile, 0, sizeof( m_InitializeProfile ) );
	m_nInitializeBufferCursor = 0u;
	m_nInitializeChannelCursor = 0u;
	m_nInitializeStage = 0u;
	m_bInitializePending = false;
	m_pRenderingDevice = NULL;
	m_pContext = NULL;
	m_bInitialized = false;
	++m_Stats.nResourceGeneration;
}

void Terrain3DVtResourceImage::seal_hot_runtime() {
	if ( !m_bInitialized ) {
		return;
	}
	m_nHotSealCreateEpoch = m_Stats.nGpuCreateCount;
	m_nHotSealFreeEpoch = m_Stats.nGpuFreeCount;
	m_bHotSealed = true;
}

void Terrain3DVtResourceImage::unseal_hot_runtime_cold() {
	m_bHotSealed = false;
}

bool Terrain3DVtResourceImage::service_idle_frame_hot() {
	if ( !m_bInitialized || !m_bHotSealed ) {
		return false;
	}
	if ( hot_epoch_unchanged() ) {
		return true;
	}

	++m_Stats.nHotSealViolationCount;
	return false;
}

bool Terrain3DVtResourceImage::validate_idle_plateau_reference_debug( uint32_t pFrameCount ) {
	if ( !m_bInitialized || !m_bHotSealed || pFrameCount == 0u ) {
		return false;
	}
	for ( uint32_t iFrame = 0u; iFrame < pFrameCount; ++iFrame ) {
		if ( !service_idle_frame_hot() ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DVtResourceImage::is_initialized() const {
	return m_bInitialized;
}

bool Terrain3DVtResourceImage::is_hot_sealed() const {
	return m_bHotSealed;
}

const T3DVtGpuResourcePlan &Terrain3DVtResourceImage::get_plan() const {
	return m_Plan;
}

const T3DRenderWorldLimits &Terrain3DVtResourceImage::get_limits() const {
	return m_Limits;
}

const T3DVtGpuResourceStats &Terrain3DVtResourceImage::get_stats() const {
	return m_Stats;
}

const RID &Terrain3DVtResourceImage::get_buffer_rid( T3DVtGpuBufferId pBuffer ) const {
	if ( ( uint32_t )pBuffer >= T3D_RW_GPU_BUFFER_COUNT ) {
		return m_hInvalid;
	}
	return m_hBuffers[pBuffer];
}



uint32_t Terrain3DVtResourceImage::get_vt_bank_count( T3DVtChannel pChannel ) const {
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return 0u;
	}
	return m_nVtBankCount[pChannel];
}

uint32_t Terrain3DVtResourceImage::get_vt_bank_layer_count( T3DVtChannel pChannel, uint32_t pBank ) const {
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return 0u;
	}
	if ( pBank >= m_nVtBankCount[pChannel] ) {
		return 0u;
	}
	return m_nVtBankLayers[pChannel][pBank];
}

const RID &Terrain3DVtResourceImage::get_vt_bank_rid( T3DVtChannel pChannel, uint32_t pBank ) const {
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return m_hInvalid;
	}
	if ( pBank >= m_nVtBankCount[pChannel] ) {
		return m_hInvalid;
	}
	return m_hVtPhysical[pChannel][pBank];
}

const RID &Terrain3DVtResourceImage::get_vt_tail_rid( T3DVtChannel pChannel ) const {
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return m_hInvalid;
	}
	return m_hVtTail[pChannel];
}

Terrain3DRdContext *Terrain3DVtResourceImage::get_rd_context() const {
	return m_pContext;
}

bool Terrain3DVtResourceImage::validate_device_cold(
        const T3DRenderWorldLimits &pLimits, const T3DVtGpuProfile &pProfile ) const {
	if ( m_pRenderingDevice == NULL ) {
		return false;
	}

	for ( uint32_t iBuffer = 0u; iBuffer < T3D_RW_GPU_BUFFER_COUNT; ++iBuffer ) {
		// godot-cpp 10.0.0-rc1 / Godot 4.6 exposes uint32_t buffer sizes.
		if ( m_Plan.nBufferBytes[iBuffer] > UINT32_MAX ) {
			return false;
		}
	}

	const uint64_t nMaxTextureSize = m_pRenderingDevice->limit_get( RenderingDevice::LIMIT_MAX_TEXTURE_SIZE_2D );
	const uint64_t nMaxArrayLayers = m_pRenderingDevice->limit_get( RenderingDevice::LIMIT_MAX_TEXTURE_ARRAY_LAYERS );
	if ( nMaxTextureSize == 0u || nMaxArrayLayers == 0u ) {
		return false;
	}
	if ( pProfile.nVtPhysicalTileSize > nMaxTextureSize || m_Plan.nVtTailAtlasWidth > nMaxTextureSize
	        || m_Plan.nVtTailAtlasHeight > nMaxTextureSize ) {
		return false;
	}

	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		const uint32_t nPages = t3d_rw_channel_page_count( pLimits, ( T3DVtChannel )iChannel );
		T3DVtBankLayout Layout = {};
		if ( !t3d_vt_compute_bank_layout( nPages, ( uint32_t )nMaxArrayLayers, Layout ) ) {
			return false;
		}
	}

	const BitField<RenderingDevice::TextureUsageBits> VtUsage =
	        ( BitField<RenderingDevice::TextureUsageBits> )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );

	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		if ( !m_pRenderingDevice->texture_is_format_supported_for_usage(
		             t3d_rw_channel_format( ( T3DVtChannel )iChannel ), VtUsage ) ) {
			return false;
		}
	}

	return true;
}

bool Terrain3DVtResourceImage::create_storage_buffer_cold( T3DVtGpuBufferId pBuffer, uint64_t pBytes ) {
	if ( m_pRenderingDevice == NULL || m_pContext == NULL ) {
		return false;
	}
	if ( ( uint32_t )pBuffer >= T3D_RW_GPU_BUFFER_COUNT ) {
		return false;
	}
	if ( pBytes == 0u || pBytes > UINT32_MAX ) {
		return false;
	}

	const BitField<RenderingDevice::StorageBufferUsage> Usage =
	        ( BitField<RenderingDevice::StorageBufferUsage> )0;

	if ( !Terrain3DRdResourceService( *m_pContext, Terrain3DRdContext::OWNER_VT )
	             .createBuffer( m_hBuffers[pBuffer], String::utf8( t3d_rw_buffer_name( pBuffer ) ),
	                     Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                     Terrain3DRdContext::MEMORY_STORAGE, ( uint32_t )pBytes, PackedByteArray(), Usage ) ) {
		return false;
	}

	++m_Stats.nGpuCreateCount;
	++m_Stats.nLiveResourceCount;
	m_Stats.nLiveBufferBytes += pBytes;
	return true;
}

bool Terrain3DVtResourceImage::create_vt_channel_cold(
        T3DVtChannel pChannel, uint32_t pPageCount, uint32_t pTileSize, RenderingDevice::DataFormat pFormat ) {
	if ( m_pRenderingDevice == NULL ) {
		return false;
	}
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT || pPageCount == 0u ) {
		return false;
	}

	const uint32_t nMaxArrayLayers = ( uint32_t )m_pRenderingDevice->limit_get( RenderingDevice::LIMIT_MAX_TEXTURE_ARRAY_LAYERS );
	T3DVtBankLayout Layout = {};
	if ( !t3d_vt_compute_bank_layout( pPageCount, nMaxArrayLayers, Layout ) ) {
		return false;
	}

	const BitField<RenderingDevice::TextureUsageBits> Usage =
	        ( BitField<RenderingDevice::TextureUsageBits> )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );

	uint32_t nRemainingPages = pPageCount;
	uint64_t nRemainingBytes = m_Plan.nVtPhysicalBytes[pChannel];
	const uint64_t nBytesPerLayer = m_Plan.nVtPhysicalBytes[pChannel] / pPageCount;

	for ( uint32_t nBank = 0u; nBank < Layout.nBankCount; ++nBank ) {
		const uint32_t nLayers = Layout.nLayers[nBank];

		if ( !create_texture_cold( m_hVtPhysical[pChannel][nBank],
		             pTileSize,
		             pTileSize,
		             nLayers,
		             1u,
		             RenderingDevice::TEXTURE_TYPE_2D_ARRAY,
		             pFormat,
		             Usage ) ) {
			return false;
		}

		const uint64_t nBankBytes = ( nRemainingPages == nLayers ) ? nRemainingBytes : nBytesPerLayer * nLayers;

		if ( m_pContext != NULL
		        && !Terrain3DRdResourceService( *m_pContext, Terrain3DRdContext::OWNER_VT )
		                    .adopt( m_pRenderingDevice, m_hVtPhysical[pChannel][nBank],
		                String( "Terrain3D VT channel " ) + String::num_uint64( ( uint64_t )pChannel ) + " bank "
		                        + String::num_uint64( ( uint64_t )nBank ),
		                Terrain3DRdContext::RESOURCE_TEXTURE,
		                Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
		                nBankBytes ) ) {
			--m_Stats.nGpuCreateCount;
			--m_Stats.nLiveResourceCount;
			return false;
		}

		m_nVtBankLayers[pChannel][nBank] = nLayers;
		m_nVtBankBytes[pChannel][nBank] = nBankBytes;
		m_Stats.nLiveTextureBytes += nBankBytes;
		nRemainingBytes -= nBankBytes;
		nRemainingPages -= nLayers;
		m_nVtBankCount[pChannel] = nBank + 1u;
	}
	return nRemainingPages == 0u && nRemainingBytes == 0u;
}

bool Terrain3DVtResourceImage::create_vt_tail_channel_cold(
        T3DVtChannel pChannel, RenderingDevice::DataFormat pFormat ) {
	if ( m_pRenderingDevice == NULL || ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT || m_Plan.nVtTailBytes[pChannel] == 0u ) {
		return false;
	}

	const BitField<RenderingDevice::TextureUsageBits> Usage =
	        ( BitField<RenderingDevice::TextureUsageBits> )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );
	if ( !create_texture_cold( m_hVtTail[pChannel],
	             m_Plan.nVtTailAtlasWidth,
	             m_Plan.nVtTailAtlasHeight,
	             1u,
	             1u,
	             RenderingDevice::TEXTURE_TYPE_2D,
	             pFormat,
	             Usage ) ) {
		return false;
	}

	if ( m_pContext != NULL
	        && !Terrain3DRdResourceService( *m_pContext, Terrain3DRdContext::OWNER_VT )
	                    .adopt( m_pRenderingDevice, m_hVtTail[pChannel],
	                String( "Terrain3D VT tail channel " ) + String::num_uint64( ( uint64_t )pChannel ),
	                Terrain3DRdContext::RESOURCE_TEXTURE,
	                Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
	                m_Plan.nVtTailBytes[pChannel] ) ) {
		--m_Stats.nGpuCreateCount;
		--m_Stats.nLiveResourceCount;
		return false;
	}
	m_Stats.nLiveTextureBytes += m_Plan.nVtTailBytes[pChannel];
	return true;
}

bool Terrain3DVtResourceImage::create_texture_cold( RID &rRid,
        uint32_t pWidth,
        uint32_t pHeight,
        uint32_t pLayers,
        uint32_t pMipCount,
        RenderingDevice::TextureType pType,
        RenderingDevice::DataFormat pFormat,
        BitField<RenderingDevice::TextureUsageBits> pUsage ) {
	if ( m_pRenderingDevice == NULL || m_pContext == NULL ) {
		return false;
	}
	if ( pWidth == 0u || pHeight == 0u || pLayers == 0u || pMipCount == 0u ) {
		return false;
	}

	Ref<RDTextureFormat> Format;
	Format.instantiate();
	if ( Format.is_null() ) {
		return false;
	}
	Format->set_format( pFormat );
	Format->set_width( pWidth );
	Format->set_height( pHeight );
	Format->set_depth( 1u );
	Format->set_array_layers( pLayers );
	Format->set_mipmaps( pMipCount );
	Format->set_texture_type( pType );
	Format->set_samples( RenderingDevice::TEXTURE_SAMPLES_1 );
	Format->set_usage_bits( pUsage );

	Ref<RDTextureView> View;
	View.instantiate();
	if ( View.is_null() ) {
		return false;
	}
	TypedArray<PackedByteArray> EmptyData;
	rRid = m_pRenderingDevice->texture_create( Format, View, EmptyData );
	if ( !rRid.is_valid() ) {
		return false;
	}

	++m_Stats.nGpuCreateCount;
	++m_Stats.nLiveResourceCount;
	return true;
}

void Terrain3DVtResourceImage::free_rid_cold( RID &rRid, uint64_t pBytes, bool pTexture ) {
	if ( m_pRenderingDevice == NULL || !rRid.is_valid() ) {
		return;
	}
	m_pContext->freeResource( rRid, pBytes );
	++m_Stats.nGpuFreeCount;
	if ( m_Stats.nLiveResourceCount != 0u ) {
		--m_Stats.nLiveResourceCount;
	}

	if ( pTexture ) {
		if ( m_Stats.nLiveTextureBytes >= pBytes ) {
			m_Stats.nLiveTextureBytes -= pBytes;
		}
	} else {
		if ( m_Stats.nLiveBufferBytes >= pBytes ) {
			m_Stats.nLiveBufferBytes -= pBytes;
		}
	}
}

void Terrain3DVtResourceImage::rollback_failed_initialize_cold() {
	shutdown_cold();
	m_bInitialized = false;
}

bool Terrain3DVtResourceImage::hot_epoch_unchanged() const {
	return m_Stats.nGpuCreateCount == m_nHotSealCreateEpoch && m_Stats.nGpuFreeCount == m_nHotSealFreeEpoch;
}
