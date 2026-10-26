// Copyright ? 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "authoring/vt/build/terrain_3d_incremental_megavt_cooker.h"
#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "core/utility/terrain_3d_generation.h"
#include "core/utility/terrain_3d_numeric.h"
#include "core/utility/terrain_3d_checked_math.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <math.h>

static bool t3dMegaVTCaptureColors( const t3dMegaVTAuthoringInputs_t &rInputs, const Vector2i &rLocation, t3dMegaVTColorCapture_t &rCapture ) {
	Terrain3DRegionSource *const pData = &rInputs.m_source;
	if ( pData == NULL || !pData->ensureCpuRegionResident( rLocation ) ) { return false; }
	// Capture each map immediately: subsequent loads may evict the live region.
	for ( int z = -1; z <= 1; ++z ) {
		for ( int x = -1; x <= 1; ++x ) {
			t3dMegaVTColorSource_t &source = rCapture.m_neighbors[(z + 1) * 3 + x + 1];
			source = t3dMegaVTColorSource_t{};
			const Vector2i location = rLocation + Vector2i( x, z );
			if ( !pData->hasRegion( location ) ) { continue; }
			if ( !pData->ensureCpuRegionResident( location ) ) { return false; }
			Ref<Terrain3DRegion> region = pData->getRegion( location );
			if ( region.is_null() ) { return false; }
			const Ref<Image> color = region->getColorMap();
			if ( color.is_null() || color->is_empty() ) { continue; }
			if ( color->get_format() != Image::FORMAT_RGBA8 || color->get_width() <= 0 || color->get_height() <= 0 ) { return false; }
			source.m_nWidth = ( uint32_t )color->get_width();
			source.m_nHeight = ( uint32_t )color->get_height();
			source.m_bytes = color->get_data();
			if ( ( uint64_t )source.m_bytes.size() < ( uint64_t )source.m_nWidth * source.m_nHeight * 4u ) { return false; }
		}
	}
	return true;
}

static float t3dMegaVTColorTap( const t3dMegaVTColorCapture_t &rCapture, int x, int z, const uint32_t nComponent ) {
	const t3dMegaVTColorSource_t &center = rCapture.m_neighbors[4];
	if ( center.m_nWidth == 0u || center.m_nHeight == 0u ) { return nComponent == 3u ? 0.5f : 1.0f; }
	const int dx = x < 0 ? -1 : ( x >= ( int )center.m_nWidth ? 1 : 0 );
	const int dz = z < 0 ? -1 : ( z >= ( int )center.m_nHeight ? 1 : 0 );
	const t3dMegaVTColorSource_t &neighbor = rCapture.m_neighbors[(dz + 1) * 3 + dx + 1];
	const bool bNeighbor = neighbor.m_nWidth == center.m_nWidth && neighbor.m_nHeight == center.m_nHeight;
	const t3dMegaVTColorSource_t &source = bNeighbor ? neighbor : center;
	if ( bNeighbor ) { x -= dx * ( int )center.m_nWidth; z -= dz * ( int )center.m_nHeight; }
	x = CLAMP( x, 0, ( int )source.m_nWidth - 1 );
	z = CLAMP( z, 0, ( int )source.m_nHeight - 1 );
	return ( float )source.m_bytes.ptr()[((uint64_t)z * source.m_nWidth + x) * 4u + nComponent] / 255.0f;
}

static float t3dMegaVTColorSample( const t3dMegaVTColorCapture_t &rCapture, const float u, const float v, const uint32_t nComponent ) {
	const t3dMegaVTColorSource_t &center = rCapture.m_neighbors[4];
	if ( center.m_nWidth == 0u || center.m_nHeight == 0u ) { return nComponent == 3u ? 0.5f : 1.0f; }
	const float x = u * center.m_nWidth - 0.5f;
	const float z = v * center.m_nHeight - 0.5f;
	const int ix = ( int )floorf( x ), iz = ( int )floorf( z );
	const float fx = x - ix, fz = z - iz;
	const float a = t3dMegaVTColorTap( rCapture, ix, iz, nComponent );
	const float b = t3dMegaVTColorTap( rCapture, ix + 1, iz, nComponent );
	const float c = t3dMegaVTColorTap( rCapture, ix, iz + 1, nComponent );
	const float d = t3dMegaVTColorTap( rCapture, ix + 1, iz + 1, nComponent );
	return ( a + ( b - a ) * fx ) * ( 1.0f - fz ) + ( c + ( d - c ) * fx ) * fz;
}

static uint8_t t3dMegaVTLinearToSrgbByte( const float fValue ) {
	const float fSrgb = fValue <= 0.0031308f ? fValue * 12.92f : 1.055f * powf( fValue, 1.0f / 2.4f ) - 0.055f;
	return t3dPackUnorm8Nearest( fSrgb );
}

