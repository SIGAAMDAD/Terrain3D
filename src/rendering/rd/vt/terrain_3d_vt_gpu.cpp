// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/vt/terrain_3d_vt_gpu.h"

#include "rendering/rd/vt/terrain_3d_vt_pack_runtime.h"
#include "rendering/rd/terrain_3d_rd_shader_compile_status.h"
#include "rendering/rd/infrastructure/terrain_3d_gpu_upload_service.h"
#include "rendering/rd/infrastructure/terrain_3d_rd_resource_service.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <limits.h>
#include <string.h>

using namespace godot;

static bool t3dVtUpload( const Terrain3DVtResourceImage *const pResources,
                         const RID& rBuffer, uint32_t nOffset, uint32_t nBytes, const PackedByteArray& rBytes,
                         Terrain3DGpuBufferPolicy policy = T3D_GPU_DIRTY_UPLOAD )
{
	Terrain3DRdContext *const context = pResources ? pResources->get_rd_context() : NULL;
	return context && Terrain3DGpuUploadService( *context, Terrain3DRdContext::OWNER_VT, policy )
	                          .updatePacked( rBuffer, nOffset, nBytes, rBytes ) == OK;
}

Terrain3DVTGpu::Terrain3DVTGpu()
{
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_pDatabaseView = NULL;
	memset( &m_Plan, 0, sizeof( m_Plan ) );
	memset( &m_Limits, 0, sizeof( m_Limits ) );
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	m_nHotCreateEpoch = 0u;
	m_nHotFreeEpoch = 0u;
	m_nUploadBatchCount = 0u;
	m_nDatabaseUploadStage = 0u;
	m_nDatabaseTailChannel = 0u;
	m_bUploadBatchActive = false;
	m_bDatabaseUploadBegun = false;
	m_bCpuPrepared = false;
	m_bStorageReady = false;
	m_bExecutableReady = false;
	m_bInitialized = false;
	m_bDatabaseUploadedWarm = false;
	m_bHotSealed = false;
}

Terrain3DVTGpu::~Terrain3DVTGpu()
{
	// Explicit cold shutdown is required by the packet ownership contract.
}

static bool t3d_vt_gpu_can_initialize( const bool bInitialized,
                                       const Terrain3DVtResourceImage *const pResources,
                                       const RenderingDevice *const pRenderingDevice )
{
	return !bInitialized && pResources != NULL && pRenderingDevice != NULL && pResources->is_initialized();
}

static bool t3d_vt_gpu_has_valid_limits( const T3DRenderWorldLimits& pLimits )
{
	return pLimits.nMaxVtRequests != 0u && pLimits.nMaxVtRequests <= UINT32_MAX / 2u && pLimits.nMaxReadbackBytesPerFrame <= UINT32_MAX;
}

bool Terrain3DVTGpu::prepare_cpu_cold( const T3DVtGpuResourcePlan& pPlan, const T3DRenderWorldLimits& pLimits )
{
	if ( m_bCpuPrepared || m_bStorageReady || m_bExecutableReady || m_bInitialized || !t3d_vt_gpu_has_valid_limits( pLimits ) || pPlan.nBufferBytes[ T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE ] == 0u ) {
		return false;
	}
	m_Limits = pLimits;
	m_Plan = pPlan;
	uint64_t nNeutralTailBytesTotal = 0u;
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c )
		nNeutralTailBytesTotal += pPlan.nVtTailBytes[ c ];
	UtilityFunctions::print( "[Terrain3D][VTInit] CPU STAGING PLAN allocation=",
	                         pPlan.nBufferBytes[ T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE ],
	                         " B physical_page_sets=", pLimits.nMaxVtPhysicalPageSets,
	                         " neutral_tail_total=", nNeutralTailBytesTotal, " B",
	                         " tail_height=", pPlan.nVtTailAtlasHeight );
	const uint64_t nAllocationScratchBytes64 = pPlan.nBufferBytes[ T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE ];
	if ( nAllocationScratchBytes64 == 0u || nAllocationScratchBytes64 > INT32_MAX ) {
		shutdown_cold();
		return false;
	}
	m_AllocationUpload.resize( (int64_t)nAllocationScratchBytes64 );
	if ( m_AllocationUpload.size() != (int64_t)nAllocationScratchBytes64 ) {
		shutdown_cold();
		return false;
	}
	const uint32_t nRetiredScratchCapacity = pLimits.nMaxVtPhysicalPageSets != 0u ? pLimits.nMaxVtPhysicalPageSets : 1u;
	const uint64_t nPageScratchBytes64 = (uint64_t)nRetiredScratchCapacity * sizeof( T3DVTPageTableEntry );
	if ( nPageScratchBytes64 > INT32_MAX ) {
		shutdown_cold();
		return false;
	}
	m_PageEntryUpload.resize( (int64_t)nPageScratchBytes64 );
	m_RetiredPageScratch.resize( (int64_t)nRetiredScratchCapacity );
	m_RetiredSlotScratch.resize( (int64_t)nRetiredScratchCapacity );
	m_UploadBatchOrder.resize( (int64_t)nRetiredScratchCapacity );
	m_UploadBatchReservations.resize( (int64_t)( (uint64_t)nRetiredScratchCapacity * sizeof( T3DVTUploadReservation ) ) );
	if ( m_PageEntryUpload.size() != (int64_t)nPageScratchBytes64 || m_RetiredPageScratch.size() != (int64_t)nRetiredScratchCapacity || m_RetiredSlotScratch.size() != (int64_t)nRetiredScratchCapacity || m_UploadBatchOrder.size() != (int64_t)nRetiredScratchCapacity || m_UploadBatchReservations.size() != (int64_t)( (uint64_t)nRetiredScratchCapacity * sizeof( T3DVTUploadReservation ) ) ) {
		shutdown_cold();
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		const uint32_t nBytes = physical_layer_bytes( c );
		if ( nBytes == 0u ) {
			shutdown_cold();
			return false;
		}
		m_PhysicalUpload[ c ].resize( nBytes );
		if ( m_PhysicalUpload[ c ].size() != (int64_t)nBytes ) {
			shutdown_cold();
			return false;
		}
	}
	UtilityFunctions::print( "[Terrain3D][VTInit] NEUTRAL PAYLOAD ALLOCATION BEGIN bytes=", nNeutralTailBytesTotal );
	if ( !initialize_neutral_payloads_cold() ) {
		UtilityFunctions::push_error( "[Terrain3D][VTInit] NEUTRAL PAYLOAD ALLOCATION FAIL" );
		shutdown_cold();
		return false;
	}
	UtilityFunctions::print( "[Terrain3D][VTInit] NEUTRAL PAYLOAD ALLOCATION READY bytes=", nNeutralTailBytesTotal );
	m_bCpuPrepared = true;
	return true;
}

bool Terrain3DVTGpu::prepare_storage_cold( const Terrain3DVtResourceImage *pResources,
                                           const T3DRenderWorldLimits& pLimits )
{
	Terrain3DRdContext *const pContext = pResources ? pResources->get_rd_context() : NULL;
	RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( !m_bCpuPrepared || m_bStorageReady || m_bExecutableReady || m_bInitialized || !t3d_vt_gpu_can_initialize( false, pResources, pRenderingDevice ) || memcmp( &pResources->get_plan(), &m_Plan, sizeof( m_Plan ) ) != 0 ) {
		return false;
	}
	const uint64_t nReadbackBytes = sizeof( T3DVTGpuCounters ) + (uint64_t)pLimits.nMaxVtRequests * sizeof( T3DVTRequest );
	if ( nReadbackBytes > pLimits.nMaxReadbackBytesPerFrame || nReadbackBytes > UINT32_MAX )
		return false;
	m_pRenderingDevice = pRenderingDevice;
	m_pResources = pResources;
	UtilityFunctions::print( "[Terrain3D][VTInit] PHYSICAL BANK VALIDATION BEGIN" );
	if ( !validate_physical_bank_layout_cold() ) {
		UtilityFunctions::push_error( "[Terrain3D][VTInit] PHYSICAL BANK VALIDATION FAIL" );
		shutdown_cold();
		return false;
	}
	UtilityFunctions::print( "[Terrain3D][VTInit] PHYSICAL BANK VALIDATION READY" );
	m_bStorageReady = true;
	return true;
}

