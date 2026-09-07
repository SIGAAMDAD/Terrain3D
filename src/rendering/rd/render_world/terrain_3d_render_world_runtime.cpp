// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/render_world/terrain_3d_render_world_runtime.h"
#include "core/terrain_3d_sync.h"
#include "memory/terrain_3d_memory.h"

#include <math.h>
#include <string.h>
using namespace godot;

static uint64_t t3dRwAlign64( const uint64_t nValue ) {
	return ( nValue + 63u ) & ~63ull;
}

static Vector4 t3dRwNormalizePlane( const Vector4 &rPlane ) {
	const double fLengthSq = rPlane.x * rPlane.x + rPlane.y * rPlane.y + rPlane.z * rPlane.z;
	if ( fLengthSq <= 1.0e-20 ) {
		return rPlane;
	}
	return rPlane * ( 1.0 / sqrt( fLengthSq ) );
}

static void t3dRwCopyProjection( const Projection &rProjection, float *pDestination ) {
	for ( uint32_t nColumn = 0u; nColumn < 4u; ++nColumn ) {
		const Vector4 v = rProjection[nColumn];
		pDestination[nColumn * 4u + 0u] = ( float )v.x;
		pDestination[nColumn * 4u + 1u] = ( float )v.y;
		pDestination[nColumn * 4u + 2u] = ( float )v.z;
		pDestination[nColumn * 4u + 3u] = ( float )v.w;
	}
}

static void t3dRwExtractFrustum( const Projection &rProjection, float pPlanes[24] ) {
	const Vector4 c0 = rProjection[0];
	const Vector4 c1 = rProjection[1];
	const Vector4 c2 = rProjection[2];
	const Vector4 c3 = rProjection[3];
	const Vector4 row0( c0.x, c1.x, c2.x, c3.x );
	const Vector4 row1( c0.y, c1.y, c2.y, c3.y );
	const Vector4 row2( c0.z, c1.z, c2.z, c3.z );
	const Vector4 row3( c0.w, c1.w, c2.w, c3.w );
	const Vector4 planes[6] = {
		t3dRwNormalizePlane( row3 + row0 ),
		t3dRwNormalizePlane( row3 - row0 ),
		t3dRwNormalizePlane( row3 + row1 ),
		t3dRwNormalizePlane( row3 - row1 ),
		t3dRwNormalizePlane( row3 - row2 ),
		t3dRwNormalizePlane( row2 ),
	};
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		pPlanes[i * 4u + 0u] = ( float )planes[i].x;
		pPlanes[i * 4u + 1u] = ( float )planes[i].y;
		pPlanes[i * 4u + 2u] = ( float )planes[i].z;
		pPlanes[i * 4u + 3u] = ( float )planes[i].w;
	}
}

Terrain3DRenderWorldRuntime::Terrain3DRenderWorldRuntime() : m_ForwardLighting( Terrain3DRdContext::getShared() ) {
	memset( &m_Limits, 0, sizeof( m_Limits ) );
	memset( &m_Profile, 0, sizeof( m_Profile ) );
	memset( &m_NeutralDatabase, 0, sizeof( m_NeutralDatabase ) );
	memset( &m_NeutralHeader, 0, sizeof( m_NeutralHeader ) );
	memset( m_NeutralAllocations, 0, sizeof( m_NeutralAllocations ) );
	memset( m_NeutralPages, 0, sizeof( m_NeutralPages ) );
	memset( m_NeutralPayloads, 0, sizeof( m_NeutralPayloads ) );
	memset( m_NeutralPayloadBytes, 0, sizeof( m_NeutralPayloadBytes ) );
	memset( &m_Residency, 0, sizeof( m_Residency ) );
	memset( &m_Streaming, 0, sizeof( m_Streaming ) );
	memset( &m_TerrainStageA, 0, sizeof( m_TerrainStageA ) );
	memset( &m_SharedVTBindings, 0, sizeof( m_SharedVTBindings ) );
	m_pRuntimeAllocatorZone = NULL;
	m_pCpuImage = NULL;
	m_nCpuImageBytes = 0u;
	m_pPageTable = NULL;
	for ( uint32_t i = 0u; i < T3D_RW_VT_CHANNELS; ++i ) {
		m_pPhysicalSlots[i] = NULL;
	}
	m_pStreamJobs = NULL;
	m_pStreamScratch = NULL;
	m_pScratchLaneOwner = NULL;
	m_pOperationEvents = NULL;
	m_nOperationWriteIndex = 0u;
	m_nFramebufferFormat = RenderingDevice::INVALID_ID;
	m_bInitialized = false;
	m_bHiZEnabled = false;
	m_bScenePublished = false;
	m_nPublishedCellCount = 0u;
	m_nPublishedInstanceCount = 0u;
	m_nPublishedMeshCount = 0u;
	m_nPublishedLodCount = 0u;
	m_nPublishedClusterCount = 0u;
	m_nPublishedMaterialCount = 0u;
	m_nScenePublicationGeneration = 0u;
	m_nTerrainPublicationGeneration = 0u;
	m_preparedViewKey = t3dRenderWorldPreparedViewKey_t{};
	m_nPreparedStatusBits = 0u;
}