static bool t3dMegaVTBuildPixels( const t3dMegaVTColorCapture_t &rCapture, t3dMegaVTRegionPixels_t &rPixels, const t3dAtomicBool_t &rCancel ) {
	const uint32_t nExtent = rCapture.m_nExtent;
	if ( nExtent != 256u && nExtent != 512u ) { return false; }
	const bool bReuse = rPixels.m_nExtent == nExtent && rPixels.m_nRegionIndex == rCapture.m_nRegionIndex
	        && rPixels.m_albedo.size() == ( int64_t )nExtent * nExtent * 4u
	        && rPixels.m_pbr.size() == ( int64_t )(nExtent / 2u) * (nExtent / 2u) * 4u;
	const uint32_t nPages = nExtent / T3D_VT_PAGE_INTERIOR;
	const uint32_t nDirtyMask = bReuse ? rCapture.m_nDirtyMip0Mask : ( 1u << (nPages * nPages) ) - 1u;
	rPixels.m_nExtent = nExtent;
	rPixels.m_nRegionIndex = rCapture.m_nRegionIndex;
	if ( rPixels.m_albedo.resize( ( int64_t )nExtent * nExtent * 4u ) != OK
	        || rPixels.m_pbr.resize( ( int64_t )(nExtent / 2u) * (nExtent / 2u) * 4u ) != OK ) { return false; }
	// Albedo stores linear painted color via the V3 sRGB family. PBR's blue
	// channel stores painted wetness alpha; normal/AO remain semantic neutral.
	for ( uint32_t channel = 0u; channel < 2u; ++channel ) {
		const uint32_t nSide = channel == 0u ? nExtent : nExtent / 2u;
		uint8_t *const pPixels = channel == 0u ? rPixels.m_albedo.ptrw() : rPixels.m_pbr.ptrw();
		for ( uint32_t z = 0u; z < nSide; ++z ) {
			if ( rCancel.load( T3D_MEMORY_ORDER_ACQUIRE ) ) { return false; }
			for ( uint32_t x = 0u; x < nSide; ++x ) {
				const uint32_t nPage = (z * nExtent / nSide / T3D_VT_PAGE_INTERIOR) * nPages + x * nExtent / nSide / T3D_VT_PAGE_INTERIOR;
				if ( ( nDirtyMask & ( 1u << nPage ) ) == 0u ) { continue; }
				uint8_t *const pPixel = pPixels + ((uint64_t)z * nSide + x) * 4u;
				const float u = ( x + 0.5f ) / nSide, v = ( z + 0.5f ) / nSide;
				if ( channel == 0u ) {
					for ( uint32_t c = 0u; c < 3u; ++c ) { pPixel[c] = t3dMegaVTLinearToSrgbByte( t3dMegaVTColorSample( rCapture, u, v, c ) ); }
					pPixel[3] = 255u;
				} else {
					pPixel[0] = 128u; pPixel[1] = 128u;
					pPixel[2] = t3dPackUnorm8Nearest( t3dMegaVTColorSample( rCapture, u, v, 3u ) );
					pPixel[3] = 255u;
				}
			}
		}
	}
	return true;
}

static bool t3dMegaVTCookPixels( const t3dMegaVTColorCapture_t &rCapture, const t3dMegaVTRegionPixels_t &rPixels, PackedByteArray &rFragment, const uint64_t nByteLimit ) {
	t3dVTCookSparseSourceV3_t source{};
	source.m_nStableKey = t3d_vt_terrain_region_stable_key( rCapture.m_nRegionIndex );
	source.m_nGenerationSeed = rCapture.m_nGeneration;
	source.m_nConsumer = T3D_VT_CONSUMER_CLIPMAP;
	source.m_nWrapMode = T3D_VT_COOK_WRAP_CLAMP;
	source.m_nAllocationFlags = T3D_VT_ALLOCATION_MEGATEXTURE | T3D_VT_ALLOCATION_CLAMP | T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE;
	source.m_nAuthoredChannelMask = T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_ALBEDO ) | T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_PBR );
	const uint32_t nExtent = rPixels.m_nExtent;
	source.m_Channels[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = { rPixels.m_albedo.ptr(), nExtent, nExtent, 4u, nExtent * 4u };
	source.m_Channels[T3D_VT_LOGICAL_CHANNEL_PBR] = { rPixels.m_pbr.ptr(), nExtent / 2u, nExtent / 2u, 4u, nExtent * 2u };
	source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = t3dHashPublicationBytes( rPixels.m_albedo.ptr(), rPixels.m_albedo.size() );
	source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_PBR] = t3dHashPublicationBytes( rPixels.m_pbr.ptr(), rPixels.m_pbr.size() );
	return t3d_vt_godot_cook_source_fragment_v3( source, rFragment, nByteLimit );
}

Terrain3DIncrementalMegaVTCooker::~Terrain3DIncrementalMegaVTCooker( void ) {
	m_cancel.store( true, T3D_MEMORY_ORDER_RELEASE );
	if ( m_worker.joinable() ) { m_worker.join(); }
}

bool Terrain3DIncrementalMegaVTCooker::captureRegionCold( const t3dMegaVTAuthoringInputs_t &rInputs, const t3dTerrainMegaVTIncrementalCookRequest_t &rRequest ) {
	// Capture and worker storage must be admitted before taking Image snapshots.
	// Native owner-thread metadata is charged by its allocator separately.
	const uint64_t nCaptureLimit = 9u * ( ( uint64_t )T3D_REGION_SIZE * T3D_REGION_SIZE * 4u + 64u );
	const uint64_t nPayloadBytes = getRetainedPayloadBytesCold();
	const uint64_t nRetained = getRetainedCpuBytesCold();
	if ( nRetained > m_nCpuByteLimit || nCaptureLimit + m_nWorkerByteLimit > m_nCpuByteLimit - nRetained
	        || !m_cpuMemory.resize( nPayloadBytes + nCaptureLimit + m_nWorkerByteLimit ) ) { return false; }
	if ( !t3dContainerReserveCold( m_pixelCache, 8u ) ) { return false; }
	m_capture = t3dMegaVTColorCapture_t{};
	m_capture.m_nExtent = T3D_REGION_WORLD_SIZE_METERS * rRequest.m_base.m_nTexelsPerMeter;
	m_capture.m_nRegionIndex = m_ticket.m_nRegionMapIndex;
	m_capture.m_nGeneration = m_ticket.m_nGeneration;
	m_capture.m_nDirtyMip0Mask = rRequest.m_nDirtyMip0Mask;
	m_pixels = t3dMegaVTRegionPixels_t{};
	for ( size_t i = 0u; i < m_pixelCache.size(); ++i ) {
		if ( m_pixelCache[i].m_nRegionIndex == m_capture.m_nRegionIndex ) { m_pixels = m_pixelCache[i]; break; }
	}
	return t3dMegaVTCaptureColors( rInputs, Vector2i( rRequest.m_base.m_nRegionX, rRequest.m_base.m_nRegionZ ), m_capture );
}

void Terrain3DIncrementalMegaVTCooker::workerEntry( Terrain3DIncrementalMegaVTCooker *const pCooker ) {
	pCooker->cookCapturedRegion();
}

