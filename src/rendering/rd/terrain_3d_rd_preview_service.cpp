// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_preview_service.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"

#include "assets/terrain_3d_assets.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "core/logger.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "eastl_config.h"
#include "memory/terrain_3d_memory.h"
#include "rendering/rd/terrain_3d_rd_context.h"
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
	const uint64_t nTerrainGeneration = rAssets->getTexturePublicationGeneration();
	nGeneration ^= nTerrainGeneration + 0x9e3779b97f4a7c15ULL + ( nGeneration << 6u ) + ( nGeneration >> 2u );
	return nGeneration == 0u ? 1u : nGeneration;
}

bool Terrain3DRdPreviewService::bindRuntimeAllocator( Terrain3DRenderCoordinator &rCoordinator,
        Terrain3DMemoryZone *const pZone,
        const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone || !m_previewRenderer.bindRuntimeAllocator( pZone, ePolicy )
	        || !m_previewRenderer.attachVirtualTextureCold( &rCoordinator.m_virtualTexture ) ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_cachedPreviewMeshes.bindRuntimeAllocator( allocator );
	m_pendingPreviewMeshes.bindRuntimeAllocator( allocator );
	return true;
}

void Terrain3DRdPreviewService::updateAuthoringCold( Terrain3DRenderCoordinator &rCoordinator ) {
	if ( rCoordinator.m_pTerrain == NULL ) {
		return;
	}
	const Ref<Terrain3DAssets> assets = rCoordinator.m_pTerrain->getAssets();
	const uint64_t nMaterialSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( assets );
	if ( assets.is_valid() && nMaterialSourceGeneration != 0u
	        && nMaterialSourceGeneration != m_nPreviewVtObservedMaterialSourceGeneration ) {
		m_nPreviewVtObservedMaterialSourceGeneration = nMaterialSourceGeneration;
		String vtError;
		if ( !preparePreviewAuthoringPackageCold( rCoordinator, assets, &vtError ) && !vtError.is_empty() ) {
			LOG( WARN, "[RD instances] Editor VT refresh failed after material source change: ", vtError );
		}
	}
	( void )queuePreviewVtRepublishCold( rCoordinator );
}

void Terrain3DRdPreviewService::shutdownRendererOnRenderThread( Terrain3DRenderCoordinator &rCoordinator ) {
	if ( m_bPreviewRendererInitialized ) {
		m_previewRenderer.shutdown();
		m_bPreviewRendererInitialized = false;
		rCoordinator.clearRenderLifecycleReadyBitOnRenderThread( T3D_RENDER_READY_PREVIEW );
	}
}

void Terrain3DRdPreviewService::resetOwnerCold( void ) {
	m_bPreviewRendererInitialized = false;
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		++m_nPreviewAsyncLifecycleGeneration;
		if ( m_nPreviewAsyncLifecycleGeneration == 0u ) {
			++m_nPreviewAsyncLifecycleGeneration;
		}
		for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
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
		t3dScopedLock_t vt_lock( m_previewVtMutex );
		m_previewVtPackageBytes.clear();
		m_previewVtPublishedPackageBytes.clear();
		m_nPreviewVtFingerprint = 0u;
		m_nPreviewVtPublishedFingerprint = 0u;
		m_nPreviewVtPackageGeneration = 0u;
		m_nPreviewVtPublishedPackageGeneration = 0u;
		m_nPreviewVtRecookCount = 0u;
		m_nPreviewVtObservedMaterialSourceGeneration = 0u;
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
	t3dScopedLock_t state_lock( m_previewStateMutex );
	for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
		if ( m_szPreviewReadbackSlots[iSlot].bReadbackInFlight ) {
			return true;
		}
	}
	return false;
}

void Terrain3DRdPreviewService::noteSharedResourceClassTransition( void ) {
	t3dScopedLock_t vt_lock( m_previewVtMutex );
	++m_nSharedResourceClassTransitionCount;
}

