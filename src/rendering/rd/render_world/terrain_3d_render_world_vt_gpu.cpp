// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_vt_gpu.h"

#include "rendering/rd/render_world/terrain_3d_render_world_rd_owner.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <limits.h>
#include <string.h>

using namespace godot;

struct T3DVTDispatchPushConstants {
	uint32_t nTier;
	uint32_t nHashCount;
	uint32_t nRequestCapacity;
	uint32_t nReserved;
};

static_assert( sizeof( T3DVTDispatchPushConstants ) == 16u, "RenderWorld VT dispatch push ABI must remain 16 bytes" );

Terrain3DRenderWorldVTGpu::Terrain3DRenderWorldVTGpu() {
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	memset( &m_Limits, 0, sizeof( m_Limits ) );
	memset( m_bReadbackInFlight, 0, sizeof( m_bReadbackInFlight ) );
	memset( m_nReadbackFrameId, 0, sizeof( m_nReadbackFrameId ) );
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	m_nHotCreateEpoch = 0u;
	m_nHotFreeEpoch = 0u;
	m_bInitialized = false;
	m_bDatabaseUploadedWarm = false;
	m_bReadbackEnabled = false;
	m_nLastReadbackScheduleFrame = T3D_RW_INVALID_ID;
	m_bHotSealed = false;
}

Terrain3DRenderWorldVTGpu::~Terrain3DRenderWorldVTGpu() {
	// Explicit cold shutdown is required by the packet ownership contract.
}

bool Terrain3DRenderWorldVTGpu::initialize_cold( const Terrain3DRenderWorldResourceImage *pResources,
        const T3DRenderWorldLimits &pLimits,
        const Callable pReadbackCallbacks[T3D_RW_GPU_FRAME_RING_COUNT],
        const t3dRdRenderWorldShaderBundle_t &rShaders ) {
	Terrain3DRdContext *const pContext = pResources ? pResources->get_rd_context() : NULL;
	RenderingDevice *const pRenderingDevice = pContext ? pContext->getDevice() : NULL;
	if ( m_bInitialized || pRenderingDevice == NULL || pResources == NULL || !pResources->is_initialized() ) {
		return false;
	}
	if ( pLimits.nMaxVtRequests == 0u || pLimits.nMaxVtRequests > UINT32_MAX / 2u || pLimits.nMaxReadbackBytesPerFrame > UINT32_MAX ) {
		return false;
	}
	const uint64_t nReadbackBytes = sizeof( T3DVTGpuCounters ) + ( uint64_t )pLimits.nMaxVtRequests * sizeof( T3DVTRequest );
	if ( nReadbackBytes > pLimits.nMaxReadbackBytesPerFrame || nReadbackBytes > UINT32_MAX ) {
		return false;
	}
	m_pRenderingDevice = pRenderingDevice;
	m_pResources = pResources;
	m_Limits = pLimits;
	m_nLastReadbackScheduleFrame = T3D_RW_INVALID_ID;
	m_bReadbackEnabled = pReadbackCallbacks != NULL;
	for ( uint32_t i = 0u; i < T3D_RW_GPU_FRAME_RING_COUNT; ++i ) {
		if ( pReadbackCallbacks != NULL && pReadbackCallbacks[i].is_valid() ) {
			m_ReadbackCallback[i] = pReadbackCallbacks[i];
		} else {
			m_ReadbackCallback[i] = Callable();
			m_bReadbackEnabled = false;
		}
	}
	static_assert( ( int )( PIPE_COUNT ) == ( int )( T3D_RD_RW_VT_COUNT ), "RenderWorld VT shader bundle mismatch" );
	if ( !create_pipeline_cold( PIPE_RESET, rShaders.m_szVT[T3D_RD_RW_VT_RESET] )
	        || !create_pipeline_cold( PIPE_PREDICT, rShaders.m_szVT[T3D_RD_RW_VT_PREDICT] )
	        || !create_pipeline_cold( PIPE_COMPACT, rShaders.m_szVT[T3D_RD_RW_VT_COMPACT] ) || !create_uniform_sets_cold() ) {
		shutdown_cold();
		return false;
	}
	const uint32_t nHashCount = pLimits.nMaxVtRequests * 2u;
	T3DVTDispatchPushConstants DispatchPush = {};
	DispatchPush.nHashCount = nHashCount;
	DispatchPush.nRequestCapacity = pLimits.nMaxVtRequests;
	m_ResetPush.resize( ( int64_t )sizeof( DispatchPush ) );
	if ( m_ResetPush.size() != ( int64_t )sizeof( DispatchPush ) ) {
		shutdown_cold();
		return false;
	}
	memcpy( m_ResetPush.ptrw(), &DispatchPush, sizeof( DispatchPush ) );
	for ( uint32_t i = 0u; i < T3D_RW_TIER_COUNT; ++i ) {
		DispatchPush.nTier = i;
		m_TierPush[i].resize( ( int64_t )sizeof( DispatchPush ) );
		if ( m_TierPush[i].size() != ( int64_t )sizeof( DispatchPush ) ) {
			shutdown_cold();
			return false;
		}
		memcpy( m_TierPush[i].ptrw(), &DispatchPush, sizeof( DispatchPush ) );
	}
	DispatchPush.nTier = 0u;
	m_CompactPush.resize( ( int64_t )sizeof( DispatchPush ) );
	if ( m_CompactPush.size() != ( int64_t )sizeof( DispatchPush ) ) {
		shutdown_cold();
		return false;
	}
	memcpy( m_CompactPush.ptrw(), &DispatchPush, sizeof( DispatchPush ) );
	const uint32_t nRetiredScratchCapacity = pLimits.nMaxVtPhysicalAlbedoPages + pLimits.nMaxVtPhysicalNormalPages
	        + pLimits.nMaxVtPhysicalSurfacePages + pLimits.nMaxVtPhysicalHeightPages;
	const uint64_t nPageScratchBytes64 = ( uint64_t )MAX( nRetiredScratchCapacity, 1u ) * sizeof( T3DVTPageTableEntry );
	if ( nPageScratchBytes64 > INT32_MAX ) {
		shutdown_cold();
		return false;
	}
	m_PageEntryUpload.resize( ( int64_t )nPageScratchBytes64 );
	m_RetiredPageScratch.resize( ( int64_t )MAX( nRetiredScratchCapacity, 1u ) );
	m_RetiredSlotScratch.resize( ( int64_t )MAX( nRetiredScratchCapacity, 1u ) );
	if ( m_PageEntryUpload.size() != ( int64_t )nPageScratchBytes64
	        || m_RetiredPageScratch.size() != ( int64_t )MAX( nRetiredScratchCapacity, 1u )
	        || m_RetiredSlotScratch.size() != ( int64_t )MAX( nRetiredScratchCapacity, 1u ) ) {
		shutdown_cold();
		return false;
	}
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		const uint32_t nBytes = physical_layer_bytes( c );
		if ( nBytes == 0u ) {
			shutdown_cold();
			return false;
		}
		m_PhysicalUpload[c].resize( nBytes );
		if ( m_PhysicalUpload[c].size() != ( int64_t )nBytes ) {
			shutdown_cold();
			return false;
		}
	}
	m_bInitialized = true;
	return true;
}