Terrain3DRenderWorldRuntime::~Terrain3DRenderWorldRuntime() {
	shutdown_cold();
}

void Terrain3DRenderWorldRuntime::record_operation( const uint64_t pFrameId,
        const uint32_t pType,
        const uint32_t pClass,
        const uint64_t pBytes,
        const uint32_t pCount,
        const uint32_t pResult,
        const uint32_t pSourceTag ) {
	if ( m_pOperationEvents == NULL || m_Limits.nOperationEventCapacity == 0u ) {
		return;
	}
	const uint32_t nSlot = m_nOperationWriteIndex % m_Limits.nOperationEventCapacity;
	T3DOperationEvent &event = m_pOperationEvents[nSlot];
	memset( &event, 0, sizeof( event ) );
	event.nFrameId = pFrameId;
	event.nBeginTicks = t3dSteadyClockUsec();
	event.nBytes = pBytes;
	event.nOperationType = pType;
	event.nOperationClass = pClass;
	event.nSubsystemId = 1u;
	event.nCount = pCount;
	event.nResultCode = pResult;
	event.nSourceTag = pSourceTag;
	++m_nOperationWriteIndex;
}

uint64_t Terrain3DRenderWorldRuntime::hash_visibility_frame_hot( const T3DVisibilityFrame &rFrame ) const {
	const uint8_t *const pBytes = ( const uint8_t * )( &rFrame );
	uint64_t nHash = 1469598103934665603ull;
	for ( size_t i = 0u; i < sizeof( rFrame ); ++i ) {
		nHash ^= ( uint64_t )( pBytes[i] );
		nHash *= 1099511628211ull;
	}
	return nHash == 0u ? 1u : nHash;
}

uint64_t Terrain3DRenderWorldRuntime::build_culling_generation_hot( void ) const {
	uint64_t nHash = 1469598103934665603ull;
	const uint64_t nValues[3] = {
		m_Resources.get_stats().nResourceGeneration,
		t3d_render_world_limits_signature( m_Limits ),
		m_bHiZEnabled ? 1u : 0u,
	};
	for ( uint32_t i = 0u; i < 3u; ++i ) {
		nHash ^= nValues[i];
		nHash *= 1099511628211ull;
	}
	return nHash == 0u ? 1u : nHash;
}

void Terrain3DRenderWorldRuntime::invalidate_prepared_view( void ) {
	m_preparedViewKey = t3dRenderWorldPreparedViewKey_t{};
	m_nPreparedStatusBits = 0u;
}

bool Terrain3DRenderWorldRuntime::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone ) {
	if ( !pZone || ( m_pRuntimeAllocatorZone && m_pRuntimeAllocatorZone != pZone ) || m_pCpuImage ) {
		return m_pRuntimeAllocatorZone == pZone && m_pCpuImage == NULL;
	}
	m_pRuntimeAllocatorZone = pZone;
	return true;
}

