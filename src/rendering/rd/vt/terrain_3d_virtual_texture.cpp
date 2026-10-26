// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/vt/terrain_3d_virtual_texture.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <limits.h>
#include <string.h>

static uint64_t t3dVtAlign64( const uint64_t pValue ) {
	return ( pValue + 63u ) & ~63ull;
}

static uint64_t t3dVtTicksUsec( void ) {
	godot::Time *const pTime = godot::Time::get_singleton();
	return pTime ? pTime->get_ticks_usec() : 0u;
}

static void *t3dVtRuntimePackAllocate( void *const pUser, const size_t nBytes, const size_t nAlignment ) {
	Terrain3DMemoryZone *const pZone = ( Terrain3DMemoryZone * )pUser;
	return pZone != NULL ? pZone->allocate( nBytes, nAlignment ) : NULL;
}

static void t3dVtRuntimePackFree( void *const pUser, void *const pMemory ) {
	Terrain3DMemoryZone *const pZone = ( Terrain3DMemoryZone * )pUser;
	if ( pZone != NULL && pMemory != NULL ) {
		pZone->deallocate( pMemory );
	}
}

static T3DVTPackRuntimeAllocator t3dVtRuntimePackAllocator( Terrain3DMemoryZone *const pZone ) {
	T3DVTPackRuntimeAllocator Allocator;
	Allocator.pUser = pZone;
	Allocator.pAllocate = t3dVtRuntimePackAllocate;
	Allocator.pFree = t3dVtRuntimePackFree;
	return Allocator;
}

Terrain3DVirtualTexture::Terrain3DVirtualTexture() {
	m_pResources = NULL;
	m_pRuntimeAllocatorZone = NULL;
	memset( &m_Limits, 0, sizeof( m_Limits ) );
	memset( &m_Residency, 0, sizeof( m_Residency ) );
	memset( &m_Streaming, 0, sizeof( m_Streaming ) );
	memset( &m_StreamingLimits, 0, sizeof( m_StreamingLimits ) );
	t3d_vt_default_residency_policy( m_ResidencyPolicy );
	memset( &m_NeutralDatabase, 0, sizeof( m_NeutralDatabase ) );
	t3d_vt_pack_runtime_clear( m_RuntimePack );
	memset( &m_NeutralHeader, 0, sizeof( m_NeutralHeader ) );
	memset( m_NeutralAllocations, 0, sizeof( m_NeutralAllocations ) );
	memset( m_NeutralPages, 0, sizeof( m_NeutralPages ) );
	memset( m_NeutralPayloads, 0, sizeof( m_NeutralPayloads ) );
	memset( m_NeutralPayloadBytes, 0, sizeof( m_NeutralPayloadBytes ) );
	m_pCpuImage = NULL;
	m_nCpuImageBytes = 0u;
	m_pAllocations = NULL;
	m_pTailMetadata = NULL;
	m_pPageTable = NULL;
	m_pPhysicalPageSets = NULL;
	m_pStreamJobs = NULL;
	m_pStreamPageToJob = NULL;
	m_pStreamFreeJobs = NULL;
	m_pStreamQueuedJobs = NULL;
	m_pStreamJobQueuePosition = NULL;
	m_pStreamReadyJobs = NULL;
	m_pStreamMergedWantedMip = NULL;
	m_pStreamMergedPriority = NULL;
	m_pFreePhysicalSlots = NULL;
	m_pRetiredPageSets = NULL;
	m_pLastEvictedFrame = NULL;
	m_pStagedRequests = NULL;
	m_nStagedRequestCount = 0u;
	m_nStagedRequestFrame = UINT32_MAX;
	m_pStreamScratch = NULL;
	m_pScratchLaneOwner = NULL;
	for ( uint32_t i = 0u; i < T3D_RW_MAX_STREAM_SCRATCH_LANES; ++i ) {
		m_ScratchLaneCompletion[i].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	}
	memset( &m_Stats, 0, sizeof( m_Stats ) );
	memset( &m_QualityStats, 0, sizeof( m_QualityStats ) );
	m_nRequestedQualityProfile = 2u; // Normal. Runtime selection remains data-only.
	m_nQualityGeneration = 0u;
	m_nGpuHelperCreateEpoch = 0u;
	m_nTelemetryStartUsec = 0u;
	m_nTelemetryLastUploadedPageSets = 0u;
	m_nTelemetryLastEvictions = 0u;
	m_nTelemetryUploadPageSetsFrame = 0u;
	m_nTelemetryUploadBytesFrame = 0u;
	m_nTelemetryEvictionsFrame = 0u;
	m_nStartupGeneration = 0u;
	m_nStartupPackagePrepareCount = 0u;
	m_nStartupPackagePrepareUsecLast = 0u;
	m_nStartupCpuImagePrepareCount = 0u;
	m_nStartupCpuImagePrepareUsecLast = 0u;
	m_nStartupGpuInitializeCount = 0u;
	m_nStartupGpuInitializeUsecLast = 0u;
	m_nStartupGpuStoragePrepareCount = 0u;
	m_nStartupGpuStoragePrepareUsecLast = 0u;
	m_nStartupGpuBindCount = 0u;
	m_nStartupGpuBindUsecLast = 0u;
	m_nStartupInitialPublicationCount = 0u;
	m_nStartupInitialPublicationUsecLast = 0u;
	m_nResolverDepth0Samples = 0u;
	m_nResolverDepth1Samples = 0u;
	m_nResolverDepth2To3Samples = 0u;
	m_nResolverDepth4PlusSamples = 0u;
	m_nResolverTailSamples = 0u;
	m_nResolverTelemetrySampleFrame = UINT32_MAX;
	m_nResolverTelemetrySamplesThisFrame = 0u;
	m_nUploadCadenceStableFrames = 0u;
	m_nUploadCadenceLastPageMisses = 0u;
	m_nUploadCadenceLastEvictions = 0u;
	m_nUploadCadenceLastThrash = 0u;
	m_bStreamingLimitsConfigured = false;
	m_bResidencyPolicyConfigured = false;
	m_bRuntimePackLoaded = false;
	m_bCpuPrepared = false;
	m_bGpuStoragePrepared = false;
	m_bExecutableBound = false;
	m_bInitialPublished = false;
	m_bInitialized = false;
	m_bHotSealed = false;
}

Terrain3DVirtualTexture::~Terrain3DVirtualTexture() {
	shutdown_cold();
}

bool Terrain3DVirtualTexture::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone ) {
	if ( pZone == NULL || m_bInitialized || ( m_pRuntimeAllocatorZone != NULL && m_pRuntimeAllocatorZone != pZone ) ) {
		return m_pRuntimeAllocatorZone == pZone && !m_bInitialized;
	}
	m_pRuntimeAllocatorZone = pZone;
	return true;
}

bool Terrain3DVirtualTexture::configure_streaming_limits_cold( const T3DVTStreamingLimits &pLimits ) {
	if ( m_bInitialized || pLimits.nMaxUploadBytesPerFrame == 0u || pLimits.nMaxPageSetsPerFrame == 0u || pLimits.nMaxInFlightIoJobs == 0u
	        || pLimits.nMaxAdmittedRequestsPerFrame == 0u ) {
		return false;
	}
	m_StreamingLimits = pLimits;
	m_bStreamingLimitsConfigured = true;
	return true;
}

void Terrain3DVirtualTexture::set_frame_budgets_hot( const uint32_t nRequestBudget, const uint32_t nAdmissionBudget ) {
	m_nFrameRequestBudget = CLAMP( nRequestBudget, 1u, MAX( 1u, m_Limits.nMaxVtRequests ) );
	m_Streaming.Limits.nMaxAdmittedRequestsPerFrame = CLAMP( nAdmissionBudget, 1u, MAX( 1u, m_Limits.nMaxStreamJobs ) );
}

const T3DVTStreamingLimits &Terrain3DVirtualTexture::get_streaming_limits() const {
	return m_StreamingLimits;
}

bool Terrain3DVirtualTexture::configure_residency_policy_cold( const T3DVTResidencyPolicy &pPolicy ) {
	if ( m_bInitialized || !t3d_vt_validate_residency_policy( pPolicy ) ) {
		return false;
	}
	m_ResidencyPolicy = pPolicy;
	m_bResidencyPolicyConfigured = true;
	return true;
}

const T3DVTResidencyPolicy &Terrain3DVirtualTexture::get_residency_policy() const {
	return m_ResidencyPolicy;
}

void Terrain3DVirtualTexture::set_startup_generation( const uint32_t nGeneration ) {
	if ( m_nStartupGeneration == nGeneration ) {
		return;
	}
	m_nStartupGeneration = nGeneration;
	t3dStartupDomainRequest( m_startupState, nGeneration, 0u, 4u );
	godot::UtilityFunctions::print( "[Terrain3D][Startup][VT] REQUESTED generation=", nGeneration );
	m_nStartupPackagePrepareCount = 0u;
	m_nStartupPackagePrepareUsecLast = 0u;
	m_nStartupCpuImagePrepareCount = 0u;
	m_nStartupCpuImagePrepareUsecLast = 0u;
	m_nStartupGpuInitializeCount = 0u;
	m_nStartupGpuInitializeUsecLast = 0u;
	m_nStartupGpuStoragePrepareCount = 0u;
	m_nStartupGpuStoragePrepareUsecLast = 0u;
	m_nStartupGpuBindCount = 0u;
	m_nStartupGpuBindUsecLast = 0u;
	m_nStartupInitialPublicationCount = 0u;
	m_nStartupInitialPublicationUsecLast = 0u;
}