void Terrain3DIncrementalMegaVTCooker::cookCapturedRegion( void ) {
	m_bWorkerSucceeded = t3dMegaVTBuildPixels( m_capture, m_pixels, m_cancel )
	        && !m_cancel.load( T3D_MEMORY_ORDER_ACQUIRE ) && t3dMegaVTCookPixels( m_capture, m_pixels, m_workerFragment, m_nWorkerByteLimit - 2u * 512u * 512u * 4u );
	m_done.store( true, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DIncrementalMegaVTCooker::retireClaimCold( Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost, const String &rError ) {
	const bool bHadPending = !m_pendingFragment.is_empty();
	if ( !rError.is_empty() && m_ticket.m_nClaimId != 0u && m_ticket.m_nRegionMapIndex < T3D_REGION_SLOTS ) {
		m_nFailedSourceGenerations[m_ticket.m_nRegionMapIndex] = m_ticket.m_nGeneration;
	}
	( void )rMegaVT.cancelIncrementalCookHot( m_ticket );
	m_ticket = t3dTerrainMegaVTCookTicket_t{};
	m_workerFragment.clear(); m_pendingFragment.clear(); m_capture = t3dMegaVTColorCapture_t{}; m_pixels = t3dMegaVTRegionPixels_t{};
	trimCpuReservationCold();
	m_nPendingFingerprint = 0u;
	if ( bHadPending && !m_bForceActive ) { rHost.requestMegaVTPackageRecomposeCold(); }
	if ( m_bForceActive ) { m_bForceFailed = true; }
	if ( !rError.is_empty() ) {
		m_lastError = rError; ++m_nFailures;
	} else { ++m_nRejected; }
}

void Terrain3DIncrementalMegaVTCooker::finishWorkerCold( Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost ) {
	if ( !m_worker.joinable() || !m_done.load( T3D_MEMORY_ORDER_ACQUIRE ) ) { return; }
	m_worker.join();
	if ( !rMegaVT.validateIncrementalCookCompletionHot( m_ticket ) ) { retireClaimCold( rMegaVT, rHost, String() ); return; }
	if ( !m_bWorkerSucceeded || m_workerFragment.is_empty() ) { retireClaimCold( rMegaVT, rHost, "MegaVT region cook failed; dirty pages retained" ); return; }
	const uint64_t nRetained = getRetainedCpuBytesCold();
	if ( nRetained > m_nCpuByteLimit ) { retireClaimCold( rMegaVT, rHost, "MegaVT CPU budget rejected retained fragment; dirty pages retained" ); return; }
	if ( m_bForceActive ) {
		const uint32_t index = m_ticket.m_nRegionMapIndex;
		m_forceFragments[index] = m_workerFragment;
		m_forceTickets[index] = m_ticket;
		m_ticket = t3dTerrainMegaVTCookTicket_t{};
		m_workerFragment.clear(); m_capture = t3dMegaVTColorCapture_t{}; m_pixels = t3dMegaVTRegionPixels_t{};
		++m_nForceCookedCount;
		trimCpuReservationCold();
		return;
	}
	m_pendingFragment = m_workerFragment;
	m_workerFragment.clear();
	m_capture = t3dMegaVTColorCapture_t{};
	trimCpuReservationCold();
	m_nPublicationDeadlineUsec = t3dSteadyClockUsec() + 30000000u;
	rHost.requestMegaVTPackageRecomposeCold();
}

static String t3dMegaVTCacheDirectory( const t3dMegaVTAuthoringInputs_t &rInputs ) {
	String directory = rInputs.m_dataDirectory.path_join( ".vt_cache/terrain_regions" );
	if ( rInputs.m_dataDirectory.is_empty() ) {
		ProjectSettings *const pSettings = ProjectSettings::get_singleton();
		if ( pSettings == NULL ) { return String(); }
		directory = pSettings->globalize_path( "res://.godot/terrain3d/vt_cache/terrain_regions" );
	}
	return directory;
}

bool Terrain3DIncrementalMegaVTCooker::publishFragmentFileCold( const t3dMegaVTAuthoringInputs_t &rInputs ) {
	const String directory = t3dMegaVTCacheDirectory( rInputs );
	if ( directory.is_empty() ) { m_lastError = "MegaVT cache directory is unavailable"; return false; }
	const Error mkdirError = DirAccess::make_dir_recursive_absolute( directory );
	if ( mkdirError != OK ) { m_lastError = "MegaVT cache directory creation failed"; return false; }
	const String path = directory.path_join( String::num_int64( m_ticket.m_nRegionMapIndex ) + ".t3dvt" );
	return t3dPublishPackedFile( path, m_pendingFragment, &m_lastError ) == OK;
}

bool Terrain3DIncrementalMegaVTCooker::restoreCachedRegionCold( const t3dMegaVTAuthoringInputs_t &rInputs,
        const uint32_t nRegionIndex, const uint32_t nSourceGeneration, const uint32_t nDirtyMip0Mask, uint64_t &rRetainedBytes ) {
	if ( nRegionIndex >= T3D_REGION_SLOTS || m_bCacheRestoreAttempted[nRegionIndex] ) { return false; }
	// A miss is checked once per region lifecycle, never polled each frame.
	m_bCacheRestoreAttempted[nRegionIndex] = true;
	if ( nDirtyMip0Mask != 0u || nSourceGeneration == 0u ) { return false; }
	const String directory = t3dMegaVTCacheDirectory( rInputs );
	if ( directory.is_empty() ) { return false; }
	const String path = directory.path_join( String::num_int64( nRegionIndex ) + ".t3dvt" );
	if ( !FileAccess::file_exists( path ) ) { return false; }
	const Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
	const uint64_t nBytes = file.is_valid() ? file->get_length() : 0u;
	if ( nBytes < T3D_VT_PACK_V3_HEADER_BYTES || nBytes > m_nWorkerByteLimit
	        || rRetainedBytes > m_nCpuByteLimit || nBytes + 64u > m_nCpuByteLimit - rRetainedBytes ) {
		++m_nCacheRejected; return false;
	}
	if ( !m_cpuMemory.resize( getRetainedPayloadBytesCold() + nBytes + 64u ) ) {
		++m_nCacheRejected; m_lastError = "Total terrain memory budget rejected MegaVT cache fragment"; return false;
	}
	const PackedByteArray bytes = file->get_buffer( nBytes );
	t3dVTPackViewV3_t view{};
	if ( ( uint64_t )bytes.size() != nBytes || !t3d_vt_pack_open_view_v3( bytes.ptr(), nBytes, view )
	        || view.m_pHeader->m_nAllocationCount != 1u
	        || view.m_pAllocationKeys[0].m_nStableKey != t3d_vt_terrain_region_stable_key( nRegionIndex )
	        || view.m_pAllocations[0].m_nGeneration != nSourceGeneration
	        || view.m_pAllocations[0].m_nConsumer != T3D_VT_CONSUMER_CLIPMAP
	        || ( view.m_pAllocations[0].m_nFlags & ( T3D_VT_ALLOCATION_MEGATEXTURE | T3D_VT_ALLOCATION_CLAMP ) )
	                != ( T3D_VT_ALLOCATION_MEGATEXTURE | T3D_VT_ALLOCATION_CLAMP ) ) {
		++m_nCacheRejected; trimCpuReservationCold(); return false;
	}
	const t3dVTChannelAllocation_t &albedo = view.m_pChannelAllocations[T3D_VT_LOGICAL_CHANNEL_ALBEDO];
	const t3dVTChannelAllocation_t &pbr = view.m_pChannelAllocations[T3D_VT_LOGICAL_CHANNEL_PBR];
	const uint32_t nPages = T3D_REGION_WORLD_SIZE_METERS * m_nTexelsPerMeter / T3D_VT_PAGE_INTERIOR;
	if ( albedo.m_nMip0WidthPages != nPages || albedo.m_nMip0HeightPages != nPages
	        || pbr.m_nMip0WidthPages != nPages / 2u || pbr.m_nMip0HeightPages != nPages / 2u ) {
		++m_nCacheRejected; trimCpuReservationCold(); return false;
	}
	m_fragments[nRegionIndex] = bytes;
	rRetainedBytes += nBytes + 64u;
	++m_nCacheRestored;
	return true;
}

void Terrain3DIncrementalMegaVTCooker::restoreCachedRegionsCold( const t3dMegaVTAuthoringInputs_t &rInputs,
        const uint32_t nTexelsPerMeter ) {
	if ( nTexelsPerMeter != T3D_TERRAIN_MEGAVT_MIN_TEXELS_PER_METER
	        && nTexelsPerMeter != T3D_TERRAIN_MEGAVT_MAX_TEXELS_PER_METER ) { return; }
	m_nCpuByteLimit = rInputs.m_nCpuByteLimit;
	m_nTexelsPerMeter = nTexelsPerMeter;
	const uint64_t nRestoredBefore = m_nCacheRestored;
	const uint64_t nRejectedBefore = m_nCacheRejected;
	uint64_t nRetainedBytes = getRetainedCpuBytesCold();
	const TypedArray<Vector2i> locations = rInputs.m_source.getRegionLocations();
	for ( int64_t i = 0; i < locations.size(); ++i ) {
		const Vector2i location = locations[i];
		uint32_t index = 0u;
		if ( !Terrain3DTerrainMegaVT::locationToRegionIndex( location.x, location.y, index )
		        || m_bCacheRestoreAttempted[index] || !rInputs.m_source.ensureCpuRegionResident( location ) ) { continue; }
		const Ref<Terrain3DRegion> region = rInputs.m_source.getRegion( location );
		if ( region.is_null() || region->isDeleted() ) { continue; }
		( void )restoreCachedRegionCold( rInputs, index, ( uint32_t )region->getMegaVTSourceGeneration(),
		        ( uint32_t )region->getMegaVTDirtyMip0Mask(), nRetainedBytes );
	}
	UtilityFunctions::print( "[Terrain3D][Startup][MegaVT] CACHE RESTORE restored=", m_nCacheRestored - nRestoredBefore,
	        " rejected=", m_nCacheRejected - nRejectedBefore );
}

void Terrain3DIncrementalMegaVTCooker::finishPublicationCold( const t3dMegaVTAuthoringInputs_t &rInputs, Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost ) {
	if ( m_pendingFragment.is_empty() ) { return; }
	if ( !rMegaVT.validateIncrementalCookCompletionHot( m_ticket ) ) { retireClaimCold( rMegaVT, rHost, String() ); return; }
	const uint64_t nNow = t3dSteadyClockUsec();
	if ( m_nPendingFingerprint == 0u || rHost.publishedMegaVTGpuFingerprintCold() != m_nPendingFingerprint ) {
		if ( nNow >= m_nPublicationDeadlineUsec ) { retireClaimCold( rMegaVT, rHost, "MegaVT GPU publication failed or timed out; dirty pages retained" ); }
		return;
	}
	if ( nNow >= m_nPublicationDeadlineUsec ) { retireClaimCold( rMegaVT, rHost, "MegaVT cache publication timed out; dirty pages retained" ); return; }
	if ( !publishFragmentFileCold( rInputs ) ) {
		retireClaimCold( rMegaVT, rHost, "MegaVT cache write failed; waiting for a texture edit" ); return;
	}
	if ( !rMegaVT.acceptIncrementalCookCompletionHot( m_ticket ) ) { retireClaimCold( rMegaVT, rHost, String() ); return; }
	m_fragments[m_ticket.m_nRegionMapIndex] = m_pendingFragment;
	for ( size_t i = 0u; i < m_pixelCache.size(); ++i ) {
		if ( m_pixelCache[i].m_nRegionIndex == m_pixels.m_nRegionIndex ) { m_pixelCache.erase( m_pixelCache.begin() + i ); break; }
	}
	if ( m_pixelCache.size() == 8u ) { m_pixelCache.erase( m_pixelCache.begin() ); }
	m_pixelCache.push_back( m_pixels );
	m_ticket = t3dTerrainMegaVTCookTicket_t{}; m_pendingFragment.clear(); m_pixels = t3dMegaVTRegionPixels_t{};
	trimCpuReservationCold();
	m_nPendingFingerprint = 0u; m_lastError = String(); ++m_nAccepted;
	rHost.persistMegaVTEditorStateCold();
}

void Terrain3DIncrementalMegaVTCooker::queueRegionCold( const t3dMegaVTAuthoringInputs_t &rInputs, Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost ) {
	if ( m_worker.joinable() || !m_pendingFragment.is_empty() || m_bForceFailed
	        || ( m_bForceActive && m_nForceCookedCount == m_nForceRegionCount ) ) { return; }
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
		const uint32_t index = m_nNextRegion++ % T3D_REGION_SLOTS;
		if ( m_bForceActive && m_forceTickets[index].m_nClaimId != 0u ) { continue; }
		const t3dTerrainMegaVTRegion_t *const region = rMegaVT.getRegion( index );
		if ( region != NULL && m_nFailedSourceGenerations[index] != 0u
		        && m_nFailedSourceGenerations[index] == region->m_nGeneration ) { continue; }
		t3dTerrainMegaVTIncrementalCookRequest_t request{};
		if ( !rMegaVT.claimIncrementalCookHot( index, request, m_ticket ) ) { continue; }
		if ( !captureRegionCold( rInputs, request ) ) { retireClaimCold( rMegaVT, rHost, "MegaVT color source capture failed; dirty pages retained" ); return; }
		m_done.store( false, T3D_MEMORY_ORDER_RELEASE ); m_cancel.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bWorkerSucceeded = false;
		const uint64_t nRetained = getRetainedCpuBytesCold();
		if ( nRetained > m_nCpuByteLimit || m_nWorkerByteLimit > m_nCpuByteLimit - nRetained ) {
			retireClaimCold( rMegaVT, rHost, "MegaVT CPU budget rejected worker admission; dirty pages retained" ); return;
		}
		m_nPeakCpuByteEstimate = MAX( m_nPeakCpuByteEstimate, nRetained + m_nWorkerByteLimit );
		m_worker = Terrain3DThread( &Terrain3DIncrementalMegaVTCooker::workerEntry, this );
		trimCpuReservationCold();
		++m_nSubmitted; return;
	}
	// No new texture work: remain idle until the source publication changes.
	m_nIdlePublicationGeneration = rMegaVT.getPublicationGeneration();
}

void Terrain3DIncrementalMegaVTCooker::updateCold( const t3dMegaVTAuthoringInputs_t &rInputs, Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost ) {
	m_nCpuByteLimit = rInputs.m_nCpuByteLimit;
	if ( !rMegaVT.isReady() ) { return; }
	if ( m_nObservedTopologyGeneration != rMegaVT.getTopologyGeneration() ) {
		m_nObservedTopologyGeneration = rMegaVT.getTopologyGeneration();
		retryDirtyRegionsCold();
	}
	if ( m_bForceRequested ) {
		// Drain the old worker safely. Its unpublished candidate can be retired:
		// the force batch replaces it while retaining the last usable fragments.
		finishWorkerCold( rMegaVT, rHost );
		if ( m_worker.joinable() ) { return; }
		if ( !m_pendingFragment.is_empty() ) {
			( void )rMegaVT.cancelIncrementalCookHot( m_ticket );
			m_ticket = t3dTerrainMegaVTCookTicket_t{}; m_pendingFragment.clear();
		}

		m_bForceRequested = false; m_bForceActive = true; m_bForceFailed = false;
		m_nForceCookedCount = 0u;
		m_nForceRegionCount = rMegaVT.requestForceRecookAllRegionsCold();
		for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
			const auto *region = rMegaVT.getRegion( i );
			m_forceSourceGenerations[i] = region != NULL ? region->m_nGeneration : 0u;
		}
		retryDirtyRegionsCold();
		for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) { m_bCacheRestoreAttempted[i] = true; }
		rHost.persistMegaVTEditorStateCold();
	}
	if ( m_bForceActive && ( m_bForceFailed || m_nForceCookedCount == m_nForceRegionCount ) ) { return; }
	if ( !m_worker.joinable() && m_pendingFragment.is_empty()
	        && m_nTexelsPerMeter == rMegaVT.getTexelsPerMeter()
	        && m_nIdlePublicationGeneration == rMegaVT.getPublicationGeneration() ) { return; }
	if ( m_nTexelsPerMeter != rMegaVT.getTexelsPerMeter() ) {
		memset( m_nFailedSourceGenerations, 0, sizeof( m_nFailedSourceGenerations ) );
		m_nTexelsPerMeter = rMegaVT.getTexelsPerMeter();
		for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) { m_fragments[i].clear(); }
		memset( m_bCacheRestoreAttempted, 0, sizeof( m_bCacheRestoreAttempted ) );
		m_pixelCache.clear();
		rHost.requestMegaVTPackageRecomposeCold();
	}
	bool bRecompose = false, bDirty = false;
	uint64_t nRestoreRetainedBytes = 0u;
	bool bRestoreBudgetReady = false;
	for ( uint32_t index = 0u; index < T3D_REGION_SLOTS; ++index ) {
		const t3dTerrainMegaVTRegion_t *const region = rMegaVT.getRegion( index );
		if ( region == NULL || ( region->m_nFlags & T3D_TERRAIN_MEGAVT_REGION_PRESENT ) == 0u ) {
			if ( !m_fragments[index].is_empty() ) { m_fragments[index].clear(); bRecompose = true; }
			m_bCacheRestoreAttempted[index] = false;
			m_nFailedSourceGenerations[index] = 0u;
			continue;
		}
		uint32_t generation = 0u, mask = 0u;
		( void )rMegaVT.getPersistentEditorState( index, generation, mask );
		if ( mask == 0u && m_fragments[index].is_empty() && m_ticket.m_nClaimId == 0u ) {
			if ( !m_bCacheRestoreAttempted[index] ) {
				if ( !bRestoreBudgetReady ) { nRestoreRetainedBytes = getRetainedCpuBytesCold(); bRestoreBudgetReady = true; }
				if ( restoreCachedRegionCold( rInputs, index, generation, mask, nRestoreRetainedBytes ) ) { bRecompose = true; continue; }
			}
			// Missing derived data is not an authored edit. Preserve the saved source
			// generation and avoid invalidating already-cached gutter neighbors.
			const uint32_t nMip0Pages = ( uint32_t )region->m_nMip0WidthPages * region->m_nMip0HeightPages;
			const uint32_t nDirtyMask = nMip0Pages >= 32u ? UINT32_MAX : ( 1u << nMip0Pages ) - 1u;
			bDirty |= rMegaVT.importPersistentEditorStateHot( index, generation, nDirtyMask );
		}
	}
	if ( bDirty ) { rHost.persistMegaVTEditorStateCold(); }
	if ( bRecompose ) { rHost.requestMegaVTPackageRecomposeCold(); }
	finishWorkerCold( rMegaVT, rHost );
	finishPublicationCold( rInputs, rMegaVT, rHost );
	trimCpuReservationCold();
	queueRegionCold( rInputs, rMegaVT, rHost );
}