void Terrain3DRenderWorldVTGpu::shutdown_cold() {
	m_bHotSealed = false;
	if ( m_pRenderingDevice != NULL ) {
		free_rid_cold( m_hUniformCompact );
		for ( int32_t i = ( int32_t )T3D_RW_TIER_COUNT - 1; i >= 0; --i ) {
			free_rid_cold( m_hUniformPredict[i] );
		}
		free_rid_cold( m_hUniformReset );
		for ( int32_t i = ( int32_t )PIPE_COUNT - 1; i >= 0; --i ) {
			free_rid_cold( m_hPipeline[i] );
		}
		for ( int32_t i = ( int32_t )PIPE_COUNT - 1; i >= 0; --i ) {
			free_rid_cold( m_hShader[i] );
		}
	}
	m_ResetPush.resize( 0 );
	for ( uint32_t i = 0u; i < T3D_RW_TIER_COUNT; ++i ) {
		m_TierPush[i].resize( 0 );
	}
	m_CompactPush.resize( 0 );
	m_PageEntryUpload.resize( 0 );
	m_RetiredPageScratch.resize( 0 );
	m_RetiredSlotScratch.resize( 0 );
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		m_PhysicalUpload[c].resize( 0 );
	}
	memset( m_bReadbackInFlight, 0, sizeof( m_bReadbackInFlight ) );
	memset( m_nReadbackFrameId, 0, sizeof( m_nReadbackFrameId ) );
	m_pRenderingDevice = NULL;
	m_pResources = NULL;
	m_nLastReadbackScheduleFrame = T3D_RW_INVALID_ID;
	m_bDatabaseUploadedWarm = false;
	m_bReadbackEnabled = false;
	m_bInitialized = false;
}

void Terrain3DRenderWorldVTGpu::seal_hot_runtime() {
	if ( !m_bInitialized ) {
		return;
	}
	m_nHotCreateEpoch = m_Stats.nGpuCreateCount;
	m_nHotFreeEpoch = m_Stats.nGpuFreeCount;
	m_bHotSealed = true;
}