bool Terrain3DRdPreviewService::bakePreviewVirtualTexturesCold( Terrain3DRenderCoordinator &rCoordinator, String *const pError ) {
	if ( rCoordinator.m_pTerrain == NULL || !rCoordinator.m_pTerrain->isEditorAuthoringMode() || Engine::get_singleton() == NULL
	        || !Engine::get_singleton()->is_editor_hint() ) {
		if ( pError ) {
			*pError = "Virtual-texture authoring bake is available only in Editor Authoring mode";
		}
		return false;
	}
	const Ref<Terrain3DAssets> assets = rCoordinator.m_pTerrain->getAssets();
	if ( assets.is_null() ) {
		if ( pError ) {
			*pError = "Virtual-texture authoring bake has no Terrain3DAssets source";
		}
		return false;
	}

	// Explicit user action is allowed to retry a quarantined generation. Keep the
	// content-addressed P08 cache intact so unchanged allocations/channels remain hits.
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		m_nPreviewVtStagedMaterialSourceGeneration = 0u;
		m_nPreviewVtFailedMaterialSourceGeneration = 0u;
		m_nPreviewVtFailedFingerprint = 0u;
		m_previewVtLastCookError = String();
	}
	m_nPreviewVtObservedMaterialSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( assets );
	return preparePreviewAuthoringPackageCold( rCoordinator, assets, pError );
}

bool Terrain3DRdPreviewService::preparePreviewAuthoringPackageCold( Terrain3DRenderCoordinator &rCoordinator, const Ref<Terrain3DAssets> &rAssets, String *const pError ) {
	if ( rAssets.is_null() ) {
		if ( pError ) {
			*pError = "RD preview VT has no Terrain3DAssets source";
		}
		return false;
	}
	const uint64_t nSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( rAssets );
	m_nPreviewVtObservedMaterialSourceGeneration = nSourceGeneration;
	bool bRepublishPending = false;
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		if ( nSourceGeneration != 0u && nSourceGeneration == m_nPreviewVtFailedMaterialSourceGeneration ) {
			if ( pError ) {
				*pError = m_previewVtLastCookError.is_empty() ? String( "RD preview VT transient package cook failed" )
				                                              : m_previewVtLastCookError;
			}
			return false;
		}
		if ( nSourceGeneration != 0u && nSourceGeneration == m_nPreviewVtStagedMaterialSourceGeneration ) {
			bRepublishPending = m_bPreviewVtRepublishPending;
			if ( !bRepublishPending ) {
				return true;
			}
		}
	}
	if ( bRepublishPending ) {
		( void )queuePreviewVtRepublishCold( rCoordinator );
		return true;
	}

	t3dVTGodotAssetMaterials_t materials;
	String collectError;
	if ( !t3d_vt_godot_collect_asset_materials( rAssets, materials, &collectError ) ) {
		t3dScopedLock_t lock( m_previewVtMutex );
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
			if ( cacheDirectory.is_empty() && rCoordinator.m_pTerrain != NULL ) {
				cacheDirectory = rCoordinator.m_pTerrain->getDataDirectory().path_join( ".vt_cache" );
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
		             &cookError )
		        || nFingerprint == 0u || packageBytes.is_empty() ) {
			t3dScopedLock_t lock( m_previewVtMutex );
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

	const t3dRenderLifecycleStatus_t renderStatus = rCoordinator.snapshotRenderLifecycleStatusOnOwnerThread();
	const bool bConsumersLive = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_RESOURCE_CONSUMERS ) != 0u;
	const t3dVtSharedResourceClass_t eLiveClass =
	        ( t3dVtSharedResourceClass_t )renderStatus.m_nSharedResourceClass;
	bool bNeedsPublication = false;
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		bNeedsPublication =
		        bStageNeutral != m_bPreviewVtPublishedNeutral || ( !bStageNeutral && nFingerprint != m_nPreviewVtPublishedFingerprint );
		m_previewVtPackageBytes = packageBytes;
		m_nPreviewVtFingerprint = nFingerprint;
		m_nPreviewVtPackageGeneration = nPackageGeneration;
		m_nPreviewVtStagedMaterialSourceGeneration = nSourceGeneration;
		m_nPreviewVtFailedMaterialSourceGeneration = 0u;
		m_nPreviewVtFailedFingerprint = 0u;
		m_previewVtLastCookError = String();
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
		( void )queuePreviewVtRepublishCold( rCoordinator );
		return true;
	}

	const bool bPublished = bStageNeutral ? rCoordinator.m_virtualTexture.clearPackageCold()
	                                      : rCoordinator.loadVirtualTexturePackageCold( packageBytes.ptr(), ( uint64_t )packageBytes.size() );
	if ( !bPublished ) {
		const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
		{
			t3dScopedLock_t lock( m_previewVtMutex );
			m_bPreviewVtRepublishPending = true;
			m_ePreviewVtRepublishResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
			++m_nPreviewVtRepublishRetryCount;
			m_nPreviewVtRepublishRetryAfterUsec = nNowUsec + t3dRdPreviewVtRetryDelayUsec( m_nPreviewVtRepublishRetryCount );
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
		t3dScopedLock_t lock( m_previewVtMutex );
		m_previewVtPublishedPackageBytes = packageBytes;
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

bool Terrain3DRdPreviewService::queuePreviewVtRepublishCold( Terrain3DRenderCoordinator &rCoordinator ) {
	const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		if ( !m_bPreviewVtRepublishPending || m_bPreviewVtRepublishQueued || m_bPreviewVtRepublishInProgress ) {
			return true;
		}
		if ( m_nPreviewVtRepublishRetryAfterUsec != 0u && nNowUsec < m_nPreviewVtRepublishRetryAfterUsec ) {
			return true;
		}
		m_bPreviewVtRepublishQueued = true;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		servicePreviewVtRepublishOnRenderThread( rCoordinator );
		return true;
	}
	if ( rCoordinator.m_effect.is_valid()
	        && Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( rCoordinator.m_effect.ptr(), &Terrain3DRdCompositorEffect::servicePreviewVtRepublishOnRenderThread ) ) ) {
		return true;
	}
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		m_bPreviewVtRepublishQueued = false;
		m_bPreviewVtRepublishPending = true;
		++m_nPreviewVtRepublishRetryCount;
		m_nPreviewVtRepublishRetryAfterUsec = nNowUsec + t3dRdPreviewVtRetryDelayUsec( m_nPreviewVtRepublishRetryCount );
		m_previewVtLastRepublishError = "RD preview VT republish render-thread submission failed; retry scheduled";
		++m_nPreviewVtRepublishFailureCount;
	}
	return false;
}