bool Terrain3DVTGpu::bind_executable_cold()
{
	while ( !m_bExecutableReady ) {
		if ( !service_bind_executable_cold( UINT32_MAX ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DVTGpu::service_bind_executable_cold(
    const uint32_t nMaxWorkUnits )
{
	if ( !m_bStorageReady || m_bInitialized || m_pRenderingDevice == NULL || m_pResources == NULL || nMaxWorkUnits == 0u ) {
		return false;
	}
	if ( m_bExecutableReady ) {
		return true;
	}
	// The scene predictor was owned by RenderWorld. Shared VT publication only
	// uploads tables and textures, so no compute executable is required here.
	m_bExecutableReady = true;
	m_bInitialized = true;
	return true;

}

bool Terrain3DVTGpu::initialize_cold( const Terrain3DVtResourceImage *pResources,
                                      const T3DRenderWorldLimits& pLimits )
{
	return pResources != NULL && prepare_cpu_cold( pResources->get_plan(), pLimits ) && prepare_storage_cold( pResources, pLimits ) && bind_executable_cold();
}

void Terrain3DVTGpu::shutdown_cold()
{
	m_bHotSealed = false;
	m_AllocationUpload.resize( 0 );
	m_PageEntryUpload.resize( 0 );
	m_RetiredPageScratch.resize( 0 );
	m_RetiredSlotScratch.resize( 0 );
	m_UploadBatchOrder.resize( 0 );
	m_UploadBatchReservations.resize( 0 );
	m_nUploadBatchCount = 0u;
	m_nDatabaseUploadStage = 0u;
	m_nDatabaseTailChannel = 0u;
	m_bUploadBatchActive = false;
	m_bDatabaseUploadBegun = false;
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		m_PhysicalUpload[ c ].resize( 0 );
		m_NeutralPhysicalUpload[ c ].resize( 0 );
		m_NeutralTailUpload[ c ].resize( 0 );
	}
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_pDatabaseView = NULL;
	m_bDatabaseUploadedWarm = false;
	m_bCpuPrepared = false;
	m_bStorageReady = false;
	m_bExecutableReady = false;
	m_bInitialized = false;
}

void Terrain3DVTGpu::seal_hot_runtime()
{
	if ( !m_bInitialized ) {
		return;
	}
	m_nHotCreateEpoch = m_Stats.nGpuCreateCount;
	m_nHotFreeEpoch = m_Stats.nGpuFreeCount;
	m_bHotSealed = true;
}

bool Terrain3DVTGpu::update_allocation_quality_hot( const T3DVTResidencyState& rResidency )
{
	if ( !m_bInitialized || !m_bDatabaseUploadedWarm || m_pRenderingDevice == NULL || m_pResources == NULL || rResidency.Buffers.pAllocations == NULL )
		return false;
	const uint64_t nBytes = (uint64_t)rResidency.nAllocationCount * sizeof( t3dVTAllocation_t );
	if ( nBytes == 0u || nBytes > UINT32_MAX || m_AllocationUpload.size() < (int64_t)nBytes )
		return false;
	memcpy( m_AllocationUpload.ptrw(), rResidency.Buffers.pAllocations, (size_t)nBytes );
	if ( !t3dVtUpload( m_pResources, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE ),
	                   0u, (uint32_t)nBytes, m_AllocationUpload ) )
		return false;
	return !m_bHotSealed || hot_epoch_unchanged();
}

bool Terrain3DVTGpu::upload_database_warm( const T3DVTDatabaseView& pDatabase, const T3DVTResidencyState& pResidency )
{
	if ( m_bDatabaseUploadedWarm ) {
		return false;
	}
	while ( !m_bDatabaseUploadedWarm ) {
		if ( !service_database_upload_warm( pDatabase, pResidency, UINT64_MAX, UINT32_MAX ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DVTGpu::service_database_upload_warm( const T3DVTDatabaseView& pDatabase,
                                                   const T3DVTResidencyState& pResidency,
                                                   const uint64_t nMaxBytes,
                                                   const uint32_t nMaxWorkUnits )
{
	m_pDatabaseView = &pDatabase;
	if ( !m_bInitialized || m_bHotSealed || m_pRenderingDevice == NULL || pDatabase.pHeader == NULL || !pResidency.bInitialized || pResidency.bNeutralPublished || pResidency.Buffers.pAllocations == NULL || pResidency.Buffers.pTailMetadata == NULL || nMaxWorkUnits == 0u || nMaxBytes == 0u ) {
		return false;
	}
	if ( m_bDatabaseUploadedWarm ) {
		return true;
	}
	uint32_t nUnits = 0u;
	uint64_t nBytesUsed = 0u;
	if ( !m_bDatabaseUploadBegun ) {
		m_bDatabaseUploadBegun = true;
		m_nDatabaseUploadStage = 0u;
		m_nDatabaseTailChannel = 0u;
	}

	if ( m_nDatabaseUploadStage == 0u && nUnits < nMaxWorkUnits ) {
		const uint64_t nAllocationBytes = (uint64_t)pResidency.nAllocationCount * sizeof( t3dVTAllocation_t );
		const uint64_t nMipLayoutCount = (uint64_t)pResidency.nAllocationCount * T3D_VT_MAX_MIPS;
		const uint64_t nMipLayoutBytes = nMipLayoutCount * sizeof( t3dVTMipLayout_t );
		const uint64_t nPageBytes = (uint64_t)pDatabase.pHeader->nPageCount * sizeof( T3DVTPageTableEntry );
		const uint64_t nTailMetadataBytes = (uint64_t)pResidency.nTailCount * sizeof( t3dVTTailMetadata_t );
		if ( nAllocationBytes == 0u || nAllocationBytes > m_pResources->get_plan().nBufferBytes[ T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE ] || nAllocationBytes > UINT32_MAX || nMipLayoutBytes == 0u || nMipLayoutBytes > UINT32_MAX || nMipLayoutBytes > m_pResources->get_plan().nBufferBytes[ T3D_RW_GPU_BUFFER_VT_MIP_LAYOUT_TABLE ] || nTailMetadataBytes == 0u || nTailMetadataBytes > m_pResources->get_plan().nBufferBytes[ T3D_RW_GPU_BUFFER_VT_TAIL_METADATA ] || nTailMetadataBytes > UINT32_MAX || nPageBytes > m_pResources->get_plan().nBufferBytes[ T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ] || nPageBytes > UINT32_MAX || m_AllocationUpload.size() < (int64_t)nAllocationBytes ) {
			return false;
		}
		memcpy( m_AllocationUpload.ptrw(), pResidency.Buffers.pAllocations, (size_t)nAllocationBytes );
		if ( !t3dVtUpload( m_pResources, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE ),
		                   0u, (uint32_t)nAllocationBytes, m_AllocationUpload, T3D_GPU_PUBLICATION ) )
			return false;

		PackedByteArray Upload;
		Upload.resize( (int64_t)nMipLayoutBytes );
		if ( Upload.size() != (int64_t)nMipLayoutBytes )
			return false;
		memset( Upload.ptrw(), 0, (size_t)nMipLayoutBytes );
		t3dVTMipLayout_t *const pMipLayouts = (t3dVTMipLayout_t *)Upload.ptrw();
		for ( uint32_t iAllocation = 0u; iAllocation < pResidency.nAllocationCount; ++iAllocation ) {
			const t3dVTAllocation_t& Allocation = pResidency.Buffers.pAllocations[ iAllocation ];
			uint64_t nBasePage = Allocation.m_nBaseVirtualPage;
			uint32_t nWidth = Allocation.m_nMip0WidthPages;
			uint32_t nHeight = Allocation.m_nMip0HeightPages;
			const uint64_t nAllocationEnd = (uint64_t)Allocation.m_nBaseVirtualPage + Allocation.m_nPageCount;
			if ( pDatabase.nRuntimeKind == T3D_VT_RUNTIME_DATABASE_PACK_V3 && Allocation.m_nPageCount == 0u )
				continue;
			if ( Allocation.m_nMipCount == 0u || Allocation.m_nMipCount > T3D_VT_MAX_MIPS || nWidth == 0u || nHeight == 0u || nAllocationEnd > pDatabase.pHeader->nPageCount )
				return false;
			for ( uint32_t iMip = 0u; iMip < Allocation.m_nMipCount; ++iMip ) {
				t3dVTMipLayout_t& Layout = pMipLayouts[ (uint64_t)iAllocation * T3D_VT_MAX_MIPS + iMip ];
				const uint64_t nMipPages = (uint64_t)nWidth * nHeight;
				if ( nBasePage >= nAllocationEnd || nMipPages == 0u || nBasePage + nMipPages > nAllocationEnd ) {
					if ( pDatabase.nRuntimeKind != T3D_VT_RUNTIME_DATABASE_PACK_V3 || Allocation.m_nPageCount == 0u )
						return false;
					Layout.m_nBaseVirtualPage = (uint32_t)( nAllocationEnd - 1u );
					Layout.m_nWidthPages = 1u;
					Layout.m_nHeightPages = 1u;
				} else {
					if ( nBasePage > UINT32_MAX || nWidth > 0xffffu || nHeight > 0xffffu )
						return false;
					Layout.m_nBaseVirtualPage = (uint32_t)nBasePage;
					Layout.m_nWidthPages = (uint16_t)nWidth;
					Layout.m_nHeightPages = (uint16_t)nHeight;
					nBasePage += nMipPages;
				}
				nWidth = nWidth > 1u ? ( nWidth + 1u ) >> 1u : 1u;
				nHeight = nHeight > 1u ? ( nHeight + 1u ) >> 1u : 1u;
			}
		}
		if ( !t3dVtUpload( m_pResources, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_MIP_LAYOUT_TABLE ),
		                   0u, (uint32_t)nMipLayoutBytes, Upload, T3D_GPU_PUBLICATION ) )
			return false;

		Upload.resize( (int64_t)nPageBytes );
		if ( Upload.size() != (int64_t)nPageBytes )
			return false;
		T3DVTPageTableEntry *pGpuEntries = (T3DVTPageTableEntry *)Upload.ptrw();
		for ( uint32_t i = 0u; i < pDatabase.pHeader->nPageCount; ++i ) {
			if ( !make_gpu_page_entry( i, pResidency.Buffers.pPageTable[ i ], pGpuEntries[ i ] ) )
				return false;
		}
		if ( !t3dVtUpload( m_pResources, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ),
		                   0u, (uint32_t)nPageBytes, Upload, T3D_GPU_PUBLICATION ) )
			return false;

		Upload.resize( (int64_t)nTailMetadataBytes );
		if ( Upload.size() != (int64_t)nTailMetadataBytes )
			return false;
		memcpy( Upload.ptrw(), pResidency.Buffers.pTailMetadata, (size_t)nTailMetadataBytes );
		if ( !t3dVtUpload( m_pResources, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_TAIL_METADATA ),
		                   0u, (uint32_t)nTailMetadataBytes, Upload, T3D_GPU_PUBLICATION ) )
			return false;

		if ( pDatabase.nRuntimeKind == T3D_VT_RUNTIME_DATABASE_PACK_V3 ) {
			const T3DVtGpuResourcePlan& Plan = m_pResources->get_plan();
			for ( uint32_t iTail = 0u; iTail < pDatabase.nRuntimeTailCount; ++iTail ) {
				for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
					const uint32_t nResourceChannel = t3d_vt_physical_family_legacy_resource_channel( (t3dVTPhysicalFormatFamily_t)iChannel );
					if ( nResourceChannel >= T3D_RW_VT_CHANNEL_COUNT || m_NeutralTailUpload[ nResourceChannel ].size() != (int64_t)Plan.nVtTailBytes[ nResourceChannel ] || !t3d_vt_pack_runtime_copy_tail_mip_chain( pDatabase, iTail, iChannel, m_NeutralTailUpload[ nResourceChannel ].ptrw(), Plan.nVtTailBytes[ nResourceChannel ], Plan.nVtTailAtlasWidth, Plan.nVtTailAtlasHeight ) )
						return false;
				}
			}
		}
		nBytesUsed += nAllocationBytes + nMipLayoutBytes + nPageBytes + nTailMetadataBytes;
		++nUnits;
		m_nDatabaseUploadStage = 1u;
	}

	while ( m_nDatabaseUploadStage == 1u && m_nDatabaseTailChannel < T3D_RW_VT_CHANNEL_COUNT && nUnits < nMaxWorkUnits ) {
		const uint32_t c = m_nDatabaseTailChannel;
		const uint64_t nTailBytes = m_Plan.nVtTailBytes[ c ];
		if ( nTailBytes == 0u || nTailBytes > UINT32_MAX || m_NeutralTailUpload[ c ].size() != (int64_t)nTailBytes || !m_pResources->get_vt_tail_rid( (T3DVtChannel)c ).is_valid() )
			return false;
		if ( nBytesUsed != 0u && nBytesUsed + nTailBytes > nMaxBytes )
			break;
		if ( m_pRenderingDevice->texture_update( m_pResources->get_vt_tail_rid( (T3DVtChannel)c ), 0u,
		                                         m_NeutralTailUpload[ c ] ) != OK )
			return false;
		m_Stats.nUploadBytes += nTailBytes;
		nBytesUsed += nTailBytes;
		++m_nDatabaseTailChannel;
		++nUnits;
	}
	if ( m_nDatabaseUploadStage == 1u && m_nDatabaseTailChannel == T3D_RW_VT_CHANNEL_COUNT ) {
		m_bDatabaseUploadedWarm = true;
		m_bDatabaseUploadBegun = false;
		m_nDatabaseUploadStage = 0u;
		m_nDatabaseTailChannel = 0u;
	}
	return true;
}

bool Terrain3DVTGpu::is_database_upload_pending_warm() const
{
	return m_bDatabaseUploadBegun && !m_bDatabaseUploadedWarm;
}

bool Terrain3DVTGpu::upload_neutral_page_set_warm( T3DVTResidencyState& rResidency, uint32_t pPhysicalSlot )
{
	if ( !m_bInitialized || !m_bDatabaseUploadedWarm || m_bHotSealed || m_pRenderingDevice == NULL || pPhysicalSlot >= rResidency.Buffers.nPhysicalPageSetCapacity || rResidency.bNeutralPublished ) {
		return false;
	}
	uint32_t nBank = 0u, nLayer = 0u;
	if ( !map_physical_slot_hot( pPhysicalSlot, nBank, nLayer ) ) {
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		const uint32_t nExpected = physical_layer_bytes( c );
		if ( nExpected == 0u || m_NeutralPhysicalUpload[ c ].size() != (int64_t)nExpected ) {
			return false;
		}
		if ( m_pRenderingDevice->texture_update(
		         m_pResources->get_vt_bank_rid( (T3DVtChannel)c, nBank ), nLayer, m_NeutralPhysicalUpload[ c ] ) != OK ) {
			return false;
		}
		m_Stats.nUploadBytes += nExpected;
	}

	// Texture uploads are ordered before page-table publication by Godot 4.7.2 automatic RD barriers.
	T3DVTPageTableEntry Previous[ T3D_RW_VT_CHANNEL_COUNT ] = {};
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		const uint32_t nRootPage = rResidency.nNeutralRootPage[ c ];
		if ( nRootPage == T3D_RW_VT_INVALID_PAGE || nRootPage >= rResidency.pDatabase->pHeader->nPageCount ) {
			return false;
		}
		Previous[ c ] = rResidency.Buffers.pPageTable[ nRootPage ];
		T3DVTPageTableEntry Preview = Previous[ c ];
		Preview.nPhysicalSlot = pPhysicalSlot;
		Preview.nFlags |= T3D_VT_PAGE_RESIDENT | T3D_VT_PAGE_PINNED | T3D_VT_PAGE_NEUTRAL | T3D_VT_PAGE_ROOT;
		if ( !update_page_entry_hot( nRootPage, Preview ) ) {
			for ( uint32_t r = 0u; r < c; ++r ) {
				update_page_entry_hot( rResidency.nNeutralRootPage[ r ], Previous[ r ] );
			}
			return false;
		}
	}
	if ( !t3d_vt_commit_neutral_backing_warm( rResidency, pPhysicalSlot ) ) {
		for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
			update_page_entry_hot( rResidency.nNeutralRootPage[ c ], Previous[ c ] );
		}
		return false;
	}
	return true;
}