bool Terrain3DRenderWorldVTGpu::upload_database_warm( const T3DVTDatabaseView &pDatabase, const T3DVTResidencyState &pResidency ) {
	if ( !m_bInitialized || m_bHotSealed || m_bDatabaseUploadedWarm || m_pRenderingDevice == NULL || pDatabase.pHeader == NULL
	        || !pResidency.bInitialized || pResidency.bNeutralPublished ) {
		return false;
	}
	const uint64_t nAllocationBytes = ( uint64_t )pDatabase.pHeader->nAllocationCount * sizeof( T3DVTAllocationRecord );
	const uint64_t nPageBytes = ( uint64_t )pDatabase.pHeader->nPageCount * sizeof( T3DVTPageTableEntry );
	if ( nAllocationBytes > m_pResources->get_plan().nBufferBytes[T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE]
	        || nAllocationBytes > UINT32_MAX ) {
		return false;
	}
	if ( nPageBytes > m_pResources->get_plan().nBufferBytes[T3D_RW_GPU_BUFFER_VT_PAGE_TABLE] || nPageBytes > UINT32_MAX ) {
		return false;
	}
	PackedByteArray Upload;
	Upload.resize( ( int64_t )nAllocationBytes );
	if ( Upload.size() != ( int64_t )nAllocationBytes ) {
		return false;
	}
	memcpy( Upload.ptrw(), pDatabase.pAllocations, ( size_t )nAllocationBytes );
	if ( m_pRenderingDevice->buffer_update(
	             m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE ), 0u, ( uint32_t )nAllocationBytes, Upload )
	        != OK ) {
		return false;
	}
	if ( Terrain3DRdContext *const pContext = m_pResources->get_rd_context() ) {
		pContext->recordBufferUpdate( nAllocationBytes );
	}

	Upload.resize( ( int64_t )nPageBytes );
	if ( Upload.size() != ( int64_t )nPageBytes ) {
		return false;
	}
	T3DVTPageTableEntry *pGpuEntries = ( T3DVTPageTableEntry * )Upload.ptrw();
	for ( uint32_t i = 0u; i < pDatabase.pHeader->nPageCount; ++i ) {
		const uint32_t nChannel = pDatabase.pPages[i].nChannel;
		if ( !make_gpu_page_entry( nChannel, pResidency.Buffers.pPageTable[i], pGpuEntries[i] ) ) {
			return false;
		}
	}
	if ( m_pRenderingDevice->buffer_update(
	             m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ), 0u, ( uint32_t )nPageBytes, Upload )
	        != OK ) {
		return false;
	}
	if ( Terrain3DRdContext *const pContext = m_pResources->get_rd_context() ) {
		pContext->recordBufferUpdate( nPageBytes );
	}
	m_bDatabaseUploadedWarm = true;
	return true;
}

bool Terrain3DRenderWorldVTGpu::upload_neutral_backing_warm( T3DVTResidencyState &rResidency,
        uint32_t pChannel,
        uint32_t pPhysicalSlot,
        const uint8_t *pDecodedBytes,
        uint32_t pDecodedByteCount ) {
	if ( !m_bInitialized || !m_bDatabaseUploadedWarm || m_bHotSealed || m_pRenderingDevice == NULL || pDecodedBytes == NULL
	        || pChannel >= T3D_RW_VT_CHANNEL_COUNT || pPhysicalSlot >= rResidency.Buffers.nPhysicalSlotCapacity[pChannel]
	        || rResidency.bNeutralPublished ) {
		return false;
	}
	const uint32_t nRootPage = rResidency.nNeutralRootPage[pChannel];
	if ( nRootPage == T3D_RW_VT_INVALID_PAGE || nRootPage >= rResidency.pDatabase->pHeader->nPageCount ) {
		return false;
	}
	const T3DVTPageTableEntry &CpuEntry = rResidency.Buffers.pPageTable[nRootPage];
	const T3DVTPhysicalSlot &CpuSlot = rResidency.Buffers.pPhysicalSlots[pChannel][pPhysicalSlot];
	if ( ( CpuEntry.nFlags & T3D_VT_PAGE_RESIDENT ) != 0u || CpuSlot.nVirtualPage != T3D_RW_VT_INVALID_PAGE ) {
		return false;
	}
	const uint32_t nExpected = physical_layer_bytes( pChannel );
	if ( pDecodedByteCount != nExpected || m_PhysicalUpload[pChannel].size() != ( int64_t )nExpected ) {
		return false;
	}
	uint32_t nBank = 0u, nLayer = 0u;
	if ( !map_physical_slot_hot( pChannel, pPhysicalSlot, nBank, nLayer ) ) {
		return false;
	}
	memcpy( m_PhysicalUpload[pChannel].ptrw(), pDecodedBytes, nExpected );
	if ( m_pRenderingDevice->texture_update(
	             m_pResources->get_vt_bank_rid( ( T3DRenderWorldVTChannel )pChannel, nBank ), nLayer, m_PhysicalUpload[pChannel] )
	        != OK ) {
		return false;
	}

	T3DVTPageTableEntry Preview = CpuEntry;
	Preview.nPhysicalSlot = pPhysicalSlot;
	Preview.nFlags |= T3D_VT_PAGE_RESIDENT | T3D_VT_PAGE_PINNED | T3D_VT_PAGE_NEUTRAL | T3D_VT_PAGE_ROOT;
	if ( !update_page_entry_hot( nRootPage, pChannel, Preview ) ) {
		return false;
	}
	if ( !t3d_render_world_vt_commit_neutral_backing_warm( rResidency, pChannel, pPhysicalSlot ) ) {
		update_page_entry_hot( nRootPage, pChannel, CpuEntry );
		return false;
	}
	m_Stats.nUploadBytes += nExpected;
	return true;
}