bool Terrain3DRenderWorldRuntime::allocate_cpu_image_cold() {
	if ( m_pCpuImage != NULL || !m_pRuntimeAllocatorZone ) {
		return false;
	}
	uint64_t nBytes = 0u;
	nBytes = t3dRwAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxVtVirtualPages * sizeof( T3DVTPageTableEntry );
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		const uint32_t nCapacity = c == T3D_RW_VT_ALBEDO ? m_Limits.nMaxVtPhysicalAlbedoPages
		        : c == T3D_RW_VT_NORMAL                  ? m_Limits.nMaxVtPhysicalNormalPages
		        : c == T3D_RW_VT_SURFACE                 ? m_Limits.nMaxVtPhysicalSurfacePages
		                                                 : m_Limits.nMaxVtPhysicalHeightPages;
		nBytes = t3dRwAlign64( nBytes ) + ( uint64_t )nCapacity * sizeof( T3DVTPhysicalSlot );
	}
	nBytes = t3dRwAlign64( nBytes ) + ( uint64_t )m_Limits.nMaxStreamJobs * sizeof( T3DVTStreamJob );
	nBytes = t3dRwAlign64( nBytes ) + ( uint64_t )m_Limits.nStreamScratchLaneCount * m_Limits.nStreamScratchBytesPerLane;
	nBytes = t3dRwAlign64( nBytes ) + ( uint64_t )m_Limits.nStreamScratchLaneCount * sizeof( uint32_t );
	nBytes = t3dRwAlign64( nBytes ) + ( uint64_t )m_Limits.nOperationEventCapacity * sizeof( T3DOperationEvent );
	if ( nBytes > SIZE_MAX ) {
		return false;
	}
	m_pCpuImage = m_pRuntimeAllocatorZone->allocate( ( size_t )nBytes, 64u );
	if ( m_pCpuImage == NULL ) {
		return false;
	}
	m_nCpuImageBytes = nBytes;
	memset( m_pCpuImage, 0, ( size_t )nBytes );

	uint8_t *pBase = ( uint8_t * )m_pCpuImage;
	uint64_t nOffset = 0u;
	nOffset = t3dRwAlign64( nOffset );
	m_pPageTable = ( T3DVTPageTableEntry * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxVtVirtualPages * sizeof( T3DVTPageTableEntry );
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		nOffset = t3dRwAlign64( nOffset );
		const uint32_t nCapacity = c == T3D_RW_VT_ALBEDO ? m_Limits.nMaxVtPhysicalAlbedoPages
		        : c == T3D_RW_VT_NORMAL                  ? m_Limits.nMaxVtPhysicalNormalPages
		        : c == T3D_RW_VT_SURFACE                 ? m_Limits.nMaxVtPhysicalSurfacePages
		                                                 : m_Limits.nMaxVtPhysicalHeightPages;
		m_pPhysicalSlots[c] = ( T3DVTPhysicalSlot * )( pBase + nOffset );
		nOffset += ( uint64_t )nCapacity * sizeof( T3DVTPhysicalSlot );
	}
	nOffset = t3dRwAlign64( nOffset );
	m_pStreamJobs = ( T3DVTStreamJob * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nMaxStreamJobs * sizeof( T3DVTStreamJob );
	nOffset = t3dRwAlign64( nOffset );
	m_pStreamScratch = pBase + nOffset;
	nOffset += ( uint64_t )m_Limits.nStreamScratchLaneCount * m_Limits.nStreamScratchBytesPerLane;
	nOffset = t3dRwAlign64( nOffset );
	m_pScratchLaneOwner = ( uint32_t * )( pBase + nOffset );
	nOffset += ( uint64_t )m_Limits.nStreamScratchLaneCount * sizeof( uint32_t );
	nOffset = t3dRwAlign64( nOffset );
	m_pOperationEvents = ( T3DOperationEvent * )( pBase + nOffset );
	return true;
}

void Terrain3DRenderWorldRuntime::release_cpu_image_cold() {
	if ( m_pCpuImage && m_pRuntimeAllocatorZone ) {
		m_pRuntimeAllocatorZone->deallocate( m_pCpuImage );
	}
	m_pCpuImage = NULL;
	m_nCpuImageBytes = 0u;
	m_pPageTable = NULL;
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		m_pPhysicalSlots[c] = NULL;
	}
	m_pStreamJobs = NULL;
	m_pStreamScratch = NULL;
	m_pScratchLaneOwner = NULL;
	m_pOperationEvents = NULL;
	m_nOperationWriteIndex = 0u;
}