void Terrain3DIncrementalMegaVTCooker::retryDirtyRegionsCold( void ) {
	memset( m_nFailedSourceGenerations, 0, sizeof( m_nFailedSourceGenerations ) );
	m_nIdlePublicationGeneration = UINT64_MAX;
}

void Terrain3DIncrementalMegaVTCooker::shutdownCold( Terrain3DTerrainMegaVT &rMegaVT ) {
	m_cancel.store( true, T3D_MEMORY_ORDER_RELEASE );
	if ( m_worker.joinable() ) { m_worker.join(); }
	( void )rMegaVT.cancelIncrementalCookHot( m_ticket );
	m_ticket = t3dTerrainMegaVTCookTicket_t{}; m_capture = t3dMegaVTColorCapture_t{}; m_pixels = t3dMegaVTRegionPixels_t{};
	m_workerFragment.clear(); m_pendingFragment.clear();
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) { m_fragments[i].clear(); }
	memset( m_bCacheRestoreAttempted, 0, sizeof( m_bCacheRestoreAttempted ) );
	m_nTexelsPerMeter = 0u;
	m_pixelCache.clear();
	m_nPendingFingerprint = 0u; m_nIdlePublicationGeneration = UINT64_MAX;
	m_nObservedTopologyGeneration = UINT64_MAX;
	memset( m_nFailedSourceGenerations, 0, sizeof( m_nFailedSourceGenerations ) );
	m_cpuMemory.reset();
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
		( void )rMegaVT.cancelIncrementalCookHot( m_forceTickets[i] );
		m_forceTickets[i] = t3dTerrainMegaVTCookTicket_t{}; m_forceFragments[i].clear();
	}
	m_bForceActive = false; m_bForceRequested = false; m_bForceFailed = false;
}