bool Terrain3DRenderWorldVTGpu::dispatch_prediction_hot( const T3DRenderWorldLimits &pLimits ) {
	if ( !m_bInitialized || !m_bHotSealed || m_pRenderingDevice == NULL || m_pResources == NULL || !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	const uint32_t nHashEntries = pLimits.nMaxVtRequests * 2u;
	const uint32_t nHashGroups = ( nHashEntries + 63u ) / 64u;
	const RID &hIndirect = m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_INDIRECT_ARGS );
	if ( nHashGroups == 0u || !hIndirect.is_valid() ) {
		return false;
	}
	static const uint32_t s_szTierIndirectOffset[T3D_RW_TIER_COUNT] = {
		T3D_RW_INDIRECT_VT_WORLD_OFFSET, T3D_RW_INDIRECT_VT_CLUTTER_OFFSET, T3D_RW_INDIRECT_VT_FOLIAGE_OFFSET
	};
	const int64_t nList = m_pRenderingDevice->compute_list_begin();
	m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPE_RESET] );
	m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformReset, 0u );
	m_pRenderingDevice->compute_list_set_push_constant( nList, m_ResetPush, ( uint32_t )m_ResetPush.size() );
	m_pRenderingDevice->compute_list_dispatch( nList, nHashGroups, 1u, 1u );
	++m_Stats.nDispatchCount;
	m_pRenderingDevice->compute_list_add_barrier( nList );
	++m_Stats.nBarrierCount;
	for ( uint32_t tier = 0u; tier < T3D_RW_TIER_COUNT; ++tier ) {
		m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPE_PREDICT] );
		m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformPredict[tier], 0u );
		m_pRenderingDevice->compute_list_set_push_constant( nList, m_TierPush[tier], ( uint32_t )m_TierPush[tier].size() );
		m_pRenderingDevice->compute_list_dispatch_indirect( nList, hIndirect, s_szTierIndirectOffset[tier] );
		++m_Stats.nDispatchCount;
		++m_Stats.nVariableTierDispatchCount;
		m_pRenderingDevice->compute_list_add_barrier( nList );
		++m_Stats.nBarrierCount;
	}
	// Hash compaction still addresses the fixed hash domain. It is intentionally
	// tracked separately from the live/indirect prediction tiers.
	m_pRenderingDevice->compute_list_bind_compute_pipeline( nList, m_hPipeline[PIPE_COMPACT] );
	m_pRenderingDevice->compute_list_bind_uniform_set( nList, m_hUniformCompact, 0u );
	m_pRenderingDevice->compute_list_set_push_constant( nList, m_CompactPush, ( uint32_t )m_CompactPush.size() );
	m_pRenderingDevice->compute_list_dispatch( nList, nHashGroups, 1u, 1u );
	++m_Stats.nDispatchCount;
	m_pRenderingDevice->compute_list_add_barrier( nList );
	++m_Stats.nBarrierCount;
	m_pRenderingDevice->compute_list_end();
	return hot_epoch_unchanged();
}

bool Terrain3DRenderWorldVTGpu::schedule_request_readback_hot( uint32_t pFrameId ) {
	if ( !m_bInitialized || !m_bHotSealed || m_pRenderingDevice == NULL || !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	if ( !m_bReadbackEnabled ) {
		return false;
	}
	// Readback service is intentionally decoupled from raster FPS. At 240-300 Hz
	// this bounds D2H callback/copy pressure without changing the GPU request rate.
	static const uint32_t s_nReadbackFrameCadence = 4u;
	if ( m_nLastReadbackScheduleFrame != T3D_RW_INVALID_ID && pFrameId - m_nLastReadbackScheduleFrame < s_nReadbackFrameCadence ) {
		++m_Stats.nReadbackCadenceSkipCount;
		return true;
	}
	const uint32_t nSlot = pFrameId % T3D_RW_GPU_FRAME_RING_COUNT;
	if ( m_bReadbackInFlight[nSlot] ) {
		++m_Stats.nReadbackRingBusyCount;
		return false;
	}
	const uint32_t nSlotBytes = ( uint32_t )m_Limits.nMaxReadbackBytesPerFrame;
	const uint32_t nBase = nSlot * nSlotBytes;
	const uint32_t nRequestBytes = m_Limits.nMaxVtRequests * ( uint32_t )sizeof( T3DVTRequest );
	const uint32_t nTotal = ( uint32_t )sizeof( T3DVTGpuCounters ) + nRequestBytes;
	const RID &hReadback = m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_READBACK_RING );
	if ( m_pRenderingDevice->buffer_copy( m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_COUNTERS ),
	             hReadback,
	             sizeof( T3DVisibilityCounters ),
	             nBase,
	             sizeof( T3DVTGpuCounters ) )
	        != OK ) {
		return false;
	}
	if ( m_pRenderingDevice->buffer_copy( m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_REQUEST_LIST ),
	             hReadback,
	             0u,
	             nBase + sizeof( T3DVTGpuCounters ),
	             nRequestBytes )
	        != OK ) {
		return false;
	}
	if ( m_pRenderingDevice->buffer_get_data_async( hReadback, m_ReadbackCallback[nSlot], nBase, nTotal ) != OK ) {
		return false;
	}
	m_bReadbackInFlight[nSlot] = true;
	m_nReadbackFrameId[nSlot] = pFrameId;
	m_nLastReadbackScheduleFrame = pFrameId;
	++m_Stats.nAsyncReadbackCount;
	m_Stats.nReadbackBytes += nTotal;
	return true;
}

bool Terrain3DRenderWorldVTGpu::consume_request_readback_owner_hot( uint32_t pSlot,
        uint32_t pFrameId,
        const uint8_t *pReadbackBytes,
        uint32_t pReadbackByteCount,
        T3DVTStreamingState &rStreaming,
        uint32_t &rAcceptedRequestCount ) {
	rAcceptedRequestCount = 0u;
	if ( !m_bInitialized || !m_bHotSealed || pSlot >= T3D_RW_GPU_FRAME_RING_COUNT || !m_bReadbackInFlight[pSlot]
	        || m_nReadbackFrameId[pSlot] != pFrameId || pReadbackBytes == NULL || pReadbackByteCount == 0u ) {
		++m_Stats.nReadbackRejectCount;
		return false;
	}
	m_bReadbackInFlight[pSlot] = false;
	m_nReadbackFrameId[pSlot] = 0u;
	T3DVTGpuCounters Counters = {};
	rAcceptedRequestCount = t3d_render_world_vt_streaming_ingest_readback_owner_hot(
	        rStreaming, pReadbackBytes, pReadbackByteCount, m_Limits.nMaxVtRequests, pFrameId, Counters );
	++m_Stats.nReadbackConsumeCount;
	m_Stats.nAcceptedRequestCount += rAcceptedRequestCount;
	return true;
}

