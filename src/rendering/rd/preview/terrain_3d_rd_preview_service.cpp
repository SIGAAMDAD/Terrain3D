// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/preview/terrain_3d_rd_preview_service.h"
#include "core/utility/terrain_3d_generation.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "core/utility/logger.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "eastl_config.h"
#include "memory/core/terrain_3d_memory.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>

#include <string.h>

static uint64_t t3dRdPreviewVtRetryDelayUsec( const uint64_t nRetryCount ) {
	const uint32_t nShift = ( uint32_t )( MIN( nRetryCount > 0u ? nRetryCount - 1u : 0u, 4u ) );
	return 100000u << nShift;
}

static uint64_t t3dRdSharedVtMaterialSourceGeneration( const Ref<Terrain3DAssets> &rAssets ) {
	if ( rAssets.is_null() ) {
		return 0u;
	}
	uint64_t nGeneration = rAssets->getInstanceVtMaterialSourceGeneration();
	const uint64_t nTerrainGeneration = rAssets->getTerrainVtContentGeneration();
	nGeneration ^= nTerrainGeneration + 0x9e3779b97f4a7c15ULL + ( nGeneration << 6u ) + ( nGeneration >> 2u );
	return nGeneration == 0u ? 1u : nGeneration;
}

bool Terrain3DRdPreviewService::bindRuntimeAllocator( const t3dPreviewServices_t &rServices,
        Terrain3DMemoryZone *const pZone,
        const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone || !m_previewRenderer.bindRuntimeAllocator( pZone, ePolicy )
	        || !m_previewRenderer.attachVirtualTextureCold( &rServices.m_virtualTexture ) ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_cachedPreviewMeshes.bindRuntimeAllocator( allocator );
	m_pendingPreviewMeshes.bindRuntimeAllocator( allocator );
	return true;
}

void Terrain3DRdPreviewService::requestPackageRecomposeCold( void ) {
	Terrain3DScopedLock lock( m_previewVtMutex );
	++m_nPreviewVtRecomposeRevision;
	m_bPreviewVtAuthoringDirty = true;
	m_nPreviewVtCookAfterUsec = (Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u) + 150000u;
	m_nPreviewVtStagedMaterialSourceGeneration = 0u;
	m_nPreviewVtFailedMaterialSourceGeneration = 0u;
}

void Terrain3DRdPreviewService::noteGpuPackageOnRenderThread( const uint32_t nGeneration, const uint64_t nSignature ) {
	Terrain3DScopedLock lock( m_previewVtMutex );
	uint64_t nFingerprint = 0u;
	if ( m_previewVtPublishedPackageBytes.size() >= ( int64_t )sizeof( t3dVTPackHeaderV3_t ) ) {
		t3dVTPackHeaderV3_t header{};
		memcpy( &header, m_previewVtPublishedPackageBytes.ptr(), sizeof( header ) );
		if ( header.m_nPackageGeneration == nGeneration && header.m_nContentSignature == nSignature ) {
			nFingerprint = m_nPreviewVtPublishedFingerprint;
		}
	}
	m_nPublishedGpuFingerprint.store( nFingerprint, T3D_MEMORY_ORDER_RELEASE );
}

void Terrain3DRdPreviewService::updateAuthoringCold( const t3dPreviewServices_t &rServices ) {
	if ( rServices.m_pTerrain == NULL ) { return; }
	const Ref<Terrain3DAssets> assets = rServices.m_pTerrain->assetsResource();
	const uint64_t nSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( assets );
	const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
	bool bCook = false;
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		if ( assets.is_valid() && nSourceGeneration != m_nPreviewVtObservedMaterialSourceGeneration ) {
			m_nPreviewVtObservedMaterialSourceGeneration = nSourceGeneration;
			m_nPreviewVtDirtyMaterialSourceGeneration = nSourceGeneration;
			m_bPreviewVtAuthoringDirty = true;
			m_nPreviewVtCookAfterUsec = nNowUsec + 150000u;
		}
		bCook = assets.is_valid() && m_bPreviewVtAuthoringDirty && nNowUsec >= m_nPreviewVtCookAfterUsec
		        && !m_bPreviewVtRepublishPending && !m_bPreviewVtRepublishInProgress;
	}
	if ( bCook ) {
		String error;
		if ( !preparePreviewAuthoringPackageCold( rServices, assets, &error, true ) && !error.is_empty() ) {
			LOG( WARN, "[RD instances] Editor VT refresh failed: ", error );
		}
	}
	(void)queuePreviewVtRepublishCold( rServices );
}

void Terrain3DRdPreviewService::shutdownRendererOnRenderThread( const t3dPreviewServices_t &rServices ) {
	m_nPublishedGpuFingerprint.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	if ( m_bPreviewRendererInitialized ) {
		m_previewRenderer.shutdown();
		m_bPreviewRendererInitialized = false;
		rServices.m_host.clearPreviewReady( T3D_RENDER_READY_PREVIEW );
	}
}

