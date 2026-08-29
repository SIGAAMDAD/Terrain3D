// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_resources.h"
#include "rendering/rd/render_world/terrain_3d_render_world_rd_owner.h"

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <limits.h>
#include <string.h>

using namespace godot;

static uint32_t t3d_rw_channel_page_count( const T3DRenderWorldLimits &pLimits, T3DRenderWorldVTChannel pChannel ) {
	switch ( pChannel ) {
		case T3D_RW_VT_ALBEDO:
			return pLimits.nMaxVtPhysicalAlbedoPages;
		case T3D_RW_VT_NORMAL:
			return pLimits.nMaxVtPhysicalNormalPages;
		case T3D_RW_VT_SURFACE:
			return pLimits.nMaxVtPhysicalSurfacePages;
		case T3D_RW_VT_HEIGHT:
			return pLimits.nMaxVtPhysicalHeightPages;
		default:
			return 0u;
	}
}

static RenderingDevice::DataFormat t3d_rw_channel_format( T3DRenderWorldVTChannel pChannel ) {
	switch ( pChannel ) {
		case T3D_RW_VT_ALBEDO:
			return RenderingDevice::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK;
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

static const char *t3d_rw_buffer_name( const T3DRenderWorldGpuBufferId pBuffer ) {
	static const char *const s_szNames[T3D_RW_GPU_BUFFER_COUNT] = {
		"RenderWorld geometry vertices",
		"RenderWorld cluster vertex indices",
		"RenderWorld cluster triangles",
		"RenderWorld mesh records",
		"RenderWorld mesh LOD records",
		"RenderWorld cluster records",
		"RenderWorld instance records",
		"RenderWorld cell records",
		"RenderWorld material records",
		"RenderWorld VT allocations",
		"RenderWorld VT page table",
		"RenderWorld visible cells",
		"RenderWorld visible instances",
		"RenderWorld visible world clusters",
		"RenderWorld visible clutter clusters",
		"RenderWorld visible foliage clusters",
		"RenderWorld selected LODs",
		"RenderWorld counters",
		"RenderWorld indirect args",
		"RenderWorld VT request hash",
		"RenderWorld VT request list",
		"RenderWorld upload ring",
		"RenderWorld readback ring",
		"RenderWorld visibility frame",
	};
	return ( uint32_t )pBuffer < T3D_RW_GPU_BUFFER_COUNT ? s_szNames[pBuffer] : "RenderWorld buffer";
}

static Terrain3DRdContext::MemoryCategory t3d_rw_buffer_category( const T3DRenderWorldGpuBufferId pBuffer ) {
	switch ( pBuffer ) {
		case T3D_RW_GPU_BUFFER_GEOMETRY_VERTEX:
			return Terrain3DRdContext::MEMORY_MESH_VERTEX_ARENA;
		case T3D_RW_GPU_BUFFER_CLUSTER_VERTEX_INDEX:
		case T3D_RW_GPU_BUFFER_CLUSTER_TRIANGLE:
			return Terrain3DRdContext::MEMORY_MESH_INDEX_ARENA;
		case T3D_RW_GPU_BUFFER_MATERIAL_RECORDS:
			return Terrain3DRdContext::MEMORY_MATERIAL_TABLE;
		case T3D_RW_GPU_BUFFER_INSTANCE_RECORDS:
		case T3D_RW_GPU_BUFFER_CELL_RECORDS:
			return Terrain3DRdContext::MEMORY_INSTANCE;
		case T3D_RW_GPU_BUFFER_INDIRECT_ARGS:
			return Terrain3DRdContext::MEMORY_INDIRECT;
		case T3D_RW_GPU_BUFFER_UPLOAD_RING:
		case T3D_RW_GPU_BUFFER_READBACK_RING:
		case T3D_RW_GPU_BUFFER_VISIBILITY_FRAME:
			return Terrain3DRdContext::MEMORY_SCRATCH;
		default:
			return Terrain3DRdContext::MEMORY_STORAGE;
	}
}

Terrain3DRenderWorldResourceImage::Terrain3DRenderWorldResourceImage() {
	m_pContext = NULL;
	m_pRenderingDevice = NULL;
	memset( m_nVtBankCount, 0, sizeof( m_nVtBankCount ) );
	memset( m_nVtBankLayers, 0, sizeof( m_nVtBankLayers ) );
	memset( m_nVtBankBytes, 0, sizeof( m_nVtBankBytes ) );
	memset( &m_Plan, 0, sizeof( m_Plan ) );
	memset( &m_Limits, 0, sizeof( m_Limits ) );
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	m_nHotSealCreateEpoch = 0u;
	m_nHotSealFreeEpoch = 0u;
	m_bInitialized = false;
	m_bHotSealed = false;
}

Terrain3DRenderWorldResourceImage::~Terrain3DRenderWorldResourceImage() {
	// Ownership is intentionally explicit. The composition root must call shutdown_cold().
}

bool Terrain3DRenderWorldResourceImage::initialize_cold(
        Terrain3DRdContext &rContext, const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile ) {
	return initialize_internal_cold( rContext, pLimits, pProfile );
}

bool Terrain3DRenderWorldResourceImage::initialize_internal_cold(
        Terrain3DRdContext &rContext, const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile ) {
	RenderingDevice *const pRenderingDevice = rContext.getDevice();
	if ( m_bInitialized || pRenderingDevice == NULL ) {
		return false;
	}
	if ( !t3d_render_world_compute_gpu_resource_plan( pLimits, pProfile, m_Plan ) ) {
		return false;
	}

	m_pContext = &rContext;
	m_pRenderingDevice = pRenderingDevice;
	m_Limits = pLimits;
	if ( !validate_device_cold( pLimits, pProfile ) ) {
		m_pRenderingDevice = NULL;
		m_pContext = NULL;
		return false;
	}

	if ( !create_buffers_cold() ) {
		rollback_failed_initialize_cold();
		return false;
	}

	if ( !create_vt_textures_cold( pLimits, pProfile ) ) {
		rollback_failed_initialize_cold();
		return false;
	}

	if ( !create_hiz_texture_cold( pProfile ) ) {
		rollback_failed_initialize_cold();
		return false;
	}

	m_bInitialized = true;
	++m_Stats.nResourceGeneration;
	return true;
}

void Terrain3DRenderWorldResourceImage::shutdown_cold() {
	m_bHotSealed = false;

	if ( m_pRenderingDevice == NULL ) {
		m_bInitialized = false;
		return;
	}

	free_rid_cold( m_hHiZ, m_Plan.nHiZBytes, true );

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
	m_pRenderingDevice = NULL;
	m_pContext = NULL;
	m_bInitialized = false;
	++m_Stats.nResourceGeneration;
}

void Terrain3DRenderWorldResourceImage::seal_hot_runtime() {
	if ( !m_bInitialized ) {
		return;
	}
	m_nHotSealCreateEpoch = m_Stats.nGpuCreateCount;
	m_nHotSealFreeEpoch = m_Stats.nGpuFreeCount;
	m_bHotSealed = true;
}

void Terrain3DRenderWorldResourceImage::unseal_hot_runtime_cold() {
	m_bHotSealed = false;
}

bool Terrain3DRenderWorldResourceImage::service_idle_frame_hot() {
	if ( !m_bInitialized || !m_bHotSealed ) {
		return false;
	}
	if ( hot_epoch_unchanged() ) {
		return true;
	}

	++m_Stats.nHotSealViolationCount;
	return false;
}

bool Terrain3DRenderWorldResourceImage::validate_idle_plateau_reference_debug( uint32_t pFrameCount ) {
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

bool Terrain3DRenderWorldResourceImage::is_initialized() const {
	return m_bInitialized;
}

bool Terrain3DRenderWorldResourceImage::is_hot_sealed() const {
	return m_bHotSealed;
}

const T3DRenderWorldGpuResourcePlan &Terrain3DRenderWorldResourceImage::get_plan() const {
	return m_Plan;
}

const T3DRenderWorldLimits &Terrain3DRenderWorldResourceImage::get_limits() const {
	return m_Limits;
}

const T3DRenderWorldGpuResourceStats &Terrain3DRenderWorldResourceImage::get_stats() const {
	return m_Stats;
}

const RID &Terrain3DRenderWorldResourceImage::get_buffer_rid( T3DRenderWorldGpuBufferId pBuffer ) const {
	if ( ( uint32_t )pBuffer >= T3D_RW_GPU_BUFFER_COUNT ) {
		return m_hInvalid;
	}
	return m_hBuffers[pBuffer];
}

const RID &Terrain3DRenderWorldResourceImage::get_hiz_rid() const {
	return m_hHiZ;
}

uint32_t Terrain3DRenderWorldResourceImage::get_vt_bank_count( T3DRenderWorldVTChannel pChannel ) const {
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return 0u;
	}
	return m_nVtBankCount[pChannel];
}

uint32_t Terrain3DRenderWorldResourceImage::get_vt_bank_layer_count( T3DRenderWorldVTChannel pChannel, uint32_t pBank ) const {
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return 0u;
	}
	if ( pBank >= m_nVtBankCount[pChannel] ) {
		return 0u;
	}
	return m_nVtBankLayers[pChannel][pBank];
}

const RID &Terrain3DRenderWorldResourceImage::get_vt_bank_rid( T3DRenderWorldVTChannel pChannel, uint32_t pBank ) const {
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return m_hInvalid;
	}
	if ( pBank >= m_nVtBankCount[pChannel] ) {
		return m_hInvalid;
	}
	return m_hVtPhysical[pChannel][pBank];
}