bool Terrain3DRenderWorldVTGpu::service_retired_slots_hot( T3DVTResidencyState &rResidency ) {
	if ( !m_bInitialized || !m_bHotSealed || !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	bool bAllRetiredPublished = true;
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNEL_COUNT; ++c ) {
		const uint32_t nCount = rResidency.nRetiredSlotCount[c];
		if ( nCount == 0u ) {
			continue;
		}
		if ( nCount > ( uint32_t )m_RetiredPageScratch.size() || nCount > ( uint32_t )m_RetiredSlotScratch.size() ) {
			return false;
		}
		int32_t *const pPages = m_RetiredPageScratch.ptrw();
		int32_t *const pSlots = m_RetiredSlotScratch.ptrw();
		for ( uint32_t i = 0u; i < nCount; ++i ) {
			const uint32_t nSlot = rResidency.Buffers.pRetiredSlots[c][i];
			if ( nSlot >= rResidency.Buffers.nPhysicalSlotCapacity[c] ) {
				return false;
			}
			const T3DVTPhysicalSlot &slot = rResidency.Buffers.pPhysicalSlots[c][nSlot];
			if ( slot.nVirtualPage >= rResidency.pDatabase->pHeader->nPageCount ) {
				return false;
			}
			uint32_t insert = i;
			while ( insert > 0u && ( uint32_t )pPages[insert - 1u] > slot.nVirtualPage ) {
				pPages[insert] = pPages[insert - 1u];
				pSlots[insert] = pSlots[insert - 1u];
				--insert;
			}
			pPages[insert] = ( int32_t )slot.nVirtualPage;
			pSlots[insert] = ( int32_t )nSlot;
		}

		uint32_t first = 0u;
		while ( first < nCount ) {
			uint32_t last = first + 1u;
			while ( last < nCount && ( uint32_t )pPages[last] == ( uint32_t )pPages[last - 1u] + 1u ) {
				++last;
			}
			const uint32_t nRunCount = last - first;
			const uint32_t nRunBytes = nRunCount * sizeof( T3DVTPageTableEntry );
			if ( nRunBytes > ( uint32_t )m_PageEntryUpload.size() ) {
				return false;
			}
			for ( uint32_t i = 0u; i < nRunCount; ++i ) {
				const uint32_t nPage = ( uint32_t )pPages[first + i];
				const uint32_t nSlot = ( uint32_t )pSlots[first + i];
				const T3DVTPhysicalSlot &slot = rResidency.Buffers.pPhysicalSlots[c][nSlot];
				const T3DVTPageTableEntry &cpuEntry = rResidency.Buffers.pPageTable[nPage];
				if ( ( slot.nFlags & T3D_VT_SLOT_RETIRED ) == 0u || slot.nVirtualPage != nPage
				        || ( cpuEntry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u || cpuEntry.nPhysicalSlot != nSlot ) {
					return false;
				}
				T3DVTPageTableEntry invalid = cpuEntry;
				invalid.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
				invalid.nFlags &= ( uint16_t )~T3D_VT_PAGE_RESIDENT;
				memcpy( m_PageEntryUpload.ptrw() + i * sizeof( T3DVTPageTableEntry ), &invalid, sizeof( invalid ) );
			}
			const uint64_t nOffset64 = ( uint64_t )( ( uint32_t )pPages[first] ) * sizeof( T3DVTPageTableEntry );
			if ( nOffset64 > UINT32_MAX
			        || m_pRenderingDevice->buffer_update( m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ),
			                   ( uint32_t )nOffset64,
			                   nRunBytes,
			                   m_PageEntryUpload )
			                != OK ) {
				++m_Stats.nRetireRetryCount;
				++rResidency.Stats.nRetireDeferredCount;
				bAllRetiredPublished = false;
				first = last;
				continue;
			}
			if ( Terrain3DRdContext *const pContext = m_pResources->get_rd_context() ) {
				pContext->recordBufferUpdate( nRunBytes );
			}
			m_Stats.nPageTableUpdateBytes += nRunBytes;
			++m_Stats.nPageTableUpdateCalls;
			for ( uint32_t i = first; i < last; ++i ) {
				const uint32_t nPage = ( uint32_t )pPages[i];
				const uint32_t nSlot = ( uint32_t )pSlots[i];
				if ( !t3d_render_world_vt_finalize_eviction_hot( rResidency, c, nSlot, nPage ) ) {
					return false;
				}
			}
			first = last;
		}
	}
	return bAllRetiredPublished;
}