void Terrain3DRdPreviewService::resetOwnerCold( void ) {
	m_nPublishedGpuFingerprint.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bPreviewRendererInitialized = false;
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		m_nPreviewAsyncLifecycleGeneration = t3dNextGeneration( m_nPreviewAsyncLifecycleGeneration );
		for ( uint32_t iSlot = 0u; iSlot < T3D_PREVIEW_READBACK_SLOT_COUNT; ++iSlot ) {
			m_szPreviewReadbackSlots[iSlot] = t3dPreviewReadbackSlot_t{};
		}
		m_nPreviewReadbackCursor = 0u;
		m_nPendingPreviewReadbackSlot = UINT32_MAX;
		m_pendingPreviewMeshes = t3dRdMeshSourceSnapshot_t{};
		m_bPendingPreviewMeshesValid = false;
		m_nPendingPreviewAssetId = 0u;
		m_pendingPreviewSize = Vector2i();
		m_pendingPreviewImage.unref();
		m_pendingPreviewError = String();
		m_pendingPreviewFailure = T3D_RD_PREVIEW_NONE;
		m_previewLastError = String();
		m_nPreviewFailureCount = 0u;
		m_lastPreviewFailure = T3D_RD_PREVIEW_NONE;
		m_nPreviewAsyncRequestGeneration = 0u;
		m_nPreviewAsyncScheduledCount = 0u;
		m_nPreviewAsyncCompletedCount = 0u;
		m_nPreviewAsyncLateCallbackCount = 0u;
		m_nPreviewAsyncTimeoutCount = 0u;
	}
	m_cachedPreviewMeshes = t3dRdMeshSourceSnapshot_t{};
	m_bCachedPreviewMeshesValid = false;
	m_nPreviewMeshFingerprint = 0u;
	m_nCachedPreviewAssetId = UINT16_MAX;
	m_nPreviewMeshGeneration = 0u;
	{
		Terrain3DScopedLock vt_lock( m_previewVtMutex );
		m_previewVtPackageBytes.clear();
		m_previewVtPublishedPackageBytes.clear();
		m_packageMemory.reset();
		m_publishedPackageMemory.reset();
		m_nPreviewVtFingerprint = 0u;
		m_nPreviewVtPublishedFingerprint = 0u;
		m_nPreviewVtPackageGeneration = 0u;
		m_nPreviewVtPublishedPackageGeneration = 0u;
		m_nForceStagedSerial = 0u;
		m_nForcePublishedSerial = 0u;
		m_bForceOwnerPending = false;
		m_nPreviewVtRecookCount = 0u;
		m_nPreviewVtObservedMaterialSourceGeneration = 0u;
		m_nPreviewVtDirtyMaterialSourceGeneration = 0u;
		m_nPreviewVtCookAfterUsec = 0u;
		m_nPreviewVtRecomposeRevision = 0u;
		m_bPreviewVtAuthoringDirty = false;
		m_nPreviewVtStagedMaterialSourceGeneration = 0u;
		m_nPreviewVtFailedMaterialSourceGeneration = 0u;
		m_nPreviewVtPublishCount = 0u;
		m_nPreviewVtRepublishSuccessCount = 0u;
		m_nPreviewVtRepublishFailureCount = 0u;
		m_nPreviewVtRepublishRetryCount = 0u;
		m_nPreviewVtRepublishRetryAfterUsec = 0u;
		m_nSharedResourceClassTransitionCount = 0u;
		m_nPreviewVtCookFailureCount = 0u;
		m_nPreviewVtFailedFingerprint = 0u;
		t3d_vt_godot_editor_cache_reset_v3( m_previewVtEditorCache );
		m_previewVtEditorCache.m_CacheDirectory = String();
		memset( &m_previewVtEditorTelemetry, 0, sizeof( m_previewVtEditorTelemetry ) );
		m_previewVtLastCookError = String();
		m_previewVtLastRepublishError = String();
		m_bPreviewVtPackageValid = false;
		m_bPreviewVtStageNeutral = true;
		m_bPreviewVtPublishedNeutral = true;
		m_bPreviewVtRepublishPending = false;
		m_bPreviewVtRepublishQueued = false;
		m_bPreviewVtRepublishInProgress = false;
		m_ePreviewVtRepublishResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	}
}

bool Terrain3DRdPreviewService::hasReadbackInFlight( void ) const {
	Terrain3DScopedLock state_lock( m_previewStateMutex );
	for ( uint32_t iSlot = 0u; iSlot < T3D_PREVIEW_READBACK_SLOT_COUNT; ++iSlot ) {
		if ( m_szPreviewReadbackSlots[iSlot].bReadbackInFlight ) {
			return true;
		}
	}
	return false;
}

void Terrain3DRdPreviewService::noteSharedResourceClassTransition( void ) {
	Terrain3DScopedLock vt_lock( m_previewVtMutex );
	++m_nSharedResourceClassTransitionCount;
}

bool Terrain3DRdPreviewService::bakePreviewVirtualTexturesCold( const t3dPreviewServices_t &rServices, String *const pError, const bool bForceRebuild, const uint64_t nForceSerial ) {
	if ( rServices.m_pTerrain == NULL || !rServices.m_pTerrain->isEditorAuthoringMode() || Engine::get_singleton() == NULL
	        || !Engine::get_singleton()->is_editor_hint() ) {
		if ( pError ) {
			*pError = "Virtual-texture authoring bake is available only in Editor Authoring mode";
		}
		return false;
	}
	const Ref<Terrain3DAssets> assets = rServices.m_pTerrain->assetsResource();
	if ( assets.is_null() ) {
		if ( pError ) {
			*pError = "Virtual-texture authoring bake has no Terrain3DAssets source";
		}
		return false;
	}

	// Explicit user action is allowed to retry a quarantined generation. Keep the
	// content-addressed P08 cache intact so unchanged allocations/channels remain hits.
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		m_bPreviewVtAuthoringDirty = true;
		m_nPreviewVtCookAfterUsec = 0u;
		m_nPreviewVtStagedMaterialSourceGeneration = 0u;
		m_nPreviewVtFailedMaterialSourceGeneration = 0u;
		m_nPreviewVtFailedFingerprint = 0u;
		m_previewVtLastCookError = String();
	}
	m_nPreviewVtObservedMaterialSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( assets );
	return preparePreviewAuthoringPackageCold( rServices, assets, pError, true, bForceRebuild, nForceSerial );
}