bool Terrain3DRdPreviewService::reloadPublishedPreviewVtPackageCold( Terrain3DRenderCoordinator &rCoordinator ) {
	PackedByteArray packageBytes;
	bool bNeutral = true;
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		if ( m_bPreviewVtPublishedNeutral || !m_previewVtPublishedPackageBytes.is_empty() ) {
			bNeutral = m_bPreviewVtPublishedNeutral;
			packageBytes = m_previewVtPublishedPackageBytes;
		} else {
			bNeutral = m_bPreviewVtStageNeutral;
			packageBytes = m_previewVtPackageBytes;
		}
	}
	if ( bNeutral ) {
		return rCoordinator.m_virtualTexture.clearPackageCold();
	}
	if ( packageBytes.is_empty() || !rCoordinator.loadVirtualTexturePackageCold( packageBytes.ptr(), ( uint64_t )packageBytes.size() ) ) {
		return false;
	}
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		++m_nPreviewVtPublishCount;
	}
	return true;
}

void Terrain3DRdPreviewService::servicePreviewVtRepublishOnRenderThread( Terrain3DRenderCoordinator &rCoordinator ) {
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
			if ( m_szPreviewReadbackSlots[iSlot].bReadbackInFlight ) {
				t3dScopedLock_t vt_lock( m_previewVtMutex );
				m_bPreviewVtRepublishQueued = false;
				return;
			}
		}
	}

	PackedByteArray stagedBytes;
	PackedByteArray previousBytes;
	uint64_t nStagedFingerprint = 0u;
	uint32_t nStagedGeneration = 0u;
	bool bStagedNeutral = true;
	bool bPreviousNeutral = true;
	t3dVtSharedResourceClass_t eResourceClass = T3D_VT_SHARED_RESOURCES_NONE;
	{
		t3dScopedLock_t lock( m_previewVtMutex );
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
		previousBytes = m_previewVtPublishedPackageBytes;
		nStagedFingerprint = m_nPreviewVtFingerprint;
		nStagedGeneration = m_nPreviewVtPackageGeneration;
		bStagedNeutral = m_bPreviewVtStageNeutral;
		bPreviousNeutral = m_bPreviewVtPublishedNeutral;
		eResourceClass = m_ePreviewVtRepublishResourceClass;
	}
	const t3dVtSharedResourceClass_t eCurrentClass = rCoordinator.m_sharedVtRuntime.get_shared_resource_class();
	if ( eCurrentClass == T3D_VT_SHARED_RESOURCES_PRODUCTION || eResourceClass == T3D_VT_SHARED_RESOURCES_NONE ) {
		eResourceClass = eCurrentClass;
	}

	rCoordinator.shutdownRendererOnRenderThread();
	bool bPublished = bStagedNeutral
	        ? rCoordinator.m_virtualTexture.clearPackageCold()
	        : ( !stagedBytes.is_empty() && rCoordinator.loadVirtualTexturePackageCold( stagedBytes.ptr(), ( uint64_t )stagedBytes.size() ) );
	if ( bPublished && eResourceClass != T3D_VT_SHARED_RESOURCES_NONE ) {
		bPublished = rCoordinator.ensureSharedVtRuntimeCold( eResourceClass );
	}

	if ( bPublished ) {
		t3dScopedLock_t lock( m_previewVtMutex );
		m_previewVtPublishedPackageBytes = stagedBytes;
		m_nPreviewVtPublishedFingerprint = nStagedFingerprint;
		m_nPreviewVtPublishedPackageGeneration = nStagedGeneration;
		m_bPreviewVtPublishedNeutral = bStagedNeutral;
		m_bPreviewVtRepublishPending = false;
		m_bPreviewVtRepublishInProgress = false;
		m_nPreviewVtRepublishRetryCount = 0u;
		m_nPreviewVtRepublishRetryAfterUsec = 0u;
		m_previewVtLastRepublishError = String();
		++m_nPreviewVtPublishCount;
		++m_nPreviewVtRepublishSuccessCount;
		return;
	}

	// The new package/resources were not fully published. Tear down any partial
	// state, restore the last committed package (including neutral), and quarantine
	// this failed material generation so it cannot cycle renderer restarts.
	rCoordinator.shutdownRendererOnRenderThread();
	bool bRolledBack = bPreviousNeutral
	        ? rCoordinator.m_virtualTexture.clearPackageCold()
	        : ( !previousBytes.is_empty() && rCoordinator.loadVirtualTexturePackageCold( previousBytes.ptr(), ( uint64_t )previousBytes.size() ) );
	if ( bRolledBack && eResourceClass != T3D_VT_SHARED_RESOURCES_NONE ) {
		bRolledBack = rCoordinator.ensureSharedVtRuntimeCold( eResourceClass );
	}
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		// Do not enter a renderer shutdown/restart retry loop. A failed live VT
		// publication is quarantined for this material generation; a new material
		// generation can stage a fresh package later. The last good publication
		// remains active after rollback.
		m_bPreviewVtRepublishPending = false;
		m_bPreviewVtRepublishInProgress = false;
		m_nPreviewVtFailedMaterialSourceGeneration = m_nPreviewVtStagedMaterialSourceGeneration;
		m_nPreviewVtRepublishRetryCount = 0u;
		m_nPreviewVtRepublishRetryAfterUsec = 0u;
		m_previewVtLastRepublishError = bRolledBack
		        ? String( "RD preview VT live republish failed; previous publication restored; generation quarantined" )
		        : String( "RD preview VT live republish failed; rollback failed; generation quarantined" );
		++m_nPreviewVtRepublishFailureCount;
	}
}