bool Terrain3DRenderWorldVTGpu::upload_commit_hot( T3DVTResidencyState &rResidency,
        const T3DVTUploadReservation &pReservation,
        const uint8_t *pDecodedBytes,
        uint32_t pDecodedByteCount,
        uint32_t pFrameId ) {
	if ( !m_bInitialized || !m_bHotSealed || m_pRenderingDevice == NULL || pDecodedBytes == NULL
	        || pReservation.nChannel >= T3D_RW_VT_CHANNEL_COUNT || !hot_epoch_unchanged() ) {
		++m_Stats.nHotViolationCount;
		return false;
	}
	const uint32_t nExpected = physical_layer_bytes( pReservation.nChannel );
	if ( pDecodedByteCount != nExpected || m_PhysicalUpload[pReservation.nChannel].size() != ( int64_t )nExpected
	        || !t3d_render_world_vt_can_commit_upload_hot( rResidency, pReservation ) ) {
		return false;
	}
	uint32_t nBank = 0u, nLayer = 0u;
	if ( !map_physical_slot_hot( pReservation.nChannel, pReservation.nPhysicalSlot, nBank, nLayer ) ) {
		return false;
	}
	memcpy( m_PhysicalUpload[pReservation.nChannel].ptrw(), pDecodedBytes, nExpected );
	if ( m_pRenderingDevice->texture_update( m_pResources->get_vt_bank_rid( ( T3DRenderWorldVTChannel )pReservation.nChannel, nBank ),
	             nLayer,
	             m_PhysicalUpload[pReservation.nChannel] )
	        != OK ) {
		return false;
	}

	// Publish the new GPU mapping first. CPU publication happens only after the
	// backing and its page-table entry are both accepted by RenderingDevice.
	const T3DVTPageTableEntry PreviousEntry = rResidency.Buffers.pPageTable[pReservation.nVirtualPage];
	T3DVTPageTableEntry Preview = PreviousEntry;
	Preview.nPhysicalSlot = pReservation.nPhysicalSlot;
	Preview.nResolvedMip = pReservation.nWantedMip;
	Preview.nFlags |= T3D_VT_PAGE_RESIDENT;
	if ( !update_page_entry_hot( pReservation.nVirtualPage, pReservation.nChannel, Preview ) ) {
		return false;
	}
	if ( !t3d_render_world_vt_commit_upload_hot( rResidency, pReservation, pFrameId ) ) {
		// This path should be unreachable under the single-writer contract because
		// the reservation was prevalidated immediately above. Restore the prior GPU
		// entry defensively if a future edit violates that assumption.
		update_page_entry_hot( pReservation.nVirtualPage, pReservation.nChannel, PreviousEntry );
		return false;
	}

	// Victim retirement is a second coherent publication. If its GPU invalidation
	// cannot be issued now, the old mapping and backing remain resident and the
	// slot stays RETIRED/non-reusable until service_retired_slots_hot() succeeds.
	if ( pReservation.nVictimPage != T3D_RW_VT_INVALID_PAGE ) {
		const T3DVTPageTableEntry &Victim = rResidency.Buffers.pPageTable[pReservation.nVictimPage];
		if ( Victim.nPhysicalSlot >= rResidency.Buffers.nPhysicalSlotCapacity[pReservation.nChannel] ) {
			return false;
		}
		if ( !retire_victim_gpu_hot( rResidency, pReservation.nChannel, Victim.nPhysicalSlot, pReservation.nVictimPage ) ) {
			++m_Stats.nRetireRetryCount;
			++rResidency.Stats.nRetireDeferredCount;
		}
	}
	m_Stats.nUploadBytes += nExpected;
	return hot_epoch_unchanged();
}

bool Terrain3DRenderWorldVTGpu::upload_commit_callback( T3DVTResidencyState &rResidency,
        const T3DVTUploadReservation &pReservation,
        const uint8_t *pDecodedBytes,
        uint32_t pDecodedByteCount,
        uint32_t pFrameId,
        void *pUser ) {
	Terrain3DRenderWorldVTGpu *pSelf = ( Terrain3DRenderWorldVTGpu * )pUser;
	return pSelf != NULL && pSelf->upload_commit_hot( rResidency, pReservation, pDecodedBytes, pDecodedByteCount, pFrameId );
}

bool Terrain3DRenderWorldVTGpu::is_readback_enabled() const {
	return m_bInitialized && m_bReadbackEnabled;
}

bool Terrain3DRenderWorldVTGpu::is_initialized() const {
	return m_bInitialized;
}
const T3DVTGpuStats &Terrain3DRenderWorldVTGpu::get_stats() const {
	return m_Stats;
}

bool Terrain3DRenderWorldVTGpu::create_pipeline_cold( PipelineId pId, const Ref<RDShaderSPIRV> &rSpirv ) {
	if ( m_pRenderingDevice == NULL || rSpirv.is_null() || ( uint32_t )pId >= PIPE_COUNT ) {
		return false;
	}
	m_hShader[pId] = m_pRenderingDevice->shader_create_from_spirv( rSpirv );
	if ( !m_hShader[pId].is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hShader[pId],
	             String( "RenderWorld VT shader " ) + String::num_uint64( ( uint64_t )pId ),
	             Terrain3DRdContext::RESOURCE_SHADER,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_VT ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hShader[pId] );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	m_hPipeline[pId] = m_pRenderingDevice->compute_pipeline_create( m_hShader[pId] );
	if ( !m_hPipeline[pId].is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             m_hPipeline[pId],
	             String( "RenderWorld VT pipeline " ) + String::num_uint64( ( uint64_t )pId ),
	             Terrain3DRdContext::RESOURCE_COMPUTE_PIPELINE,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_VT ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, m_hPipeline[pId] );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	return true;
}