bool Terrain3DRdPreviewService::preparePreviewAuthoringPackageCold( const t3dPreviewServices_t &rServices, const Ref<Terrain3DAssets> &rAssets, String *const pError, const bool bImmediate, const bool bForceRebuild, const uint64_t nForceSerial ) {
	if ( rAssets.is_null() ) {
		if ( pError ) {
			*pError = "RD preview VT has no Terrain3DAssets source";
		}
		return false;
	}
	const uint64_t nSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( rAssets );
	const t3dRenderLifecycleStatus_t initialStatus = rServices.m_host.previewLifecycleStatus();
	const bool bLive = initialStatus.m_nSharedResourceClass != T3D_VT_SHARED_RESOURCES_NONE;
	uint64_t nCookRevision = 0u;
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		if ( m_bPreviewVtRepublishPending || m_bPreviewVtRepublishInProgress ) { return true; }
		if ( bLive && !bImmediate ) {
			if ( nSourceGeneration != m_nPreviewVtStagedMaterialSourceGeneration && !m_bPreviewVtAuthoringDirty ) {
				m_bPreviewVtAuthoringDirty = true;
				m_nPreviewVtDirtyMaterialSourceGeneration = nSourceGeneration;
				m_nPreviewVtCookAfterUsec = (Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u) + 150000u;
			}
			return true;
		}
		if ( m_bForceOwnerPending && !bForceRebuild ) { return true; }
		nCookRevision = m_nPreviewVtRecomposeRevision;
		m_bPreviewVtAuthoringDirty = false;
	}
	m_nPreviewVtObservedMaterialSourceGeneration = nSourceGeneration;
	bool bRepublishPending = false;
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		if ( !bForceRebuild && nSourceGeneration != 0u && nSourceGeneration == m_nPreviewVtFailedMaterialSourceGeneration ) {
			// Viewport/thumbnail polls keep using the last publication. Failure
			// details remain in status; an explicit bake clears this quarantine.
			if ( !bImmediate ) { return true; }
			if ( pError ) {
				*pError = !m_previewVtLastRepublishError.is_empty() ? m_previewVtLastRepublishError
				        : m_previewVtLastCookError.is_empty() ? String( "RD preview VT transient package cook failed" ) : m_previewVtLastCookError;
			}
			return false;
		}
		if ( !bForceRebuild && nSourceGeneration != 0u && nSourceGeneration == m_nPreviewVtStagedMaterialSourceGeneration ) {
			bRepublishPending = m_bPreviewVtRepublishPending;
			if ( !bRepublishPending ) {
				return true;
			}
		}
	}
	if ( bRepublishPending ) {
		( void )queuePreviewVtRepublishCold( rServices );
		return true;
	}

	// Failed source generations stay quarantined until a texture edit or explicit bake.
	t3dVTGodotAssetMaterials_t materials;
	String collectError;
	if ( !t3d_vt_godot_collect_asset_materials( rAssets, materials, &collectError ) ) {
		Terrain3DScopedLock lock( m_previewVtMutex );
		m_nPreviewVtFailedMaterialSourceGeneration = nSourceGeneration;
		m_previewVtLastCookError = collectError.is_empty() ? String( "RD preview VT material collection failed" ) : collectError;
		++m_nPreviewVtCookFailureCount;
		if ( pError ) {
			*pError = m_previewVtLastCookError;
		}
		return false;
	}

	const uint32_t nMaterialCount = ( uint32_t )materials.m_stableKeys.size();
	PackedByteArray packageBytes;
	Terrain3DSharedMemoryReservation package_memory;
	uint64_t nFingerprint = 0u;
	uint32_t nPackageGeneration = 0u;
	t3dVTEditorCookTelemetryV3_t cookTelemetry{};
	bool bStageNeutral = nMaterialCount == 0u;
	if ( !bStageNeutral ) {
		if ( m_previewVtEditorCache.m_CacheDirectory.is_empty() ) {
			String cacheDirectory;
			ProjectSettings *const pSettings = ProjectSettings::get_singleton();
			if ( pSettings != NULL ) {
				cacheDirectory = pSettings->globalize_path( "res://.godot/terrain3d/vt_cache" );
			}
			if ( cacheDirectory.is_empty() && rServices.m_pTerrain != NULL ) {
				cacheDirectory = rServices.m_pTerrain->getDataDirectory().path_join( ".vt_cache" );
			}
			t3d_vt_godot_editor_cache_set_directory_v3( m_previewVtEditorCache, cacheDirectory );
		}
		String cookError;
		if ( !t3d_vt_godot_cook_editor_package_incremental_v3( rAssets,
		             materials.m_stableKeys.data(),
		             materials.m_records.data(),
		             nMaterialCount,
		             m_previewVtEditorCache,
		             packageBytes,
		             nFingerprint,
		             nPackageGeneration,
		             cookTelemetry,
		             &cookError, &package_memory, bForceRebuild )
		        || nFingerprint == 0u || packageBytes.is_empty() ) {
			Terrain3DScopedLock lock( m_previewVtMutex );
			m_previewVtEditorTelemetry = cookTelemetry;
			m_nPreviewVtFailedMaterialSourceGeneration = nSourceGeneration;
			m_nPreviewVtFailedFingerprint = nFingerprint;
			m_previewVtLastCookError = cookError.is_empty() ? String( "RD preview VT transient package cook failed" ) : cookError;
			++m_nPreviewVtCookFailureCount;
			if ( pError ) {
				*pError = m_previewVtLastCookError;
			}
			return false;
		}
	}

	if ( !rServices.m_host.composePreviewTerrainPackageCold( packageBytes, nPackageGeneration, nFingerprint, package_memory ) ) {
		const String compositionError = rServices.m_host.previewTerrainCompositionErrorCold();
		Terrain3DScopedLock lock( m_previewVtMutex );
		m_nPreviewVtFailedMaterialSourceGeneration = nSourceGeneration;
		m_previewVtLastCookError = compositionError;
		++m_nPreviewVtCookFailureCount;
		if ( pError ) { *pError = m_previewVtLastCookError; }
		return false;
	}
	bStageNeutral = packageBytes.is_empty();
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		if ( nSourceGeneration != t3dRdSharedVtMaterialSourceGeneration( rAssets )
		        || nCookRevision != m_nPreviewVtRecomposeRevision ) {
			m_bPreviewVtAuthoringDirty = true;
			m_nPreviewVtCookAfterUsec = (Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u) + 150000u;
			if ( bForceRebuild ) {
				m_previewVtLastCookError = "Terrain or material sources changed during forced VT cook";
				if ( pError ) { *pError = m_previewVtLastCookError; }
				return false;
			}
			return true;
		}
	}

	const t3dRenderLifecycleStatus_t renderStatus = rServices.m_host.previewLifecycleStatus();
	const bool bConsumersLive = renderStatus.m_nSharedResourceClass != T3D_VT_SHARED_RESOURCES_NONE
	        || ( renderStatus.m_nReadyMask & T3D_RENDER_READY_RESOURCE_CONSUMERS ) != 0u;
	const t3dVTSharedResourceClass_t eLiveClass =
	        ( t3dVTSharedResourceClass_t )renderStatus.m_nSharedResourceClass;
	bool bNeedsPublication = false;
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		if ( !bForceRebuild && nFingerprint == m_nPreviewVtFailedFingerprint ) {
			m_nPreviewVtFailedMaterialSourceGeneration = nSourceGeneration;
			return true;
		}
		bNeedsPublication =
		        bForceRebuild || bStageNeutral != m_bPreviewVtPublishedNeutral || ( !bStageNeutral && nFingerprint != m_nPreviewVtPublishedFingerprint );
		m_nForceStagedSerial = bForceRebuild ? nForceSerial : 0u;
		m_previewVtPackageBytes = packageBytes;
		m_packageMemory = package_memory;
		m_nPreviewVtFingerprint = nFingerprint;
		m_nPreviewVtPackageGeneration = nPackageGeneration;
		m_nPreviewVtStagedMaterialSourceGeneration = nSourceGeneration;
		m_nPreviewVtFailedMaterialSourceGeneration = 0u;
		m_nPreviewVtFailedFingerprint = 0u;
		m_previewVtLastCookError = String();
		m_previewVtLastRepublishError = String();
		m_previewVtEditorTelemetry = cookTelemetry;
		m_bPreviewVtPackageValid = !bStageNeutral;
		m_bPreviewVtStageNeutral = bStageNeutral;
		if ( !bStageNeutral && cookTelemetry.m_nDirtyAllocations != 0u ) {
			++m_nPreviewVtRecookCount;
		}
		m_bPreviewVtRepublishPending = bConsumersLive && bNeedsPublication;
		if ( m_bPreviewVtRepublishPending ) {
			m_ePreviewVtRepublishResourceClass = eLiveClass;
		}
	}

	if ( !bNeedsPublication ) {
		return true;
	}
	if ( bConsumersLive ) {
		( void )queuePreviewVtRepublishCold( rServices );
		return true;
	}

	const bool bPublished = bStageNeutral ? rServices.m_virtualTexture.clearPackageCold()
	                                      : rServices.m_host.loadPreviewPackage( packageBytes.ptr(), ( uint64_t )packageBytes.size() );
	if ( !bPublished ) {
		const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
		{
			Terrain3DScopedLock lock( m_previewVtMutex );
			m_bPreviewVtRepublishPending = true;
			m_ePreviewVtRepublishResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
			++m_nPreviewVtRepublishRetryCount;
			m_nPreviewVtRepublishRetryAfterUsec = nNowUsec + t3dRdPreviewVtRetryDelayUsec( m_nPreviewVtRepublishRetryCount );
			if ( bForceRebuild ) { m_bPreviewVtRepublishPending = false; m_nPreviewVtRepublishRetryAfterUsec = 0u; }
			m_previewVtLastRepublishError = bStageNeutral ? String( "RD preview VT neutral publication failed; retry scheduled" )
			                                              : String( "RD preview VT transient package publication failed; retry scheduled" );
			++m_nPreviewVtRepublishFailureCount;
			if ( pError ) {
				*pError = m_previewVtLastRepublishError;
			}
		}
		return true;
	}
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		m_nForcePublishedSerial = MAX( m_nForcePublishedSerial, m_nForceStagedSerial );
		m_previewVtPublishedPackageBytes = packageBytes;
		m_publishedPackageMemory = m_packageMemory;
		m_nPreviewVtPublishedFingerprint = nFingerprint;
		m_nPreviewVtPublishedPackageGeneration = nPackageGeneration;
		m_bPreviewVtPublishedNeutral = bStageNeutral;
		m_previewVtLastRepublishError = String();
		m_nPreviewVtRepublishRetryCount = 0u;
		m_nPreviewVtRepublishRetryAfterUsec = 0u;
		++m_nPreviewVtPublishCount;
	}
	return true;
}