bool Terrain3DRenderWorldRuntime::initialize_neutral_vt_cold() {
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
		T3DVTAllocationRecord &allocation = m_NeutralAllocations[c];
		allocation.nBaseVirtualPage = c;
		allocation.nPageCount = 1u;
		allocation.nMip0WidthPages = 1u;
		allocation.nMip0HeightPages = 1u;
		allocation.nMipCount = 1u;
		allocation.nChannel = ( uint16_t )c;
		allocation.nFlags = T3D_VT_PAGE_NEUTRAL | T3D_VT_PAGE_ROOT;
		allocation.nRootPage = c;
		T3DVTPageFileRecord &page = m_NeutralPages[c];
		page.nVirtualPage = c;
		page.nParentPage = T3D_RW_VT_INVALID_PAGE;
		page.nAllocationId = c;
		page.nPayloadId = c;
		page.nMip = 0u;
		page.nChannel = ( uint16_t )c;
		page.nFlags = T3D_VT_PAGE_NEUTRAL | T3D_VT_PAGE_ROOT;
		page.nGeneration = 1u;
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

	T3DVTResidencyBuffers residencyBuffers = {};
	residencyBuffers.pPageTable = m_pPageTable;
	residencyBuffers.nPageTableCapacity = m_Limits.nMaxVtVirtualPages;
	residencyBuffers.pPhysicalSlots[T3D_RW_VT_ALBEDO] = m_pPhysicalSlots[T3D_RW_VT_ALBEDO];
	residencyBuffers.pPhysicalSlots[T3D_RW_VT_NORMAL] = m_pPhysicalSlots[T3D_RW_VT_NORMAL];
	residencyBuffers.pPhysicalSlots[T3D_RW_VT_SURFACE] = m_pPhysicalSlots[T3D_RW_VT_SURFACE];
	residencyBuffers.pPhysicalSlots[T3D_RW_VT_HEIGHT] = m_pPhysicalSlots[T3D_RW_VT_HEIGHT];
	residencyBuffers.nPhysicalSlotCapacity[T3D_RW_VT_ALBEDO] = m_Limits.nMaxVtPhysicalAlbedoPages;
	residencyBuffers.nPhysicalSlotCapacity[T3D_RW_VT_NORMAL] = m_Limits.nMaxVtPhysicalNormalPages;
	residencyBuffers.nPhysicalSlotCapacity[T3D_RW_VT_SURFACE] = m_Limits.nMaxVtPhysicalSurfacePages;
	residencyBuffers.nPhysicalSlotCapacity[T3D_RW_VT_HEIGHT] = m_Limits.nMaxVtPhysicalHeightPages;
	if ( !t3d_render_world_vt_initialize_residency_cold( m_NeutralDatabase, residencyBuffers, 1u, m_Residency ) ) {
		return false;
	}

	T3DVTStreamingBuffers streamBuffers = {};
	streamBuffers.pJobs = m_pStreamJobs;
	streamBuffers.nJobCapacity = m_Limits.nMaxStreamJobs;
	streamBuffers.pScratchBytes = m_pStreamScratch;
	streamBuffers.nScratchLaneCount = m_Limits.nStreamScratchLaneCount;
	streamBuffers.nScratchLaneBytes = ( uint32_t )m_Limits.nStreamScratchBytesPerLane;
	streamBuffers.pScratchLaneOwner = m_pScratchLaneOwner;
	streamBuffers.pScratchLaneCompletion = m_ScratchLaneCompletion;
	for ( uint32_t i = 0u; i < m_Limits.nStreamScratchLaneCount; ++i ) {
		m_ScratchLaneCompletion[i].store( T3D_VT_STREAM_COMPLETION_IDLE, T3D_MEMORY_ORDER_RELAXED );
	}
	if ( !t3d_render_world_vt_streaming_initialize_cold( m_Residency, streamBuffers, m_Limits.nMaxUploadBytesPerFrame, m_Streaming ) ) {
		return false;
	}

	if ( !m_VT.upload_database_warm( m_NeutralDatabase, m_Residency ) ) {
		return false;
	}
	uint8_t neutralBytes[T3D_RW_VT_PHYSICAL_TILE * T3D_RW_VT_PHYSICAL_TILE * 4u] = {};
	for ( uint32_t c = 0u; c < T3D_RW_VT_CHANNELS; ++c ) {
		uint32_t nLayers = 0u;
		for ( uint32_t b = 0u; b < m_Resources.get_vt_bank_count( ( T3DRenderWorldVTChannel )c ); ++b ) {
			nLayers += m_Resources.get_vt_bank_layer_count( ( T3DRenderWorldVTChannel )c, b );
		}
		if ( nLayers == 0u ) {
			return false;
		}
		const uint64_t nLayerBytes64 = m_Resources.get_plan().nVtPhysicalBytes[c] / nLayers;
		if ( nLayerBytes64 == 0u || nLayerBytes64 > sizeof( neutralBytes ) ) {
			return false;
		}
		if ( !m_VT.upload_neutral_backing_warm( m_Residency, c, 0u, neutralBytes, ( uint32_t )nLayerBytes64 ) ) {
			return false;
		}
	}
	return t3d_render_world_vt_finalize_neutral_publication_warm( m_Residency );
}

bool Terrain3DRenderWorldRuntime::initialize_cold(
        Terrain3DRdContext &rContext, const t3dRdRenderWorldShaderBundle_t &rShaders, const int64_t pFramebufferFormat ) {
	if ( m_bInitialized || pFramebufferFormat == RenderingDevice::INVALID_ID ) {
		return false;
	}
	m_Limits = t3d_render_world_make_prototype_limits();
	m_Profile = t3d_render_world_make_development_gpu_profile();
	if ( !t3d_render_world_validate_limits( m_Limits ) || !t3d_render_world_validate_gpu_profile( m_Profile )
	        || !allocate_cpu_image_cold() ) {
		return false;
	}
	if ( !m_Resources.initialize_cold( rContext, m_Limits, m_Profile ) ) {
		shutdown_cold();
		return false;
	}
	if ( !m_Visibility.initialize_cold( &m_Resources, rShaders ) ) {
		shutdown_cold();
		return false;
	}
	if ( !m_VT.initialize_cold( &m_Resources, m_Limits, NULL, rShaders ) ) {
		shutdown_cold();
		return false;
	}
	if ( !m_Raster.initialize_cold( &m_Resources, rShaders ) ) {
		shutdown_cold();
		return false;
	}
	if ( !initialize_neutral_vt_cold() ) {
		shutdown_cold();
		return false;
	}
	T3DTerrainRenderWorldGenerations initialGenerations = {};
	if ( !t3d_terrain_render_world_stage_a_initialize_cold( initialGenerations, m_TerrainStageA ) ) {
		shutdown_cold();
		return false;
	}
	if ( !m_TerrainBridge.initialize_cold( &m_Resources, &m_VT, &m_Residency, &m_TerrainStageA ) ) {
		shutdown_cold();
		return false;
	}
	if ( !m_RenderWorld.initialize_cold( &m_Resources, &m_Visibility, NULL, &m_VT, &m_Raster, &m_Streaming, &m_Residency ) ) {
		shutdown_cold();
		return false;
	}
	m_RenderWorld.set_submission_enabled_cold( false );
	m_RenderWorld.seal_hot_runtime();
	m_Resources.seal_hot_runtime();
	m_nFramebufferFormat = pFramebufferFormat;
	m_bHiZEnabled = false;
	m_bInitialized = true;
	record_operation( 0u, T3D_OP_FALLBACK, T3D_OPCLASS_COLD, 0u, 1u, 0u, 0x48495A30u );
	record_operation( 0u, T3D_OP_PUBLISH, T3D_OPCLASS_WARM, 0u, T3D_RW_VT_CHANNELS, 0u, 0x56544E30u );
	return true;
}