void Terrain3DRdPreviewService::renderPreviewOnRenderThread( Terrain3DRenderCoordinator &rCoordinator ) {
	t3dScopedLock_t lock( m_previewStateMutex );
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
		if ( rCoordinator.m_shaderLibrary.getInstances().is_null() ) {
			m_pendingPreviewError = "RD editor preview base instance shader is unavailable";
			break;
		}
		if ( !rCoordinator.ensureSharedVtRuntimeCold( T3D_VT_SHARED_RESOURCES_PREVIEW ) ) {
			m_pendingPreviewError = "RD editor preview shared VT resources are unavailable";
			break;
		}
		if ( !m_bPreviewRendererInitialized ) {
			m_bPreviewRendererInitialized = m_previewRenderer.initializePreview( rCoordinator.m_shaderLibrary.getInstances() );
			if ( m_bPreviewRendererInitialized ) {
				rCoordinator.setRenderLifecycleReadyBitOnRenderThread(
				        rCoordinator.m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ), T3D_RENDER_READY_PREVIEW );
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
		rCoordinator.clearRenderLifecycleReadyBitOnRenderThread( T3D_RENDER_READY_PREVIEW );
	}
	context.endRecoverableFailureScope();
}

Ref<Image> Terrain3DRdPreviewService::failMeshPreview( Terrain3DRenderCoordinator &rCoordinator, const t3dRdPreviewFailure_t eFailure, const String &rError, t3dRdPreviewFailure_t *const pFailure ) {
	if ( pFailure ) {
		*pFailure = eFailure;
	}
	t3dScopedLock_t lock( rCoordinator.m_diagnosticsMutex );
	m_previewLastError = rError;
	m_lastPreviewFailure = eFailure;
	++m_nPreviewFailureCount;
	return Ref<Image>();
}