bool Terrain3DRenderWorldVTGpu::create_buffer_uniform_set_cold(
        PipelineId pId, const T3DRenderWorldGpuBufferId *pBuffers, uint32_t pCount, RID &rSet ) {
	TypedArray<Ref<RDUniform>> Uniforms;
	for ( uint32_t i = 0u; i < pCount; ++i ) {
		Ref<RDUniform> U;
		U.instantiate();
		if ( U.is_null() ) {
			return false;
		}
		U->set_uniform_type( RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER );
		U->set_binding( ( int32_t )i );
		U->add_id( m_pResources->get_buffer_rid( pBuffers[i] ) );
		Uniforms.push_back( U );
	}
	rSet = m_pRenderingDevice->uniform_set_create( Uniforms, m_hShader[pId], 0u );
	if ( !rSet.is_valid() ) {
		return false;
	}
	if ( !t3dRenderWorldRegisterRid( m_pResources->get_rd_context(),
	             rSet,
	             String( "RenderWorld VT uniform set " ) + String::num_uint64( ( uint64_t )pId ),
	             Terrain3DRdContext::RESOURCE_UNIFORM_SET,
	             Terrain3DRdContext::OWNER_RENDER_WORLD_VT ) ) {
		t3dRenderWorldDiscardUnregisteredRid( m_pRenderingDevice, rSet );
		return false;
	}
	++m_Stats.nGpuCreateCount;
	return true;
}

bool Terrain3DRenderWorldVTGpu::create_predict_uniform_set_cold( T3DRenderWorldGpuBufferId pVisibleBuffer, RID &rSet ) {
	const T3DRenderWorldGpuBufferId a[] = { T3D_RW_GPU_BUFFER_VISIBILITY_FRAME,
		pVisibleBuffer,
		T3D_RW_GPU_BUFFER_CLUSTER_RECORDS,
		T3D_RW_GPU_BUFFER_INSTANCE_RECORDS,
		T3D_RW_GPU_BUFFER_MATERIAL_RECORDS,
		T3D_RW_GPU_BUFFER_VT_ALLOCATION_TABLE,
		T3D_RW_GPU_BUFFER_VT_PAGE_TABLE,
		T3D_RW_GPU_BUFFER_VT_REQUEST_HASH,
		T3D_RW_GPU_BUFFER_COUNTERS };
	return create_buffer_uniform_set_cold( PIPE_PREDICT, a, sizeof( a ) / sizeof( a[0] ), rSet );
}

bool Terrain3DRenderWorldVTGpu::create_uniform_sets_cold() {
	const T3DRenderWorldGpuBufferId aReset[] = { T3D_RW_GPU_BUFFER_COUNTERS, T3D_RW_GPU_BUFFER_VT_REQUEST_HASH };
	const T3DRenderWorldGpuBufferId aCompact[] = {
		T3D_RW_GPU_BUFFER_VT_REQUEST_HASH, T3D_RW_GPU_BUFFER_VT_REQUEST_LIST, T3D_RW_GPU_BUFFER_COUNTERS
	};
	if ( !create_buffer_uniform_set_cold( PIPE_RESET, aReset, sizeof( aReset ) / sizeof( aReset[0] ), m_hUniformReset ) ) {
		return false;
	}
	const T3DRenderWorldGpuBufferId aVisible[T3D_RW_TIER_COUNT] = {
		T3D_RW_GPU_BUFFER_VISIBLE_WORLD_CLUSTERS, T3D_RW_GPU_BUFFER_VISIBLE_CLUTTER_CLUSTERS, T3D_RW_GPU_BUFFER_VISIBLE_FOLIAGE_CLUSTERS
	};
	for ( uint32_t i = 0u; i < T3D_RW_TIER_COUNT; ++i ) {
		if ( !create_predict_uniform_set_cold( aVisible[i], m_hUniformPredict[i] ) ) {
			return false;
		}
	}
	return create_buffer_uniform_set_cold( PIPE_COMPACT, aCompact, sizeof( aCompact ) / sizeof( aCompact[0] ), m_hUniformCompact );
}