void Terrain3DVirtualTexture::mark_startup_failed( const uint32_t nGeneration ) {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( nGeneration != m_nStartupGeneration || startup.m_nGeneration != nGeneration
	        || startup.m_eStage == T3D_STARTUP_COMMITTED ) {
		return;
	}
	const uint64_t nNowUsec = t3dVtTicksUsec();
	t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, nNowUsec );
}

void Terrain3DVirtualTexture::cancel_startup_cold( const uint32_t nGeneration ) {
	if ( nGeneration != m_nStartupGeneration || !t3dStartupDomainMatches( m_startupState, nGeneration ) ) {
		return;
	}
	const t3dStartupStage_t eStage = t3dStartupDomainGetStage( m_startupState );
	if ( eStage == T3D_STARTUP_FAILED || eStage == T3D_STARTUP_CANCELLED ) {
		return;
	}
	( void )t3dStartupDomainCancel( m_startupState, nGeneration, t3dVtTicksUsec() );
}

godot::Dictionary Terrain3DVirtualTexture::get_startup_diagnostics() const {
	godot::Dictionary result;
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	result["startup_generation"] = ( int64_t )( m_nStartupGeneration );
	const t3dStartupStage_t eStartupStage = startup.m_eStage;
	result["startup_stage"] = ( int64_t )( eStartupStage );
	result["startup_stage_name"] = godot::String( t3dStartupStageName( eStartupStage ) );
	result["startup_state_generation"] = ( int64_t )( startup.m_nGeneration );
	result["startup_completed_units"] = ( int64_t )( startup.m_nCompletedUnits );
	result["startup_total_units"] = ( int64_t )( startup.m_nTotalUnits );
	result["startup_start_usec"] = ( int64_t )( startup.m_nStartUsec );
	result["startup_end_usec"] = ( int64_t )( startup.m_nEndUsec );
	result["package_prepare_count"] = ( int64_t )( m_nStartupPackagePrepareCount );
	result["package_prepare_usec_last"] = ( int64_t )( m_nStartupPackagePrepareUsecLast );
	result["cpu_image_prepare_count"] = ( int64_t )( m_nStartupCpuImagePrepareCount );
	result["cpu_image_prepare_usec_last"] = ( int64_t )( m_nStartupCpuImagePrepareUsecLast );
	result["gpu_initialize_count"] = ( int64_t )( m_nStartupGpuInitializeCount );
	result["gpu_initialize_usec_last"] = ( int64_t )( m_nStartupGpuInitializeUsecLast );
	result["gpu_storage_prepare_count"] = ( int64_t )( m_nStartupGpuStoragePrepareCount );
	result["gpu_storage_prepare_usec_last"] = ( int64_t )( m_nStartupGpuStoragePrepareUsecLast );
	result["gpu_bind_count"] = ( int64_t )( m_nStartupGpuBindCount );
	result["gpu_bind_usec_last"] = ( int64_t )( m_nStartupGpuBindUsecLast );
	result["cpu_ready"] = m_bCpuPrepared;
	result["gpu_storage_ready"] = m_bGpuStoragePrepared;
	result["gpu_ready"] = m_bExecutableBound;
	result["initial_published"] = m_bInitialPublished;
	result["initial_publication_count"] = ( int64_t )( m_nStartupInitialPublicationCount );
	result["initial_publication_usec_last"] = ( int64_t )( m_nStartupInitialPublicationUsecLast );
	result["vt_cpu_ready"] = m_bCpuPrepared;
	result["vt_storage_ready"] = m_bGpuStoragePrepared;
	result["vt_gpu_ready"] = m_bExecutableBound;
	result["vt_cpu_bytes"] = ( int64_t )( m_nCpuImageBytes );
	result["vt_gpu_bytes"] = ( int64_t )( get_gpu_reserved_bytes() );
	const uint64_t nNowUsec = t3dVtTicksUsec();
	result["vt_elapsed_ms"] = ( double )( t3dStartupDomainElapsedUsec( startup, nNowUsec ) ) / 1000.0;
	return result;
}

bool Terrain3DVirtualTexture::loadPackageCold( const void *const pPackageBytes, const uint64_t nPackageBytes ) {
	if ( pPackageBytes == NULL || nPackageBytes == 0u || m_pRuntimeAllocatorZone == NULL || m_bInitialized || m_bHotSealed ) {
		return false;
	}
	const uint64_t nPrepareStartedUsec = t3dVtTicksUsec();
	const bool bTrackStartup = m_nStartupGeneration != 0u && t3dStartupDomainGetStage( m_startupState ) != T3D_STARTUP_COMMITTED;
	if ( bTrackStartup ) {
		t3dStartupDomainSetStartUsecIfZero( m_startupState, nPrepareStartedUsec );
		t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_PREPARING );
	}
	++m_nStartupPackagePrepareCount;
	T3DVTPackRuntimeStorage Candidate;
	t3d_vt_pack_runtime_clear( Candidate );
	const T3DVTPackRuntimeAllocator Allocator = t3dVtRuntimePackAllocator( m_pRuntimeAllocatorZone );
	if ( !t3d_vt_pack_runtime_open_cold( pPackageBytes, nPackageBytes, Allocator, Candidate ) ) {
		m_nStartupPackagePrepareUsecLast = t3dVtTicksUsec() - nPrepareStartedUsec;
		return false;
	}
	if ( m_bRuntimePackLoaded ) {
		const T3DVTPackRuntimeAllocator Allocator = t3dVtRuntimePackAllocator( m_pRuntimeAllocatorZone );
		t3d_vt_pack_runtime_release_cold( Allocator, m_RuntimePack );
	}
	m_RuntimePack = Candidate;
	m_bRuntimePackLoaded = true;
	if ( !apply_quality_profile_state_v3( m_RuntimePack.pRuntimeAllocations, m_RuntimePack.Database.pHeader->nAllocationCount ) ) {
		release_runtime_pack_cold();
		m_nStartupPackagePrepareUsecLast = t3dVtTicksUsec() - nPrepareStartedUsec;
		return false;
	}
	++m_nQualityGeneration;
	m_nStartupPackagePrepareUsecLast = t3dVtTicksUsec() - nPrepareStartedUsec;
	godot::UtilityFunctions::print( "[Terrain3D][Startup][VT] PACKAGE PREP generation=", m_nStartupGeneration,
	        " bytes=", nPackageBytes, " elapsed=", m_nStartupPackagePrepareUsecLast, " us" );
	return true;
}

bool Terrain3DVirtualTexture::clearPackageCold( void ) {
	if ( m_bInitialized || m_bHotSealed ) {
		return false;
	}
	release_runtime_pack_cold();
	return true;
}

void Terrain3DVirtualTexture::release_runtime_pack_cold() {
	if ( m_bRuntimePackLoaded ) {
		const T3DVTPackRuntimeAllocator Allocator = t3dVtRuntimePackAllocator( m_pRuntimeAllocatorZone );
		t3d_vt_pack_runtime_release_cold( Allocator, m_RuntimePack );
	}
	m_bRuntimePackLoaded = false;
	memset( &m_QualityStats, 0, sizeof( m_QualityStats ) );
	m_QualityStats.nRequestedProfile = m_nRequestedQualityProfile;
}

bool Terrain3DVirtualTexture::findMaterialAllocationCold( const uint64_t nStableKey, uint32_t &rAllocationId ) const {
	if ( !m_bRuntimePackLoaded ) {
		rAllocationId = T3D_VT_INVALID_ALLOCATION;
		return false;
	}
	if ( !t3d_vt_pack_runtime_find_allocation_by_stable_key( m_RuntimePack, nStableKey, rAllocationId ) ) {
		return false;
	}
	if ( m_RuntimePack.Database.nRuntimeKind == T3D_VT_RUNTIME_DATABASE_PACK_V3 ) {
		rAllocationId = t3d_vt_v3_runtime_allocation_base( rAllocationId );
	}
	return true;
}