bool Terrain3DRdPreviewService::prepareMeshPreviewSourceCold( Terrain3DRenderCoordinator &rCoordinator, const uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *const pFailure ) {
	if ( pFailure ) {
		*pFailure = T3D_RD_PREVIEW_NONE;
	}
	if ( !rCoordinator.m_pTerrain || !rCoordinator.m_effect.is_valid() ) {
		( void )failMeshPreview( rCoordinator, T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview coordinator is unavailable", pFailure );
		return false;
	}
	// Preview rendering/readback owns separate RD targets. Region-cache uploads do
	// not alias those targets, so global authoring residency is not a correctness
	// prerequisite. Transient deferral is reserved for preview-owned in-flight
	// slots and publication/rebuild seams instead of unrelated terrain streaming.
	// Mesh-list edits can still discover a larger immutable instance mesh source.
	// Do not launch previews against the stale bank while that cold capacity
	// transition is pending.
	if ( rCoordinator.m_publicationBuilder.isInstancePublicationCapacityPending() || rCoordinator.m_publicationBuilder.isInstanceMeshBankCapacityPending() ) {
		( void )failMeshPreview( rCoordinator, T3D_RD_PREVIEW_TRANSIENT_DEFERRED, "RD preview is waiting for instance publication capacity", pFailure );
		return false;
	}
	const Ref<Terrain3DAssets> assets = rCoordinator.m_pTerrain->getAssets();
	if ( rSize.x < 2 || rSize.y < 2 || assets.is_null() || nAssetId >= assets->getMeshCount() ) {
		( void )failMeshPreview( rCoordinator, T3D_RD_PREVIEW_ASSET_SOURCE, "RD preview asset or dimensions are invalid", pFailure );
		return false;
	}
	const uint64_t fingerprint = Terrain3DRdMeshCache::computeSourceFingerprintForAsset( assets, nAssetId );
	if ( !m_bCachedPreviewMeshesValid || nAssetId != m_nCachedPreviewAssetId || fingerprint != m_nPreviewMeshFingerprint ) {
		String error;
		m_nPreviewMeshGeneration = rCoordinator.advanceGeneration( m_nPreviewMeshGeneration );
		if ( !Terrain3DRdMeshCache::buildSourceSnapshotForAsset(
		             assets, nAssetId, m_cachedPreviewMeshes, fingerprint, m_nPreviewMeshGeneration, &error ) ) {
			( void )failMeshPreview( rCoordinator, T3D_RD_PREVIEW_ASSET_SOURCE, error, pFailure );
			return false;
		}
		m_bCachedPreviewMeshesValid = true;
		m_nPreviewMeshFingerprint = fingerprint;
		m_nCachedPreviewAssetId = nAssetId;
	}
	String vtError;
	if ( !preparePreviewAuthoringPackageCold( rCoordinator, assets, &vtError ) ) {
		LOG( WARN, "[RD instances] Mesh preview VT package unavailable; using neutral material backing: ", vtError );
	}
	{
		t3dScopedLock_t lock( m_previewStateMutex );
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
			( void )failMeshPreview( rCoordinator, T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview snapshot copy failed", pFailure );
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

Ref<Image> Terrain3DRdPreviewService::createMeshPreview( Terrain3DRenderCoordinator &rCoordinator, const uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *const pFailure ) {
	t3dScopedLock_t call_lock( m_previewCallMutex );
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
			if ( m_szPreviewReadbackSlots[iSlot].bActive ) {
				return failMeshPreview( rCoordinator, T3D_RD_PREVIEW_TRANSIENT_DEFERRED, "RD async preview readback is in flight", pFailure );
			}
		}
	}
	if ( !prepareMeshPreviewSourceCold( rCoordinator, nAssetId, rSize, pFailure ) ) {
		return Ref<Image>();
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		renderPreviewOnRenderThread( rCoordinator );
	} else {
		if ( rCoordinator.m_effect.is_null()
		        || !Terrain3DRdContext::enqueueOnRenderThread(
		                callable_mp( rCoordinator.m_effect.ptr(), &Terrain3DRdCompositorEffect::renderPreviewOnRenderThread ) ) ) {
			return failMeshPreview( rCoordinator, T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview render-thread submission failed", pFailure );
		}
		Terrain3DRdContext::forceSync();
	}
	t3dScopedLock_t lock( m_previewStateMutex );
	if ( m_pendingPreviewImage.is_null() ) {
		return failMeshPreview( rCoordinator, m_pendingPreviewFailure == T3D_RD_PREVIEW_NONE ? T3D_RD_PREVIEW_SYSTEMIC_RD : m_pendingPreviewFailure,
		        m_pendingPreviewError,
		        pFailure );
	}
	{
		t3dScopedLock_t diagnostics_lock( rCoordinator.m_diagnosticsMutex );
		m_previewLastError = String();
		m_lastPreviewFailure = T3D_RD_PREVIEW_NONE;
	}
	return m_pendingPreviewImage;
}

bool Terrain3DRdPreviewService::requestMeshPreviewAsync( Terrain3DRenderCoordinator &rCoordinator, const uint16_t nAssetId,
        const Vector2i &rSize,
        const uint64_t nCompletionOwnerId,
        const uint64_t nCompletionGeneration,
        t3dRdPreviewFailure_t *const pFailure ) {
	t3dScopedLock_t call_lock( m_previewCallMutex );
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
			if ( m_szPreviewReadbackSlots[iSlot].bActive ) {
				if ( pFailure ) {
					*pFailure = T3D_RD_PREVIEW_TRANSIENT_DEFERRED;
				}
				return false;
			}
		}
	}
	if ( !prepareMeshPreviewSourceCold( rCoordinator, nAssetId, rSize, pFailure ) ) {
		return false;
	}
	const Ref<Terrain3DAssets> assets = rCoordinator.m_pTerrain ? rCoordinator.m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
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
		t3dScopedLock_t state_lock( m_previewStateMutex );
		for ( uint32_t i = 0u; i < s_nPreviewReadbackSlotCount; ++i ) {
			const uint32_t iSlot = ( m_nPreviewReadbackCursor + i ) % s_nPreviewReadbackSlotCount;
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
		m_nPreviewReadbackCursor = ( nSlot + 1u ) % s_nPreviewReadbackSlotCount;
	}
	if ( rCoordinator.m_effect.is_null()
	        || !Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( rCoordinator.m_effect.ptr(), &Terrain3DRdCompositorEffect::renderPreviewAsyncOnRenderThread ) ) ) {
		t3dScopedLock_t state_lock( m_previewStateMutex );
		m_szPreviewReadbackSlots[nSlot] = t3dPreviewReadbackSlot_t{};
		m_nPendingPreviewReadbackSlot = UINT32_MAX;
		if ( pFailure ) {
			*pFailure = T3D_RD_PREVIEW_SYSTEMIC_RD;
		}
		return false;
	}
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
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
		t3dScopedLock_t state_lock( m_previewStateMutex );
		++m_nPreviewAsyncCompletedCount;
	}
}