bool Terrain3DRdPreviewService::queuePreviewVtRepublishCold( const t3dPreviewServices_t &rServices ) {
	const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		if ( !m_bPreviewVtRepublishPending || m_bPreviewVtRepublishQueued || m_bPreviewVtRepublishInProgress ) {
			return true;
		}
		if ( m_nPreviewVtRepublishRetryAfterUsec != 0u && nNowUsec < m_nPreviewVtRepublishRetryAfterUsec ) {
			return true;
		}
		m_bPreviewVtRepublishQueued = true;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		servicePreviewVtRepublishOnRenderThread( rServices );
		return true;
	}
	if ( rServices.m_effect.is_valid()
	        && Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( rServices.m_effect.ptr(), &Terrain3DRdCompositorEffect::servicePreviewVtRepublishOnRenderThread ) ) ) {
		return true;
	}
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		m_bPreviewVtRepublishQueued = false;
		m_bPreviewVtRepublishPending = true;
		++m_nPreviewVtRepublishRetryCount;
		m_nPreviewVtRepublishRetryAfterUsec = nNowUsec + t3dRdPreviewVtRetryDelayUsec( m_nPreviewVtRepublishRetryCount );
		m_previewVtLastRepublishError = "RD preview VT republish render-thread submission failed";
		if ( m_nForceStagedSerial != 0u ) { m_bPreviewVtRepublishPending = false; m_nPreviewVtRepublishRetryAfterUsec = 0u; }
		++m_nPreviewVtRepublishFailureCount;
	}
	return false;
}

bool Terrain3DRdPreviewService::reloadPublishedPreviewVtPackageCold( const t3dPreviewServices_t &rServices ) {
	if ( rServices.m_virtualTexture.is_initialized() ) { return false; }
	PackedByteArray packageBytes;
	bool bNeutral = true;
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		if ( m_bPreviewVtPublishedNeutral || !m_previewVtPublishedPackageBytes.is_empty() ) {
			bNeutral = m_bPreviewVtPublishedNeutral;
			packageBytes = m_previewVtPublishedPackageBytes;
		} else {
			bNeutral = m_bPreviewVtStageNeutral;
			packageBytes = m_previewVtPackageBytes;
		}
	}
	if ( bNeutral ) {
		return rServices.m_virtualTexture.clearPackageCold();
	}
	if ( packageBytes.is_empty() || !rServices.m_host.loadPreviewPackage( packageBytes.ptr(), ( uint64_t )packageBytes.size() ) ) {
		return false;
	}
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		++m_nPreviewVtPublishCount;
	}
	return true;
}