void Terrain3DRenderWorldRuntime::shutdown_cold() {
	m_bInitialized = false;
	m_bHiZEnabled = false;
	release_scene_cold();
	m_RenderWorld.shutdown_cold();
	m_TerrainBridge.shutdown_cold();
	m_HiZ.shutdown_cold();
	m_ForwardLighting.shutdown();
	m_Raster.shutdown_cold();
	m_VT.shutdown_cold();
	m_Visibility.shutdown_cold();
	m_Resources.unseal_hot_runtime_cold();
	m_Resources.shutdown_cold();
	memset( &m_Residency, 0, sizeof( m_Residency ) );
	memset( &m_Streaming, 0, sizeof( m_Streaming ) );
	memset( &m_TerrainStageA, 0, sizeof( m_TerrainStageA ) );
	memset( &m_SharedVTBindings, 0, sizeof( m_SharedVTBindings ) );
	release_cpu_image_cold();
	m_nFramebufferFormat = RenderingDevice::INVALID_ID;
	m_nTerrainPublicationGeneration = 0u;
	invalidate_prepared_view();
}

bool Terrain3DRenderWorldRuntime::rebuild_raster_cold( const int64_t pFramebufferFormat ) {
	if ( !m_bInitialized || pFramebufferFormat == RenderingDevice::INVALID_ID ) {
		return false;
	}
	// Raster specialization is cached by the raster owner. Framebuffer changes do
	// not invalidate the fixed scene, geometry, VT, or visibility databases.
	m_nFramebufferFormat = pFramebufferFormat;
	return true;
}

bool Terrain3DRenderWorldRuntime::prepare_forward_cold( const t3dRdForwardPassContext_t &pContext ) {
	if ( !m_bInitialized || pContext.m_phase != T3D_RD_FORWARD_PHASE_PREPARE
	        || ( pContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && pContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION )
	        || pContext.m_nativePassMode != T3D_RD_FORWARD_NATIVE_PASS_COLOR || pContext.m_nViewCount != 1u ) {
		return false;
	}
	if ( pContext.m_bViewportReconfigured ) {
		m_ForwardLighting.invalidateBorrowedState();
	}
	if ( !m_Raster.prepare_forward_cold( pContext, m_ForwardLighting ) ) {
		return false;
	}
	m_nFramebufferFormat = pContext.m_nFramebufferFormat;
	record_operation( pContext.m_nRenderFrame, T3D_OP_GPU_CREATE, T3D_OPCLASS_COLD, 0u, 1u, 0u, 0x46575052u );
	return true;
}

bool Terrain3DRenderWorldRuntime::publish_terrain_generations_hot( const uint64_t pLogicalTopologyGeneration,
        const uint64_t pPhysicalPageTableGeneration,
        const uint64_t pClipmapPlacementGeneration,
        const uint64_t pMaterialGeneration,
        const uint64_t pLightingGeneration ) {
	if ( !m_bInitialized ) {
		return false;
	}
	uint64_t terrain_generation = 1469598103934665603ull;
	const uint64_t terrain_generations[5] = {
		pLogicalTopologyGeneration, pPhysicalPageTableGeneration, pClipmapPlacementGeneration, pMaterialGeneration, pLightingGeneration
	};
	for ( uint32_t i = 0u; i < 5u; ++i ) {
		terrain_generation ^= terrain_generations[i];
		terrain_generation *= 1099511628211ull;
	}
	if ( terrain_generation == 0u ) {
		terrain_generation = 1u;
	}
	if ( !t3d_terrain_render_world_stage_a_publish_terrain_hot( m_TerrainStageA,
	             pLogicalTopologyGeneration,
	             pPhysicalPageTableGeneration,
	             pClipmapPlacementGeneration,
	             pMaterialGeneration,
	             pLightingGeneration ) ) {
		return false;
	}
	if ( !t3d_terrain_render_world_stage_a_publish_vt_hot( m_TerrainStageA, m_Residency.nPageTableGeneration ) ) {
		return false;
	}
	if ( !t3d_terrain_render_world_stage_a_publish_resource_hot( m_TerrainStageA, m_Resources.get_stats().nResourceGeneration ) ) {
		return false;
	}
	const bool bPublished = m_TerrainBridge.refresh_shared_bindings_hot( m_SharedVTBindings );
	if ( bPublished ) {
		m_nTerrainPublicationGeneration = terrain_generation;
	}
	record_operation( 0u, T3D_OP_PUBLISH, T3D_OPCLASS_HOT, 0u, bPublished ? 1u : 0u, bPublished ? 0u : 1u, 0x5447454Eu );
	return bPublished;
}