Terrain3DRdContext *Terrain3DRenderWorldResourceImage::get_rd_context() const {
	return m_pContext;
}

bool Terrain3DRenderWorldResourceImage::validate_device_cold(
        const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile ) const {
	if ( m_pRenderingDevice == NULL ) {
		return false;
	}

	for ( uint32_t iBuffer = 0u; iBuffer < T3D_RW_GPU_BUFFER_COUNT; ++iBuffer ) {
		// godot-cpp 10.0.0-rc1 / Godot 4.6 exposes uint32_t buffer sizes.
		if ( m_Plan.nBufferBytes[iBuffer] == 0u || m_Plan.nBufferBytes[iBuffer] > UINT32_MAX ) {
			return false;
		}
	}

	const uint64_t nMaxTextureSize = m_pRenderingDevice->limit_get( RenderingDevice::LIMIT_MAX_TEXTURE_SIZE_2D );
	const uint64_t nMaxArrayLayers = m_pRenderingDevice->limit_get( RenderingDevice::LIMIT_MAX_TEXTURE_ARRAY_LAYERS );
	if ( nMaxTextureSize == 0u || nMaxArrayLayers == 0u ) {
		return false;
	}
	if ( pProfile.nVtPhysicalTileSize > nMaxTextureSize ) {
		return false;
	}
	if ( pProfile.nHiZWidth > nMaxTextureSize || pProfile.nHiZHeight > nMaxTextureSize ) {
		return false;
	}

	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		const uint32_t nPages = t3d_rw_channel_page_count( pLimits, ( T3DRenderWorldVTChannel )iChannel );
		T3DRenderWorldVTBankLayout Layout = {};
		if ( !t3d_render_world_compute_vt_bank_layout( nPages, ( uint32_t )nMaxArrayLayers, Layout ) ) {
			return false;
		}
	}

	const BitField<RenderingDevice::TextureUsageBits> VtUsage =
	        ( BitField<RenderingDevice::TextureUsageBits> )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );

	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		if ( !m_pRenderingDevice->texture_is_format_supported_for_usage(
		             t3d_rw_channel_format( ( T3DRenderWorldVTChannel )iChannel ), VtUsage ) ) {
			return false;
		}
	}

	const BitField<RenderingDevice::TextureUsageBits> HiZUsage =
	        ( BitField<RenderingDevice::TextureUsageBits> )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT
	                | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );

	return m_pRenderingDevice->texture_is_format_supported_for_usage( RenderingDevice::DATA_FORMAT_R32_SFLOAT, HiZUsage );
}