void Terrain3DRdPreviewService::servicePreviewVtRepublishOnRenderThread( const t3dPreviewServices_t &rServices ) {
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		if ( m_bPreviewVtAuthoringDirty && !m_bPreviewVtRepublishInProgress ) {
			// A newer source arrived before the atomic GPU commit. Supersede the
			// staged package instead of publishing an intermediate editor state.
			m_bPreviewVtRepublishQueued = false;
			m_bPreviewVtRepublishPending = false;
			if ( m_nForceStagedSerial != 0u ) {
				m_previewVtLastRepublishError = "Terrain or material sources changed before forced VT publication";
				m_nForceStagedSerial = 0u;
			}
			rServices.m_sharedVtRuntime.abortPackageRepublishWarm();
			return;
		}
	}
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < T3D_PREVIEW_READBACK_SLOT_COUNT; ++iSlot ) {
			if ( m_szPreviewReadbackSlots[iSlot].bReadbackInFlight ) {
				Terrain3DScopedLock vt_lock( m_previewVtMutex );
				m_bPreviewVtRepublishQueued = false;
				return;
			}
		}
	}

	Terrain3DSharedMemoryReservation staged_memory;
	PackedByteArray stagedBytes;
	uint64_t nStagedFingerprint = 0u;
	uint32_t nStagedGeneration = 0u;
	bool bStagedNeutral = true;
	t3dVTSharedResourceClass_t eResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		m_bPreviewVtRepublishQueued = false;
		if ( !m_bPreviewVtRepublishPending || m_bPreviewVtRepublishInProgress ) {
			return;
		}
		const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
		if ( m_nPreviewVtRepublishRetryAfterUsec != 0u && nNowUsec < m_nPreviewVtRepublishRetryAfterUsec ) {
			return;
		}
		m_bPreviewVtRepublishInProgress = true;
		stagedBytes = m_previewVtPackageBytes;
		staged_memory = m_packageMemory;
		nStagedFingerprint = m_nPreviewVtFingerprint;
		nStagedGeneration = m_nPreviewVtPackageGeneration;
		bStagedNeutral = m_bPreviewVtStageNeutral;
		eResourceClass = m_ePreviewVtRepublishResourceClass;
	}
	const t3dVTSharedResourceClass_t eCurrentClass = rServices.m_sharedVtRuntime.get_shared_resource_class();
	if ( eCurrentClass == T3D_VT_SHARED_RESOURCES_PRODUCTION || eResourceClass == T3D_VT_SHARED_RESOURCES_NONE ) {
		eResourceClass = eCurrentClass;
	}

	// A pending runtime candidate belongs to this unchanged stage: authoring
	// cooks are deferred until the transaction finishes or is aborted.
	bool bPublished = rServices.m_virtualTexture.isPackageRepublishPendingWarm()
	        || rServices.m_host.stagePreviewPackageWarm( bStagedNeutral ? NULL : stagedBytes.ptr(),
	                bStagedNeutral ? 0u : (uint64_t)stagedBytes.size() );
	if ( bPublished ) {
		bPublished = rServices.m_host.servicePreviewPackageWarm( 8u * 1024u * 1024u, 1u );
	}
	if ( bPublished && rServices.m_virtualTexture.isPackageRepublishPendingWarm() ) {
		Terrain3DScopedLock lock( m_previewVtMutex );
		m_bPreviewVtRepublishInProgress = false;
		return;
	}

	const bool bResourceRebuildRequested = !bPublished && rServices.m_virtualTexture.isPackageResourceRebuildRequiredWarm();
	bool bControlledRebuildRollbackReady = true;
	if ( bResourceRebuildRequested
	        && eResourceClass != T3D_VT_SHARED_RESOURCES_NONE ) {
		PackedByteArray previousBytes;
		bool bPreviousNeutral = true;
		{
			Terrain3DScopedLock lock( m_previewVtMutex );
			previousBytes = m_previewVtPublishedPackageBytes;
			bPreviousNeutral = m_bPreviewVtPublishedNeutral;
		}
		bPublished = rServices.m_host.rebuildPreviewPackageResourcesCold(
		        bStagedNeutral ? NULL : stagedBytes.ptr(),
		        bStagedNeutral ? 0u : (uint64_t)stagedBytes.size(),
		        bStagedNeutral, eResourceClass );
		if ( !bPublished ) {
			bControlledRebuildRollbackReady = rServices.m_host.rebuildPreviewPackageResourcesCold(
			        bPreviousNeutral ? NULL : previousBytes.ptr(),
			        bPreviousNeutral ? 0u : (uint64_t)previousBytes.size(),
			        bPreviousNeutral, eResourceClass );
		}
	}

	if ( bPublished ) {
		Terrain3DScopedLock lock( m_previewVtMutex );
		m_nForcePublishedSerial = MAX( m_nForcePublishedSerial, m_nForceStagedSerial );
		m_previewVtPublishedPackageBytes = stagedBytes;
		m_publishedPackageMemory = staged_memory;
		m_nPreviewVtPublishedFingerprint = nStagedFingerprint;
		m_nPreviewVtPublishedPackageGeneration = nStagedGeneration;
		m_bPreviewVtPublishedNeutral = bStagedNeutral;
		m_bPreviewVtRepublishPending = m_nPreviewVtFingerprint != nStagedFingerprint || m_bPreviewVtStageNeutral != bStagedNeutral;
		if ( eResourceClass != T3D_VT_SHARED_RESOURCES_NONE ) {
			m_nPublishedGpuFingerprint.store( nStagedFingerprint, T3D_MEMORY_ORDER_RELEASE );
		}
		m_bPreviewVtRepublishInProgress = false;
		m_nPreviewVtRepublishRetryCount = 0u;
		m_nPreviewVtRepublishRetryAfterUsec = 0u;
		m_previewVtLastRepublishError = String();
		++m_nPreviewVtPublishCount;
		++m_nPreviewVtRepublishSuccessCount;
		return;
	}

	// The runtime owns rollback; the renderer and last-good package stay live.
	const bool bRolledBack = bControlledRebuildRollbackReady && rServices.m_virtualTexture.isPackageRollbackReadyWarm();

	{
		Terrain3DScopedLock lock( m_previewVtMutex );
		const bool bSameStage = m_nPreviewVtFingerprint == nStagedFingerprint && m_bPreviewVtStageNeutral == bStagedNeutral;
		m_bPreviewVtRepublishPending = !bSameStage;
		m_bPreviewVtRepublishInProgress = false;
		if ( bSameStage ) {
			m_nPreviewVtFailedMaterialSourceGeneration = m_nPreviewVtStagedMaterialSourceGeneration;
			m_nPreviewVtFailedFingerprint = nStagedFingerprint;
			m_bPreviewVtAuthoringDirty = false;
		}
		m_nPreviewVtRepublishRetryCount = 0u;
		m_nPreviewVtRepublishRetryAfterUsec = 0u;
		m_previewVtLastRepublishError = bRolledBack
		        ? (bResourceRebuildRequested
		                   ? String( "RD preview VT resource-plan rebuild failed; previous publication restored; generation quarantined" )
		                   : String( "RD preview VT warm republish failed; previous publication retained; generation quarantined" ))
		        : String( "RD preview VT live republish failed; rollback failed; generation quarantined" );
		++m_nPreviewVtRepublishFailureCount;
	}
}

void Terrain3DRdPreviewService::renderPreviewOnRenderThread( const t3dPreviewServices_t &rServices ) {
	Terrain3DScopedLock lock( m_previewStateMutex );
	m_pendingPreviewImage.unref();
	m_pendingPreviewError = String();
	m_pendingPreviewFailure = T3D_RD_PREVIEW_NONE;
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	context.beginRecoverableFailureScope();
	// One exit keeps shared acquisition, partial initialization and cleanup recoverable.
	do {
		if ( !m_bPendingPreviewMeshesValid ) {
			m_pendingPreviewFailure = T3D_RD_PREVIEW_ASSET_SOURCE;
			m_pendingPreviewError = "RD editor preview source is unavailable";
			break;
		}
		m_pendingPreviewFailure = T3D_RD_PREVIEW_SYSTEMIC_RD;
		if ( rServices.m_shaderLibrary.getInstances().is_null() ) {
			m_pendingPreviewError = "RD editor preview base instance shader is unavailable";
			break;
		}
		if ( !rServices.m_host.ensurePreviewSharedVt( T3D_VT_SHARED_RESOURCES_PREVIEW ) ) {
			m_pendingPreviewError = "RD editor preview shared VT resources are unavailable";
			break;
		}
		if ( !m_bPreviewRendererInitialized ) {
			m_bPreviewRendererInitialized = m_previewRenderer.initializePreview();
			if ( m_bPreviewRendererInitialized ) {
				rServices.m_host.setPreviewReady(
				        rServices.m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ), T3D_RENDER_READY_PREVIEW );
			}
		}
		Ref<Image> image;
		const bool ok = m_bPreviewRendererInitialized
		        && m_previewRenderer.renderPreview( &m_pendingPreviewMeshes, m_nPendingPreviewAssetId, m_pendingPreviewSize, image );
		if ( !ok || image.is_null() ) {
			m_pendingPreviewError = m_previewRenderer.getLastError();
			if ( m_pendingPreviewError.is_empty() ) {
				m_pendingPreviewError = "RD editor preview renderer failed without a stage diagnostic";
			}
			break;
		}
		m_pendingPreviewImage = image;
		m_pendingPreviewFailure = T3D_RD_PREVIEW_NONE;
	} while ( false );
	if ( m_pendingPreviewFailure != T3D_RD_PREVIEW_NONE ) {
		// Release preview scratch only; production renderers and the VT owner remain intact.
		m_previewRenderer.shutdown();
		m_bPreviewRendererInitialized = false;
		rServices.m_host.clearPreviewReady( T3D_RENDER_READY_PREVIEW );
	}
	context.endRecoverableFailureScope();
}