bool Terrain3DRenderWorldRuntime::upload_scene_database_warm( const T3DGeometryDatabase &pGeometry,
        const T3DSceneCell *pCells,
        const uint32_t pCellCount,
        const T3DInstanceRecord *pInstances,
        const uint32_t pInstanceCount,
        const T3DMaterialRecord *pMaterials,
        const uint32_t pMaterialCount ) {
	if ( !m_bInitialized || pCellCount > m_Limits.nMaxCells || pInstanceCount > m_Limits.nMaxInstances
	        || pGeometry.nMeshCount > m_Limits.nMaxMeshes || pGeometry.nLodCount > m_Limits.nMaxMeshLods
	        || pGeometry.nClusterCount > m_Limits.nMaxClusters || pMaterialCount > m_Limits.nMaxMaterials ) {
		return false;
	}
	if ( ( pCellCount > 0u && pCells == NULL ) || ( pInstanceCount > 0u && pInstances == NULL )
	        || ( pMaterialCount > 0u && pMaterials == NULL ) ) {
		return false;
	}

	m_RenderWorld.set_submission_enabled_cold( false );
	T3DThreeTierUploadStats stats = {};
	if ( !t3d_render_world_upload_scene_database_warm(
	             m_Resources, pGeometry, pCells, pCellCount, pInstances, pInstanceCount, pMaterials, pMaterialCount, stats ) ) {
		return false;
	}
	m_nPublishedCellCount = pCellCount;
	m_nPublishedInstanceCount = pInstanceCount;
	m_nPublishedMeshCount = pGeometry.nMeshCount;
	m_nPublishedLodCount = pGeometry.nLodCount;
	m_nPublishedClusterCount = pGeometry.nClusterCount;
	m_nPublishedMaterialCount = pMaterialCount;
	m_nScenePublicationGeneration = m_nScenePublicationGeneration == UINT64_MAX ? 1u : m_nScenePublicationGeneration + 1u;
	m_bScenePublished = true;
	m_RenderWorld.set_submission_enabled_cold( true );
	record_operation( 0u, T3D_OP_GPU_UPLOAD, T3D_OPCLASS_WARM, stats.nUploadBytes, stats.nUploadCallCount, 0u, 0x53434E45u );
	record_operation( 0u, T3D_OP_PUBLISH, T3D_OPCLASS_WARM, 0u, pInstanceCount, 0u, 0x53434E50u );
	return true;
}

void Terrain3DRenderWorldRuntime::release_scene_cold() {
	m_RenderWorld.set_submission_enabled_cold( false );
	m_bScenePublished = false;
	m_nPublishedCellCount = 0u;
	m_nPublishedInstanceCount = 0u;
	m_nPublishedMeshCount = 0u;
	m_nPublishedLodCount = 0u;
	m_nPublishedClusterCount = 0u;
	m_nPublishedMaterialCount = 0u;
	m_nScenePublicationGeneration = m_nScenePublicationGeneration == UINT64_MAX ? 1u : m_nScenePublicationGeneration + 1u;
	invalidate_prepared_view();
	record_operation( 0u, T3D_OP_PUBLISH, T3D_OPCLASS_COLD, 0u, 0u, 0u, 0x53434E52u );
}