void Terrain3DRdPreviewService::completePreviewAsyncOnRenderThread( const uint32_t nSlot, const t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage ) {
	t3dPreviewReadbackSlot_t slot;
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		if ( nSlot >= s_nPreviewReadbackSlotCount || !m_szPreviewReadbackSlots[nSlot].bActive ) {
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

void Terrain3DRdPreviewService::servicePreviewReadbackWatchdogCold( Terrain3DRenderCoordinator &rCoordinator ) {
	const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
	if ( nNowUsec == 0u ) {
		return;
	}
	t3dPreviewReadbackSlot_t expired[s_nPreviewReadbackSlotCount];
	uint32_t nExpired = 0u;
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
			const t3dPreviewReadbackSlot_t &slot = m_szPreviewReadbackSlots[iSlot];
			if ( !slot.bActive || !slot.bReadbackInFlight || slot.nReadbackSubmitUsec == 0u
			        || nNowUsec - slot.nReadbackSubmitUsec < s_nPreviewReadbackTimeoutUsec ) {
				continue;
			}
			expired[nExpired++] = slot;
			m_szPreviewReadbackSlots[iSlot] = t3dPreviewReadbackSlot_t{};
			if ( m_nPendingPreviewReadbackSlot == iSlot ) {
				m_nPendingPreviewReadbackSlot = UINT32_MAX;
			}
		}
		if ( nExpired > 0u ) {
			++m_nPreviewAsyncLifecycleGeneration;
			if ( m_nPreviewAsyncLifecycleGeneration == 0u ) {
				++m_nPreviewAsyncLifecycleGeneration;
			}
			m_nPreviewAsyncTimeoutCount += nExpired;
		}
	}
	for ( uint32_t iExpired = 0u; iExpired < nExpired; ++iExpired ) {
		dispatchPreviewAsyncCompletionCold( expired[iExpired], T3D_RD_PREVIEW_TRANSIENT_DEFERRED, Ref<Image>() );
	}
	if ( nExpired > 0u ) {
		( void )queuePreviewVtRepublishCold( rCoordinator );
	}
}