Ref<Image> Terrain3DRdPreviewService::failMeshPreview( const t3dPreviewServices_t &rServices, const t3dRdPreviewFailure_t eFailure, const String &rError, t3dRdPreviewFailure_t *const pFailure ) {
	if ( pFailure ) {
		*pFailure = eFailure;
	}
	Terrain3DScopedLock lock( rServices.m_diagnosticsMutex );
	m_previewLastError = rError;
	m_lastPreviewFailure = eFailure;
	++m_nPreviewFailureCount;
	return Ref<Image>();
}

bool Terrain3DRdPreviewService::prepareMeshPreviewSourceCold( const t3dPreviewServices_t &rServices, const uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *const pFailure ) {
	if ( pFailure ) {
		*pFailure = T3D_RD_PREVIEW_NONE;
	}
	if ( !rServices.m_pTerrain || !rServices.m_effect.is_valid() ) {
		( void )failMeshPreview( rServices, T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview coordinator is unavailable", pFailure );
		return false;
	}
	// Preview rendering/readback owns separate RD targets. Region-cache uploads do
	// not alias those targets, so global authoring residency is not a correctness
	// prerequisite. Transient deferral is reserved for preview-owned in-flight
	// slots and publication/rebuild seams instead of unrelated terrain streaming.
	// Mesh-list edits can still discover a larger immutable instance mesh source.
	// Do not launch previews against the stale bank while that cold capacity
	// transition is pending.
	if ( rServices.m_publicationBuilder.isInstancePublicationCapacityPending() || rServices.m_publicationBuilder.isInstanceMeshBankCapacityPending() ) {
		( void )failMeshPreview( rServices, T3D_RD_PREVIEW_TRANSIENT_DEFERRED, "RD preview is waiting for instance publication capacity", pFailure );
		return false;
	}
	const Ref<Terrain3DAssets> assets = rServices.m_pTerrain->assetsResource();
	if ( rSize.x < 2 || rSize.y < 2 || assets.is_null() || nAssetId >= assets->getMeshCount() ) {
		( void )failMeshPreview( rServices, T3D_RD_PREVIEW_ASSET_SOURCE, "RD preview asset or dimensions are invalid", pFailure );
		return false;
	}
	const uint64_t fingerprint = Terrain3DRdMeshCache::computeSourceFingerprintForAsset( assets, nAssetId );
	if ( !m_bCachedPreviewMeshesValid || nAssetId != m_nCachedPreviewAssetId || fingerprint != m_nPreviewMeshFingerprint ) {
		String error;
		m_nPreviewMeshGeneration = t3dNextGeneration( m_nPreviewMeshGeneration );
		if ( !Terrain3DRdMeshCache::buildSourceSnapshotForAsset(
		             assets, nAssetId, m_cachedPreviewMeshes, fingerprint, m_nPreviewMeshGeneration, &error ) ) {
			( void )failMeshPreview( rServices, T3D_RD_PREVIEW_ASSET_SOURCE, error, pFailure );
			return false;
		}
		m_bCachedPreviewMeshesValid = true;
		m_nPreviewMeshFingerprint = fingerprint;
		m_nCachedPreviewAssetId = nAssetId;
	}
	String vtError;
	if ( !preparePreviewAuthoringPackageCold( rServices, assets, &vtError ) ) {
		LOG( WARN, "[RD instances] Mesh preview VT package unavailable; using neutral material backing: ", vtError );
	}
	{
		Terrain3DScopedLock lock( m_previewStateMutex );
		if ( !t3dContainerReserveCold( m_pendingPreviewMeshes.m_vertexChunkHashes, m_cachedPreviewMeshes.m_vertexChunkHashes.size() )
		        || !t3dContainerReserveCold(
		                m_pendingPreviewMeshes.m_index32ChunkHashes, m_cachedPreviewMeshes.m_index32ChunkHashes.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_surfaces, m_cachedPreviewMeshes.m_surfaces.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_lods, m_cachedPreviewMeshes.m_lods.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_materials, m_cachedPreviewMeshes.m_materials.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_vtMaterials, m_cachedPreviewMeshes.m_vtMaterials.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_materialKeys, m_cachedPreviewMeshes.m_materialKeys.size() )
		        || !t3dContainerReserveCold( m_pendingPreviewMeshes.m_vtStableKeys, m_cachedPreviewMeshes.m_vtStableKeys.size() )
		        || !m_pendingPreviewMeshes.copyFrom( m_cachedPreviewMeshes ) ) {
			( void )failMeshPreview( rServices, T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview snapshot copy failed", pFailure );
			return false;
		}
		m_bPendingPreviewMeshesValid = true;
		m_nPendingPreviewAssetId = nAssetId;
		m_pendingPreviewSize = rSize;
		m_pendingPreviewImage.unref();
		m_pendingPreviewError = String();
		m_pendingPreviewFailure = T3D_RD_PREVIEW_NONE;
	}
	return true;
}

Ref<Image> Terrain3DRdPreviewService::createMeshPreview( const t3dPreviewServices_t &rServices, const uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *const pFailure ) {
	Terrain3DScopedLock call_lock( m_previewCallMutex );
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < T3D_PREVIEW_READBACK_SLOT_COUNT; ++iSlot ) {
			if ( m_szPreviewReadbackSlots[iSlot].bActive ) {
				return failMeshPreview( rServices, T3D_RD_PREVIEW_TRANSIENT_DEFERRED, "RD async preview readback is in flight", pFailure );
			}
		}
	}
	if ( !prepareMeshPreviewSourceCold( rServices, nAssetId, rSize, pFailure ) ) {
		return Ref<Image>();
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		renderPreviewOnRenderThread( rServices );
	} else {
		if ( rServices.m_effect.is_null()
		        || !Terrain3DRdContext::enqueueOnRenderThread(
		                callable_mp( rServices.m_effect.ptr(), &Terrain3DRdCompositorEffect::renderPreviewOnRenderThread ) ) ) {
			return failMeshPreview( rServices, T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview render-thread submission failed", pFailure );
		}
		Terrain3DRdContext::forceSync();
	}
	Terrain3DScopedLock lock( m_previewStateMutex );
	if ( m_pendingPreviewImage.is_null() ) {
		return failMeshPreview( rServices, m_pendingPreviewFailure == T3D_RD_PREVIEW_NONE ? T3D_RD_PREVIEW_SYSTEMIC_RD : m_pendingPreviewFailure,
		        m_pendingPreviewError,
		        pFailure );
	}
	{
		Terrain3DScopedLock diagnostics_lock( rServices.m_diagnosticsMutex );
		m_previewLastError = String();
		m_lastPreviewFailure = T3D_RD_PREVIEW_NONE;
	}
	return m_pendingPreviewImage;
}

bool Terrain3DRdPreviewService::requestMeshPreviewAsync( const t3dPreviewServices_t &rServices, const uint16_t nAssetId,
        const Vector2i &rSize,
        const uint64_t nCompletionOwnerId,
        const uint64_t nCompletionGeneration,
        t3dRdPreviewFailure_t *const pFailure ) {
	Terrain3DScopedLock call_lock( m_previewCallMutex );
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < T3D_PREVIEW_READBACK_SLOT_COUNT; ++iSlot ) {
			if ( m_szPreviewReadbackSlots[iSlot].bActive ) {
				if ( pFailure ) {
					*pFailure = T3D_RD_PREVIEW_TRANSIENT_DEFERRED;
				}
				return false;
			}
		}
	}
	if ( !prepareMeshPreviewSourceCold( rServices, nAssetId, rSize, pFailure ) ) {
		return false;
	}
	const Ref<Terrain3DAssets> assets = rServices.m_pTerrain ? rServices.m_pTerrain->assetsResource() : Ref<Terrain3DAssets>();
	const Ref<Terrain3DMeshAsset> asset = assets.is_valid() ? assets->getMeshAsset( ( int )nAssetId ) : Ref<Terrain3DMeshAsset>();
	if ( assets.is_null() || asset.is_null() ) {
		if ( pFailure ) {
			*pFailure = T3D_RD_PREVIEW_ASSET_SOURCE;
		}
		return false;
	}
	const uint64_t nMeshSourceGeneration = assets->getMeshSourceGeneration();
	const uint64_t nAssetObjectId = ( uint64_t )asset->get_instance_id();
	uint32_t nSlot = UINT32_MAX;
	uint64_t nRequestGeneration = 0u;
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		for ( uint32_t i = 0u; i < T3D_PREVIEW_READBACK_SLOT_COUNT; ++i ) {
			const uint32_t iSlot = ( m_nPreviewReadbackCursor + i ) % T3D_PREVIEW_READBACK_SLOT_COUNT;
			if ( !m_szPreviewReadbackSlots[iSlot].bActive ) {
				nSlot = iSlot;
				break;
			}
		}
		if ( nSlot == UINT32_MAX ) {
			if ( pFailure ) {
				*pFailure = T3D_RD_PREVIEW_TRANSIENT_DEFERRED;
			}
			return false;
		}
		nRequestGeneration = ++m_nPreviewAsyncRequestGeneration;
		if ( nRequestGeneration == 0u ) {
			nRequestGeneration = ++m_nPreviewAsyncRequestGeneration;
		}
		t3dPreviewReadbackSlot_t &slot = m_szPreviewReadbackSlots[nSlot];
		slot.bActive = true;
		slot.nAssetId = nAssetId;
		slot.size = rSize;
		slot.nRequestGeneration = nRequestGeneration;
		slot.nLifecycleGeneration = m_nPreviewAsyncLifecycleGeneration;
		slot.nMeshSourceGeneration = nMeshSourceGeneration;
		slot.nAssetObjectId = nAssetObjectId;
		slot.nCompletionOwnerId = nCompletionOwnerId;
		slot.nCompletionGeneration = nCompletionGeneration;
		m_nPendingPreviewReadbackSlot = nSlot;
		m_nPreviewReadbackCursor = ( nSlot + 1u ) % T3D_PREVIEW_READBACK_SLOT_COUNT;
	}
	if ( rServices.m_effect.is_null()
	        || !Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( rServices.m_effect.ptr(), &Terrain3DRdCompositorEffect::renderPreviewAsyncOnRenderThread ) ) ) {
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		m_szPreviewReadbackSlots[nSlot] = t3dPreviewReadbackSlot_t{};
		m_nPendingPreviewReadbackSlot = UINT32_MAX;
		if ( pFailure ) {
			*pFailure = T3D_RD_PREVIEW_SYSTEMIC_RD;
		}
		return false;
	}
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		++m_nPreviewAsyncScheduledCount;
	}
	return true;
}