void Terrain3DRenderWorldRuntime::build_visibility_frame_hot( const t3dRdForwardPassContext_t &pContext,
        const uint32_t pCellCount,
        const uint32_t pInstanceCount,
        const uint32_t pMeshCount,
        const uint32_t pLodCount,
        const uint32_t pClusterCount,
        T3DVisibilityFrame &rFrame ) const {
	memset( &rFrame, 0, sizeof( rFrame ) );
	const Projection &view = pContext.m_szViewProjection[0];
	const Projection &previous = pContext.m_bTemporalHistoryValid ? pContext.m_szPreviousViewProjection[0] : view;
	t3dRwCopyProjection( view, rFrame.mViewProjection );
	t3dRwCopyProjection( previous, rFrame.mPreviousViewProjection );
	const Projection view_matrix( pContext.m_cameraTransform.affine_inverse() );
	t3dRwCopyProjection( view_matrix, rFrame.mViewMatrix );
	t3dRwExtractFrustum( view, rFrame.vFrustumPlanes );
	const Vector3 camera = pContext.m_cameraTransform.origin;
	rFrame.vCameraPositionAndMaxDistance[0] = ( float )camera.x;
	rFrame.vCameraPositionAndMaxDistance[1] = ( float )camera.y;
	rFrame.vCameraPositionAndMaxDistance[2] = ( float )camera.z;
	rFrame.vCameraPositionAndMaxDistance[3] = pContext.m_fCameraZFar > 0.0f ? pContext.m_fCameraZFar : 100000.0f;
	rFrame.vViewportAndMotion[0] = ( float )MAX( 1, pContext.m_renderSize.x );
	rFrame.vViewportAndMotion[1] = ( float )MAX( 1, pContext.m_renderSize.y );
	rFrame.vViewportAndMotion[2] = pContext.m_bTemporalHistoryValid ? 1.0f : 0.0f;
	rFrame.vViewportAndMotion[3] = pContext.m_bViewportReconfigured ? 1.0f : 0.0f;
	rFrame.vMotionJitter[0] = ( float )( pContext.m_taaJitter.x );
	rFrame.vMotionJitter[1] = ( float )( pContext.m_taaJitter.y );
	rFrame.vMotionJitter[2] = ( float )( pContext.m_previousTaaJitter.x );
	rFrame.vMotionJitter[3] = ( float )( pContext.m_previousTaaJitter.y );
	rFrame.nFrameId = ( uint32_t )pContext.m_nRenderFrame;
	rFrame.nFlags = pContext.m_bTemporalHistoryValid ? 1u : 0u;
	rFrame.nHiZInvalidReason = m_bHiZEnabled ? 0u : 1u;
	rFrame.nCellCount = MIN( pCellCount, m_Limits.nMaxCells );
	rFrame.nInstanceCount = MIN( pInstanceCount, m_Limits.nMaxInstances );
	rFrame.nMeshCount = MIN( pMeshCount, m_Limits.nMaxMeshes );
	rFrame.nLodCount = MIN( pLodCount, m_Limits.nMaxMeshLods );
	rFrame.nClusterCount = MIN( pClusterCount, m_Limits.nMaxClusters );
	rFrame.nVisibleCellCapacity = m_Limits.nMaxVisibleCells;
	rFrame.nVisibleInstanceCapacity = m_Limits.nMaxVisibleInstances;
	rFrame.nVisibleWorldCapacity = m_Limits.nMaxVisibleClusters[T3D_RW_TIER_WORLD];
	rFrame.nVisibleClutterCapacity = m_Limits.nMaxVisibleClusters[T3D_RW_TIER_CLUTTER];
	rFrame.nVisibleFoliageCapacity = m_Limits.nMaxVisibleClusters[T3D_RW_TIER_FOLIAGE];
	rFrame.nMaterialCount = MIN( m_nPublishedMaterialCount, m_Limits.nMaxMaterials );
	if ( m_Residency.pDatabase && m_Residency.pDatabase->pHeader ) {
		const uint64_t allocation_capacity = ( uint64_t )( m_Limits.nMaxMaterials ) * ( uint64_t )( T3D_RW_VT_CHANNELS );
		rFrame.nVtAllocationCount = ( uint32_t )MIN<uint64_t>( m_Residency.pDatabase->pHeader->nAllocationCount, allocation_capacity );
		rFrame.nVtPageCount = MIN( m_Residency.pDatabase->pHeader->nPageCount, m_Limits.nMaxVtVirtualPages );
	}
	rFrame.nHiZWidth = MIN( ( uint32_t )MAX( 1, pContext.m_renderSize.x ), m_Profile.nHiZWidth );
	rFrame.nHiZHeight = MIN( ( uint32_t )MAX( 1, pContext.m_renderSize.y ), m_Profile.nHiZHeight );
	rFrame.nHiZMipCount = 1u;
	uint32_t nWidth = rFrame.nHiZWidth;
	uint32_t nHeight = rFrame.nHiZHeight;
	while ( ( nWidth > 1u || nHeight > 1u ) && rFrame.nHiZMipCount < m_Profile.nHiZMipCount ) {
		if ( nWidth > 1u ) {
			nWidth >>= 1u;
		}
		if ( nHeight > 1u ) {
			nHeight >>= 1u;
		}
		++rFrame.nHiZMipCount;
	}
}