bool Terrain3DVTGpu::service_retired_slots_hot( T3DVTResidencyState& rResidency, const uint32_t pFrameId )
{
	if ( !m_bInitialized || !m_bHotSealed || !hot_epoch_unchanged() || rResidency.Buffers.pRetiredPageSets == NULL ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	const uint32_t nRetiredCount = rResidency.Stats.nRetiredSlotCount;
	if ( nRetiredCount == 0u ) {
		return true;
	}
	if ( nRetiredCount > (uint32_t)m_RetiredPageScratch.size() || nRetiredCount > (uint32_t)m_RetiredSlotScratch.size() ) {
		return false;
	}

	int32_t *const pPages = m_RetiredPageScratch.ptrw();
	int32_t *const pSlots = m_RetiredSlotScratch.ptrw();
	uint32_t nReadyCount = 0u;
	for ( uint32_t i = 0u; i < nRetiredCount; ++i ) {
		const uint32_t nRingIndex = ( rResidency.nRetireHead + i ) % rResidency.Buffers.nPhysicalPageSetCapacity;
		const T3DVTRetiredPageSet& Retired = rResidency.Buffers.pRetiredPageSets[ nRingIndex ];
		if ( !t3d_vt_retirement_ready_reference_debug( rResidency, Retired, pFrameId ) ) {
			++rResidency.Stats.nRetireDeferredCount;
			break;
		}
		if ( Retired.nPhysicalSlot >= rResidency.Buffers.nPhysicalPageSetCapacity || Retired.nVirtualPage >= rResidency.pDatabase->pHeader->nPageCount ) {
			return false;
		}
		const t3dVTPhysicalSlot_t& Slot = rResidency.Buffers.pPhysicalPageSets[ Retired.nPhysicalSlot ];
		const T3DVTPageTableEntry& Entry = rResidency.Buffers.pPageTable[ Retired.nVirtualPage ];
		if ( ( Slot.m_nFlags & T3D_VT_SLOT_RETIRED ) == 0u || Slot.m_nVirtualPage != Retired.nVirtualPage || Slot.m_nGeneration != Retired.nGeneration || ( Entry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u || Entry.nPhysicalSlot != Retired.nPhysicalSlot ) {
			return false;
		}
		uint32_t nInsert = nReadyCount;
		while ( nInsert > 0u && (uint32_t)pPages[ nInsert - 1u ] > Retired.nVirtualPage ) {
			pPages[ nInsert ] = pPages[ nInsert - 1u ];
			pSlots[ nInsert ] = pSlots[ nInsert - 1u ];
			--nInsert;
		}
		pPages[ nInsert ] = (int32_t)Retired.nVirtualPage;
		pSlots[ nInsert ] = (int32_t)Retired.nPhysicalSlot;
		++nReadyCount;
	}
	if ( nReadyCount == 0u ) {
		return false;
	}

	uint32_t nFirst = 0u;
	while ( nFirst < nReadyCount ) {
		uint32_t nLast = nFirst + 1u;
		while ( nLast < nReadyCount && (uint32_t)pPages[ nLast ] == (uint32_t)pPages[ nLast - 1u ] + 1u ) {
			++nLast;
		}
		const uint32_t nRunCount = nLast - nFirst;
		const uint32_t nRunBytes = nRunCount * sizeof( T3DVTPageTableEntry );
		if ( nRunBytes > (uint32_t)m_PageEntryUpload.size() ) {
			return false;
		}
		for ( uint32_t i = 0u; i < nRunCount; ++i ) {
			const uint32_t nPage = (uint32_t)pPages[ nFirst + i ];
			T3DVTPageTableEntry Invalid = rResidency.Buffers.pPageTable[ nPage ];
			Invalid.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
			Invalid.nFlags &= (uint16_t)~T3D_VT_PAGE_RESIDENT;
			T3DVTPageTableEntry GpuEntry = {};
			if ( !make_gpu_page_entry( nPage, Invalid, GpuEntry ) ) {
				return false;
			}
			memcpy( m_PageEntryUpload.ptrw() + i * sizeof( T3DVTPageTableEntry ), &GpuEntry, sizeof( GpuEntry ) );
		}
		const uint64_t nOffset64 = (uint64_t)(uint32_t)pPages[ nFirst ] * sizeof( T3DVTPageTableEntry );
		if ( nOffset64 > UINT32_MAX || !t3dVtUpload( m_pResources, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ),
		                                             (uint32_t)nOffset64, nRunBytes, m_PageEntryUpload ) ) {
			++m_Stats.nRetireRetryCount;
			++rResidency.Stats.nRetireDeferredCount;
			return false;
		}
		m_Stats.nPageTableUpdateBytes += nRunBytes;
		++m_Stats.nPageTableUpdateCalls;
		nFirst = nLast;
	}

	// Buffer updates become visible to later RD consumers through Godot 4.7.2 automatic barriers.
	const uint32_t nOldHead = rResidency.nRetireHead;
	for ( uint32_t i = 0u; i < nReadyCount; ++i ) {
		if ( !t3d_vt_finalize_eviction_hot( rResidency, (uint32_t)pSlots[ i ], (uint32_t)pPages[ i ], pFrameId ) ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < nReadyCount; ++i ) {
		T3DVTRetiredPageSet& Retired = rResidency.Buffers.pRetiredPageSets[ ( nOldHead + i ) % rResidency.Buffers.nPhysicalPageSetCapacity ];
		Retired.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
		Retired.nVirtualPage = T3D_RW_VT_INVALID_PAGE;
		Retired.nGeneration = 0u;
		Retired.nReleaseFrame = 0u;
	}
	rResidency.nRetireHead = ( nOldHead + nReadyCount ) % rResidency.Buffers.nPhysicalPageSetCapacity;
	return nReadyCount == nRetiredCount;
}
bool Terrain3DVTGpu::begin_upload_batch_hot()
{
	if ( !m_bInitialized || !m_bHotSealed || m_bUploadBatchActive || !hot_epoch_unchanged() ) {
		return false;
	}
	m_nUploadBatchCount = 0u;
	m_bUploadBatchActive = true;
	return true;
}

void Terrain3DVTGpu::abort_upload_batch_hot( T3DVTResidencyState& rResidency )
{
	if ( m_UploadBatchReservations.size() >= (int64_t)( m_nUploadBatchCount * sizeof( T3DVTUploadReservation ) ) ) {
		const T3DVTUploadReservation *const pReservations =
		    (const T3DVTUploadReservation *)m_UploadBatchReservations.ptr();
		for ( uint32_t i = 0u; i < m_nUploadBatchCount; ++i ) {
			t3d_vt_abort_upload_hot( rResidency, pReservations[ i ] );
		}
	}
	m_nUploadBatchCount = 0u;
	m_bUploadBatchActive = false;
}

bool Terrain3DVTGpu::publish_upload_batch_page_table_hot( const T3DVTResidencyState& rResidency )
{
	if ( m_nUploadBatchCount == 0u ) {
		return true;
	}
	if ( m_nUploadBatchCount > (uint32_t)m_UploadBatchOrder.size() || m_UploadBatchReservations.size() < (int64_t)( m_nUploadBatchCount * sizeof( T3DVTUploadReservation ) ) ) {
		return false;
	}
	const T3DVTUploadReservation *const pReservations =
	    (const T3DVTUploadReservation *)m_UploadBatchReservations.ptr();
	int32_t *const pOrder = m_UploadBatchOrder.ptrw();
	for ( uint32_t i = 0u; i < m_nUploadBatchCount; ++i ) {
		pOrder[ i ] = (int32_t)i;
		uint32_t nInsert = i;
		while ( nInsert > 0u && pReservations[ (uint32_t)pOrder[ nInsert - 1u ] ].nVirtualPage > pReservations[ i ].nVirtualPage ) {
			pOrder[ nInsert ] = pOrder[ nInsert - 1u ];
			--nInsert;
		}
		pOrder[ nInsert ] = (int32_t)i;
	}
	uint32_t nFirst = 0u;
	while ( nFirst < m_nUploadBatchCount ) {
		uint32_t nLast = nFirst + 1u;
		while ( nLast < m_nUploadBatchCount ) {
			const uint32_t nPrev = pReservations[ (uint32_t)pOrder[ nLast - 1u ] ].nVirtualPage;
			const uint32_t nNext = pReservations[ (uint32_t)pOrder[ nLast ] ].nVirtualPage;
			if ( nNext != nPrev + 1u ) {
				break;
			}
			++nLast;
		}
		const uint32_t nRunCount = nLast - nFirst;
		const uint32_t nRunBytes = nRunCount * sizeof( T3DVTPageTableEntry );
		if ( nRunBytes > (uint32_t)m_PageEntryUpload.size() ) {
			return false;
		}
		for ( uint32_t i = 0u; i < nRunCount; ++i ) {
			const T3DVTUploadReservation& Reservation = pReservations[ (uint32_t)pOrder[ nFirst + i ] ];
			if ( Reservation.nVirtualPage >= rResidency.pDatabase->pHeader->nPageCount ) {
				return false;
			}
			T3DVTPageTableEntry Preview = rResidency.Buffers.pPageTable[ Reservation.nVirtualPage ];
			Preview.nPhysicalSlot = Reservation.nPhysicalSlot;
			Preview.nResolvedMip = Reservation.nWantedMip;
			Preview.nFlags |= T3D_VT_PAGE_RESIDENT;
			T3DVTPageTableEntry GpuEntry = {};
			if ( !make_gpu_page_entry( Reservation.nVirtualPage, Preview, GpuEntry ) ) {
				return false;
			}
			memcpy( m_PageEntryUpload.ptrw() + i * sizeof( T3DVTPageTableEntry ), &GpuEntry, sizeof( GpuEntry ) );
		}
		const uint32_t nFirstPage = pReservations[ (uint32_t)pOrder[ nFirst ] ].nVirtualPage;
		const uint64_t nOffset64 = (uint64_t)nFirstPage * sizeof( T3DVTPageTableEntry );
		if ( nOffset64 > UINT32_MAX || !t3dVtUpload( m_pResources, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ),
		                                             (uint32_t)nOffset64, nRunBytes, m_PageEntryUpload ) ) {
			return false;
		}
		m_Stats.nPageTableUpdateBytes += nRunBytes;
		++m_Stats.nPageTableUpdateCalls;
		nFirst = nLast;
	}
	++m_Stats.nPageTableBatchCount;
	m_Stats.nPageTableBatchEntries += m_nUploadBatchCount;
	return true;
}

bool Terrain3DVTGpu::end_upload_batch_hot( T3DVTResidencyState& rResidency, const uint32_t pFrameId )
{
	if ( !m_bUploadBatchActive || !m_bInitialized || !m_bHotSealed || !hot_epoch_unchanged() ) {
		return false;
	}
	if ( m_nUploadBatchCount == 0u ) {
		m_bUploadBatchActive = false;
		return true;
	}
	const T3DVTUploadReservation *const pReservations =
	    (const T3DVTUploadReservation *)m_UploadBatchReservations.ptr();
	uint32_t nVictimCount = 0u;
	for ( uint32_t i = 0u; i < m_nUploadBatchCount; ++i ) {
		if ( !t3d_vt_can_commit_upload_hot( rResidency, pReservations[ i ] ) ) {
			abort_upload_batch_hot( rResidency );
			return false;
		}
		for ( uint32_t j = 0u; j < i; ++j ) {
			const bool bDuplicateVirtualPage = pReservations[ j ].nVirtualPage == pReservations[ i ].nVirtualPage;
			const bool bDuplicatePhysicalSlot = pReservations[ j ].nPhysicalSlot == pReservations[ i ].nPhysicalSlot;
			const bool bDuplicateVictim = pReservations[ i ].nVictimPage != T3D_RW_VT_INVALID_PAGE && pReservations[ j ].nVictimPage == pReservations[ i ].nVictimPage;
			if ( bDuplicateVirtualPage || bDuplicatePhysicalSlot || bDuplicateVictim ) {
				abort_upload_batch_hot( rResidency );
				return false;
			}
		}
		if ( pReservations[ i ].nVictimPage != T3D_RW_VT_INVALID_PAGE ) {
			++nVictimCount;
		}
	}
	if ( nVictimCount > rResidency.Buffers.nPhysicalPageSetCapacity - rResidency.Stats.nRetiredSlotCount ) {
		abort_upload_batch_hot( rResidency );
		return false;
	}
	// Publish all logical mappings only after every backing texture update is staged.
	// CPU residency state remains unchanged until the full page-table batch is visible.
	if ( !publish_upload_batch_page_table_hot( rResidency ) ) {
		for ( uint32_t i = 0u; i < m_nUploadBatchCount; ++i ) {
			update_page_entry_hot( pReservations[ i ].nVirtualPage, rResidency.Buffers.pPageTable[ pReservations[ i ].nVirtualPage ] );
		}
		abort_upload_batch_hot( rResidency );
		return false;
	}
	for ( uint32_t i = 0u; i < m_nUploadBatchCount; ++i ) {
		const uint32_t nMask = rResidency.pDatabase->nRuntimeKind == T3D_VT_RUNTIME_DATABASE_PACK_V3
		                           ? ( 1u << rResidency.pDatabase->pPages[ pReservations[ i ].nVirtualPage ].nChannel )
		                           : T3D_VT_PAGE_SET_CHANNEL_MASK;
		if ( !t3d_vt_commit_upload_hot( rResidency, pReservations[ i ], nMask, pFrameId ) ) {
			++m_Stats.nHotViolationCount;
			m_nUploadBatchCount = 0u;
			m_bUploadBatchActive = false;
			return false;
		}
	}
	m_nUploadBatchCount = 0u;
	m_bUploadBatchActive = false;
	return hot_epoch_unchanged();
}

bool Terrain3DVTGpu::upload_batch_begin_callback( void *pUser )
{
	Terrain3DVTGpu *const pSelf = (Terrain3DVTGpu *)pUser;
	return pSelf != NULL && pSelf->begin_upload_batch_hot();
}

bool Terrain3DVTGpu::upload_batch_end_callback( T3DVTResidencyState& rResidency, const uint32_t pFrameId, void *pUser )
{
	Terrain3DVTGpu *const pSelf = (Terrain3DVTGpu *)pUser;
	return pSelf != NULL && pSelf->end_upload_batch_hot( rResidency, pFrameId );
}

bool Terrain3DVTGpu::upload_page_set_commit_hot( T3DVTResidencyState& rResidency,
                                                 const T3DVTUploadReservation& pReservation,
                                                 const T3DVTPageSetPayloadView& pPayloads,
                                                 uint32_t pFrameId )
{
	if ( !m_bInitialized || !m_bHotSealed || m_pRenderingDevice == NULL || !hot_epoch_unchanged() || ( pPayloads.nPresentMask & ~T3D_VT_PAGE_SET_CHANNEL_MASK ) != 0u || !t3d_vt_can_commit_upload_hot( rResidency, pReservation ) ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	uint32_t nBank = 0u, nLayer = 0u;
	if ( !map_physical_slot_hot( pReservation.nPhysicalSlot, nBank, nLayer ) ) {
		return false;
	}

	uint64_t nUploadedBytes = 0u;
	const bool bV3 = rResidency.pDatabase != NULL && rResidency.pDatabase->nRuntimeKind == T3D_VT_RUNTIME_DATABASE_PACK_V3;
	const uint32_t nFirstChannel = bV3 ? rResidency.pDatabase->pPages[ pReservation.nVirtualPage ].nChannel : 0u;
	const uint32_t nLastChannel = bV3 ? nFirstChannel + 1u : T3D_RW_VT_CHANNEL_COUNT;
	if ( bV3 && pPayloads.nPresentMask != ( 1u << nFirstChannel ) ) {
		return false;
	}
	for ( uint32_t c = nFirstChannel; c < nLastChannel; ++c ) {
		const uint32_t nResourceChannel = bV3 ? t3d_vt_physical_family_legacy_resource_channel( (t3dVTPhysicalFormatFamily_t)c ) : c;
		if ( nResourceChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
			return false;
		}
		const uint32_t nExpected = physical_layer_bytes( nResourceChannel );
		const bool bPresent = ( pPayloads.nPresentMask & ( 1u << c ) ) != 0u;
		const uint8_t *const pBytes = bPresent ? pPayloads.pChannelBytes[ c ] : m_NeutralPhysicalUpload[ nResourceChannel ].ptr();
		const uint32_t nBytes = bPresent ? pPayloads.nChannelBytes[ c ] : (uint32_t)m_NeutralPhysicalUpload[ nResourceChannel ].size();
		if ( pBytes == NULL || nExpected == 0u || nBytes != nExpected || m_PhysicalUpload[ nResourceChannel ].size() != (int64_t)nExpected ) {
			return false;
		}
		memcpy( m_PhysicalUpload[ nResourceChannel ].ptrw(), pBytes, nExpected );
		if ( m_pRenderingDevice->texture_update( m_pResources->get_vt_bank_rid( (T3DVtChannel)nResourceChannel, nBank ),
		                                         nLayer, m_PhysicalUpload[ nResourceChannel ] ) != OK ) {
			return false;
		}
		nUploadedBytes += nExpected;
	}
	// All four backing layers become visible before any logical mapping can expose the page set.

	// Godot 4.7.2 automatically orders backing texture transfers before later shader reads.
	// Victim publication remains valid until the delayed retirement service invalidates it.

	if ( m_bUploadBatchActive ) {
		const uint32_t nCapacity = (uint32_t)( m_UploadBatchReservations.size() / sizeof( T3DVTUploadReservation ) );
		if ( m_nUploadBatchCount >= nCapacity ) {
			return false;
		}
		T3DVTUploadReservation *const pReservations = (T3DVTUploadReservation *)m_UploadBatchReservations.ptrw();
		pReservations[ m_nUploadBatchCount++ ] = pReservation;
		m_Stats.nUploadBytes += nUploadedBytes;
		return hot_epoch_unchanged();
	}

	const T3DVTPageTableEntry PreviousEntry = rResidency.Buffers.pPageTable[ pReservation.nVirtualPage ];
	T3DVTPageTableEntry Preview = PreviousEntry;
	Preview.nPhysicalSlot = pReservation.nPhysicalSlot;
	Preview.nResolvedMip = pReservation.nWantedMip;
	Preview.nFlags |= T3D_VT_PAGE_RESIDENT;
	if ( !update_page_entry_hot( pReservation.nVirtualPage, Preview ) ) {
		return false;
	}
	// The page-table buffer update is automatically synchronized before later RD consumers.
	const uint32_t nCommitMask = bV3 ? pPayloads.nPresentMask : T3D_VT_PAGE_SET_CHANNEL_MASK;
	if ( !t3d_vt_commit_upload_hot( rResidency, pReservation, nCommitMask, pFrameId ) ) {
		update_page_entry_hot( pReservation.nVirtualPage, PreviousEntry );
		return false;
	}
	m_Stats.nUploadBytes += nUploadedBytes;
	return hot_epoch_unchanged();
}

bool Terrain3DVTGpu::upload_commit_hot( T3DVTResidencyState& rResidency,
                                        const T3DVTUploadReservation& pReservation,
                                        const uint8_t *pDecodedBytes,
                                        uint32_t pDecodedByteCount,
                                        uint32_t pFrameId )
{
	if ( pDecodedBytes == NULL || rResidency.pDatabase == NULL || rResidency.pDatabase->pHeader == NULL || pReservation.nVirtualPage >= rResidency.pDatabase->pHeader->nPageCount ) {
		return false;
	}
	const T3DVTPageFileRecord& Page = rResidency.pDatabase->pPages[ pReservation.nVirtualPage ];
	const uint32_t nResourceChannel = t3d_vt_physical_family_legacy_resource_channel( (t3dVTPhysicalFormatFamily_t)Page.nChannel );
	if ( Page.nChannel >= T3D_RW_VT_CHANNEL_COUNT || nResourceChannel >= T3D_RW_VT_CHANNEL_COUNT || pDecodedByteCount != physical_layer_bytes( nResourceChannel ) ) {
		return false;
	}
	T3DVTPageSetPayloadView Payloads = {};
	Payloads.pChannelBytes[ Page.nChannel ] = pDecodedBytes;
	Payloads.nChannelBytes[ Page.nChannel ] = pDecodedByteCount;
	Payloads.nPresentMask = 1u << Page.nChannel;
	return upload_page_set_commit_hot( rResidency, pReservation, Payloads, pFrameId );
}

bool Terrain3DVTGpu::upload_commit_callback( T3DVTResidencyState& rResidency,
                                             const T3DVTUploadReservation& pReservation,
                                             const uint8_t *pDecodedBytes,
                                             uint32_t pDecodedByteCount,
                                             uint32_t pFrameId,
                                             void *pUser )
{
	Terrain3DVTGpu *pSelf = (Terrain3DVTGpu *)pUser;
	return pSelf != NULL && pSelf->upload_commit_hot( rResidency, pReservation, pDecodedBytes, pDecodedByteCount, pFrameId );
}

bool Terrain3DVTGpu::is_readback_enabled() const
{
	return false;
}

bool Terrain3DVTGpu::is_initialized() const
{
	return m_bInitialized;
}
bool Terrain3DVTGpu::is_storage_ready() const
{
	return m_bStorageReady;
}
bool Terrain3DVTGpu::is_executable_ready() const
{
	return m_bExecutableReady;
}
uint64_t Terrain3DVTGpu::get_reserved_gpu_bytes() const
{
	uint64_t nBytes = 0u;
	for ( uint32_t iChannel = 0u; iChannel < T3D_RW_VT_CHANNEL_COUNT; ++iChannel ) {
		nBytes += m_Plan.nVtPhysicalBytes[ iChannel ] + m_Plan.nVtTailBytes[ iChannel ];
	}
	static const T3DVtGpuBufferId s_vtBuffers[] = {
		T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE,
		T3D_RW_GPU_BUFFER_VT_PAGE_TABLE,
		T3D_RW_GPU_BUFFER_VT_MIP_LAYOUT_TABLE,
		T3D_RW_GPU_BUFFER_VT_TAIL_METADATA,
	};
	for ( size_t iBuffer = 0u; iBuffer < sizeof( s_vtBuffers ) / sizeof( s_vtBuffers[ 0 ] ); ++iBuffer ) {
		nBytes += m_Plan.nBufferBytes[ s_vtBuffers[ iBuffer ] ];
	}
	return nBytes;
}

const T3DVTGpuStats& Terrain3DVTGpu::get_stats() const
{
	return m_Stats;
}

bool Terrain3DVTGpu::make_gpu_page_entry( const uint32_t nVirtualPage, const T3DVTPageTableEntry& pCpuEntry, T3DVTPageTableEntry& rGpuEntry ) const
{
	rGpuEntry = pCpuEntry;
	if ( ( pCpuEntry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u || pCpuEntry.nPhysicalSlot == T3D_RW_VT_INVALID_SLOT ) {
		rGpuEntry.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
		return true;
	}
	uint32_t nBank = 0u, nLayer = 0u;
	if ( !map_physical_slot_hot( pCpuEntry.nPhysicalSlot, nBank, nLayer ) ) {
		return false;
	}
	if ( m_pDatabaseView != NULL && m_pDatabaseView->nRuntimeKind == T3D_VT_RUNTIME_DATABASE_PACK_V3 ) {
		if ( m_pDatabaseView->pPages == NULL || nVirtualPage >= m_pDatabaseView->pHeader->nPageCount ) {
			return false;
		}
		const uint32_t nFamily = m_pDatabaseView->pPages[ nVirtualPage ].nChannel;
		if ( nFamily >= T3D_VT_PHYSICAL_FORMAT_COUNT ) {
			return false;
		}
		rGpuEntry.nPhysicalSlot = t3d_vt_pack_physical_address_v3( (t3dVTPhysicalFormatFamily_t)nFamily, nLayer );
		return rGpuEntry.nPhysicalSlot != T3D_VT_INVALID_SLOT;
	}
	if ( nBank > 0xffffu || nLayer > 0xffffu ) {
		return false;
	}
	rGpuEntry.nPhysicalSlot = ( nBank << 16u ) | nLayer;
	return true;
}

bool Terrain3DVTGpu::update_page_entry_hot( uint32_t pVirtualPage, const T3DVTPageTableEntry& pEntry )
{
	const uint64_t nOffset = (uint64_t)pVirtualPage * sizeof( T3DVTPageTableEntry );
	if ( nOffset + sizeof( T3DVTPageTableEntry ) > m_pResources->get_plan().nBufferBytes[ T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ] || nOffset > UINT32_MAX || m_PageEntryUpload.size() < (int64_t)sizeof( T3DVTPageTableEntry ) ) {
		return false;
	}
	T3DVTPageTableEntry GpuEntry = {};
	if ( !make_gpu_page_entry( pVirtualPage, pEntry, GpuEntry ) ) {
		return false;
	}
	memcpy( m_PageEntryUpload.ptrw(), &GpuEntry, sizeof( GpuEntry ) );
	if ( !t3dVtUpload( m_pResources, m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ),
	                   (uint32_t)nOffset, sizeof( T3DVTPageTableEntry ), m_PageEntryUpload ) ) {
		return false;
	}
	m_Stats.nPageTableUpdateBytes += sizeof( T3DVTPageTableEntry );
	++m_Stats.nPageTableUpdateCalls;
	return true;
}

bool Terrain3DVTGpu::retire_victim_gpu_hot(
    T3DVTResidencyState& rResidency, uint32_t pPhysicalSlot, uint32_t pVictimPage, uint32_t pFrameId )
{
	if ( pPhysicalSlot >= rResidency.Buffers.nPhysicalPageSetCapacity || pVictimPage >= rResidency.pDatabase->pHeader->nPageCount ) {
		return false;
	}
	const t3dVTPhysicalSlot_t& Slot = rResidency.Buffers.pPhysicalPageSets[ pPhysicalSlot ];
	const T3DVTPageTableEntry CpuEntry = rResidency.Buffers.pPageTable[ pVictimPage ];
	if ( Slot.m_nVirtualPage != pVictimPage || ( Slot.m_nFlags & T3D_VT_SLOT_RETIRED ) == 0u || ( CpuEntry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u || CpuEntry.nPhysicalSlot != pPhysicalSlot ) {
		return false;
	}
	T3DVTPageTableEntry Invalid = CpuEntry;
	Invalid.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
	Invalid.nFlags &= (uint16_t)~T3D_VT_PAGE_RESIDENT;
	if ( !update_page_entry_hot( pVictimPage, Invalid ) ) {
		return false;
	}
	// Godot 4.7.2 automatically synchronizes the invalidation buffer update for later GPU consumers.
	return t3d_vt_finalize_eviction_hot( rResidency, pPhysicalSlot, pVictimPage, pFrameId );
}
bool Terrain3DVTGpu::get_physical_family_binding_v3(
    const uint32_t pFamily, RID& rRid, uint32_t& rLayerCount ) const
{
	rRid = RID();
	rLayerCount = 0u;
	if ( m_pResources == NULL || pFamily >= T3D_VT_PHYSICAL_FORMAT_COUNT ) {
		return false;
	}
	const uint32_t nResourceChannel = t3d_vt_physical_family_legacy_resource_channel( (t3dVTPhysicalFormatFamily_t)pFamily );
	if ( nResourceChannel >= T3D_RW_VT_CHANNEL_COUNT || m_pResources->get_vt_bank_count( (T3DVtChannel)nResourceChannel ) != 1u ) {
		return false;
	}
	rRid = m_pResources->get_vt_bank_rid( (T3DVtChannel)nResourceChannel, 0u );
	rLayerCount = m_pResources->get_vt_bank_layer_count( (T3DVtChannel)nResourceChannel, 0u );
	return rRid.is_valid() && rLayerCount > 0u;
}

bool Terrain3DVTGpu::validate_physical_format_families_v3_cold() const
{
	if ( m_pResources == NULL || m_Limits.nMaxVtPhysicalPageSets == 0u ) {
		return false;
	}
	for ( uint32_t nFamily = 0u; nFamily < T3D_VT_PHYSICAL_FORMAT_COUNT; ++nFamily ) {
		RID hBank;
		uint32_t nLayers = 0u;
		if ( !get_physical_family_binding_v3( nFamily, hBank, nLayers ) || nLayers != m_Limits.nMaxVtPhysicalPageSets ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DVTGpu::map_physical_slot_hot( uint32_t pPhysicalSlot, uint32_t& rBank, uint32_t& rLayer ) const
{
	if ( m_pResources == NULL || pPhysicalSlot >= m_Limits.nMaxVtPhysicalPageSets || m_pResources->get_vt_bank_count( T3D_RW_VT_ALBEDO ) != 1u || pPhysicalSlot >= m_pResources->get_vt_bank_layer_count( T3D_RW_VT_ALBEDO, 0u ) ) {
		return false;
	}
	rBank = 0u;
	rLayer = pPhysicalSlot;
	return true;
}

bool Terrain3DVTGpu::validate_physical_bank_layout_cold() const
{
	if ( m_pResources == NULL || m_Limits.nMaxVtPhysicalPageSets == 0u ) {
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		const T3DVtChannel eChannel = (T3DVtChannel)c;
		if ( m_pResources->get_vt_bank_count( eChannel ) != 1u || m_pResources->get_vt_bank_layer_count( eChannel, 0u ) != m_Limits.nMaxVtPhysicalPageSets ) {
			return false;
		}
	}
	return validate_physical_format_families_v3_cold();
}

static void t3d_vt_set_bit_cold( uint8_t *pBytes, uint32_t& rBit, uint32_t pValue )
{
	if ( pValue != 0u ) {
		pBytes[ rBit >> 3u ] |= (uint8_t)( 1u << ( rBit & 7u ) );
	}
	++rBit;
}

static void t3d_vt_write_bits_cold( uint8_t *pBytes, uint32_t& rBit, uint32_t pValue, uint32_t pCount )
{
	for ( uint32_t i = 0u; i < pCount; ++i ) {
		t3d_vt_set_bit_cold( pBytes, rBit, ( pValue >> i ) & 1u );
	}
}

bool Terrain3DVTGpu::initialize_neutral_payloads_cold()
{
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		const uint32_t nBytes = physical_layer_bytes( c );
		const uint64_t nTailBytes = m_Plan.nVtTailBytes[ c ];
		m_NeutralPhysicalUpload[ c ].resize( nBytes );
		if ( nBytes == 0u || m_NeutralPhysicalUpload[ c ].size() != (int64_t)nBytes || nTailBytes == 0u || nTailBytes > INT64_MAX ) {
			return false;
		}
		m_NeutralTailUpload[ c ].resize( (int64_t)nTailBytes );
		if ( m_NeutralTailUpload[ c ].size() != (int64_t)nTailBytes ) {
			return false;
		}
		memset( m_NeutralPhysicalUpload[ c ].ptrw(), 0, nBytes );
		memset( m_NeutralTailUpload[ c ].ptrw(), 0, (size_t)nTailBytes );
	}

	// BC1: endpoint 0 is exact white and every texel selects endpoint 0.
	uint8_t *const pAlbedo = m_NeutralPhysicalUpload[ T3D_VT_CHANNEL_ALBEDO ].ptrw();
	for ( uint32_t i = 0u; i < (uint32_t)m_NeutralPhysicalUpload[ T3D_VT_CHANNEL_ALBEDO ].size(); i += 8u ) {
		pAlbedo[ i + 0u ] = 0xffu;
		pAlbedo[ i + 1u ] = 0xffu;
	}

	// BC5: two constant BC4 blocks at 128 encode the neutral tangent-space normal.
	uint8_t *const pNormal = m_NeutralPhysicalUpload[ T3D_VT_CHANNEL_NORMAL ].ptrw();
	for ( uint32_t i = 0u; i < (uint32_t)m_NeutralPhysicalUpload[ T3D_VT_CHANNEL_NORMAL ].size(); i += 16u ) {
		pNormal[ i + 0u ] = T3D_VT_NEUTRAL_NORMAL_X8;
		pNormal[ i + 1u ] = T3D_VT_NEUTRAL_NORMAL_X8;
		pNormal[ i + 8u ] = T3D_VT_NEUTRAL_NORMAL_Y8;
		pNormal[ i + 9u ] = T3D_VT_NEUTRAL_NORMAL_Y8;
	}

	// BC7 mode 6 constant RGBA=(255,255,255,255), all indices zero.
	uint8_t bc7[ 16 ] = {};
	uint32_t nBit = 0u;
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		t3d_vt_set_bit_cold( bc7, nBit, 0u );
	}
	t3d_vt_set_bit_cold( bc7, nBit, 1u );
	for ( uint32_t i = 0u; i < 8u; ++i ) {
		t3d_vt_write_bits_cold( bc7, nBit, 0x7fu, 7u );
	}
	t3d_vt_set_bit_cold( bc7, nBit, 1u );
	t3d_vt_set_bit_cold( bc7, nBit, 1u );
	uint8_t *const pSurface = m_NeutralPhysicalUpload[ T3D_VT_CHANNEL_SURFACE ].ptrw();
	for ( uint32_t i = 0u; i < (uint32_t)m_NeutralPhysicalUpload[ T3D_VT_CHANNEL_SURFACE ].size(); i += 16u ) {
		memcpy( pSurface + i, bc7, sizeof( bc7 ) );
	}

	memset( m_NeutralPhysicalUpload[ T3D_VT_CHANNEL_HEIGHT ].ptrw(),
	        T3D_VT_NEUTRAL_HEIGHT8,
	        (size_t)m_NeutralPhysicalUpload[ T3D_VT_CHANNEL_HEIGHT ].size() );

	uint8_t *const pTailAlbedo = m_NeutralTailUpload[ T3D_VT_CHANNEL_ALBEDO ].ptrw();
	for ( int64_t i = 0; i < m_NeutralTailUpload[ T3D_VT_CHANNEL_ALBEDO ].size(); i += 8 ) {
		pTailAlbedo[ i + 0u ] = 0xffu;
		pTailAlbedo[ i + 1u ] = 0xffu;
	}
	uint8_t *const pTailNormal = m_NeutralTailUpload[ T3D_VT_CHANNEL_NORMAL ].ptrw();
	for ( int64_t i = 0; i < m_NeutralTailUpload[ T3D_VT_CHANNEL_NORMAL ].size(); i += 16 ) {
		pTailNormal[ i + 0u ] = T3D_VT_NEUTRAL_NORMAL_X8;
		pTailNormal[ i + 1u ] = T3D_VT_NEUTRAL_NORMAL_X8;
		pTailNormal[ i + 8u ] = T3D_VT_NEUTRAL_NORMAL_Y8;
		pTailNormal[ i + 9u ] = T3D_VT_NEUTRAL_NORMAL_Y8;
	}
	uint8_t *const pTailSurface = m_NeutralTailUpload[ T3D_VT_CHANNEL_SURFACE ].ptrw();
	for ( int64_t i = 0; i < m_NeutralTailUpload[ T3D_VT_CHANNEL_SURFACE ].size(); i += 16 ) {
		memcpy( pTailSurface + i, bc7, sizeof( bc7 ) );
	}
	memset( m_NeutralTailUpload[ T3D_VT_CHANNEL_HEIGHT ].ptrw(),
	        T3D_VT_NEUTRAL_HEIGHT8,
	        (size_t)m_NeutralTailUpload[ T3D_VT_CHANNEL_HEIGHT ].size() );
	return true;
}

uint32_t Terrain3DVTGpu::physical_layer_bytes( uint32_t pChannel ) const
{
	if ( pChannel >= T3D_RW_VT_CHANNEL_COUNT || m_Limits.nMaxVtPhysicalPageSets == 0u ) {
		return 0u;
	}
	const uint64_t nBytes = m_Plan.nVtPhysicalBytes[ pChannel ] / m_Limits.nMaxVtPhysicalPageSets;
	return nBytes != 0u && nBytes <= UINT32_MAX ? (uint32_t)nBytes : 0u;
}



bool Terrain3DVTGpu::hot_epoch_unchanged() const
{
	return m_Stats.nGpuCreateCount == m_nHotCreateEpoch && m_Stats.nGpuFreeCount == m_nHotFreeEpoch;
}