void Terrain3DRdPreviewService::dispatchPreviewAsyncCompletionCold( const t3dPreviewReadbackSlot_t &rSlot, const t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage ) {
	Object *const pOwner = ObjectDB::get_instance( rSlot.nCompletionOwnerId );
	if ( pOwner ) {
		pOwner->call_deferred( StringName( "_complete_mesh_thumbnail_async" ),
		        ( int64_t )rSlot.nAssetId,
		        rImage,
		        ( int64_t )eFailure,
		        ( int64_t )rSlot.nCompletionGeneration,
		        ( int64_t )rSlot.nMeshSourceGeneration,
		        ( int64_t )rSlot.nAssetObjectId );
	}
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		++m_nPreviewAsyncCompletedCount;
	}
}

void Terrain3DRdPreviewService::completePreviewAsyncOnRenderThread( const uint32_t nSlot, const t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage ) {
	t3dPreviewReadbackSlot_t slot;
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		if ( nSlot >= T3D_PREVIEW_READBACK_SLOT_COUNT || !m_szPreviewReadbackSlots[nSlot].bActive ) {
			return;
		}
		slot = m_szPreviewReadbackSlots[nSlot];
		m_szPreviewReadbackSlots[nSlot] = t3dPreviewReadbackSlot_t{};
		if ( m_nPendingPreviewReadbackSlot == nSlot ) {
			m_nPendingPreviewReadbackSlot = UINT32_MAX;
		}
	}
	dispatchPreviewAsyncCompletionCold( slot, eFailure, rImage );
}

void Terrain3DRdPreviewService::servicePreviewReadbackWatchdogCold( const t3dPreviewServices_t &rServices ) {
	const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
	if ( nNowUsec == 0u ) {
		return;
	}
	t3dPreviewReadbackSlot_t expired[T3D_PREVIEW_READBACK_SLOT_COUNT];
	uint32_t nExpired = 0u;
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < T3D_PREVIEW_READBACK_SLOT_COUNT; ++iSlot ) {
			const t3dPreviewReadbackSlot_t &slot = m_szPreviewReadbackSlots[iSlot];
			if ( !slot.bActive || !slot.bReadbackInFlight || slot.nReadbackSubmitUsec == 0u
			        || nNowUsec - slot.nReadbackSubmitUsec < T3D_PREVIEW_READBACK_TIMEOUT_USEC ) {
				continue;
			}
			expired[nExpired++] = slot;
			m_szPreviewReadbackSlots[iSlot] = t3dPreviewReadbackSlot_t{};
			if ( m_nPendingPreviewReadbackSlot == iSlot ) {
				m_nPendingPreviewReadbackSlot = UINT32_MAX;
			}
		}
		if ( nExpired > 0u ) {
			m_nPreviewAsyncLifecycleGeneration = t3dNextGeneration( m_nPreviewAsyncLifecycleGeneration );
			m_nPreviewAsyncTimeoutCount += nExpired;
		}
	}
	for ( uint32_t iExpired = 0u; iExpired < nExpired; ++iExpired ) {
		dispatchPreviewAsyncCompletionCold( expired[iExpired], T3D_RD_PREVIEW_TRANSIENT_DEFERRED, Ref<Image>() );
	}
	if ( nExpired > 0u ) {
		( void )queuePreviewVtRepublishCold( rServices );
	}
}