bool Terrain3DVirtualTexture::allocate_cpu_image_cold() {
	if ( m_pCpuImage != NULL || m_pRuntimeAllocatorZone == NULL || m_Limits.nStreamScratchLaneCount > T3D_RW_MAX_STREAM_SCRATCH_LANES ) {
		return false;
	}
	if ( m_Limits.nMaxMaterials > UINT32_MAX / 4u ) {
		return false;
	}
	const uint32_t nAllocationCapacity = m_Limits.nMaxMaterials * 4u;
	uint64_t nBytes = 0u;
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )nAllocationCapacity * sizeof( t3dVTAllocation_t );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )nAllocationCapacity * sizeof( t3dVTTailMetadata_t );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxVtVirtualPages * sizeof( T3DVTPageTableEntry );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxVtPhysicalPageSets * sizeof( t3dVTPhysicalSlot_t );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxStreamJobs * sizeof( T3DVTStreamJob );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxVtVirtualPages * sizeof( uint16_t );
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxStreamJobs * sizeof( uint16_t );
	}
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxVtPhysicalPageSets * sizeof( uint32_t );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxVtPhysicalPageSets * sizeof( T3DVTRetiredPageSet );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxVtVirtualPages * sizeof( uint32_t );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxVtRequests * sizeof( T3DVTStagedRequest );
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nStreamScratchLaneCount * m_Limits.nStreamScratchBytesPerLane;
	nBytes = t3dVtAlign64( nBytes ) + ( uint64_t )m_Limits.nStreamScratchLaneCount * sizeof( uint32_t );
	if ( nBytes == 0u || nBytes > SIZE_MAX ) {
		return false;
	}
	m_pCpuImage = m_pRuntimeAllocatorZone->allocate( ( size_t )nBytes, 64u );
	if ( m_pCpuImage == NULL ) {
		return false;
	}
	m_nCpuImageBytes = nBytes;
	memset( m_pCpuImage, 0, ( size_t )nBytes );
	uint8_t *const pBase = ( uint8_t * )m_pCpuImage;
	uint64_t nOffset = 0u;
	nOffset = t3dVtAlign64( nOffset );
	m_pAllocations = ( t3dVTAllocation_t * )( pBase + nOffset );
	nOffset += ( uint64_t )nAllocationCapacity * sizeof( t3dVTAllocation_t );
	nOffset = t3dVtAlign64( nOffset );
	m_pTailMetadata = ( t3dVTTailMetadata_t * )( pBase + nOffset );
	nOffset += ( uint64_t )nAllocationCapacity * sizeof( t3dVTTailMetadata_t );
	nOffset = t3dVtAlign64( nOffset );
	m_pPageTable = ( T3DVTPageTableEntry * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxVtVirtualPages * sizeof( T3DVTPageTableEntry );
	nOffset = t3dVtAlign64( nOffset );
	m_pPhysicalPageSets = ( t3dVTPhysicalSlot_t * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxVtPhysicalPageSets * sizeof( t3dVTPhysicalSlot_t );
	nOffset = t3dVtAlign64( nOffset );
	m_pStreamJobs = ( T3DVTStreamJob * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxStreamJobs * sizeof( T3DVTStreamJob );
	nOffset = t3dVtAlign64( nOffset );
	m_pStreamPageToJob = ( uint16_t * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxVtVirtualPages * sizeof( uint16_t );
	uint16_t **const pJobArrays[6] = { &m_pStreamFreeJobs,
		&m_pStreamQueuedJobs,
		&m_pStreamJobQueuePosition,
		&m_pStreamReadyJobs,
		&m_pStreamMergedWantedMip,
		&m_pStreamMergedPriority };
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		nOffset = t3dVtAlign64( nOffset );
		*pJobArrays[i] = ( uint16_t * )( pBase + nOffset );
		nOffset += ( uint64_t )m_Limits.nMaxStreamJobs * sizeof( uint16_t );
	}
	nOffset = t3dVtAlign64( nOffset );
	m_pFreePhysicalSlots = ( uint32_t * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxVtPhysicalPageSets * sizeof( uint32_t );
	nOffset = t3dVtAlign64( nOffset );
	m_pRetiredPageSets = ( T3DVTRetiredPageSet * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxVtPhysicalPageSets * sizeof( T3DVTRetiredPageSet );
	nOffset = t3dVtAlign64( nOffset );
	m_pLastEvictedFrame = ( uint32_t * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxVtVirtualPages * sizeof( uint32_t );
	nOffset = t3dVtAlign64( nOffset );
	m_pStagedRequests = ( T3DVTStagedRequest * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxVtRequests * sizeof( T3DVTStagedRequest );
	m_nStagedRequestCount = 0u;
	m_nStagedRequestFrame = UINT32_MAX;
	nOffset = t3dVtAlign64( nOffset );
	m_pStreamScratch = pBase + nOffset;
	nOffset += ( uint64_t )m_Limits.nStreamScratchLaneCount * m_Limits.nStreamScratchBytesPerLane;
	nOffset = t3dVtAlign64( nOffset );
	m_pScratchLaneOwner = ( uint32_t * )( pBase + nOffset );
	return nOffset + ( uint64_t )m_Limits.nStreamScratchLaneCount * sizeof( uint32_t ) <= nBytes;
}

void Terrain3DVirtualTexture::release_cpu_image_cold() {
	if ( m_pCpuImage != NULL && m_pRuntimeAllocatorZone != NULL ) {
		m_pRuntimeAllocatorZone->deallocate( m_pCpuImage );
	}
	m_pCpuImage = NULL;
	m_nCpuImageBytes = 0u;
	m_pAllocations = NULL;
	m_pTailMetadata = NULL;
	m_pPageTable = NULL;
	m_pPhysicalPageSets = NULL;
	m_pStreamJobs = NULL;
	m_pStreamPageToJob = NULL;
	m_pStreamFreeJobs = NULL;
	m_pStreamQueuedJobs = NULL;
	m_pStreamJobQueuePosition = NULL;
	m_pStreamReadyJobs = NULL;
	m_pStreamMergedWantedMip = NULL;
	m_pStreamMergedPriority = NULL;
	m_pFreePhysicalSlots = NULL;
	m_pRetiredPageSets = NULL;
	m_pLastEvictedFrame = NULL;
	m_pStagedRequests = NULL;
	m_nStagedRequestCount = 0u;
	m_nStagedRequestFrame = UINT32_MAX;
	m_pStreamScratch = NULL;
	m_pScratchLaneOwner = NULL;
}

bool Terrain3DVirtualTexture::prepare_neutral_legacy_cpu_cold() {
	memset( &m_NeutralHeader, 0, sizeof( m_NeutralHeader ) );
	m_NeutralHeader.nMagic = T3D_RW_VT_FILE_MAGIC;
	m_NeutralHeader.nVersion = T3D_RW_VT_FILE_VERSION;
	m_NeutralHeader.nHeaderBytes = sizeof( T3DVTFileHeader );
	m_NeutralHeader.nAllocationCount = T3D_RW_VT_CHANNELS;
	m_NeutralHeader.nPageCount = T3D_RW_VT_CHANNELS;
	m_NeutralHeader.nPayloadCount = T3D_RW_VT_CHANNELS;
	m_NeutralHeader.nChannelCount = T3D_RW_VT_CHANNELS;
	m_NeutralHeader.nPayloadDataBytes = sizeof( m_NeutralPayloadBytes );
	m_NeutralHeader.nInteriorTexels = T3D_RW_VT_PAGE_INTERIOR;
	m_NeutralHeader.nGutterTexels = T3D_RW_VT_PAGE_GUTTER;
	m_NeutralHeader.nPhysicalTexels = T3D_RW_VT_PHYSICAL_TILE;
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		T3DVTAllocationRecordV1 &allocation = m_NeutralAllocations[c];
		memset( &allocation, 0, sizeof( allocation ) );
		allocation.nBaseVirtualPage = c;
		allocation.nPageCount = 1u;
		allocation.nMip0WidthPages = 1u;
		allocation.nMip0HeightPages = 1u;
		allocation.nMipCount = 1u;
		allocation.nChannel = ( uint16_t )c;
		allocation.nFlags = T3D_VT_PAGE_NEUTRAL | T3D_VT_PAGE_ROOT;
		allocation.nRootPage = c;
		T3DVTPageFileRecord &page = m_NeutralPages[c];
		memset( &page, 0, sizeof( page ) );
		page.nVirtualPage = c;
		page.nParentPage = T3D_RW_VT_INVALID_PAGE;
		page.nAllocationId = c;
		page.nPayloadId = c;
		page.nMip = 0u;
		page.nChannel = ( uint16_t )c;
		page.nFlags = T3D_VT_PAGE_NEUTRAL | T3D_VT_PAGE_ROOT;
		page.nGeneration = 1u;
		memset( &m_NeutralPayloads[c], 0, sizeof( m_NeutralPayloads[c] ) );
		m_NeutralPayloads[c].nOffset = c;
		m_NeutralPayloads[c].nCompressedBytes = 1u;
		m_NeutralPayloads[c].nDecodedBytes = 1u;
	}
	m_NeutralDatabase.pHeader = &m_NeutralHeader;
	m_NeutralDatabase.pAllocations = m_NeutralAllocations;
	m_NeutralDatabase.pPages = m_NeutralPages;
	m_NeutralDatabase.pPayloads = m_NeutralPayloads;
	m_NeutralDatabase.pPayloadData = m_NeutralPayloadBytes;
	m_NeutralDatabase.nFileBytes = sizeof( m_NeutralHeader ) + sizeof( m_NeutralAllocations ) + sizeof( m_NeutralPages )
	        + sizeof( m_NeutralPayloads ) + sizeof( m_NeutralPayloadBytes );

	T3DVTResidencyBuffers ResidencyBuffers = {};
	ResidencyBuffers.pAllocations = m_pAllocations;
	ResidencyBuffers.nAllocationCapacity = m_Limits.nMaxMaterials * 4u;
	ResidencyBuffers.pPageTable = m_pPageTable;
	ResidencyBuffers.nPageTableCapacity = m_Limits.nMaxVtVirtualPages;
	ResidencyBuffers.pTailMetadata = m_pTailMetadata;
	ResidencyBuffers.nTailMetadataCapacity = m_Limits.nMaxMaterials * 4u;
	ResidencyBuffers.pPhysicalPageSets = m_pPhysicalPageSets;
	ResidencyBuffers.nPhysicalPageSetCapacity = m_Limits.nMaxVtPhysicalPageSets;
	ResidencyBuffers.pFreeSlots = m_pFreePhysicalSlots;
	ResidencyBuffers.pRetiredPageSets = m_pRetiredPageSets;
	ResidencyBuffers.pLastEvictedFrame = m_pLastEvictedFrame;
	if ( !t3d_vt_initialize_residency_cold( m_NeutralDatabase, ResidencyBuffers, 1u, m_ResidencyPolicy.nRetireDelayFrames, m_Residency ) ) {
		return false;
	}
	m_Residency.Policy = m_ResidencyPolicy;
	m_Residency.nRetirementDelayFrames = m_ResidencyPolicy.nRetireDelayFrames;

	T3DVTStreamingBuffers StreamBuffers = {};
	StreamBuffers.pJobs = m_pStreamJobs;
	StreamBuffers.nJobCapacity = m_Limits.nMaxStreamJobs;
	StreamBuffers.pScratchBytes = m_pStreamScratch;
	StreamBuffers.nScratchLaneCount = m_Limits.nStreamScratchLaneCount;
	StreamBuffers.nScratchLaneBytes = ( uint32_t )m_Limits.nStreamScratchBytesPerLane;
	StreamBuffers.pScratchLaneOwner = m_pScratchLaneOwner;
	StreamBuffers.pScratchLaneCompletion = m_ScratchLaneCompletion;
	StreamBuffers.pPageToJob = m_pStreamPageToJob;
	StreamBuffers.nPageToJobCapacity = m_Limits.nMaxVtVirtualPages;
	StreamBuffers.pFreeJobs = m_pStreamFreeJobs;
	StreamBuffers.pQueuedJobs = m_pStreamQueuedJobs;
	StreamBuffers.pJobQueuePosition = m_pStreamJobQueuePosition;
	StreamBuffers.pReadyJobs = m_pStreamReadyJobs;
	StreamBuffers.pMergedWantedMip = m_pStreamMergedWantedMip;
	StreamBuffers.pMergedPriority = m_pStreamMergedPriority;
	for ( uint32_t i = 0u; i < m_Limits.nStreamScratchLaneCount; ++i ) {
		m_ScratchLaneCompletion[i].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	}
	return t3d_vt_streaming_initialize_cold( m_Residency, StreamBuffers, m_StreamingLimits, m_Streaming );
}

bool Terrain3DVirtualTexture::publish_neutral_legacy_warm() {
	return m_Gpu.upload_database_warm( m_NeutralDatabase, m_Residency )
	        && m_Gpu.upload_neutral_page_set_warm( m_Residency, 0u )
	        && t3d_vt_finalize_neutral_publication_warm( m_Residency );
}

bool Terrain3DVirtualTexture::prepare_runtime_package_cpu_cold() {
	const T3DVTDatabaseView &Database = m_RuntimePack.Database;
	if ( !m_bRuntimePackLoaded || Database.pHeader == NULL
	        || Database.nRuntimeKind != T3D_VT_RUNTIME_DATABASE_PACK_V3
	        || Database.pRuntimePayloads == NULL || Database.pRuntimeAllocations == NULL
	        || Database.pHeader->nAllocationCount == 0u || Database.pHeader->nAllocationCount > m_Limits.nMaxMaterials * 4u
	        || Database.pHeader->nPageCount == 0u || Database.pHeader->nPageCount > m_Limits.nMaxVtVirtualPages
	        || Database.nRuntimeTailCount == 0u || Database.nRuntimeTailCount > m_Limits.nMaxMaterials * 4u
	        || m_Limits.nStreamScratchBytesPerLane > UINT32_MAX ) {
		return false;
	}
	for ( uint32_t iPage = 0u; iPage < Database.pHeader->nPageCount; ++iPage ) {
		const T3DVTPageFileRecord &Page = Database.pPages[iPage];
		if ( Page.nPayloadId >= Database.nRuntimePayloadCount ) { return false; }
		const uint32_t nDecodedBytes = Database.pRuntimePayloads[Page.nPayloadId].nBytes;
		if ( nDecodedBytes == 0u || nDecodedBytes > m_Limits.nStreamScratchBytesPerLane ) { return false; }
	}

	if ( !apply_quality_profile_state_v3( m_RuntimePack.pRuntimeAllocations, Database.pHeader->nAllocationCount ) ) {
		return false;
	}

	T3DVTResidencyBuffers ResidencyBuffers = {};
	ResidencyBuffers.pAllocations = m_pAllocations;
	ResidencyBuffers.nAllocationCapacity = m_Limits.nMaxMaterials * 4u;
	ResidencyBuffers.pPageTable = m_pPageTable;
	ResidencyBuffers.nPageTableCapacity = m_Limits.nMaxVtVirtualPages;
	ResidencyBuffers.pTailMetadata = m_pTailMetadata;
	ResidencyBuffers.nTailMetadataCapacity = m_Limits.nMaxMaterials * 4u;
	ResidencyBuffers.pPhysicalPageSets = m_pPhysicalPageSets;
	ResidencyBuffers.nPhysicalPageSetCapacity = m_Limits.nMaxVtPhysicalPageSets;
	ResidencyBuffers.pFreeSlots = m_pFreePhysicalSlots;
	ResidencyBuffers.pRetiredPageSets = m_pRetiredPageSets;
	ResidencyBuffers.pLastEvictedFrame = m_pLastEvictedFrame;
	const uint32_t nDatabaseGeneration = Database.nPackageGeneration != 0u ? Database.nPackageGeneration : 1u;
	if ( !t3d_vt_initialize_residency_cold(
	             Database, ResidencyBuffers, nDatabaseGeneration, m_ResidencyPolicy.nRetireDelayFrames, m_Residency ) ) {
		return false;
	}
	m_Residency.Policy = m_ResidencyPolicy;
	m_Residency.nRetirementDelayFrames = m_ResidencyPolicy.nRetireDelayFrames;

	T3DVTStreamingBuffers StreamBuffers = {};
	StreamBuffers.pJobs = m_pStreamJobs;
	StreamBuffers.nJobCapacity = m_Limits.nMaxStreamJobs;
	StreamBuffers.pScratchBytes = m_pStreamScratch;
	StreamBuffers.nScratchLaneCount = m_Limits.nStreamScratchLaneCount;
	StreamBuffers.nScratchLaneBytes = ( uint32_t )m_Limits.nStreamScratchBytesPerLane;
	StreamBuffers.pScratchLaneOwner = m_pScratchLaneOwner;
	StreamBuffers.pScratchLaneCompletion = m_ScratchLaneCompletion;
	StreamBuffers.pPageToJob = m_pStreamPageToJob;
	StreamBuffers.nPageToJobCapacity = m_Limits.nMaxVtVirtualPages;
	StreamBuffers.pFreeJobs = m_pStreamFreeJobs;
	StreamBuffers.pQueuedJobs = m_pStreamQueuedJobs;
	StreamBuffers.pJobQueuePosition = m_pStreamJobQueuePosition;
	StreamBuffers.pReadyJobs = m_pStreamReadyJobs;
	StreamBuffers.pMergedWantedMip = m_pStreamMergedWantedMip;
	StreamBuffers.pMergedPriority = m_pStreamMergedPriority;
	for ( uint32_t i = 0u; i < m_Limits.nStreamScratchLaneCount; ++i ) {
		m_ScratchLaneCompletion[i].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	}
	return t3d_vt_streaming_initialize_cold( m_Residency, StreamBuffers, m_StreamingLimits, m_Streaming );
}

bool Terrain3DVirtualTexture::publish_runtime_package_warm() {
	const T3DVTDatabaseView &Database = m_RuntimePack.Database;
	return m_Gpu.upload_database_warm( Database, m_Residency ) && t3d_vt_finalize_package_publication_warm( m_Residency );
}

uint32_t Terrain3DVirtualTexture::count_physical_banks() const {
	if ( m_pResources == NULL ) {
		return 0u;
	}
	uint32_t nCount = 0u;
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		nCount += m_pResources->get_vt_bank_count( ( T3DVtChannel )c );
	}
	return nCount;
}

bool Terrain3DVirtualTexture::prepare_cpu_cold( const T3DRenderWorldLimits &pLimits,
        const T3DVtGpuResourcePlan &pPlan,
        const uint32_t nStartupGeneration ) {
	if ( m_bInitialized || m_bCpuPrepared || m_pRuntimeAllocatorZone == NULL || !t3d_render_world_validate_limits( pLimits ) ) {
		return false;
	}
	if ( nStartupGeneration != 0u ) {
		set_startup_generation( nStartupGeneration );
	}
	if ( !m_bStreamingLimitsConfigured ) {
		m_StreamingLimits.nMaxUploadBytesPerFrame = pLimits.nMaxUploadBytesPerFrame;
		m_StreamingLimits.nMaxPageSetsPerFrame = pLimits.nMaxStreamJobs < 8u ? pLimits.nMaxStreamJobs : 8u;
		m_StreamingLimits.nMaxInFlightIoJobs = pLimits.nStreamScratchLaneCount < 4u ? pLimits.nStreamScratchLaneCount : 4u;
		m_StreamingLimits.nMaxAdmittedRequestsPerFrame = pLimits.nMaxStreamJobs < 64u ? pLimits.nMaxStreamJobs : 64u;
	}
	if ( m_StreamingLimits.nMaxUploadBytesPerFrame == 0u || m_StreamingLimits.nMaxUploadBytesPerFrame > pLimits.nMaxUploadBytesPerFrame
	        || m_StreamingLimits.nMaxPageSetsPerFrame == 0u || m_StreamingLimits.nMaxPageSetsPerFrame > pLimits.nMaxStreamJobs
	        || m_StreamingLimits.nMaxInFlightIoJobs == 0u || m_StreamingLimits.nMaxInFlightIoJobs > pLimits.nStreamScratchLaneCount
	        || m_StreamingLimits.nMaxAdmittedRequestsPerFrame == 0u
	        || m_StreamingLimits.nMaxAdmittedRequestsPerFrame > pLimits.nMaxStreamJobs ) {
		return false;
	}
	m_Limits = pLimits;
	m_nFrameRequestBudget = MAX( 1u, pLimits.nMaxVtRequests );
	const bool bTrackStartup = m_nStartupGeneration != 0u && t3dStartupDomainGetStage( m_startupState ) != T3D_STARTUP_COMMITTED;
	const uint64_t nCpuStartedUsec = t3dVtTicksUsec();
	if ( bTrackStartup ) {
		t3dStartupDomainSetStartUsecIfZero( m_startupState, nCpuStartedUsec );
		t3dStartupDomainSetTotalUnits( m_startupState, 4u );
		t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_PREPARING );
	}
	++m_nStartupCpuImagePrepareCount;
	const bool bCpuReady = allocate_cpu_image_cold()
	        && ( m_bRuntimePackLoaded ? prepare_runtime_package_cpu_cold() : prepare_neutral_legacy_cpu_cold() )
	        && m_Gpu.prepare_cpu_cold( pPlan, pLimits );
	m_nStartupCpuImagePrepareUsecLast = t3dVtTicksUsec() - nCpuStartedUsec;
	godot::UtilityFunctions::print( "[Terrain3D][Startup][VT] CPU PREP generation=", m_nStartupGeneration,
	        " bytes=", m_nCpuImageBytes, " elapsed=", m_nStartupCpuImagePrepareUsecLast, " us ready=", bCpuReady );
	if ( !bCpuReady ) {
		if ( bTrackStartup ) {
			mark_startup_failed( m_nStartupGeneration );
		}
		shutdown_cold();
		return false;
	}
	m_bCpuPrepared = true;
	if ( bTrackStartup ) {
		t3dStartupDomainSetCompletedUnits( m_startupState, 1u );
		t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_CPU_READY );
		godot::UtilityFunctions::print( "[Terrain3D][Startup][VT] CPU READY generation=", m_nStartupGeneration );
	}
	return true;
}

bool Terrain3DVirtualTexture::prepare_gpu_storage_cold( Terrain3DVtResourceImage *const pResources ) {
	if ( !m_bCpuPrepared || m_bGpuStoragePrepared || pResources == NULL || !pResources->is_initialized() ) {
		return false;
	}
	m_pResources = pResources;
	const bool bTrackStartup = m_nStartupGeneration != 0u && t3dStartupDomainGetStage( m_startupState ) != T3D_STARTUP_COMMITTED;
	const uint64_t nStartedUsec = t3dVtTicksUsec();
	++m_nStartupGpuStoragePrepareCount;
	const bool bReady = m_Gpu.prepare_storage_cold( pResources, m_Limits );
	m_nStartupGpuStoragePrepareUsecLast = t3dVtTicksUsec() - nStartedUsec;
	godot::UtilityFunctions::print( "[Terrain3D][Startup][VT] GPU STORAGE generation=", m_nStartupGeneration,
	        " elapsed=", m_nStartupGpuStoragePrepareUsecLast, " us ready=", bReady );
	if ( !bReady ) {
		if ( bTrackStartup ) mark_startup_failed( m_nStartupGeneration );
		shutdown_cold();
		return false;
	}
	m_bGpuStoragePrepared = true;
	if ( bTrackStartup ) {
		t3dStartupDomainSetCompletedUnits( m_startupState, 2u );
		t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_GPU_PENDING );
		godot::UtilityFunctions::print( "[Terrain3D][Startup][VT] STORAGE READY generation=", m_nStartupGeneration );
	}
	return true;
}

bool Terrain3DVirtualTexture::bind_gpu_executable_cold() {
	while ( !m_bExecutableBound ) {
		if ( !service_gpu_executable_cold( UINT32_MAX ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DVirtualTexture::service_gpu_executable_cold(
        const uint32_t nMaxWorkUnits ) {
	if ( !m_bGpuStoragePrepared || m_bInitialPublished || nMaxWorkUnits == 0u ) {
		return false;
	}
	if ( m_bExecutableBound ) {
		return true;
	}
	const bool bTrackStartup = m_nStartupGeneration != 0u && t3dStartupDomainGetStage( m_startupState ) != T3D_STARTUP_COMMITTED;
	const uint64_t nStartedUsec = t3dVtTicksUsec();
	++m_nStartupGpuBindCount;
	++m_nStartupGpuInitializeCount;
	const bool bReady = m_Gpu.service_bind_executable_cold( nMaxWorkUnits );
	m_nStartupGpuBindUsecLast += t3dVtTicksUsec() - nStartedUsec;
	m_nStartupGpuInitializeUsecLast = m_nStartupGpuStoragePrepareUsecLast + m_nStartupGpuBindUsecLast;
	if ( !bReady ) {
		if ( bTrackStartup ) mark_startup_failed( m_nStartupGeneration );
		shutdown_cold();
		return false;
	}
	if ( m_Gpu.is_executable_ready() ) {
		m_bExecutableBound = true;
		if ( bTrackStartup ) {
			t3dStartupDomainSetCompletedUnits( m_startupState, 3u );
			t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_GPU_READY );
			godot::UtilityFunctions::print( "[Terrain3D][Startup][VT] GPU READY generation=", m_nStartupGeneration );
		}
	}
	return true;
}

bool Terrain3DVirtualTexture::publish_initial_cold() {
	while ( !m_bInitialPublished ) {
		if ( !service_initial_publication_cold( UINT64_MAX, UINT32_MAX ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DVirtualTexture::service_initial_publication_cold( const uint64_t nMaxBytes, const uint32_t nMaxWorkUnits ) {
	if ( !m_bCpuPrepared || !m_bGpuStoragePrepared || !m_bExecutableBound || m_pResources == NULL
	        || nMaxBytes == 0u || nMaxWorkUnits == 0u ) {
		return false;
	}
	if ( m_bInitialPublished ) {
		return true;
	}
	const bool bTrackStartup = m_nStartupGeneration != 0u && t3dStartupDomainGetStage( m_startupState ) != T3D_STARTUP_COMMITTED;
	const uint64_t nPublicationStartedUsec = t3dVtTicksUsec();
	++m_nStartupInitialPublicationCount;
	const T3DVTDatabaseView &Database = m_bRuntimePackLoaded ? m_RuntimePack.Database : m_NeutralDatabase;
	if ( !m_Gpu.service_database_upload_warm( Database, m_Residency, nMaxBytes, nMaxWorkUnits ) ) {
		if ( bTrackStartup ) mark_startup_failed( m_nStartupGeneration );
		shutdown_cold();
		return false;
	}
	if ( m_Gpu.is_database_upload_pending_warm() ) {
		m_nStartupInitialPublicationUsecLast += t3dVtTicksUsec() - nPublicationStartedUsec;
		return true;
	}
	bool bPublicationReady = true;
	if ( m_bRuntimePackLoaded ) {
		bPublicationReady = t3d_vt_finalize_package_publication_warm( m_Residency );
	} else if ( !m_Residency.bNeutralPublished ) {
		bPublicationReady = m_Gpu.upload_neutral_page_set_warm( m_Residency, 0u )
		        && t3d_vt_finalize_neutral_publication_warm( m_Residency );
	}
	m_nStartupInitialPublicationUsecLast += t3dVtTicksUsec() - nPublicationStartedUsec;
	if ( !bPublicationReady ) {
		if ( bTrackStartup ) mark_startup_failed( m_nStartupGeneration );
		shutdown_cold();
		return false;
	}
	m_bInitialPublished = true;
	m_Stats.nPhysicalPageSetCapacity = m_Limits.nMaxVtPhysicalPageSets;
	m_Stats.nPhysicalCacheAllocationEvents = count_physical_banks();
	m_Stats.nCpuImageBytes = m_nCpuImageBytes;
	m_nTelemetryStartUsec = godot::Time::get_singleton() != NULL ? godot::Time::get_singleton()->get_ticks_usec() : 0u;
	m_nTelemetryLastUploadedPageSets = m_Streaming.Stats.nUploadedPageCount;
	m_nTelemetryLastEvictions = m_Residency.Stats.nEvictionCount;
	m_nTelemetryUploadPageSetsFrame = 0u;
	m_nTelemetryUploadBytesFrame = 0u;
	m_nTelemetryEvictionsFrame = 0u;
	m_nResolverDepth0Samples = 0u;
	m_nResolverDepth1Samples = 0u;
	m_nResolverDepth2To3Samples = 0u;
	m_nResolverDepth4PlusSamples = 0u;
	m_nResolverTailSamples = 0u;
	m_nResolverTelemetrySampleFrame = UINT32_MAX;
	m_nResolverTelemetrySamplesThisFrame = 0u;
	m_nUploadCadenceStableFrames = 0u;
	m_nUploadCadenceLastPageMisses = m_Streaming.Stats.nPageMissCount;
	m_nUploadCadenceLastEvictions = m_Residency.Stats.nEvictionCount;
	m_nUploadCadenceLastThrash = m_Residency.Stats.nThrashCount;
	++m_Stats.nInitializeCount;
	m_bInitialized = true;
	if ( bTrackStartup ) {
		t3dStartupDomainSetCompletedUnits( m_startupState, 4u );
		t3dStartupDomainFinish( m_startupState, T3D_STARTUP_COMMITTED, t3dVtTicksUsec() );
	}
	godot::UtilityFunctions::print( "[Terrain3D][Startup][VT] INITIAL PUBLICATION generation=", m_nStartupGeneration,
	        " elapsed=", m_nStartupInitialPublicationUsecLast, " us ready=true" );
	return true;
}


void Terrain3DVirtualTexture::shutdown_cold() {
	m_Gpu.shutdown_cold();
	if ( m_bInitialized ) {
		++m_Stats.nShutdownCount;
	}
	m_bHotSealed = false;
	m_bCpuPrepared = false;
	m_bGpuStoragePrepared = false;
	m_bExecutableBound = false;
	m_bInitialPublished = false;
	m_bInitialized = false;
	memset( &m_Residency, 0, sizeof( m_Residency ) );
	memset( &m_Streaming, 0, sizeof( m_Streaming ) );
	memset( &m_NeutralDatabase, 0, sizeof( m_NeutralDatabase ) );
	release_cpu_image_cold();
	release_runtime_pack_cold();
	m_pResources = NULL;
	m_nGpuHelperCreateEpoch = 0u;
	m_nTelemetryStartUsec = 0u;
	m_nTelemetryLastUploadedPageSets = 0u;
	m_nTelemetryLastEvictions = 0u;
	m_nTelemetryUploadPageSetsFrame = 0u;
	m_nTelemetryUploadBytesFrame = 0u;
	m_nTelemetryEvictionsFrame = 0u;
	m_nResolverDepth0Samples = 0u;
	m_nResolverDepth1Samples = 0u;
	m_nResolverDepth2To3Samples = 0u;
	m_nResolverDepth4PlusSamples = 0u;
	m_nResolverTailSamples = 0u;
	m_nResolverTelemetrySampleFrame = UINT32_MAX;
	m_nResolverTelemetrySamplesThisFrame = 0u;
	memset( &m_Limits, 0, sizeof( m_Limits ) );
}

void Terrain3DVirtualTexture::seal_hot_runtime() {
	if ( !m_bInitialized || m_pResources == NULL ) {
		return;
	}
	m_Gpu.seal_hot_runtime();
	m_Stats.nPhysicalCacheCreateEpoch = m_pResources->get_stats().nGpuCreateCount;
	m_Stats.nPhysicalCacheFreeEpoch = m_pResources->get_stats().nGpuFreeCount;
	m_nGpuHelperCreateEpoch = m_Gpu.get_stats().nGpuCreateCount;
	m_bHotSealed = true;
}

bool Terrain3DVirtualTexture::validate_hot_plateau_reference_debug() {
	if ( !m_bInitialized || !m_bHotSealed || m_pResources == NULL ) {
		return false;
	}
	if ( m_pResources->get_stats().nGpuCreateCount != m_Stats.nPhysicalCacheCreateEpoch
	        || m_pResources->get_stats().nGpuFreeCount != m_Stats.nPhysicalCacheFreeEpoch ) {
		++m_Stats.nPhysicalCacheAllocationEventsAfterSeal;
		++m_Stats.nHotViolationCount;
		return false;
	}
	return true;
}

bool Terrain3DVirtualTexture::is_initialized() const {
	return m_bInitialized;
}
bool Terrain3DVirtualTexture::is_cpu_ready() const {
	return m_bCpuPrepared;
}
bool Terrain3DVirtualTexture::is_gpu_storage_ready() const {
	return m_bGpuStoragePrepared;
}
bool Terrain3DVirtualTexture::is_gpu_ready() const {
	return m_bExecutableBound;
}
bool Terrain3DVirtualTexture::is_initial_published() const {
	return m_bInitialPublished;
}
bool Terrain3DVirtualTexture::is_hot_sealed() const {
	return m_bHotSealed;
}
bool Terrain3DVirtualTexture::is_readback_enabled() const {
	return m_Gpu.is_readback_enabled();
}
uint64_t Terrain3DVirtualTexture::get_cpu_image_bytes() const {
	return m_nCpuImageBytes;
}

uint64_t Terrain3DVirtualTexture::get_gpu_reserved_bytes() const {
	return m_Gpu.get_reserved_gpu_bytes();
}
uint32_t Terrain3DVirtualTexture::get_page_table_generation() const {
	return m_Residency.nPageTableGeneration;
}

uint32_t Terrain3DVirtualTexture::getActiveAllocationCount() const {
	return m_Residency.pDatabase != NULL && m_Residency.pDatabase->pHeader != NULL ? m_Residency.pDatabase->pHeader->nAllocationCount : 0u;
}

uint32_t Terrain3DVirtualTexture::getActiveVirtualPageCount() const {
	return m_Residency.pDatabase != NULL && m_Residency.pDatabase->pHeader != NULL ? m_Residency.pDatabase->pHeader->nPageCount : 0u;
}

uint32_t Terrain3DVirtualTexture::getLoadedPackageAllocationCountCold() const {
	return m_bRuntimePackLoaded && m_RuntimePack.Database.pHeader != NULL ? m_RuntimePack.Database.pHeader->nAllocationCount : 0u;
}

uint32_t Terrain3DVirtualTexture::getLoadedPackageVirtualPageCountCold() const {
	return m_bRuntimePackLoaded && m_RuntimePack.Database.pHeader != NULL ? m_RuntimePack.Database.pHeader->nPageCount : 0u;
}

uint32_t Terrain3DVirtualTexture::getDatabaseGeneration() const {
	return m_Residency.nDatabaseGeneration;
}

uint32_t Terrain3DVirtualTexture::getPackageGeneration() const {
	return m_Residency.pDatabase != NULL ? m_Residency.pDatabase->nPackageGeneration : 0u;
}

uint64_t Terrain3DVirtualTexture::getPackageContentSignature() const {
	return m_Residency.pDatabase != NULL ? m_Residency.pDatabase->nPackageContentSignature : 0u;
}

uint32_t Terrain3DVirtualTexture::getDatabaseKind() const {
	return m_Residency.pDatabase != NULL ? m_Residency.pDatabase->nRuntimeKind : T3D_VT_RUNTIME_DATABASE_LEGACY_NEUTRAL;
}

uint32_t Terrain3DVirtualTexture::getMaterialVTAllocationId( const uint32_t nMaterialId ) const {
	if ( nMaterialId >= m_Limits.nMaxMaterials || !m_bRuntimePackLoaded ) {
		return T3D_VT_INVALID_ALLOCATION;
	}
	uint32_t nAllocationId = T3D_VT_INVALID_ALLOCATION;
	return findMaterialAllocationCold( t3d_vt_terrain_material_stable_key( nMaterialId ), nAllocationId ) ? nAllocationId
	                                                                                                      : T3D_VT_INVALID_ALLOCATION;
}

bool Terrain3DVirtualTexture::supportsSparseV3() const {
	return m_bInitialized && m_Residency.pDatabase != NULL && m_Residency.pDatabase->nRuntimeKind == T3D_VT_RUNTIME_DATABASE_PACK_V3;
}

uint32_t Terrain3DVirtualTexture::getTerrainMegaVTAllocationBase() const {
	return m_Limits.nMaxMaterials * T3D_VT_CHANNEL_COUNT;
}

uint32_t Terrain3DVirtualTexture::getTerrainMegaVTAllocationId( const uint32_t nRegionMapIndex ) const {
	if ( nRegionMapIndex >= T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY ) {
		return T3D_VT_INVALID_ALLOCATION;
	}
	return getTerrainMegaVTAllocationBase() + nRegionMapIndex;
}
const T3DVTDatabaseView *Terrain3DVirtualTexture::get_database() const {
	return m_Residency.pDatabase;
}
const Terrain3DVtResourceImage *Terrain3DVirtualTexture::get_resource_image() const {
	return m_bInitialized ? m_pResources : NULL;
}
bool Terrain3DVirtualTexture::apply_quality_profile_state_v3( t3dVTAllocation_t *const pAllocations, const uint32_t nAllocationCount ) {
	if ( !m_bRuntimePackLoaded || m_RuntimePack.Database.nRuntimeKind != T3D_VT_RUNTIME_DATABASE_PACK_V3 ) {
		memset( &m_QualityStats, 0, sizeof( m_QualityStats ) );
		m_QualityStats.nRequestedProfile = m_nRequestedQualityProfile;
		return true;
	}
	return t3d_vt_pack_runtime_apply_quality_profile_v3(
	        m_RuntimePack.Database, pAllocations, nAllocationCount, m_nRequestedQualityProfile, m_QualityStats );
}

bool Terrain3DVirtualTexture::set_quality_profile( const uint32_t nProfile ) {
	if ( nProfile >= T3D_VT_QUALITY_PROFILE_COUNT ) return false;
	if ( nProfile == m_nRequestedQualityProfile && m_nQualityGeneration != 0u ) return true;

	const uint32_t nOldRequestedProfile = m_nRequestedQualityProfile;
	const T3DVTQualityRuntimeStats OldStats = m_QualityStats;
	m_nRequestedQualityProfile = nProfile;

	if ( !m_bRuntimePackLoaded || m_RuntimePack.Database.nRuntimeKind != T3D_VT_RUNTIME_DATABASE_PACK_V3 ) {
		memset( &m_QualityStats, 0, sizeof( m_QualityStats ) );
		m_QualityStats.nRequestedProfile = nProfile;
		++m_nQualityGeneration;
		return true;
	}

	const uint32_t nAllocationCount = m_RuntimePack.Database.pHeader->nAllocationCount;
	if ( !apply_quality_profile_state_v3( m_RuntimePack.pRuntimeAllocations, nAllocationCount ) ) {
		m_nRequestedQualityProfile = nOldRequestedProfile;
		m_QualityStats = OldStats;
		return false;
	}

	if ( m_bInitialized ) {
		memcpy( m_Residency.Buffers.pAllocations, m_RuntimePack.pRuntimeAllocations,
		        ( size_t )nAllocationCount * sizeof( t3dVTAllocation_t ) );
		if ( !m_Gpu.update_allocation_quality_hot( m_Residency ) ) {
			m_nRequestedQualityProfile = nOldRequestedProfile;
			m_QualityStats = OldStats;
			if ( apply_quality_profile_state_v3( m_RuntimePack.pRuntimeAllocations, nAllocationCount ) ) {
				memcpy( m_Residency.Buffers.pAllocations, m_RuntimePack.pRuntimeAllocations,
				        ( size_t )nAllocationCount * sizeof( t3dVTAllocation_t ) );
				m_Gpu.update_allocation_quality_hot( m_Residency );
			}
			m_QualityStats = OldStats;
			return false;
		}
		m_nStagedRequestCount = 0u;
		m_nStagedRequestFrame = UINT32_MAX;
		m_Stats.nQualityDemandResetCount += t3d_vt_streaming_invalidate_queued_demand_hot( m_Streaming );
	}
	++m_nQualityGeneration;
	return true;
}

uint32_t Terrain3DVirtualTexture::get_requested_quality_profile() const { return m_nRequestedQualityProfile; }
uint32_t Terrain3DVirtualTexture::get_package_fully_supported_profile_mask() const { return m_QualityStats.nPackageFullySupportedProfileMask; }
uint32_t Terrain3DVirtualTexture::get_quality_fallback_allocation_count() const { return m_QualityStats.nFallbackAllocationCount; }
uint32_t Terrain3DVirtualTexture::get_quality_generation() const { return m_nQualityGeneration; }
uint32_t Terrain3DVirtualTexture::get_allocations_by_highest_profile( const uint32_t nProfile ) const {
	return nProfile < T3D_VT_QUALITY_PROFILE_COUNT ? m_QualityStats.nAllocationsByHighestProfile[nProfile] : 0u;
}
uint32_t Terrain3DVirtualTexture::get_allocations_by_resolved_profile( const uint32_t nProfile ) const {
	return nProfile < T3D_VT_QUALITY_PROFILE_COUNT ? m_QualityStats.nAllocationsByResolvedProfile[nProfile] : 0u;
}

uint32_t Terrain3DVirtualTexture::getPhysicalFormatFamilyCountV3() const {
	return T3D_VT_PHYSICAL_FORMAT_COUNT;
}
bool Terrain3DVirtualTexture::getPhysicalFormatFamilyBindingV3(
        const uint32_t nFamily, RID &rRid, uint32_t &rLayerCount ) const {
	return m_bInitialized && m_Gpu.get_physical_family_binding_v3( nFamily, rRid, rLayerCount );
}
T3DVTResidencyState &Terrain3DVirtualTexture::get_residency() {
	return m_Residency;
}
const T3DVTResidencyState &Terrain3DVirtualTexture::get_residency() const {
	return m_Residency;
}
T3DVTStreamingState &Terrain3DVirtualTexture::get_streaming() {
	return m_Streaming;
}
const T3DVTStreamingState &Terrain3DVirtualTexture::get_streaming() const {
	return m_Streaming;
}
Terrain3DVTGpu &Terrain3DVirtualTexture::get_gpu() {
	return m_Gpu;
}
const Terrain3DVTGpu &Terrain3DVirtualTexture::get_gpu() const {
	return m_Gpu;
}
const T3DVirtualTextureStats &Terrain3DVirtualTexture::get_stats() const {
	return m_Stats;
}

T3DVTFinalTelemetry Terrain3DVirtualTexture::get_final_telemetry_reference_debug() const {
	T3DVTFinalTelemetry Result = {};
	if ( !m_bInitialized || m_Residency.pDatabase == NULL || m_Residency.pDatabase->pHeader == NULL ) {
		return Result;
	}
	Result.nAllocations = m_Residency.pDatabase->pHeader->nAllocationCount;
	Result.nVirtualPages = m_Residency.pDatabase->pHeader->nPageCount;
	Result.nPhysicalCapacity = m_Residency.Buffers.nPhysicalPageSetCapacity;
	for ( uint32_t iSlot = 0u; iSlot < m_Residency.Buffers.nPhysicalPageSetCapacity; ++iSlot ) {
		const t3dVTPhysicalSlot_t &Slot = m_Residency.Buffers.pPhysicalPageSets[iSlot];
		const bool bResident = Slot.m_nVirtualPage != T3D_RW_VT_INVALID_PAGE;
		const bool bPending = ( Slot.m_nFlags & T3D_VT_SLOT_PENDING ) != 0u;
		const bool bRetiring = ( Slot.m_nFlags & T3D_VT_SLOT_RETIRED ) != 0u;
		Result.nPhysicalResident += bResident ? 1u : 0u;
		Result.nPhysicalFree += !bResident && !bPending ? 1u : 0u;
		Result.nPhysicalRetiring += bRetiring ? 1u : 0u;
		if ( !bResident || bRetiring ) {
			continue;
		}
		const uint16_t nCategory = t3d_vt_residency_category_reference_debug( *m_Residency.pDatabase, Slot.m_nVirtualPage );
		switch ( nCategory ) {
			case T3D_VT_RESIDENCY_CLUTTER:
				++Result.nResidentClutter;
				break;
			case T3D_VT_RESIDENCY_GROUND_COVER:
				++Result.nResidentGroundCover;
				break;
			case T3D_VT_RESIDENCY_OCCLUDER:
				++Result.nResidentOccluder;
				break;
			case T3D_VT_RESIDENCY_HLOD:
				++Result.nResidentHlod;
				break;
			case T3D_VT_RESIDENCY_TERRAIN:
			default:
				++Result.nResidentTerrain;
				break;
		}
	}
	const T3DVTGpuStats &GpuStats = m_Gpu.get_stats();
	Result.nRequestCapacity = m_Limits.nMaxVtRequests;
	Result.nRequestAttempts = GpuStats.nRequestAttempts;
	Result.nRequestUnique = GpuStats.nRequestUnique;
	Result.nRequestSameKey = GpuStats.nRequestSameKey;
	Result.nRequestHashOverflow = GpuStats.nRequestHashOverflow;
	Result.nRequestListOverflow = GpuStats.nRequestListOverflow;
	Result.nRequestDropped = GpuStats.nRequestHashOverflow + GpuStats.nRequestListOverflow;
	Result.nPageHits = m_Streaming.Stats.nPageHitCount;
	Result.nPageMisses = m_Streaming.Stats.nPageMissCount;
	Result.nParentFallbackHits = m_Residency.Stats.nAncestorFallbackCount;
	Result.nTailFallbackHits = m_Residency.Stats.nTailFallbackCount;
	Result.nResolverDepth0Samples = m_nResolverDepth0Samples;
	Result.nResolverDepth1Samples = m_nResolverDepth1Samples;
	Result.nResolverDepth2To3Samples = m_nResolverDepth2To3Samples;
	Result.nResolverDepth4PlusSamples = m_nResolverDepth4PlusSamples;
	Result.nResolverTailSamples = m_nResolverTailSamples;
	Result.nIoJobsPending = m_Streaming.Stats.nActiveJobCount;
	Result.nIoBytesTotal = m_Streaming.Stats.nIoBytesTotal;
	const godot::Time *const pTime = godot::Time::get_singleton();
	if ( pTime != NULL && m_nTelemetryStartUsec != 0u ) {
		const uint64_t nNowUsec = pTime->get_ticks_usec();
		const uint64_t nElapsedUsec = nNowUsec > m_nTelemetryStartUsec ? nNowUsec - m_nTelemetryStartUsec : 0u;
		if ( nElapsedUsec != 0u && Result.nIoBytesTotal <= UINT64_MAX / 1000000u ) {
			Result.nIoBytesPerSecond = Result.nIoBytesTotal * 1000000u / nElapsedUsec;
		}
	}
	Result.nUploadPageSetsFrame = m_nTelemetryUploadPageSetsFrame;
	Result.nUploadBytesFrame = m_nTelemetryUploadBytesFrame;
	Result.nUploadBytesTotal = m_Streaming.Stats.nUploadedBytes;
	Result.nEvictionsFrame = m_nTelemetryEvictionsFrame;
	Result.nEvictionsTotal = m_Residency.Stats.nEvictionCount;
	Result.nThrashCount = m_Residency.Stats.nThrashCount;
	if ( m_bHotSealed && m_pResources != NULL ) {
		const uint64_t nResourceCreates = m_pResources->get_stats().nGpuCreateCount;
		const uint64_t nHelperCreates = GpuStats.nGpuCreateCount;
		Result.nGpuResourceCreatesAfterSeal =
		        ( nResourceCreates > m_Stats.nPhysicalCacheCreateEpoch ? nResourceCreates - m_Stats.nPhysicalCacheCreateEpoch : 0u )
		        + ( nHelperCreates > m_nGpuHelperCreateEpoch ? nHelperCreates - m_nGpuHelperCreateEpoch : 0u );
	}
	return Result;
}

uint32_t Terrain3DVirtualTexture::stage_material_footprint_hot( const uint32_t nAllocationId,
        const uint32_t nFrameId,
        const float pUvMin[2],
        const float pUvMax[2],
        const uint16_t nWantedMip,
        const uint32_t nMaxRequests,
        const uint16_t nPriority,
        const T3DVTRequestProducer eProducer ) {
	if ( !m_bInitialized || !m_Streaming.bInitialized || m_pStagedRequests == NULL || nMaxRequests == 0u
	        || nAllocationId >= m_Residency.nAllocationCount || m_Residency.Buffers.pAllocations == NULL
	        || eProducer >= T3D_VT_REQUEST_PRODUCER_COUNT ) {
		return 0u;
	}
	if ( m_nStagedRequestFrame != nFrameId ) {
		m_nStagedRequestFrame = nFrameId;
		m_nStagedRequestCount = 0u;
	}
	const t3dVTAllocation_t &allocation = m_Residency.Buffers.pAllocations[nAllocationId];
	if ( !t3d_vt_allocation_quality_enabled( allocation.m_nFlags ) || allocation.m_nMipCount == 0u ) return 0u;
	const uint32_t nBaseMip = t3d_vt_allocation_quality_base_mip( allocation.m_nFlags );
	const uint16_t nAdjustedMip = ( uint16_t )( nBaseMip + nWantedMip < allocation.m_nMipCount ? nBaseMip + nWantedMip : allocation.m_nMipCount - 1u );
	T3DVTPredictedPageRect rect;
	if ( !t3d_vt_predict_page_rect_reference_debug( allocation, pUvMin, pUvMax, nAdjustedMip, nMaxRequests, rect ) ) {
		return 0u;
	}
	const uint32_t nAvailableFrameRequests =
	        m_nStagedRequestCount < m_nFrameRequestBudget ? m_nFrameRequestBudget - m_nStagedRequestCount : 0u;
	const uint32_t nRequestLimit = MIN( nMaxRequests, nAvailableFrameRequests );
	uint32_t nAttempted = 0u;
	for ( uint32_t y = rect.nMinPageY; y <= rect.nMaxPageY && nAttempted < nRequestLimit; ++y ) {
		for ( uint32_t x = rect.nMinPageX; x <= rect.nMaxPageX && nAttempted < nRequestLimit; ++x ) {
			uint32_t nVirtualPage = T3D_RW_VT_INVALID_PAGE;
			if ( !t3d_vt_allocation_page_id( allocation, rect.nMip, x, y, nVirtualPage ) ) {
				continue;
			}
			++nAttempted;
			if ( ( nFrameId & 63u ) == 0u ) {
				if ( m_nResolverTelemetrySampleFrame != nFrameId ) {
					m_nResolverTelemetrySampleFrame = nFrameId;
					m_nResolverTelemetrySamplesThisFrame = 0u;
				}
				if ( m_nResolverTelemetrySamplesThisFrame < 32u ) {
					++m_nResolverTelemetrySamplesThisFrame;
					t3dVTResolveResult_t Resolve = {};
					if ( t3d_vt_resolve_allocation_page_reference_debug(
					             m_Residency, nAllocationId, nVirtualPage, ( uint16_t )rect.nMip, Resolve ) ) {
						if ( ( Resolve.m_nFlags & T3D_VT_RESOLVE_TAIL ) != 0u ) {
							++m_nResolverTailSamples;
						} else if ( Resolve.m_nFallbackDepth == 0u ) {
							++m_nResolverDepth0Samples;
						} else if ( Resolve.m_nFallbackDepth == 1u ) {
							++m_nResolverDepth1Samples;
						} else if ( Resolve.m_nFallbackDepth <= 3u ) {
							++m_nResolverDepth2To3Samples;
						} else {
							++m_nResolverDepth4PlusSamples;
						}
					}
				}
			}
			// Visible resident pages still refresh the existing hot/warm hysteresis;
			// only misses enter the shared CPU admission phase.
			if ( t3d_vt_touch_page_hot( m_Residency, nVirtualPage, nFrameId ) ) {
				++m_Streaming.Stats.nPageHitCount;
				++m_Streaming.Stats.nResidentRequestCount;
				continue;
			}
			if ( m_nStagedRequestCount >= m_Limits.nMaxVtRequests ) {
				continue;
			}
			T3DVTStagedRequest &staged = m_pStagedRequests[m_nStagedRequestCount++];
			staged.Request.nVirtualPage = nVirtualPage;
			staged.Request.nWantedMip = ( uint16_t )rect.nMip;
			staged.Request.nPriority = nPriority;
			staged.nProducer = ( uint16_t )eProducer;
			staged.nReserved = 0u;
		}
	}
	return nAttempted;
}

bool Terrain3DVirtualTexture::flush_staged_material_requests_hot(
        const uint32_t nFrameId, uint32_t &rTerrainAccepted, uint32_t &rInstanceAccepted ) {
	rTerrainAccepted = 0u;
	rInstanceAccepted = 0u;
	if ( !m_bInitialized || !m_Streaming.bInitialized || m_pStagedRequests == NULL ) {
		return false;
	}
	if ( m_nStagedRequestFrame != nFrameId || m_nStagedRequestCount == 0u ) {
		return true;
	}
	// Admit both native producers through one owner-controlled phase. Interleave
	// them so a terrain-heavy frame cannot consume the whole admission budget
	// before visible instance materials get a chance to enter the same queue.
	uint32_t cursor[T3D_VT_REQUEST_PRODUCER_COUNT] = { 0u, 0u };
	uint32_t remaining[T3D_VT_REQUEST_PRODUCER_COUNT] = { 0u, 0u };
	for ( uint32_t i = 0u; i < m_nStagedRequestCount; ++i ) {
		const uint16_t nProducer = m_pStagedRequests[i].nProducer;
		if ( nProducer < T3D_VT_REQUEST_PRODUCER_COUNT ) {
			++remaining[nProducer];
		}
	}
	bool bProgress = true;
	while ( bProgress ) {
		bProgress = false;
		for ( uint32_t producer = 0u; producer < T3D_VT_REQUEST_PRODUCER_COUNT; ++producer ) {
			if ( remaining[producer] == 0u ) {
				continue;
			}
			while ( cursor[producer] < m_nStagedRequestCount
			        && m_pStagedRequests[cursor[producer]].nProducer != producer ) {
				++cursor[producer];
			}
			if ( cursor[producer] >= m_nStagedRequestCount ) {
				remaining[producer] = 0u;
				continue;
			}
			const T3DVTAdmissionStatus eStatus = t3d_vt_streaming_admit_request_hot(
			        m_Streaming, m_pStagedRequests[cursor[producer]].Request, nFrameId );
			if ( eStatus == T3D_VT_ADMISSION_ACCEPTED_NEW ) {
				if ( producer == T3D_VT_REQUEST_PRODUCER_TERRAIN ) {
					++rTerrainAccepted;
				} else {
					++rInstanceAccepted;
				}
			}
			++cursor[producer];
			--remaining[producer];
			bProgress = true;
		}
	}
	m_nStagedRequestCount = 0u;
	return true;
}

bool Terrain3DVirtualTexture::claim_stream_job_owner_hot( uint32_t &rJobIndex ) {
	return m_bInitialized && m_bHotSealed && t3d_vt_streaming_claim_job_owner_hot( m_Streaming, rJobIndex );
}

bool Terrain3DVirtualTexture::cancel_stream_job_owner_hot( const uint32_t pJobIndex ) {
	return m_bInitialized && t3d_vt_streaming_cancel_claim_owner_hot( m_Streaming, pJobIndex );
}

bool Terrain3DVirtualTexture::execute_stream_job_async( const uint32_t pJobIndex, T3DVTStreamReadDecodeFn pReadDecode, void *const pUser ) {
	return m_bInitialized && m_bHotSealed && t3d_vt_streaming_execute_job_async( m_Streaming, pJobIndex, pReadDecode, pUser );
}

uint64_t Terrain3DVirtualTexture::service_uploads_hot( const uint32_t pFrameId ) {
	if ( !m_bInitialized || !m_bHotSealed ) {
		return 0u;
	}
	const uint64_t nPageMisses = m_Streaming.Stats.nPageMissCount;
	const uint64_t nEvictionsBefore = m_Residency.Stats.nEvictionCount;
	const uint64_t nThrash = m_Residency.Stats.nThrashCount;
	const uint32_t nConfiguredPageSets = m_Streaming.Limits.nMaxPageSetsPerFrame;
	const uint32_t nCapacity = m_Residency.Buffers.nPhysicalPageSetCapacity;
	const uint32_t nResident = m_Residency.Stats.nResidentCount;
	const bool bQuiet = nPageMisses == m_nUploadCadenceLastPageMisses
	        && nEvictionsBefore == m_nUploadCadenceLastEvictions && nThrash == m_nUploadCadenceLastThrash;
	const bool bCacheWarm = nCapacity != 0u && ( uint64_t )nResident * 4u >= ( uint64_t )nCapacity * 3u;
	const bool bSmallBacklog = m_Streaming.nReadyJobCount <= nConfiguredPageSets;
	if ( bQuiet && bCacheWarm && bSmallBacklog ) {
		if ( m_nUploadCadenceStableFrames != UINT32_MAX ) {
			++m_nUploadCadenceStableFrames;
		}
	} else {
		m_nUploadCadenceStableFrames = 0u;
	}
	uint32_t nEffectivePageSets = nConfiguredPageSets;
	if ( m_nUploadCadenceStableFrames >= 16u && nEffectivePageSets > 2u ) {
		nEffectivePageSets = 2u;
	} else if ( m_nUploadCadenceStableFrames >= 8u && nEffectivePageSets > 4u ) {
		nEffectivePageSets = 4u;
	}
	m_Streaming.Limits.nMaxPageSetsPerFrame = nEffectivePageSets;
	const uint64_t nUploadedBytes = t3d_vt_streaming_service_uploads_batched_hot( m_Streaming,
	        pFrameId,
	        Terrain3DVTGpu::upload_batch_begin_callback,
	        Terrain3DVTGpu::upload_commit_callback,
	        Terrain3DVTGpu::upload_batch_end_callback,
	        &m_Gpu );
	m_Streaming.Limits.nMaxPageSetsPerFrame = nConfiguredPageSets;
	m_nUploadCadenceLastPageMisses = nPageMisses;
	m_nUploadCadenceLastEvictions = m_Residency.Stats.nEvictionCount;
	m_nUploadCadenceLastThrash = nThrash;
	const uint64_t nUploadedPageSets = m_Streaming.Stats.nUploadedPageCount;
	const uint64_t nEvictions = m_Residency.Stats.nEvictionCount;
	m_nTelemetryUploadPageSetsFrame =
	        nUploadedPageSets >= m_nTelemetryLastUploadedPageSets ? nUploadedPageSets - m_nTelemetryLastUploadedPageSets : 0u;
	m_nTelemetryUploadBytesFrame = nUploadedBytes;
	m_nTelemetryEvictionsFrame = nEvictions >= m_nTelemetryLastEvictions ? nEvictions - m_nTelemetryLastEvictions : 0u;
	m_nTelemetryLastUploadedPageSets = nUploadedPageSets;
	m_nTelemetryLastEvictions = nEvictions;
	return nUploadedBytes;
}

bool Terrain3DVirtualTexture::service_retired_slots_hot( const uint32_t pFrameId ) {
	return m_bInitialized && m_bHotSealed && m_Gpu.service_retired_slots_hot( m_Residency, pFrameId );
}