bool Terrain3DIncrementalMegaVTCooker::composePackageCold( PackedByteArray &rPackage, uint32_t &rGeneration, uint64_t &rFingerprint,
        Terrain3DSharedMemoryReservation *const pPackageMemory ) {
	eastl::vector<PackedByteArray, Terrain3DEastlAllocator> packages;
	if ( !t3dContainerReserveCold( packages, T3D_REGION_SLOTS + 1u ) ) {
		m_lastError = "MegaVT package list allocation failed"; return false;
	}
	if ( !rPackage.is_empty() ) { packages.push_back( rPackage ); }
	uint64_t nSignature = rGeneration;
	for ( uint32_t index = 0u; index < T3D_REGION_SLOTS; ++index ) {
		const PackedByteArray &fragment = isForceRecookReadyCold() && !m_forceFragments[index].is_empty() ? m_forceFragments[index] : !m_pendingFragment.is_empty() && m_ticket.m_nRegionMapIndex == index ? m_pendingFragment : m_fragments[index];
		if ( fragment.is_empty() ) { continue; }
		packages.push_back( fragment );
		nSignature ^= t3dHashPublicationBytes( fragment.ptr(), fragment.size() );
	}
	if ( packages.size() <= ( rPackage.is_empty() ? 0u : 1u ) ) { return true; }
	const uint32_t nGeneration = t3dNextGeneration( ( uint32_t )( nSignature ^ ( nSignature >> 32u ) ) );
	PackedByteArray merged;
	Terrain3DSharedMemoryReservation mergedMemory;
	const uint64_t nRetained = getRetainedCpuBytesCold() + ( uint64_t )rPackage.size();
	m_nCompositionWorkingBytes = 0u;
	m_nCompositionWorkingLimit = nRetained < m_nCpuByteLimit ? m_nCpuByteLimit - nRetained : 0u;
	Terrain3DMemoryAccount *const pAccount = g_t3dMemoryAccount;
	if ( pAccount != NULL ) {
		// The quarter-budget ceiling bounds retained cache and worker growth.
		// Composition is transient: it can borrow unused shared headroom instead
		// of requiring the entire package to fit that same quarter a second time.
		const auto stats = pAccount->stats();
		m_nCompositionWorkingLimit = MIN(t3dMemoryCompositionLimit(pAccount), stats.limit - stats.committed);
	}
	if ( pAccount == NULL && nRetained >= m_nCpuByteLimit ) {
		m_lastError = "MegaVT retained CPU bytes " + String::num_uint64( nRetained )
		        + " exceed composition ceiling " + String::num_uint64( m_nCpuByteLimit ); return false;
	}
	t3dVTMergeStatistics_t statistics;
	const uint64_t nStart = t3dSteadyClockUsec();
	const bool bMerged = t3d_vt_godot_merge_packages_v3( packages.data(), ( uint32_t )packages.size(), nGeneration, merged,
	        m_nCompositionWorkingLimit, &statistics, &mergedMemory );
	m_nMergeUsec = t3dSteadyClockUsec() - nStart;
	m_nPayloadComparisons = statistics.m_nPayloadComparisons;
	m_nCompositionWorkingBytes = statistics.m_nWorkingBytes;
	m_nPeakCpuByteEstimate = MAX( m_nPeakCpuByteEstimate, nRetained + statistics.m_nWorkingBytes );
	if ( !bMerged ) {
		if ( statistics.m_bWorkingLimitExceeded ) {
			if ( pAccount != NULL ) { pAccount->recordDenial(); }
			m_lastError = "MegaVT composition requires working bytes " + String::num_uint64( statistics.m_nWorkingBytes )
			        + "; available " + String::num_uint64( m_nCompositionWorkingLimit );
		} else { m_lastError = "MegaVT package composition failed (invalid input or shared memory admission denied)"; }
		return false;
	}
	m_lastError = String();
	rPackage = merged; rGeneration = nGeneration;
	if ( pPackageMemory != NULL ) { *pPackageMemory = mergedMemory; }
	rFingerprint = t3dHashPublicationBytes( merged.ptr(), merged.size() );
	if ( rFingerprint == 0u ) { rFingerprint = 1u; }
	if ( !m_pendingFragment.is_empty() ) { m_nPendingFingerprint = rFingerprint; }
	return true;
}