bool Terrain3DRenderWorldResourceImage::create_buffers_cold() {
	for ( uint32_t iBuffer = 0u; iBuffer < T3D_RW_GPU_BUFFER_COUNT; ++iBuffer ) {
		const bool bIndirect = iBuffer == T3D_RW_GPU_BUFFER_INDIRECT_ARGS;
		if ( !create_storage_buffer_cold( ( T3DRenderWorldGpuBufferId )iBuffer, m_Plan.nBufferBytes[iBuffer], bIndirect ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DRenderWorldResourceImage::create_vt_textures_cold(
        const T3DRenderWorldLimits &pLimits, const T3DRenderWorldGpuProfile &pProfile ) {
	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		const T3DRenderWorldVTChannel Channel = ( T3DRenderWorldVTChannel )iChannel;
		if ( !create_vt_channel_cold( Channel,
		             t3d_rw_channel_page_count( pLimits, Channel ),
		             pProfile.nVtPhysicalTileSize,
		             t3d_rw_channel_format( Channel ) ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DRenderWorldResourceImage::create_hiz_texture_cold( const T3DRenderWorldGpuProfile &pProfile ) {
	const BitField<RenderingDevice::TextureUsageBits> Usage =
	        ( BitField<RenderingDevice::TextureUsageBits> )( RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT
	                | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT
	                | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT );

	if ( !create_texture_cold( m_hHiZ,
	             pProfile.nHiZWidth,
	             pProfile.nHiZHeight,
	             1u,
	             pProfile.nHiZMipCount,
	             RenderingDevice::TEXTURE_TYPE_2D,
	             RenderingDevice::DATA_FORMAT_R32_SFLOAT,
	             Usage ) ) {
		return false;
	}

	if ( m_pContext != NULL
	        && !m_pContext->registerResource( m_hHiZ,
	                "RenderWorld Hi-Z",
	                Terrain3DRdContext::RESOURCE_TEXTURE,
	                Terrain3DRdContext::MEMORY_SCRATCH,
	                m_Plan.nHiZBytes,
	                Terrain3DRdContext::OWNER_RENDER_WORLD ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hHiZ );
		return false;
	}
	m_Stats.nLiveTextureBytes += m_Plan.nHiZBytes;
	return true;
}

bool Terrain3DRenderWorldResourceImage::create_storage_buffer_cold( T3DRenderWorldGpuBufferId pBuffer, uint64_t pBytes, bool pIndirect ) {
	if ( m_pRenderingDevice == NULL ) {
		return false;
	}
	if ( ( uint32_t )pBuffer >= T3D_RW_GPU_BUFFER_COUNT ) {
		return false;
	}
	if ( pBytes == 0u || pBytes > UINT32_MAX ) {
		return false;
	}

	const BitField<RenderingDevice::StorageBufferUsage> Usage = pIndirect
	        ? ( BitField<RenderingDevice::StorageBufferUsage> )RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT
	        : ( BitField<RenderingDevice::StorageBufferUsage> )0;

	m_hBuffers[pBuffer] = m_pRenderingDevice->storage_buffer_create( ( uint32_t )pBytes, PackedByteArray(), Usage );
	if ( !m_hBuffers[pBuffer].is_valid() ) {
		return false;
	}
	if ( m_pContext != NULL
	        && !m_pContext->registerResource( m_hBuffers[pBuffer],
	                String::utf8( t3d_rw_buffer_name( pBuffer ) ),
	                pIndirect ? Terrain3DRdContext::RESOURCE_INDIRECT_BUFFER : Terrain3DRdContext::RESOURCE_STORAGE_BUFFER,
	                t3d_rw_buffer_category( pBuffer ),
	                pBytes,
	                Terrain3DRdContext::OWNER_RENDER_WORLD ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hBuffers[pBuffer] );
		return false;
	}

	++m_Stats.nGpuCreateCount;
	++m_Stats.nLiveResourceCount;
	m_Stats.nLiveBufferBytes += pBytes;
	return true;
}

bool Terrain3DRenderWorldResourceImage::create_vt_channel_cold(
        T3DRenderWorldVTChannel pChannel, uint32_t pPageCount, uint32_t pTileSize, RenderingDevice::DataFormat pFormat ) {
	if ( m_pRenderingDevice == NULL ) {
		return false;
	}
	if ( ( uint32_t )pChannel >= T3D_RW_VT_CHANNEL_COUNT || pPageCount == 0u ) {
		return false;
	}

	const uint32_t nMaxArrayLayers = ( uint32_t )m_pRenderingDevice->limit_get( RenderingDevice::LIMIT_MAX_TEXTURE_ARRAY_LAYERS );
	T3DRenderWorldVTBankLayout Layout = {};
	if ( !t3d_render_world_compute_vt_bank_layout( pPageCount, nMaxArrayLayers, Layout ) ) {
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
		        && !m_pContext->registerResource( m_hVtPhysical[pChannel][nBank],
		                String( "RenderWorld VT channel " ) + String::num_uint64( ( uint64_t )pChannel ) + " bank "
		                        + String::num_uint64( ( uint64_t )nBank ),
		                Terrain3DRdContext::RESOURCE_TEXTURE,
		                Terrain3DRdContext::MEMORY_TERRAIN_TEXTURE,
		                nBankBytes,
		                Terrain3DRdContext::OWNER_RENDER_WORLD ) ) {
			t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hVtPhysical[pChannel][nBank] );
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

bool Terrain3DRenderWorldResourceImage::create_texture_cold( RID &rRid,
        uint32_t pWidth,
        uint32_t pHeight,
        uint32_t pLayers,
        uint32_t pMipCount,
        RenderingDevice::TextureType pType,
        RenderingDevice::DataFormat pFormat,
        BitField<RenderingDevice::TextureUsageBits> pUsage ) {
	if ( m_pRenderingDevice == NULL ) {
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

void Terrain3DRenderWorldResourceImage::free_rid_cold( RID &rRid, uint64_t pBytes, bool pTexture ) {
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

void Terrain3DRenderWorldResourceImage::rollback_failed_initialize_cold() {
	shutdown_cold();
	m_bInitialized = false;
}

bool Terrain3DRenderWorldResourceImage::hot_epoch_unchanged() const {
	return m_Stats.nGpuCreateCount == m_nHotSealCreateEpoch && m_Stats.nGpuFreeCount == m_nHotSealFreeEpoch;
}