bool Terrain3DRenderWorldVTGpu::make_gpu_page_entry(
        uint32_t pChannel, const T3DVTPageTableEntry &pCpuEntry, T3DVTPageTableEntry &rGpuEntry ) const {
	rGpuEntry = pCpuEntry;
	if ( ( pCpuEntry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u || pCpuEntry.nPhysicalSlot == T3D_RW_VT_INVALID_SLOT ) {
		rGpuEntry.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
		return true;
	}
	uint32_t nBank = 0u, nLayer = 0u;
	if ( !map_physical_slot_hot( pChannel, pCpuEntry.nPhysicalSlot, nBank, nLayer ) || nBank > 0xffffu || nLayer > 0xffffu ) {
		return false;
	}
	rGpuEntry.nPhysicalSlot = ( nBank << 16u ) | nLayer;
	return true;
}

bool Terrain3DRenderWorldVTGpu::update_page_entry_hot( uint32_t pVirtualPage, uint32_t pChannel, const T3DVTPageTableEntry &pEntry ) {
	const uint64_t nOffset = ( uint64_t )pVirtualPage * sizeof( T3DVTPageTableEntry );
	if ( nOffset + sizeof( T3DVTPageTableEntry ) > m_pResources->get_plan().nBufferBytes[T3D_RW_GPU_BUFFER_VT_PAGE_TABLE]
	        || nOffset > UINT32_MAX ) {
		return false;
	}
	T3DVTPageTableEntry GpuEntry = {};
	if ( !make_gpu_page_entry( pChannel, pEntry, GpuEntry ) ) {
		return false;
	}
	memcpy( m_PageEntryUpload.ptrw(), &GpuEntry, sizeof( GpuEntry ) );
	const Error result = m_pRenderingDevice->buffer_update( m_pResources->get_buffer_rid( T3D_RW_GPU_BUFFER_VT_PAGE_TABLE ),
	        ( uint32_t )nOffset,
	        sizeof( T3DVTPageTableEntry ),
	        m_PageEntryUpload );
	if ( result != OK ) {
		return false;
	}
	if ( Terrain3DRdContext *const pContext = m_pResources->get_rd_context() ) {
		pContext->recordBufferUpdate( sizeof( T3DVTPageTableEntry ) );
	}
	m_Stats.nPageTableUpdateBytes += sizeof( T3DVTPageTableEntry );
	++m_Stats.nPageTableUpdateCalls;
	return true;
}

bool Terrain3DRenderWorldVTGpu::retire_victim_gpu_hot(
        T3DVTResidencyState &rResidency, uint32_t pChannel, uint32_t pPhysicalSlot, uint32_t pVictimPage ) {
	if ( pChannel >= T3D_RW_VT_CHANNEL_COUNT || pPhysicalSlot >= rResidency.Buffers.nPhysicalSlotCapacity[pChannel]
	        || pVictimPage >= rResidency.pDatabase->pHeader->nPageCount ) {
		return false;
	}
	const T3DVTPhysicalSlot &Slot = rResidency.Buffers.pPhysicalSlots[pChannel][pPhysicalSlot];
	const T3DVTPageTableEntry &CpuEntry = rResidency.Buffers.pPageTable[pVictimPage];
	if ( ( Slot.nFlags & T3D_VT_SLOT_RETIRED ) == 0u || Slot.nVirtualPage != pVictimPage || ( CpuEntry.nFlags & T3D_VT_PAGE_RESIDENT ) == 0u
	        || CpuEntry.nPhysicalSlot != pPhysicalSlot ) {
		return false;
	}
	T3DVTPageTableEntry Invalid = CpuEntry;
	Invalid.nPhysicalSlot = T3D_RW_VT_INVALID_SLOT;
	Invalid.nFlags &= ( uint16_t )~T3D_VT_PAGE_RESIDENT;
	if ( !update_page_entry_hot( pVictimPage, pChannel, Invalid ) ) {
		return false;
	}
	return t3d_render_world_vt_finalize_eviction_hot( rResidency, pChannel, pPhysicalSlot, pVictimPage );
}

bool Terrain3DRenderWorldVTGpu::map_physical_slot_hot(
        uint32_t pChannel, uint32_t pPhysicalSlot, uint32_t &rBank, uint32_t &rLayer ) const {
	if ( pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return false;
	}
	uint32_t nBase = 0u;
	for ( uint32_t bank = 0u; bank < m_pResources->get_vt_bank_count( ( T3DRenderWorldVTChannel )pChannel ); ++bank ) {
		const uint32_t nLayers = m_pResources->get_vt_bank_layer_count( ( T3DRenderWorldVTChannel )pChannel, bank );
		if ( pPhysicalSlot < nBase + nLayers ) {
			rBank = bank;
			rLayer = pPhysicalSlot - nBase;
			return true;
		}
		nBase += nLayers;
	}
	return false;
}

uint32_t Terrain3DRenderWorldVTGpu::physical_layer_bytes( uint32_t pChannel ) const {
	if ( m_pResources == NULL || pChannel >= T3D_RW_VT_CHANNEL_COUNT ) {
		return 0u;
	}
	uint32_t nLayers = 0u;
	for ( uint32_t bank = 0u; bank < m_pResources->get_vt_bank_count( ( T3DRenderWorldVTChannel )pChannel ); ++bank ) {
		nLayers += m_pResources->get_vt_bank_layer_count( ( T3DRenderWorldVTChannel )pChannel, bank );
	}
	if ( nLayers == 0u ) {
		return 0u;
	}
	const uint64_t nBytes = m_pResources->get_plan().nVtPhysicalBytes[pChannel] / nLayers;
	return nBytes <= UINT32_MAX ? ( uint32_t )nBytes : 0u;
}

void Terrain3DRenderWorldVTGpu::free_rid_cold( RID &rRid ) {
	if ( m_pRenderingDevice == NULL || !rRid.is_valid() ) {
		return;
	}
	t3dRenderWorldFreeRid( m_pResources ? m_pResources->get_rd_context() : NULL, rRid );
	++m_Stats.nGpuFreeCount;
}

bool Terrain3DRenderWorldVTGpu::hot_epoch_unchanged() const {
	return m_Stats.nGpuCreateCount == m_nHotCreateEpoch && m_Stats.nGpuFreeCount == m_nHotFreeEpoch;
}