struct t3dMegaVTRetainedByteCounter_t {
	const uint8_t *m_seen[T3D_REGION_SLOTS * 2u + 32u] = {};
	size_t m_nSeen = 0u;
	uint64_t m_nBytes = 0u;
	void add( const PackedByteArray &rBytes ) {
		if ( rBytes.is_empty() ) { return; }
		const uint8_t *const pBytes = rBytes.ptr();
		for ( size_t i = 0u; i < m_nSeen; ++i ) { if ( m_seen[i] == pBytes ) { return; } }
		m_seen[m_nSeen++] = pBytes;
		// Godot exposes size, not backing capacity. Include allocation/header
		// slack and report this as an estimate; aliases count only once.
		m_nBytes += ( uint64_t )rBytes.size() + 64u;
	}
};

size_t Terrain3DIncrementalMegaVTCooker::getRetainedPayloadBytesCold( void ) const {
	t3dMegaVTRetainedByteCounter_t counter;
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) { counter.add( m_fragments[i] ); counter.add( m_forceFragments[i] ); }
	for ( size_t i = 0u; i < m_pixelCache.size(); ++i ) {
		counter.add( m_pixelCache[i].m_albedo ); counter.add( m_pixelCache[i].m_pbr );
	}
	// Capture bytes are immutable until join. Mutable worker storage is covered
	// by its admitted reservation rather than read concurrently for diagnostics.
	for ( uint32_t i = 0u; i < 9u; ++i ) { counter.add( m_capture.m_neighbors[i].m_bytes ); }
	if ( m_worker.joinable() ) { counter.m_nBytes += m_nWorkerByteLimit; }
	else {
		counter.add( m_pixels.m_albedo ); counter.add( m_pixels.m_pbr );
		counter.add( m_workerFragment );
	}
	counter.add( m_pendingFragment );
	return counter.m_nBytes > SIZE_MAX ? SIZE_MAX : ( size_t )counter.m_nBytes;
}