void Terrain3DRdPreviewService::cancelPreviewAsyncRequestsOnRenderThread( void ) {
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		++m_nPreviewAsyncLifecycleGeneration;
		if ( m_nPreviewAsyncLifecycleGeneration == 0u ) {
			++m_nPreviewAsyncLifecycleGeneration;
		}
		m_nPendingPreviewReadbackSlot = UINT32_MAX;
	}
	for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
		if ( m_szPreviewReadbackSlots[iSlot].bActive ) {
			completePreviewAsyncOnRenderThread( iSlot, T3D_RD_PREVIEW_TRANSIENT_DEFERRED, Ref<Image>() );
		}
	}
}

void Terrain3DRdPreviewService::renderPreviewAsyncOnRenderThread( Terrain3DRenderCoordinator &rCoordinator ) {
	uint32_t nSlot = UINT32_MAX;
	uint64_t nRequestGeneration = 0u;
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		nSlot = m_nPendingPreviewReadbackSlot;
		if ( nSlot >= s_nPreviewReadbackSlotCount || !m_szPreviewReadbackSlots[nSlot].bActive ) {
			return;
		}
		nRequestGeneration = m_szPreviewReadbackSlots[nSlot].nRequestGeneration;
	}
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	context.beginRecoverableFailureScope();
	t3dRdPreviewFailure_t eFailure = T3D_RD_PREVIEW_SYSTEMIC_RD;
	bool bScheduled = false;
	do {
		if ( !m_bPendingPreviewMeshesValid || rCoordinator.m_shaderLibrary.getInstances().is_null() ) {
			break;
		}
		if ( !rCoordinator.ensureSharedVtRuntimeCold( T3D_VT_SHARED_RESOURCES_PREVIEW ) ) {
			break;
		}
		if ( !m_bPreviewRendererInitialized ) {
			m_bPreviewRendererInitialized = m_previewRenderer.initializePreview( rCoordinator.m_shaderLibrary.getInstances() );
			if ( m_bPreviewRendererInitialized ) {
				rCoordinator.setRenderLifecycleReadyBitOnRenderThread(
				        rCoordinator.m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ), T3D_RENDER_READY_PREVIEW );
			}
		}
		if ( !m_bPreviewRendererInitialized
		        || !m_previewRenderer.renderPreviewToTarget( &m_pendingPreviewMeshes, m_nPendingPreviewAssetId, m_pendingPreviewSize ) ) {
			break;
		}
		const Callable callback = callable_mp( rCoordinator.m_effect.ptr(), &Terrain3DRdCompositorEffect::consumePreviewReadbackOnRenderThread )
		                                  .bind( ( int32_t )nSlot, ( int64_t )nRequestGeneration );
		if ( !m_previewRenderer.schedulePreviewReadback( callback ) ) {
			break;
		}
		{
			t3dScopedLock_t state_lock( m_previewStateMutex );
			if ( nSlot < s_nPreviewReadbackSlotCount && m_szPreviewReadbackSlots[nSlot].bActive
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
		rCoordinator.clearRenderLifecycleReadyBitOnRenderThread( T3D_RENDER_READY_PREVIEW );
		completePreviewAsyncOnRenderThread( nSlot, eFailure, Ref<Image>() );
	}
	context.endRecoverableFailureScope();
}

void Terrain3DRdPreviewService::consumePreviewReadbackOnRenderThread( Terrain3DRenderCoordinator &rCoordinator, const uint32_t nSlot, const uint64_t nRequestGeneration, const PackedByteArray &rReadbackBytes ) {
	Vector2i size;
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		if ( nSlot >= s_nPreviewReadbackSlotCount ) {
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
	servicePreviewVtRepublishOnRenderThread( rCoordinator );
}