void Terrain3DRdPreviewService::cancelPreviewAsyncRequestsOnRenderThread( void ) {
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		m_nPreviewAsyncLifecycleGeneration = t3dNextGeneration( m_nPreviewAsyncLifecycleGeneration );
		m_nPendingPreviewReadbackSlot = UINT32_MAX;
	}
	for ( uint32_t iSlot = 0u; iSlot < T3D_PREVIEW_READBACK_SLOT_COUNT; ++iSlot ) {
		if ( m_szPreviewReadbackSlots[iSlot].bActive ) {
			completePreviewAsyncOnRenderThread( iSlot, T3D_RD_PREVIEW_TRANSIENT_DEFERRED, Ref<Image>() );
		}
	}
}

void Terrain3DRdPreviewService::renderPreviewAsyncOnRenderThread( const t3dPreviewServices_t &rServices ) {
	uint32_t nSlot = UINT32_MAX;
	uint64_t nRequestGeneration = 0u;
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		nSlot = m_nPendingPreviewReadbackSlot;
		if ( nSlot >= T3D_PREVIEW_READBACK_SLOT_COUNT || !m_szPreviewReadbackSlots[nSlot].bActive ) {
			return;
		}
		nRequestGeneration = m_szPreviewReadbackSlots[nSlot].nRequestGeneration;
	}
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	context.beginRecoverableFailureScope();
	t3dRdPreviewFailure_t eFailure = T3D_RD_PREVIEW_SYSTEMIC_RD;
	bool bScheduled = false;
	do {
		if ( !m_bPendingPreviewMeshesValid || rServices.m_shaderLibrary.getInstances().is_null() ) {
			break;
		}
		if ( !rServices.m_host.ensurePreviewSharedVt( T3D_VT_SHARED_RESOURCES_PREVIEW ) ) {
			break;
		}
		if ( !m_bPreviewRendererInitialized ) {
			m_bPreviewRendererInitialized = m_previewRenderer.initializePreview();
			if ( m_bPreviewRendererInitialized ) {
				rServices.m_host.setPreviewReady(
				        rServices.m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ), T3D_RENDER_READY_PREVIEW );
			}
		}
		if ( !m_bPreviewRendererInitialized
		        || !m_previewRenderer.renderPreviewToTarget( &m_pendingPreviewMeshes, m_nPendingPreviewAssetId, m_pendingPreviewSize ) ) {
			break;
		}
		const Callable callback = callable_mp( rServices.m_effect.ptr(), &Terrain3DRdCompositorEffect::consumePreviewReadbackOnRenderThread )
		                                  .bind( ( int32_t )nSlot, ( int64_t )nRequestGeneration );
		if ( !m_previewRenderer.schedulePreviewReadback( callback ) ) {
			break;
		}
		{
			Terrain3DScopedLock state_lock( m_previewStateMutex );
			if ( nSlot < T3D_PREVIEW_READBACK_SLOT_COUNT && m_szPreviewReadbackSlots[nSlot].bActive
			        && m_szPreviewReadbackSlots[nSlot].nRequestGeneration == nRequestGeneration ) {
				m_szPreviewReadbackSlots[nSlot].bReadbackInFlight = true;
				m_szPreviewReadbackSlots[nSlot].nReadbackSubmitUsec =
				        Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
			}
		}
		eFailure = T3D_RD_PREVIEW_NONE;
		bScheduled = true;
	} while ( false );
	if ( !bScheduled ) {
		m_previewRenderer.shutdown();
		m_bPreviewRendererInitialized = false;
		rServices.m_host.clearPreviewReady( T3D_RENDER_READY_PREVIEW );
		completePreviewAsyncOnRenderThread( nSlot, eFailure, Ref<Image>() );
	}
	context.endRecoverableFailureScope();
}

void Terrain3DRdPreviewService::consumePreviewReadbackOnRenderThread( const t3dPreviewServices_t &rServices, const uint32_t nSlot, const uint64_t nRequestGeneration, const PackedByteArray &rReadbackBytes ) {
	Vector2i size;
	{
		Terrain3DScopedLock state_lock( m_previewStateMutex );
		if ( nSlot >= T3D_PREVIEW_READBACK_SLOT_COUNT ) {
			++m_nPreviewAsyncLateCallbackCount;
			return;
		}
		const t3dPreviewReadbackSlot_t &slot = m_szPreviewReadbackSlots[nSlot];
		if ( !slot.bActive || slot.nRequestGeneration != nRequestGeneration
		        || slot.nLifecycleGeneration != m_nPreviewAsyncLifecycleGeneration ) {
			++m_nPreviewAsyncLateCallbackCount;
			return;
		}
		size = slot.size;
	}
	if ( rReadbackBytes.size() != ( int64_t )size.x * ( int64_t )size.y * 4 ) {
		completePreviewAsyncOnRenderThread( nSlot, T3D_RD_PREVIEW_SYSTEMIC_RD, Ref<Image>() );
		return;
	}
	Ref<Image> image = Image::create_from_data( size.x, size.y, false, Image::FORMAT_RGBA8, rReadbackBytes );
	if ( image.is_null() ) {
		completePreviewAsyncOnRenderThread( nSlot, T3D_RD_PREVIEW_SYSTEMIC_RD, Ref<Image>() );
		return;
	}
	image->flip_y();
	completePreviewAsyncOnRenderThread( nSlot, T3D_RD_PREVIEW_NONE, image );
	servicePreviewVtRepublishOnRenderThread( rServices );
}

Dictionary Terrain3DRdPreviewService::getForceRebakeStatusCold() const {
	Terrain3DScopedLock lock( m_previewVtMutex );
	Dictionary result;
	result["published_serial"] = m_nForcePublishedSerial;
	result["publication_serial"] = m_nPreviewVtPublishCount;
	result["package_generation"] = m_nPreviewVtPublishedPackageGeneration;
	result["forced_material_allocations"] = m_previewVtEditorTelemetry.m_nDirtyAllocations;
	result["pending"] = m_bPreviewVtRepublishPending || m_bPreviewVtRepublishInProgress || m_bPreviewVtRepublishQueued;
	result["error"] = !m_previewVtLastRepublishError.is_empty() ? m_previewVtLastRepublishError : m_previewVtLastCookError;
	return result;
}

void Terrain3DRdPreviewService::setForceOwnerPendingCold( const bool bPending ) {
	Terrain3DScopedLock lock( m_previewVtMutex );
	m_bForceOwnerPending = bPending;
	if ( !bPending ) { m_bPreviewVtAuthoringDirty = false; }
}