size_t Terrain3DIncrementalMegaVTCooker::getRetainedCpuBytesCold( void ) const {
	return getRetainedPayloadBytesCold() + sizeof( *this ) + m_pixelCache.capacity() * sizeof( t3dMegaVTRegionPixels_t );
}

void Terrain3DIncrementalMegaVTCooker::trimCpuReservationCold( void ) {
	// Call only after releasing bytes or after a previously admitted transition.
	// A live worker keeps its bounded allowance until it has joined.
	( void )m_cpuMemory.resize( getRetainedPayloadBytesCold() );
}

Dictionary Terrain3DIncrementalMegaVTCooker::getDiagnosticsCold( void ) const {
	Dictionary result;
	result["worker_active"] = m_worker.joinable();
	result["publication_pending"] = !m_pendingFragment.is_empty();
	result["submitted"] = ( int64_t )m_nSubmitted;
	result["accepted"] = ( int64_t )m_nAccepted;
	result["stale_rejected"] = ( int64_t )m_nRejected;
	result["failures"] = ( int64_t )m_nFailures;
	result["last_error"] = m_lastError;
	result["pixel_cache_regions"] = ( int64_t )m_pixelCache.size();
	result["cache_restored_regions"] = ( int64_t )m_nCacheRestored;
	result["cache_rejected_regions"] = ( int64_t )m_nCacheRejected;
	result["retained_cpu_bytes"] = ( int64_t )getRetainedCpuBytesCold();
	result["reserved_cpu_bytes"] = ( int64_t )m_cpuMemory.bytes();
	result["cpu_byte_limit"] = ( int64_t )m_nCpuByteLimit;
	result["peak_cpu_byte_estimate"] = ( int64_t )m_nPeakCpuByteEstimate;
	result["composition_usec"] = ( int64_t )m_nMergeUsec;
	result["payload_comparisons"] = ( int64_t )m_nPayloadComparisons;
	result["composition_required_working_bytes"] = ( int64_t )m_nCompositionWorkingBytes;
	result["composition_working_byte_limit"] = ( int64_t )m_nCompositionWorkingLimit;
	return result;
}