bool Terrain3DRenderWorldRuntime::render_main_camera_hot( const t3dRdForwardPassContext_t &pContext,
        const uint32_t pCellCount,
        const uint32_t pInstanceCount,
        const uint32_t pMeshCount,
        const uint32_t pLodCount,
        const uint32_t pClusterCount,
        T3DRenderWorldFrameResult &rResult ) {
	memset( &rResult, 0, sizeof( rResult ) );
	if ( !m_bInitialized || pContext.m_nDrawList < 0
	        || ( pContext.m_passKind != T3D_RD_FORWARD_PASS_OPAQUE && pContext.m_passKind != T3D_RD_FORWARD_PASS_MOTION ) ) {
		return false;
	}
	T3DVisibilityFrame frame = {};
	const uint32_t nCellCount = m_bScenePublished ? m_nPublishedCellCount : pCellCount;
	const uint32_t nInstanceCount = m_bScenePublished ? m_nPublishedInstanceCount : pInstanceCount;
	const uint32_t nMeshCount = m_bScenePublished ? m_nPublishedMeshCount : pMeshCount;
	const uint32_t nLodCount = m_bScenePublished ? m_nPublishedLodCount : pLodCount;
	const uint32_t nClusterCount = m_bScenePublished ? m_nPublishedClusterCount : pClusterCount;
	build_visibility_frame_hot( pContext, nCellCount, nInstanceCount, nMeshCount, nLodCount, nClusterCount, frame );

	t3dRenderWorldPreparedViewKey_t key;
	key.m_nRenderFrame = pContext.m_nRenderFrame;
	key.m_nViewportKey = pContext.m_nViewportKey;
	key.m_nViewStateHash = hash_visibility_frame_hot( frame );
	key.m_nSceneGeneration = m_nScenePublicationGeneration;
	key.m_nTerrainGeneration = m_nTerrainPublicationGeneration;
	key.m_nCullingGeneration = build_culling_generation_hot();
	key.m_nViewCount = pContext.m_nViewCount;
	key.m_bValid = true;

	const bool bReuse = m_preparedViewKey.matches( key );
	T3DRenderWorldFrameResult prepared = {};
	if ( bReuse ) {
		prepared.nStatusBits = m_nPreparedStatusBits;
		m_RenderWorld.record_compute_reuse_hot();
	} else {
		if ( !m_RenderWorld.prepare_camera_visibility_hot( m_Limits, frame, prepared ) ) {
			rResult = prepared;
			record_operation( pContext.m_nRenderFrame, T3D_OP_GPU_DISPATCH, T3D_OPCLASS_HOT, 0u, m_bHiZEnabled ? 4u : 3u, 1u, 0x56495330u );
			return false;
		}
		m_preparedViewKey = key;
		m_nPreparedStatusBits = prepared.nStatusBits;
		record_operation( pContext.m_nRenderFrame, T3D_OP_GPU_DISPATCH, T3D_OPCLASS_HOT, 0u, m_bHiZEnabled ? 4u : 3u, 0u, 0x56495330u );
		if ( prepared.nVTUploadBytes > 0u ) {
			record_operation( pContext.m_nRenderFrame, T3D_OP_GPU_UPLOAD, T3D_OPCLASS_HOT, prepared.nVTUploadBytes, 1u, 0u, 0x56545550u );
		}
	}

	rResult = prepared;
	const bool bRendered = m_RenderWorld.submit_camera_pass_hot( frame, pContext.m_nDrawList, rResult );
	record_operation( pContext.m_nRenderFrame, T3D_OP_GPU_DRAW, T3D_OPCLASS_HOT, 0u, rResult.nDrawCount, bRendered ? 0u : 1u, 0x44524157u );
	return bRendered;
}

bool Terrain3DRenderWorldRuntime::is_initialized() const {
	return m_bInitialized;
}
bool Terrain3DRenderWorldRuntime::is_hiz_enabled() const {
	return m_bInitialized && m_bHiZEnabled;
}
bool Terrain3DRenderWorldRuntime::has_scene() const {
	return m_bInitialized && m_bScenePublished;
}
uint64_t Terrain3DRenderWorldRuntime::get_cpu_image_bytes() const {
	return m_nCpuImageBytes;
}
uint32_t Terrain3DRenderWorldRuntime::get_operation_event_count() const {
	return MIN( m_nOperationWriteIndex, m_Limits.nOperationEventCapacity );
}
uint64_t Terrain3DRenderWorldRuntime::get_compute_prepare_count() const {
	return m_RenderWorld.get_stats().nComputePrepareCount;
}
uint64_t Terrain3DRenderWorldRuntime::get_compute_reuse_count() const {
	return m_RenderWorld.get_stats().nComputeReuseCount;
}
uint64_t Terrain3DRenderWorldRuntime::get_raster_submit_count() const {
	return m_RenderWorld.get_stats().nRasterSubmitCount;
}

uint32_t Terrain3DRenderWorldRuntime::copy_operation_events_reference_debug(
        T3DOperationEvent *pOutEvents, const uint32_t pCapacity ) const {
	if ( pOutEvents == NULL || pCapacity == 0u || m_pOperationEvents == NULL || m_Limits.nOperationEventCapacity == 0u ) {
		return 0u;
	}
	const uint32_t nAvailable = get_operation_event_count();
	const uint32_t nTake = nAvailable < pCapacity ? nAvailable : pCapacity;
	const uint32_t nStart = m_nOperationWriteIndex > nTake ? m_nOperationWriteIndex - nTake : 0u;
	for ( uint32_t i = 0u; i < nTake; ++i ) {
		const uint32_t nSource = ( nStart + i ) % m_Limits.nOperationEventCapacity;
		pOutEvents[i] = m_pOperationEvents[nSource];
	}
	return nTake;
}
const T3DRenderWorldLimits &Terrain3DRenderWorldRuntime::get_limits() const {
	return m_Limits;
}
const Terrain3DRenderWorldResourceImage &Terrain3DRenderWorldRuntime::get_resources() const {
	return m_Resources;
}
Terrain3DRenderWorld &Terrain3DRenderWorldRuntime::get_render_world() {
	return m_RenderWorld;
}
T3DTerrainRenderWorldStageAState &Terrain3DRenderWorldRuntime::get_stage_a_state() {
	return m_TerrainStageA;
}