bool Terrain3DIncrementalMegaVTCooker::appendRuntimeRegionsCold( const t3dMegaVTAuthoringInputs_t &rInputs, PackedByteArray &rPackage, uint32_t &rGeneration, String &rError ) {
	// The CPU limit is a ceiling. Charge retained Godot buffers by their actual
	// size; native cook/merge scratch is charged separately by its allocator.
	Terrain3DMemoryReservation buffer_memory;
	Terrain3DRegionSource *const pData = &rInputs.m_source;
	if ( pData == NULL ) { rError = "Terrain MegaVT export has no data source"; return false; }
	const TypedArray<Vector2i> locations = pData->getRegionLocations();
	eastl::vector<PackedByteArray, Terrain3DEastlAllocator> packages;
	if ( !t3dContainerReserveCold( packages, ( size_t )locations.size() + 1u ) ) {
		rError = "Terrain MegaVT export package list allocation failed"; return false;
	}
	if ( !rPackage.is_empty() ) { packages.push_back( rPackage ); }
	t3dAtomicBool_t cancel{ false };
	uint64_t nSignature = rGeneration;
	uint64_t nRetainedBytes = rPackage.is_empty() ? 0u : ( uint64_t )rPackage.size() + 64u;
	if (!locations.is_empty() && !buffer_memory.resize(nRetainedBytes)) { rError = "Total terrain memory budget rejected MegaVT export inputs"; return false; }
	for ( int64_t i = 0; i < locations.size(); ++i ) {
		const Vector2i location = locations[i];
		t3dMegaVTColorCapture_t capture{};
		if ( !Terrain3DTerrainMegaVT::locationToRegionIndex( location.x, location.y, capture.m_nRegionIndex ) ) { rError = "Terrain MegaVT export location is outside the world"; return false; }
		capture.m_nExtent = T3D_REGION_WORLD_SIZE_METERS * T3D_TERRAIN_MEGAVT_DEFAULT_TEXELS_PER_METER;
		capture.m_nGeneration = 1u;
		capture.m_nDirtyMip0Mask = T3D_TERRAIN_MEGAVT_ALL_MIP0_PAGES_MASK;
		t3dMegaVTRegionPixels_t pixels;
		PackedByteArray fragment;
		uint64_t nCaptureBytes = 0u;
		if ( !t3dMegaVTCaptureColors( rInputs, location, capture ) ) { rError = "Terrain MegaVT source capture failed"; return false; }
		for ( uint32_t n = 0u; n < 9u; ++n ) {
			if (!capture.m_neighbors[n].m_bytes.is_empty()) { nCaptureBytes += ( uint64_t )capture.m_neighbors[n].m_bytes.size() + 64u; }
		}
		const uint64_t nPixelBytes = 2u * 512u * 512u * 4u;
		if ( nRetainedBytes > rInputs.m_nCpuByteLimit || nCaptureBytes + nPixelBytes > rInputs.m_nCpuByteLimit - nRetainedBytes ) { rError = "Terrain MegaVT export CPU budget exceeded"; return false; }
		if (!buffer_memory.resize(nRetainedBytes + nCaptureBytes + nPixelBytes)) { rError = "Total terrain memory budget rejected MegaVT export buffers"; return false; }
		const uint64_t nAvailableBytes = rInputs.m_nCpuByteLimit - nRetainedBytes - nCaptureBytes - nPixelBytes;
		if ( !t3dMegaVTBuildPixels( capture, pixels, cancel )
		        || !t3dMegaVTCookPixels( capture, pixels, fragment, nAvailableBytes ) ) { rError = "Terrain MegaVT region export failed"; return false; }
		nSignature ^= t3dHashPublicationBytes( fragment.ptr(), fragment.size() );
		packages.push_back( fragment );
		nRetainedBytes += ( uint64_t )fragment.size() + 64u;
		if (!buffer_memory.resize(nRetainedBytes + nCaptureBytes + nPixelBytes)) { rError = "Total terrain memory budget rejected MegaVT export fragment"; return false; }
		// Release the snapshots/pixels before admitting the next region or merge.
		capture = t3dMegaVTColorCapture_t{}; pixels = t3dMegaVTRegionPixels_t{};
		if (!buffer_memory.resize(nRetainedBytes)) { rError = "Total terrain memory budget rejected retained MegaVT export fragments"; return false; }
	}
	if ( locations.is_empty() ) { return true; }
	const uint32_t nGeneration = t3dNextGeneration( ( uint32_t )(nSignature ^ (nSignature >> 32u)) );
	PackedByteArray merged;
	if ( nRetainedBytes >= rInputs.m_nCpuByteLimit || !t3d_vt_godot_merge_packages_v3( packages.data(), ( uint32_t )packages.size(), nGeneration, merged, rInputs.m_nCpuByteLimit - nRetainedBytes ) ) { rError = "Terrain MegaVT export composition failed"; return false; }
	rPackage = merged; rGeneration = nGeneration; return true;
}

bool Terrain3DIncrementalMegaVTCooker::finishForceRecookAllRegionsCold( const t3dMegaVTAuthoringInputs_t &rInputs,
        Terrain3DTerrainMegaVT &rMegaVT, Terrain3DMegaVTCookHost &rHost, const bool bCommit ) {
	bool success = bCommit && isForceRecookReadyCold();
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS && success; ++i ) {
		if ( m_forceTickets[i].m_nClaimId != 0u && !rMegaVT.validateIncrementalCookCompletionHot( m_forceTickets[i] ) ) { success = false; }
	}
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
		if ( m_forceTickets[i].m_nClaimId == 0u ) { continue; }
		if ( success ) {
			// The batch is already GPU-published. Keep old fragment bytes until now.
			m_ticket = m_forceTickets[i]; m_pendingFragment = m_forceFragments[i];
			if ( !publishFragmentFileCold( rInputs ) ) { success = false; }
			else if ( !rMegaVT.acceptIncrementalCookCompletionHot( m_forceTickets[i] ) ) { success = false; }
			else { m_fragments[i] = m_forceFragments[i]; ++m_nAccepted; }
		}
		if ( !success ) {
			( void )rMegaVT.cancelIncrementalCookHot( m_forceTickets[i] );
		}
		m_forceFragments[i].clear(); m_forceTickets[i] = t3dTerrainMegaVTCookTicket_t{};
	}
	if ( !success ) {
		for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
			const t3dTerrainMegaVTRegion_t *region = rMegaVT.getRegion( i );
			if ( region != NULL && region->m_nGeneration == m_forceSourceGenerations[i] ) { m_nFailedSourceGenerations[i] = region->m_nGeneration; }
		}
	}
	m_ticket = t3dTerrainMegaVTCookTicket_t{}; m_pendingFragment.clear();
	m_bForceActive = false; m_bForceFailed = false; m_bForceRequested = false;
	m_nIdlePublicationGeneration = rMegaVT.getPublicationGeneration();
	trimCpuReservationCold(); rHost.persistMegaVTEditorStateCold();
	return success;
}

bool Terrain3DIncrementalMegaVTCooker::validateForceRecookSnapshotCold( Terrain3DTerrainMegaVT &rMegaVT ) const {
	if ( !isForceRecookReadyCold() ) { return false; }
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
		if ( m_forceTickets[i].m_nClaimId != 0u && !rMegaVT.validateIncrementalCookCompletionHot( m_forceTickets[i] ) ) { return false; }
	}
	return true;
}
