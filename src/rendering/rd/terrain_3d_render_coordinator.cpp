// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"

#include "assets/terrain_3d_assets.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "authoring/vt/terrain_3d_vt_godot_cooker.h"
#include "core/logger.h"
#include "data/terrain_3d_data.h"
#include "data/terrain_3d_region.h"
#include "instances/terrain_3d_instancer.h"
#include "instances/terrain_3d_instance_occlusion.h"
#include "instances/terrain_3d_instancer_bringup.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "memory/terrain_3d_memory.h"
#include "rendering/terrain_3d_material.h"
#include "rendering/rd/terrain_3d_rd_forward_plus_bridge.h"
#include "terrain_3d.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/fast_noise_lite.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/object.hpp>

#include <math.h>
#include <string.h>


static Terrain3DHLODStreamer *t3dRdGetInstanceHlodStreamer( Terrain3D *const pTerrain ) {
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	( void )pTerrain;
	return NULL;
#else
	return pTerrain ? pTerrain->getHlodStreamer() : NULL;
#endif
}

static bool t3dRdShouldReportInstanceFailure( const uint64_t nFailureCount ) {
	return nFailureCount <= 4u || ( nFailureCount & ( nFailureCount - 1u ) ) == 0u;
}

static uint64_t t3dRdPreviewVtRetryDelayUsec( const uint64_t nRetryCount ) {
	const uint32_t nShift = ( uint32_t )( MIN( nRetryCount > 0u ? nRetryCount - 1u : 0u, 4u ) );
	return 100000u << nShift;
}

static uint64_t t3dRdStartupTicksUsec( void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_usec() : 0u;
}

static Vector4 t3dRdGlobalVector4( const char *const pszName ) {
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) {
		return Vector4();
	}
	const Variant value = pServer->global_shader_parameter_get( StringName( pszName ) );
	if ( value.get_type() == Variant::VECTOR4 ) {
		return ( Vector4 )( value );
	}
	if ( value.get_type() == Variant::COLOR ) {
		const Color color = value;
		return Vector4( color.r, color.g, color.b, color.a );
	}
	return Vector4();
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

static float t3dRdMaterialFloatParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const float fDefault ) {
	if ( !pMaterial ) {
		return fDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT ? ( float )( value ) : fDefault;
}

static uint32_t t3dRdMaterialUintParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const uint32_t nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::INT ? ( uint32_t )( MAX( ( int64_t )( value ), int64_t( 0 ) ) ) : nDefault;
}

static bool t3dRdMaterialBoolParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const bool bDefault ) {
	if ( !pMaterial ) {
		return bDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::BOOL ? ( bool )( value ) : bDefault;
}

static Color t3dRdMaterialColorParam( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Color &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	if ( value.get_type() == Variant::COLOR ) {
		return ( Color )( value );
	}
	if ( value.get_type() == Variant::VECTOR3 ) {
		const Vector3 vector = value;
		return Color( vector.x, vector.y, vector.z, 1.0f );
	}
	return rDefault;
}

static Vector2 t3dRdMaterialVector2Param( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Vector2 &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::VECTOR2 ? ( Vector2 )( value ) : rDefault;
}

static Vector3 t3dRdMaterialVector3Param( const Terrain3DMaterial *const pMaterial, const char *const pszName, const Vector3 &rDefault ) {
	if ( !pMaterial ) {
		return rDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::VECTOR3 ? ( Vector3 )( value ) : rDefault;
}


static Variant t3dRdOceanParam( const ShaderMaterial *const pMaterial, const char *const pszName ) {
	return pMaterial ? pMaterial->get_shader_parameter( StringName( pszName ) ) : Variant();
}
static float t3dRdOceanFloatParam( const ShaderMaterial *const pMaterial, const char *const pszName, const float fDefault ) {
	const Variant value = t3dRdOceanParam( pMaterial, pszName );
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT ? ( float )( value ) : fDefault;
}
static Color t3dRdOceanColorParam( const ShaderMaterial *const pMaterial, const char *const pszName, const Color &rDefault ) {
	const Variant value = t3dRdOceanParam( pMaterial, pszName );
	if ( value.get_type() == Variant::COLOR ) {
		return ( Color )( value );
	}
	if ( value.get_type() == Variant::VECTOR3 ) {
		const Vector3 v = value;
		return Color( v.x, v.y, v.z, 1.0f );
	}
	return rDefault;
}

static uint32_t t3dRdSamplingMode( const t3dMaterialSamplingQuality_t quality ) {
	return quality == MATERIAL_SAMPLING_FAST ? ( uint32_t )( TERRAIN_SAMPLING_SINGLE_SAMPLE )
	                                         : ( uint32_t )( TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED );
}

static uint32_t t3dRdNormalMode( const t3dTerrainNormalQuality_t quality ) {
	return quality == TERRAIN_NORMAL_QUALITY_GEOMETRY ? ( uint32_t )( TERRAIN_NORMAL_GEOMETRY_DERIVATIVE )
	                                                  : ( uint32_t )( TERRAIN_NORMAL_DERIVED_TEXTURE );
}

static uint32_t t3dRdColorMode( const t3dTerrainColorQuality_t quality ) {
	return quality == TERRAIN_COLOR_QUALITY_FAST ? ( uint32_t )( COLOR_SAMPLING_HARDWARE_FILTERED )
	                                             : ( uint32_t )( COLOR_SAMPLING_EXACT_4_TAP );
}

static uint32_t t3dRdDetilingMode( const t3dTierDetilingQuality_t quality ) {
	return quality == TIER_DETILING_DISABLED ? ( uint32_t )( DETILING_DISABLED ) : ( uint32_t )( DETILING_DISCRETE_8 );
}

static void t3dRdFillTierConfig( t3dRdTierMaterialConfig_t &rConfig,
        const t3dMaterialSamplingQuality_t sampling,
        const t3dTerrainNormalQuality_t normal,
        const t3dTerrainColorQuality_t color,
        const t3dSurfaceResponseQuality_t surface,
        const t3dTextureSurfaceMode_t textureSurface,
        const t3dTierLightingModel_t lighting,
        const t3dTierDetilingQuality_t detiling,
        const t3dTextureFiltering_t filtering,
        const float fAuthoredColor,
        const float fWetness,
        const float fLodBias,
        const float fQualityLodOffset ) {
	rConfig.m_nSamplingMode = t3dRdSamplingMode( sampling );
	rConfig.m_nNormalMode = t3dRdNormalMode( normal );
	rConfig.m_nColorMode = t3dRdColorMode( color );
	rConfig.m_nSurfaceResponse = ( uint32_t )( surface );
	rConfig.m_nTextureSurfaceMode = ( uint32_t )( textureSurface );
	rConfig.m_nLightingModel = ( uint32_t )( lighting );
	rConfig.m_nDetilingMode = t3dRdDetilingMode( detiling );
	rConfig.m_nFiltering = ( uint32_t )( filtering );
	rConfig.m_fAuthoredColorContribution = CLAMP( fAuthoredColor, 0.0f, 1.0f );
	rConfig.m_fWetnessContribution = CLAMP( fWetness, 0.0f, 1.0f );
	rConfig.m_fMaterialLodBias = fLodBias;
	rConfig.m_fQualityLodOffset = fQualityLodOffset;
}


bool Terrain3DRenderCoordinator::bindRuntimeAllocator( Terrain3DMemoryZone *const pZone, const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone || !m_virtualTexture.bindRuntimeAllocator( pZone ) || !m_terrainMegaVT.attachVirtualTextureCold( &m_virtualTexture )
	        || !m_publicationBuilder.bindRuntimeAllocator( pZone, ePolicy ) || !m_instanceRenderer.bindRuntimeAllocator( pZone, ePolicy )
	        || !m_instanceRenderer.attachVirtualTextureCold( &m_virtualTexture )
	        || !m_previewRenderer.bindRuntimeAllocator( pZone, ePolicy ) || !m_previewRenderer.attachVirtualTextureCold( &m_virtualTexture )
	        || !m_sharedVtRuntime.bindRuntimeAllocator( pZone ) || !m_sharedVtRuntime.bindVirtualTexture( &m_virtualTexture ) ) {
		return false;
	}
	const Terrain3DEastlAllocator allocator = Terrain3DEastlAllocator::zoneAllocator( pZone, ePolicy );
	m_cachedPreviewMeshes.bindRuntimeAllocator( allocator );
	m_pendingPreviewMeshes.bindRuntimeAllocator( allocator );
	m_pRuntimeAllocatorZone = pZone;
	m_eRuntimeAllocatorOverflowPolicy = ePolicy;
	m_bRuntimeAllocatorBound = true;
	return true;
}

bool Terrain3DRenderCoordinator::resolveVirtualTextureMaterialAllocationCold( const uint64_t nStableKey, uint32_t &rAllocationId ) const {
	return m_virtualTexture.findMaterialAllocationCold( nStableKey, rAllocationId );
}

bool Terrain3DRenderCoordinator::setVirtualTextureQualityProfile( const uint32_t nProfile ) {
	return m_virtualTexture.set_quality_profile( nProfile );
}

bool Terrain3DRenderCoordinator::bakePreviewVirtualTexturesCold( String *const pError ) {
	if ( m_pTerrain == NULL || !m_pTerrain->isEditorAuthoringMode() || Engine::get_singleton() == NULL
	        || !Engine::get_singleton()->is_editor_hint() ) {
		if ( pError ) {
			*pError = "Virtual-texture authoring bake is available only in Editor Authoring mode";
		}
		return false;
	}
	const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
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
	return preparePreviewAuthoringPackageCold( assets, pError );
}

bool Terrain3DRenderCoordinator::loadVirtualTexturePackageCold( const void *const pPackageBytes, const uint64_t nPackageBytes ) {
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( !m_bRuntimeAllocatorBound || ( renderStatus.m_nReadyMask & T3D_RENDER_READY_RESOURCE_CONSUMERS ) != 0u ) {
		return false;
	}
	return m_virtualTexture.loadPackageCold( pPackageBytes, nPackageBytes );
}

bool Terrain3DRenderCoordinator::loadRuntimeVirtualTexturePackageFileCold( String *const pError ) {
	if ( m_pTerrain == NULL ) {
		if ( pError ) {
			*pError = "RD runtime VT package has no Terrain3D owner";
		}
		return false;
	}
	const String path = m_pTerrain->getDataDirectory().path_join( "terrain3d_instances.t3dvt" );
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		if ( pError ) {
			*pError = String( "RD runtime VT package is missing: " ) + path;
		}
		return false;
	}
	const PackedByteArray bytes = FileAccess::get_file_as_bytes( path );
	if ( bytes.is_empty() || !loadVirtualTexturePackageCold( bytes.ptr(), ( uint64_t )bytes.size() ) ) {
		if ( pError ) {
			*pError = String( "RD runtime VT package failed validation/publication: " ) + path;
		}
		return false;
	}
	if ( pError ) {
		*pError = String();
	}
	return true;
}

bool Terrain3DRenderCoordinator::preparePreviewAuthoringPackageCold( const Ref<Terrain3DAssets> &rAssets, String *const pError ) {
	if ( rAssets.is_null() ) {
		if ( pError ) {
			*pError = "RD preview VT has no Terrain3DAssets source";
		}
		return false;
	}
	const uint64_t nSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( rAssets );
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
		( void )queuePreviewVtRepublishCold();
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
			if ( cacheDirectory.is_empty() && m_pTerrain != NULL ) {
				cacheDirectory = m_pTerrain->getDataDirectory().path_join( ".vt_cache" );
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

	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool bConsumersLive = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_RESOURCE_CONSUMERS ) != 0u;
	const t3dRenderWorldSharedResourceClass_t eLiveClass =
	        ( t3dRenderWorldSharedResourceClass_t )renderStatus.m_nSharedResourceClass;
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
		( void )queuePreviewVtRepublishCold();
		return true;
	}

	const bool bPublished = bStageNeutral ? m_virtualTexture.clearPackageCold()
	                                      : loadVirtualTexturePackageCold( packageBytes.ptr(), ( uint64_t )packageBytes.size() );
	if ( !bPublished ) {
		const uint64_t nNowUsec = Time::get_singleton() != NULL ? Time::get_singleton()->get_ticks_usec() : 0u;
		{
			t3dScopedLock_t lock( m_previewVtMutex );
			m_bPreviewVtRepublishPending = true;
			m_ePreviewVtRepublishResourceClass = T3D_RW_SHARED_RESOURCES_NONE;
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

bool Terrain3DRenderCoordinator::queuePreviewVtRepublishCold( void ) {
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
		servicePreviewVtRepublishOnRenderThread();
		return true;
	}
	if ( m_effect.is_valid()
	        && Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::servicePreviewVtRepublishOnRenderThread ) ) ) {
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

Terrain3DRenderCoordinator::~Terrain3DRenderCoordinator( void ) {
	shutdown();
}

uint64_t Terrain3DRenderCoordinator::hashBytes( const void *const pData, const size_t nBytes ) const {
	uint64_t hash = 1469598103934665603ULL;
	const uint8_t *const bytes = ( const uint8_t * )( pData );
	for ( size_t i = 0u; i < nBytes; ++i ) {
		hash ^= ( uint64_t )( bytes[i] );
		hash *= 1099511628211ULL;
	}
	return hash;
}

uint64_t Terrain3DRenderCoordinator::advanceGeneration( const uint64_t nGeneration ) const {
	return nGeneration == UINT64_MAX ? 1u : nGeneration + 1u;
}

void Terrain3DRenderCoordinator::serviceColdCapacity( void ) {
	m_publicationBuilder.serviceColdCapacity( *this );
}

void Terrain3DRenderCoordinator::publishSnapshot( void ) {
	m_publicationBuilder.publish( *this );
}

const t3dRdFrameSnapshotSlot_t *Terrain3DRenderCoordinator::acquireSnapshotOnRenderThread( void ) {
	return m_publicationBuilder.acquireOnRenderThread();
}

void Terrain3DRenderCoordinator::resetSnapshotPublication( void ) {
	m_publicationBuilder.reset();
	m_materialStateBuilder.resetPublicationState();
}

void Terrain3DRenderCoordinator::importTerrainMegaVTPersistentStateHot( void ) {
	Terrain3DData *const pData = m_pTerrain ? m_pTerrain->getData() : NULL;
	if ( pData == NULL || !m_terrainMegaVT.isReady() ) {
		return;
	}
	for ( uint32_t iRegion = 0u; iRegion < ( uint32_t )( s_nRegionSlots ); ++iRegion ) {
		const t3dTerrainMegaVTRegion_t *const pMegaRegion = m_terrainMegaVT.getRegion( iRegion );
		if ( pMegaRegion == NULL || ( pMegaRegion->m_nFlags & T3D_TERRAIN_MEGAVT_REGION_PRESENT ) == 0u ) {
			continue;
		}
		int32_t nRegionX = 0;
		int32_t nRegionZ = 0;
		Terrain3DTerrainMegaVT::regionIndexToLocation( iRegion, nRegionX, nRegionZ );
		Terrain3DRegion *const pRegion = pData->getRegionPtr( Vector2i( nRegionX, nRegionZ ) );
		if ( pRegion == NULL || pRegion->isDeleted() ) {
			continue;
		}
		m_terrainMegaVT.importPersistentEditorStateHot(
		        iRegion, ( uint32_t )( pRegion->getMegaVTSourceGeneration() ), ( uint32_t )( pRegion->getMegaVTDirtyMip0Mask() ) );
	}
}

void Terrain3DRenderCoordinator::persistTerrainMegaVTEditorStateHot( void ) {
	Terrain3DData *const pData = m_pTerrain ? m_pTerrain->getData() : NULL;
	if ( pData == NULL || !m_terrainMegaVT.isReady() ) {
		return;
	}
	for ( uint32_t iRegion = 0u; iRegion < ( uint32_t )( s_nRegionSlots ); ++iRegion ) {
		const t3dTerrainMegaVTRegion_t *const pMegaRegion = m_terrainMegaVT.getRegion( iRegion );
		if ( pMegaRegion == NULL || ( pMegaRegion->m_nFlags & T3D_TERRAIN_MEGAVT_REGION_PRESENT ) == 0u ) {
			continue;
		}
		uint32_t nSourceGeneration = 1u;
		uint32_t nDirtyMip0Mask = 0u;
		if ( !m_terrainMegaVT.getPersistentEditorState( iRegion, nSourceGeneration, nDirtyMip0Mask ) ) {
			continue;
		}
		int32_t nRegionX = 0;
		int32_t nRegionZ = 0;
		Terrain3DTerrainMegaVT::regionIndexToLocation( iRegion, nRegionX, nRegionZ );
		Terrain3DRegion *const pRegion = pData->getRegionPtr( Vector2i( nRegionX, nRegionZ ) );
		if ( pRegion == NULL || pRegion->isDeleted() ) {
			continue;
		}
		if ( ( uint32_t )( pRegion->getMegaVTSourceGeneration() ) != nSourceGeneration
		        || ( uint32_t )( pRegion->getMegaVTDirtyMip0Mask() ) != nDirtyMip0Mask ) {
			pRegion->setMegaVTEditorStateInternal( nSourceGeneration, nDirtyMip0Mask );
		}
	}
}

void Terrain3DRenderCoordinator::onTerrainMapsEdited( const AABB &rEditedArea ) {
	if ( !m_terrainMegaVT.isReady() || rEditedArea.size.x <= 0.0f || rEditedArea.size.z <= 0.0f ) {
		return;
	}
	const Vector3 editEnd = rEditedArea.get_end();
	t3dTerrainMegaVTEditBounds_t bounds;
	bounds.m_fMinWorldX = rEditedArea.position.x;
	bounds.m_fMinWorldZ = rEditedArea.position.z;
	bounds.m_fMaxWorldX = editEnd.x;
	bounds.m_fMaxWorldZ = editEnd.z;
	if ( m_terrainMegaVT.markEditorDirtyBoundsHot( bounds ) ) {
		persistTerrainMegaVTEditorStateHot();
	}
}

bool Terrain3DRenderCoordinator::claimTerrainMegaVTIncrementalCook(
        const uint32_t nRegionMapIndex, t3dTerrainMegaVTIncrementalCookRequest_t &rRequest, t3dTerrainMegaVTCookTicket_t &rTicket ) {
	return m_terrainMegaVT.claimIncrementalCookHot( nRegionMapIndex, rRequest, rTicket );
}

bool Terrain3DRenderCoordinator::validateTerrainMegaVTIncrementalCookCompletion( const t3dTerrainMegaVTCookTicket_t &rTicket ) {
	return m_terrainMegaVT.validateIncrementalCookCompletionHot( rTicket );
}

bool Terrain3DRenderCoordinator::acceptTerrainMegaVTIncrementalCookCompletion( const t3dTerrainMegaVTCookTicket_t &rTicket ) {
	if ( !m_terrainMegaVT.acceptIncrementalCookCompletionHot( rTicket ) ) {
		return false;
	}
	persistTerrainMegaVTEditorStateHot();
	return true;
}


bool Terrain3DRenderCoordinator::attachForwardBridge( void ) {
	Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	m_bForwardBridgeAttached = pBridge && pBridge->attachCoordinator( this );
	return m_bForwardBridgeAttached && pBridge->isBridgeReady();
}

void Terrain3DRenderCoordinator::detachForwardBridge( void ) {
	Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	if ( m_bForwardBridgeAttached && pBridge ) {
		pBridge->detachCoordinator( this );
	}
	m_bForwardBridgeAttached = false;
}

bool Terrain3DRenderCoordinator::isForwardBridgeReady( void ) const {
	const Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
	return m_bForwardBridgeAttached && pBridge && pBridge->isBridgeReady();
}

void Terrain3DRenderCoordinator::updateRenderRoute( void ) {
	const bool bBridgeReady = isForwardBridgeReady();
	const uint32_t nStartupGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool bStatusGenerationMatch = nStartupGeneration != 0u && renderStatus.m_nStartupGeneration == nStartupGeneration;
	const bool bShaderGpuReady = bStatusGenerationMatch
	        && ( renderStatus.m_nReadyMask & T3D_RENDER_READY_SHADER_GPU ) != 0u;
	const bool bVtGpuReady = bStatusGenerationMatch
	        && ( renderStatus.m_nReadyMask & ( T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED ) )
	                == ( T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED );
	const bool bLifecycleRevisionMatch = renderStatus.m_nRecoveryRevision == m_nRecoveryRequestRevision;
	const bool bRenderLifecycleReady = bStatusGenerationMatch && bLifecycleRevisionMatch
	        && renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_READY
	        && ( renderStatus.m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) == T3D_RENDER_READY_STARTUP_REQUIRED;
	const bool bOwnerActive = m_pTerrain != NULL && m_pTerrain->isInitialized();
	const bool bActivationCommitted = nStartupGeneration == 0u || m_bStartupActivationCommitted;
	m_bRenderEnabled.store( bOwnerActive && bActivationCommitted && bBridgeReady && bRenderLifecycleReady && !m_bFailureLatched,
	        T3D_MEMORY_ORDER_RELEASE );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( bOwnerActive && m_bCompositorDebugEnabled && bShaderGpuReady && bVtGpuReady
		        && bRenderLifecycleReady && !m_bFailureLatched );
	}
	if ( !bBridgeReady ) {
		Terrain3DRdForwardPlusBridgeAdapter *const pBridge = Terrain3DRdForwardPlusBridgeAdapter::getSingleton();
		const String bridge_error = pBridge ? pBridge->getLastError() : String( "Terrain3D Forward+ bridge adapter is unavailable" );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		if ( m_initializationError.is_empty() ) {
			m_initializationError = bridge_error;
		}
	}
}


void Terrain3DRenderCoordinator::attachEffectToActiveCamera( void ) {
	if ( !m_pTerrain || m_effect.is_null() || !m_bCompositorDebugEnabled ) {
		return;
	}
	Camera3D *camera = NULL;
	if ( Engine::get_singleton()->is_editor_hint() ) {
		EditorInterface *const editor = EditorInterface::get_singleton();
		SubViewport *const viewport = editor ? editor->get_editor_viewport_3d( 0 ) : NULL;
		camera = viewport ? viewport->get_camera_3d() : NULL;
	} else if ( m_pTerrain->get_viewport() ) {
		camera = m_pTerrain->get_viewport()->get_camera_3d();
	}
	if ( !camera ) {
		return;
	}
	if ( m_nAttachedCameraId == camera->get_instance_id() && m_attachedCompositor.is_valid()
	        && camera->get_compositor() == m_attachedCompositor ) {
		return;
	}
	detachEffect();
	Ref<Compositor> compositor = camera->get_compositor();
	m_bCreatedCompositor = compositor.is_null();
	if ( compositor.is_null() ) {
		compositor.instantiate();
		camera->set_compositor( compositor );
	}
	TypedArray<Ref<CompositorEffect>> effects = compositor->get_compositor_effects();
	bool bFound = false;
	for ( int64_t i = 0; i < effects.size(); ++i ) {
		if ( effects[i] == m_effect ) {
			bFound = true;
			break;
		}
	}
	if ( !bFound ) {
		effects.push_back( m_effect );
		compositor->set_compositor_effects( effects );
	}
	m_attachedCompositor = compositor;
	m_nAttachedCameraId = camera->get_instance_id();
}

void Terrain3DRenderCoordinator::detachEffect( void ) {
	if ( m_effect.is_null() || m_attachedCompositor.is_null() ) {
		m_attachedCompositor.unref();
		m_nAttachedCameraId = 0u;
		m_bCreatedCompositor = false;
		return;
	}
	TypedArray<Ref<CompositorEffect>> effects = m_attachedCompositor->get_compositor_effects();
	TypedArray<Ref<CompositorEffect>> remaining;
	for ( int64_t i = 0; i < effects.size(); ++i ) {
		Ref<CompositorEffect> effect = effects[i];
		if ( effect != m_effect ) {
			remaining.push_back( effect );
		}
	}
	m_attachedCompositor->set_compositor_effects( remaining );
	if ( m_bCreatedCompositor && remaining.is_empty() && m_nAttachedCameraId != 0u ) {
		Object *const object = ObjectDB::get_instance( m_nAttachedCameraId );
		Camera3D *const camera = Object::cast_to<Camera3D>( object );
		if ( camera && camera->get_compositor() == m_attachedCompositor ) {
			camera->set_compositor( Ref<Compositor>() );
		}
	}
	m_attachedCompositor.unref();
	m_nAttachedCameraId = 0u;
	m_bCreatedCompositor = false;
}

bool Terrain3DRenderCoordinator::reloadPublishedPreviewVtPackageCold( void ) {
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
		return m_virtualTexture.clearPackageCold();
	}
	if ( packageBytes.is_empty() || !loadVirtualTexturePackageCold( packageBytes.ptr(), ( uint64_t )packageBytes.size() ) ) {
		return false;
	}
	{
		t3dScopedLock_t lock( m_previewVtMutex );
		++m_nPreviewVtPublishCount;
	}
	return true;
}

bool Terrain3DRenderCoordinator::snapshotSharedVtBindingsHot( T3DSharedVTBindings &rBindings ) const {
	return m_sharedVtRuntime.snapshot_shared_vt_bindings_hot( rBindings );
}

const T3DSharedVTBindings &Terrain3DRenderCoordinator::getSharedVtBindingsHot( void ) const {
	return m_sharedVtRuntime.get_shared_vt_bindings();
}

bool Terrain3DRenderCoordinator::ensureSharedVtRuntimeCold( const t3dRenderWorldSharedResourceClass_t eResourceClass ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	if ( !m_shaderLibrary.isReady() ) {
		return false;
	}
	const t3dRenderWorldSharedResourceClass_t eCurrentClass = m_sharedVtRuntime.get_shared_resource_class();
	if ( m_sharedVtRuntime.are_shared_resources_ready()
	        && ( eCurrentClass == eResourceClass
	                || ( eCurrentClass == T3D_RW_SHARED_RESOURCES_PRODUCTION && eResourceClass == T3D_RW_SHARED_RESOURCES_PREVIEW ) ) ) {
		publishRenderSharedResourceReadyOnRenderThread( eCurrentClass );
		return true;
	}
	if ( eCurrentClass == T3D_RW_SHARED_RESOURCES_PREVIEW && eResourceClass == T3D_RW_SHARED_RESOURCES_PRODUCTION ) {
		{
			t3dScopedLock_t state_lock( m_previewStateMutex );
			for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
				if ( m_szPreviewReadbackSlots[iSlot].bReadbackInFlight ) {
					return false;
				}
			}
		}
		{
			t3dScopedLock_t vt_lock( m_previewVtMutex );
			++m_nSharedResourceClassTransitionCount;
		}
		shutdownRendererOnRenderThread();
		const t3dRdFrameSnapshotSlot_t *const pPublication = acquireSnapshotOnRenderThread();
		const bool bEditorAuthoringMode = pPublication != NULL && pPublication->m_snapshot.m_bEditorAuthoringMode;
		if ( bEditorAuthoringMode ) {
			if ( !reloadPublishedPreviewVtPackageCold() ) {
				return false;
			}
		} else {
			String vtError;
			if ( !loadRuntimeVirtualTexturePackageFileCold( &vtError ) ) {
				return false;
			}
		}
	}


	uint32_t nPreviewMaterialCapacity = 0u;
	uint32_t nPreviewVirtualPageCapacity = 0u;
	if ( eResourceClass == T3D_RW_SHARED_RESOURCES_PREVIEW ) {
		nPreviewMaterialCapacity = MAX( 1u, m_virtualTexture.getLoadedPackageAllocationCountCold() );
		nPreviewVirtualPageCapacity = MAX( 1u, m_virtualTexture.getLoadedPackageVirtualPageCountCold() );
	}
	Terrain3DColdPhaseScope cold_phase_scope;
	const bool bReady = m_sharedVtRuntime.ensure_shared_resources_cold( Terrain3DRdContext::getShared(),
	        eResourceClass,
	        nPreviewMaterialCapacity,
	        nPreviewVirtualPageCapacity,
	        eResourceClass == T3D_RW_SHARED_RESOURCES_PRODUCTION ? &m_worldRuntime.getLimits() : NULL );
	if ( bReady ) {
		publishRenderSharedResourceReadyOnRenderThread( eResourceClass );
	}
	return bReady;
}

void Terrain3DRenderCoordinator::servicePreviewVtRepublishOnRenderThread( void ) {
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
	t3dRenderWorldSharedResourceClass_t eResourceClass = T3D_RW_SHARED_RESOURCES_NONE;
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
	const t3dRenderWorldSharedResourceClass_t eCurrentClass = m_sharedVtRuntime.get_shared_resource_class();
	if ( eCurrentClass == T3D_RW_SHARED_RESOURCES_PRODUCTION || eResourceClass == T3D_RW_SHARED_RESOURCES_NONE ) {
		eResourceClass = eCurrentClass;
	}

	shutdownRendererOnRenderThread();
	bool bPublished = bStagedNeutral
	        ? m_virtualTexture.clearPackageCold()
	        : ( !stagedBytes.is_empty() && loadVirtualTexturePackageCold( stagedBytes.ptr(), ( uint64_t )stagedBytes.size() ) );
	if ( bPublished && eResourceClass != T3D_RW_SHARED_RESOURCES_NONE ) {
		bPublished = ensureSharedVtRuntimeCold( eResourceClass );
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
	shutdownRendererOnRenderThread();
	bool bRolledBack = bPreviousNeutral
	        ? m_virtualTexture.clearPackageCold()
	        : ( !previousBytes.is_empty() && loadVirtualTexturePackageCold( previousBytes.ptr(), ( uint64_t )previousBytes.size() ) );
	if ( bRolledBack && eResourceClass != T3D_RW_SHARED_RESOURCES_NONE ) {
		bRolledBack = ensureSharedVtRuntimeCold( eResourceClass );
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

bool Terrain3DRenderCoordinator::initializeRendererOnRenderThread( const uint64_t nRenderFrame ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	// P08: forward/debug rendering never drives startup. The budgeted startup
	// service is the sole authority until the final activation barrier commits.
	if ( m_bStartupColdBegun && !m_bStartupActivationCommitted ) {
		return false;
	}
	if ( m_sharedVtRuntime.get_shared_resource_class() == T3D_RW_SHARED_RESOURCES_PREVIEW
	        && !ensureSharedVtRuntimeCold( T3D_RW_SHARED_RESOURCES_PRODUCTION ) ) {
		return false;
	}
	const bool shader_ready = m_shaderLibrary.isReady();
	bool terrain_ready = m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	bool instance_ready = m_bInstanceRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	bool ocean_ready = m_bOceanRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );

	if ( !terrain_ready ) {
		Terrain3DColdPhaseScope cold_phase_scope;
		terrain_ready = shader_ready
		        && m_renderer.initialize( m_shaderLibrary.getNear(),
		                m_shaderLibrary.getMiddle(),
		                m_shaderLibrary.getFar(),
		                m_shaderLibrary.getCull(),
		                m_shaderLibrary.getForwardBundle() );
	}

	if ( terrain_ready ) {
		Terrain3DColdPhaseScope cold_phase_scope;
		Ref<RDShaderSPIRV> world_terrain_strip[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
		Ref<RDShaderSPIRV> world_mesh[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
		for ( uint32_t i = 0u; i < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++i ) {
			world_terrain_strip[i] = m_shaderLibrary.getWorldRasterTerrainStrip( ( t3dRdForwardShaderVariant_t )i );
			world_mesh[i] = m_shaderLibrary.getWorldRasterMesh( ( t3dRdForwardShaderVariant_t )i );
		}
		terrain_ready = m_worldRuntime.initializeCold( m_shaderLibrary.getWorldHiZReduce(),
		        m_shaderLibrary.getWorldVisibilityReset(),
		        m_shaderLibrary.getWorldVisibilityCull(),
		        m_shaderLibrary.getWorldVisibilityFinalize(),
		        world_terrain_strip,
		        m_shaderLibrary.getWorldRasterTerrainStripShadow(),
		        world_mesh,
		        m_shaderLibrary.getWorldRasterMeshShadow(),
		        m_renderer.usesPersistentFrameBuffer(),
		        m_renderer.usesPersistentCpuBuffers() );
	}

	if ( terrain_ready ) {
		// Companion lanes recover independently. A valid height-driven terrain renderer
		// must not permanently suppress a companion retry after a transient RD failure.
		const bool instance_retry_allowed =
		        !instance_ready && ( nRenderFrame == UINT64_MAX || m_nLastInstanceInitializationRenderFrame != nRenderFrame );
		if ( instance_retry_allowed ) {
			Terrain3DColdPhaseScope cold_phase_scope;
			m_nLastInstanceInitializationRenderFrame = nRenderFrame;
			++m_nInstanceInitializationAttempts;
			const bool had_previous_failure = m_nInstanceInitializationFailures > 0u;
			instance_ready = m_instanceRenderer.initialize( m_shaderLibrary.getInstances(),
			        m_shaderLibrary.getHlodCull(),
			        m_shaderLibrary.getInstanceCull(),
			        m_shaderLibrary.getInstanceHiZReduce(),
			        m_shaderLibrary.getInstanceOccluderDepth(),
			        m_shaderLibrary.getForwardBundle() );
			if ( !instance_ready ) {
				++m_nInstanceInitializationFailures;
				const String instance_error = m_instanceRenderer.getLastError();
				m_instanceRenderer.shutdown();
				if ( t3dRdShouldReportInstanceFailure( m_nInstanceInitializationFailures ) ) {
					LOG( WARN, "[RD instances] Companion renderer unavailable; terrain rendering remains active: ", instance_error );
				}
			} else if ( had_previous_failure ) {
				++m_nInstanceInitializationRecoveries;
				LOG( INFO, "[RD instances] Companion renderer recovered after initialization failure" );
			}
		}

		const bool ocean_retry_allowed =
		        !ocean_ready && ( nRenderFrame == UINT64_MAX || m_nLastOceanInitializationRenderFrame != nRenderFrame );
		if ( ocean_retry_allowed ) {
			Terrain3DColdPhaseScope cold_phase_scope;
			m_nLastOceanInitializationRenderFrame = nRenderFrame;
			ocean_ready = m_oceanRenderer.initialize( m_shaderLibrary.getOceanColor(), m_shaderLibrary.getOceanShadow() );
			if ( !ocean_ready ) {
				LOG( WARN,
				        "[RD ocean] Companion renderer unavailable; terrain rendering remains active: ",
				        m_oceanRenderer.getLastError() );
			}
		}
	}

	m_bRendererInitialized.store( terrain_ready, T3D_MEMORY_ORDER_RELEASE );
	m_bInstanceRendererInitialized.store( instance_ready, T3D_MEMORY_ORDER_RELEASE );
	m_bOceanRendererInitialized.store( ocean_ready, T3D_MEMORY_ORDER_RELEASE );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( !terrain_ready ) {
		m_worldRuntime.shutdownCold();
		m_oceanRenderer.shutdown();
		m_instanceRenderer.shutdown();
		m_renderer.shutdown();
		m_bInstanceRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bOceanRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		resetForwardLifecycle();
		t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
		renderStatus.m_nReadyMask &= ~( T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD
		        | T3D_RENDER_READY_INSTANCES | T3D_RENDER_READY_OCEAN );
		publishRenderLifecycleStatusOnRenderThread( renderStatus );
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
		if ( failures >= 8u ) {
			m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		}
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = shader_ready ? m_renderer.getLastError() : m_shaderLibrary.getLastError();
	} else {
		const uint32_t nStartupGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
		uint32_t nReadyMask = T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD;
		if ( shader_ready ) {
			nReadyMask |= T3D_RENDER_READY_SHADER_GPU;
		}
		if ( instance_ready ) {
			nReadyMask |= T3D_RENDER_READY_INSTANCES;
		}
		if ( ocean_ready ) {
			nReadyMask |= T3D_RENDER_READY_OCEAN;
		}
		setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, nReadyMask );
	}
	return terrain_ready;
}

void Terrain3DRenderCoordinator::initializeOcean( void ) {
	m_publicationBuilder.initializeOcean( *this );
}

bool Terrain3DRenderCoordinator::isStartupGenerationCurrent( const uint32_t nStartupGeneration ) const {
	// P4/P8 generation invalidation is the render-lane authority. Render callbacks
	// must never reach back into the live Terrain3D node to validate ownership.
	return nStartupGeneration != 0u
	        && m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) == nStartupGeneration
	        && m_bStartupColdBegun.load( T3D_MEMORY_ORDER_ACQUIRE );
}

void Terrain3DRenderCoordinator::publishRenderLifecycleStatusOnRenderThread( const t3dRenderLifecycleStatus_t &rStatus ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	t3dScopedLock_t lock( m_renderLifecycleMutex );
	t3dRenderLifecycleStatus_t status = rStatus;
	status.m_nRevision = advanceGeneration( m_renderLifecycleStatus.m_nRevision );
	m_renderLifecycleStatus = status;
}

void Terrain3DRenderCoordinator::publishRenderLifecycleStatusCold( const t3dRenderLifecycleStatus_t &rStatus ) {
	t3dScopedLock_t lock( m_renderLifecycleMutex );
	t3dRenderLifecycleStatus_t status = rStatus;
	status.m_nRevision = advanceGeneration( m_renderLifecycleStatus.m_nRevision );
	m_renderLifecycleStatus = status;
}

t3dRenderLifecycleStatus_t Terrain3DRenderCoordinator::snapshotRenderLifecycleStatusOnOwnerThread( void ) const {
	t3dScopedLock_t lock( m_renderLifecycleMutex );
	return m_renderLifecycleStatus;
}

void Terrain3DRenderCoordinator::setRenderLifecycleReadyBitOnRenderThread(
        const uint32_t nStartupGeneration, const uint32_t nReadyBit ) {
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( status.m_nStartupGeneration != nStartupGeneration ) {
		status = t3dRenderLifecycleStatus_t{};
		status.m_nStartupGeneration = nStartupGeneration;
		status.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
	} else if ( status.m_eStage == T3D_RENDER_LIFECYCLE_IDLE || status.m_eStage == T3D_RENDER_LIFECYCLE_STOPPED ) {
		status.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
	}
	status.m_nReadyMask |= nReadyBit;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::clearRenderLifecycleReadyBitOnRenderThread( const uint32_t nReadyBit ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	status.m_nReadyMask &= ~nReadyBit;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::publishRenderSharedResourceReadyOnRenderThread(
        const t3dRenderWorldSharedResourceClass_t eResourceClass ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	const uint32_t nStartupGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( nStartupGeneration != 0u && status.m_nStartupGeneration != nStartupGeneration ) {
		status = t3dRenderLifecycleStatus_t{};
		status.m_nStartupGeneration = nStartupGeneration;
		status.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
	}
	status.m_nReadyMask |= T3D_RENDER_READY_VT_STORAGE | T3D_RENDER_READY_VT_EXECUTABLE | T3D_RENDER_READY_VT_PUBLISHED;
	status.m_nSharedResourceClass = ( uint32_t )eResourceClass;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::publishRenderLifecycleFailureOnRenderThread(
        const uint32_t nStartupGeneration, const t3dRenderLifecycleFailure_t eFailure ) {
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( status.m_nStartupGeneration != nStartupGeneration ) {
		status = t3dRenderLifecycleStatus_t{};
		status.m_nStartupGeneration = nStartupGeneration;
	}
	status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
	status.m_nFailureCode = ( uint32_t )eFailure;
	status.m_nPreparedWorldPublicationGeneration = 0u;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::acknowledgeStartupCancellationOnRenderThread( const uint32_t nStartupGeneration ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	if ( nStartupGeneration == 0u ) {
		return;
	}
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( status.m_nStartupGeneration != nStartupGeneration ) {
		status = t3dRenderLifecycleStatus_t{};
		status.m_nStartupGeneration = nStartupGeneration;
	}
	status.m_eStage = T3D_RENDER_LIFECYCLE_CANCELLING;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	publishRenderLifecycleStatusOnRenderThread( status );

	// The coordinator generation was invalidated before this callback was queued.
	// Once this callback runs, all earlier generation-bound callbacks have reached
	// the render lane and can no longer publish into a future startup generation.
	m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bShaderGpuStartupService.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupGpuServiceComplete.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationPrepareUsecLast.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupTerrainRendererPrepared = false;
	m_bStartupWorldRuntimePrepared = false;

	status.m_nReadyMask = T3D_RENDER_READY_NONE;
	status.m_eStage = T3D_RENDER_LIFECYCLE_STOPPED;
	status.m_nPreparedWorldPublicationGeneration = 0u;
	publishRenderLifecycleStatusOnRenderThread( status );
}

bool Terrain3DRenderCoordinator::queueStartupCancellationAckCold(
        const uint32_t nCancelledGeneration, const bool bForceDrain ) {
	if ( nCancelledGeneration == 0u ) {
		return true;
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		acknowledgeStartupCancellationOnRenderThread( nCancelledGeneration );
		return true;
	}
	if ( m_effect.is_valid()
	        && Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::acknowledgeStartupCancellationOnRenderThread )
	                        .bind( ( int64_t )nCancelledGeneration ) ) ) {
		if ( bForceDrain ) {
			Terrain3DRdContext::forceSync();
			++m_nStartupRenderDrainCount;
		}
		const t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
		return !bForceDrain || ( status.m_nStartupGeneration == nCancelledGeneration
		        && status.m_eStage == T3D_RENDER_LIFECYCLE_STOPPED );
	}

	RenderingServer *const pServer = RenderingServer::get_singleton();
	t3dRenderLifecycleStatus_t status{};
	status.m_nStartupGeneration = nCancelledGeneration;
	status.m_eStage = pServer == NULL ? T3D_RENDER_LIFECYCLE_STOPPED : T3D_RENDER_LIFECYCLE_FAILED;
	status.m_nFailureCode = pServer == NULL
	        ? T3D_RENDER_LIFECYCLE_FAILURE_NONE
	        : T3D_RENDER_LIFECYCLE_FAILURE_CANCELLATION_SCHEDULING;
	publishRenderLifecycleStatusCold( status );
	if ( pServer != NULL ) {
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Render-thread startup cancellation acknowledgement could not be scheduled";
		return false;
	}
	return true;
}

void Terrain3DRenderCoordinator::cancelStartupCold( const uint32_t nStartupGeneration ) {
	if ( nStartupGeneration == 0u || m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) != nStartupGeneration ) {
		return;
	}
	++m_nStartupCancellationCount;

	// P4 ordering: fail the external route first, then invalidate the coordinator
	// generation before mutating any generation-bound child startup domain.
	m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( false );
	}
	const uint32_t nCancelledGeneration = m_nStartupGeneration.exchange( 0u, T3D_MEMORY_ORDER_ACQ_REL );
	if ( nCancelledGeneration == 0u ) {
		return;
	}
	m_shaderLibrary.cancelLoadCold( nCancelledGeneration );
	m_virtualTexture.cancel_startup_cold( nCancelledGeneration );

	// The render acknowledgement is queued after every already-queued startup
	// callback. forceSync() makes the acknowledgement a cold control-plane drain,
	// after which render-visible startup bookkeeping can be reset safely.
	const bool bAcknowledged = queueStartupCancellationAckCold( nCancelledGeneration, true );
	m_bStartupColdBegun.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupActivationPrepared = false;
	m_bStartupActivationCommitted = false;
	m_nStartupActivationSnapshotGeneration = 0u;
	if ( !bAcknowledged ) {
		m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
}

bool Terrain3DRenderCoordinator::beginStartupCold( Terrain3D *const pTerrain, const uint32_t nStartupGeneration ) {
	if ( pTerrain == NULL || nStartupGeneration == 0u ) {
		return false;
	}
	if ( m_bStartupColdBegun && m_pTerrain == pTerrain && m_nStartupGeneration == nStartupGeneration ) {
		return !m_bStartupColdFailed;
	}
	shutdown();
	m_pTerrain = pTerrain;
	m_nStartupGeneration.store( nStartupGeneration, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupPublicationPrepareCount = 0u;
	m_nStartupPublicationPrepareUsecLast = 0u;
	m_nStartupRendererInitializeAttemptCount = 0u;
	m_nStartupRendererInitializeSuccessCount = 0u;
	m_nStartupRendererInitializeUsecLast = 0u;
	m_nStartupRendererInitializeUsecTotal = 0u;
	m_nStartupGpuServiceCount = 0u;
	m_nStartupGpuServiceUsecLast = 0u;
	m_nRecoveryRequestRevision = 0u;
	m_nRecoveryQueuedRevision = 0u;
	m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bRecoveryInProgress.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupStaleCompletionRejectCount.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupActivationBarrierCount = 0u;
	m_nStartupActivationBarrierUsecLast = 0u;
	m_nStartupActivationSnapshotGeneration = 0u;
	m_nStartupFinalSyncSuccessCount = 0u;
	m_nStartupFinalSyncFailureCount = 0u;
	m_nStartupFinalSyncBeginUsec = 0u;
	m_nStartupFinalSyncElapsedUsecLast = 0u;
	m_nStartupGpuStage.store( T3D_STARTUP_GPU_VT_STORAGE, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupTerrainRendererPrepared = false;
	m_bStartupWorldRuntimePrepared = false;
	m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationPrepareUsecLast.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupGpuServiceComplete.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupActivationPrepared = false;
	m_bStartupActivationCommitted = false;
	m_bStartupColdBegun.store( true, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
	{
		t3dRenderLifecycleStatus_t renderStatus{};
		renderStatus.m_nStartupGeneration = nStartupGeneration;
		renderStatus.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
		publishRenderLifecycleStatusCold( renderStatus );
	}
	m_shaderLibrary.setStartupGeneration( m_nStartupGeneration );
	m_virtualTexture.set_startup_generation( m_nStartupGeneration );
	m_virtualTexture.set_quality_profile( ( uint32_t )m_pTerrain->getVirtualTextureQualityProfile() );
	m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bShaderGpuStartupService.store( false, T3D_MEMORY_ORDER_RELEASE );
	{
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = String();
	}
	if ( m_effect.is_null() ) {
		m_effect.instantiate();
		m_effect->setCoordinator( this );
		m_effect->set_enabled( false );
	}
	attachForwardBridge();
	if ( !m_shaderLibrary.beginLoadCold( m_nStartupGeneration ) ) {
		m_bStartupColdFailed = true;
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.getLastError();
		return false;
	}
	return true;
}

void Terrain3DRenderCoordinator::serviceStartupCold( const uint32_t nStartupGeneration ) {
	if ( !m_bStartupColdBegun || m_bStartupColdFailed || nStartupGeneration == 0u || m_nStartupGeneration != nStartupGeneration ) {
		return;
	}
	m_shaderLibrary.serviceLoadCold();
	if ( m_shaderLibrary.isFailed( m_nStartupGeneration ) ) {
		m_bStartupColdFailed = true;
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.getLastError();
		return;
	}
	if ( !m_bStartupColdPrepared ) {
		return;
	}
	if ( !m_virtualTexture.is_cpu_ready() ) {
		if ( !m_sharedVtRuntime.prepare_shared_vt_cpu_cold( T3D_RW_SHARED_RESOURCES_PRODUCTION, 0u, 0u, &m_worldRuntime.getLimits() ) ) {
			m_virtualTexture.mark_startup_failed( m_nStartupGeneration );
			m_bStartupColdFailed = true;
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = "Failed preparing startup VT CPU state";
			return;
		}
	}
	serviceShaderStartupCold();
}

void Terrain3DRenderCoordinator::logStartupActivationFailureState( const char *pReason, const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t rShaderState = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t rVtState = m_virtualTexture.get_startup_snapshot();
	const Terrain3DData *const pData = m_pTerrain ? m_pTerrain->getData() : NULL;
	const t3dStartupDomainSnapshot_t dataState = pData ? pData->getStartupSnapshot() : t3dStartupDomainSnapshot_t{};
	const t3dStartupDomainSnapshot_t *const pDataState = pData ? &dataState : NULL;
	const bool bOwnerPresent = m_pTerrain != NULL;
	const bool bOwnerGenerationMatch = bOwnerPresent && m_pTerrain->getStartupGenerationInternal() == nStartupGeneration;
	const bool bDataGenerationMatch = pDataState != NULL && pDataState->m_nGeneration == nStartupGeneration;
	const bool bDataCommitted = pData != NULL && pData->isStartupCommitted( nStartupGeneration );
	const bool bShaderGenerationMatch = rShaderState.m_nGeneration == nStartupGeneration;
	const bool bVtGenerationMatch = rVtState.m_nGeneration == nStartupGeneration;
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool bRendererInitialized = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_TERRAIN ) != 0u;
	const bool bInstanceRendererInitialized = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_INSTANCES ) != 0u;
	const bool bOceanRendererInitialized = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_OCEAN ) != 0u;
	const bool bSharedVtReady = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_VT_PUBLISHED ) != 0u;
	const bool bVtInitialPublished = bSharedVtReady;
	const bool bForwardBridgeReady = isForwardBridgeReady();
	const bool bRendererFailed = renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED;
	const bool bRenderEnabled = m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE );

	LOG( ERROR,
	        "[Terrain3D][Startup][Activation][FAIL] reason=", pReason ? pReason : "unknown",
	        " requested_generation=", nStartupGeneration,
	        " coordinator_generation=", m_nStartupGeneration,
	        " owner_generation=", bOwnerPresent ? m_pTerrain->getStartupGenerationInternal() : 0u,
	        " owner_present=", bOwnerPresent,
	        " owner_generation_match=", bOwnerGenerationMatch,
	        " owner_initialized=", bOwnerPresent ? m_pTerrain->isInitialized() : false,
	        " startup_begun=", m_bStartupColdBegun.load( T3D_MEMORY_ORDER_ACQUIRE ),
	        " startup_prepared=", m_bStartupColdPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ),
	        " startup_failed=", m_bStartupColdFailed.load( T3D_MEMORY_ORDER_ACQUIRE ),
	        " failure_latched=", m_bFailureLatched );
	LOG( ERROR,
	        "[Terrain3D][Startup][Activation][FAIL][Domains] data_present=", pData != NULL,
	        " data_generation=", pDataState ? pDataState->m_nGeneration : 0u,
	        " data_generation_match=", bDataGenerationMatch,
	        " data_stage=", pDataState ? t3dStartupStageName( t3dStartupDomainGetStage( *pDataState ) ) : "MISSING",
	        " data_committed=", bDataCommitted,
	        " shader_generation=", rShaderState.m_nGeneration,
	        " shader_generation_match=", bShaderGenerationMatch,
	        " shader_stage=", t3dStartupStageName( t3dStartupDomainGetStage( rShaderState ) ),
	        " shader_ready=", ( renderStatus.m_nReadyMask & T3D_RENDER_READY_SHADER_GPU ) != 0u,
	        " vt_generation=", rVtState.m_nGeneration,
	        " vt_generation_match=", bVtGenerationMatch,
	        " vt_stage=", t3dStartupStageName( t3dStartupDomainGetStage( rVtState ) ),
	        " vt_initial_published=", bVtInitialPublished );
	LOG( ERROR,
	        "[Terrain3D][Startup][Activation][FAIL][Render] renderer_initialized=", bRendererInitialized,
	        " renderer_ready=", bRendererInitialized,
	        " renderer_failed=", bRendererFailed,
	        " world_runtime_initialized=", ( renderStatus.m_nReadyMask & T3D_RENDER_READY_WORLD ) != 0u,
	        " world_ready=", ( renderStatus.m_nReadyMask & T3D_RENDER_READY_WORLD ) != 0u,
	        " world_publication_queued=", m_bStartupWorldPublicationQueued.load( T3D_MEMORY_ORDER_ACQUIRE ),
	        " world_publication_prepared=", m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ),
	        " world_publication_generation=", renderStatus.m_nPreparedWorldPublicationGeneration,
	        " instance_renderer_initialized=", bInstanceRendererInitialized,
	        " ocean_renderer_initialized=", bOceanRendererInitialized,
	        " shared_vt_ready=", bSharedVtReady,
	        " forward_bridge_ready=", bForwardBridgeReady,
	        " activation_prepared=", m_bStartupActivationPrepared,
	        " activation_committed=", m_bStartupActivationCommitted,
	        " render_enabled=", bRenderEnabled );
	LOG( ERROR,
	        "[Terrain3D][Startup][Activation][FAIL][Lifecycle] stage=", ( uint32_t )renderStatus.m_eStage,
	        " generation=", renderStatus.m_nStartupGeneration,
	        " ready_mask=", renderStatus.m_nReadyMask,
	        " failure_code=", renderStatus.m_nFailureCode,
	        " prepared_world_generation=", renderStatus.m_nPreparedWorldPublicationGeneration,
	        " revision=", renderStatus.m_nRevision );
	LOG( ERROR,
	        "[Terrain3D][Startup][Activation][FAIL][Snapshot] published_generation=", m_publicationBuilder.getPublishedSnapshotGeneration(),
	        " activation_snapshot_generation=", m_nStartupActivationSnapshotGeneration,
	        " barrier_count=", m_nStartupActivationBarrierCount,
	        " final_sync_failures=", m_nStartupFinalSyncFailureCount,
	        " gpu_stage=", m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE ) );
}

bool Terrain3DRenderCoordinator::isStartupActivationBaseReady( const uint32_t nStartupGeneration ) const {
	if ( !m_bStartupColdBegun || m_bStartupColdFailed || !m_bStartupColdPrepared || m_pTerrain == NULL
	        || nStartupGeneration == 0u || m_nStartupGeneration != nStartupGeneration
	        || m_pTerrain->getStartupGenerationInternal() != nStartupGeneration ) {
		return false;
	}
	const t3dStartupDomainSnapshot_t rShaderState = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t rVtState = m_virtualTexture.get_startup_snapshot();
	if ( rShaderState.m_nGeneration != nStartupGeneration || rVtState.m_nGeneration != nStartupGeneration ) {
		return false;
	}
	const t3dStartupStage_t eShaderStage = t3dStartupDomainGetStage( rShaderState );
	const t3dStartupStage_t eVtStage = t3dStartupDomainGetStage( rVtState );
	const bool bShaderReady = eShaderStage == T3D_STARTUP_GPU_READY || eShaderStage == T3D_STARTUP_COMMITTED;
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool bRenderReady = renderStatus.m_nStartupGeneration == nStartupGeneration
	        && ( renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_STARTING || renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_READY )
	        && ( renderStatus.m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) == T3D_RENDER_READY_STARTUP_REQUIRED
	        && renderStatus.m_nFailureCode == T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	return bShaderReady && eVtStage == T3D_STARTUP_COMMITTED
	        && bRenderReady && isForwardBridgeReady() && !m_bFailureLatched;
}

bool Terrain3DRenderCoordinator::isStartupActivationReady( const uint32_t nStartupGeneration ) const {
	if ( !isStartupActivationBaseReady( nStartupGeneration )
	        || !m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return false;
	}
	const uint64_t nPreparedWorldGeneration = m_nStartupWorldPublicationGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	return nPreparedWorldGeneration != 0u
	        && renderStatus.m_nStartupGeneration == nStartupGeneration
	        && renderStatus.m_eStage == T3D_RENDER_LIFECYCLE_READY
	        && renderStatus.m_nPreparedWorldPublicationGeneration == nPreparedWorldGeneration
	        && renderStatus.m_nFailureCode == T3D_RENDER_LIFECYCLE_FAILURE_NONE;
}

bool Terrain3DRenderCoordinator::prepareStartupActivationCold( const uint32_t nStartupGeneration ) {
	if ( m_bStartupActivationPrepared ) {
		return m_nStartupGeneration == nStartupGeneration && !m_bStartupColdFailed;
	}
	if ( m_bStartupColdFailed ) {
		return false;
	}
	if ( m_nStartupFinalSyncBeginUsec == 0u ) {
		m_nStartupFinalSyncBeginUsec = t3dRdStartupTicksUsec();
		++m_nStartupActivationBarrierCount;
		LOG( INFO, "[Terrain3D][Startup] FINAL SYNC BEGIN generation=", nStartupGeneration );
	}
	const uint64_t nStartedUsec = m_nStartupFinalSyncBeginUsec;
	if ( !isStartupActivationBaseReady( nStartupGeneration )
	        || m_pTerrain->getData() == NULL || !m_pTerrain->getData()->isStartupCommitted( nStartupGeneration ) ) {
		m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
		logStartupActivationFailureState( "barrier_prerequisites", nStartupGeneration );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation barrier prerequisites are incomplete or generation-mismatched";
		return false;
	}

	T3DSharedVTBindings sharedBindings{};
	if ( !snapshotSharedVtBindingsHot( sharedBindings ) ) {
		m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
		logStartupActivationFailureState( "shared_vt_bindings", nStartupGeneration );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation barrier found invalid shared VT descriptor bindings";
		return false;
	}

	if ( m_nStartupActivationSnapshotGeneration == 0u ) {
		const uint64_t nSnapshotBefore = m_publicationBuilder.getPublishedSnapshotGeneration();
		publishSnapshot();
		uint64_t nSnapshotAfter = m_publicationBuilder.getPublishedSnapshotGeneration();
		if ( nSnapshotAfter == 0u ) {
			serviceColdCapacity();
			publishSnapshot();
			nSnapshotAfter = m_publicationBuilder.getPublishedSnapshotGeneration();
		}
		const t3dRenderLifecycleStatus_t snapshotStatus = snapshotRenderLifecycleStatusOnOwnerThread();
		if ( nSnapshotAfter == 0u || snapshotStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED ) {
			m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
			m_bStartupColdFailed = true;
			++m_nStartupFinalSyncFailureCount;
			m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
			LOG( ERROR, "[Terrain3D][Startup][Activation][FAIL][SnapshotPublish] before=", nSnapshotBefore,
			        " after=", nSnapshotAfter,
			        " renderer_failed=", snapshotStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED );
			logStartupActivationFailureState( "snapshot_publish", nStartupGeneration );
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = "Final startup activation barrier could not publish a valid terrain/ocean snapshot";
			return false;
		}
		m_nStartupActivationSnapshotGeneration = nSnapshotAfter;
		LOG( INFO, "[Terrain3D][Startup][Activation] SNAPSHOT generation=", nStartupGeneration,
		        " snapshot_generation=", nSnapshotAfter,
		        " previous_generation=", nSnapshotBefore,
		        " reused=", nSnapshotAfter == nSnapshotBefore );
	}

	if ( !m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		bool bExpected = false;
		if ( m_bStartupWorldPublicationQueued.compare_exchange_strong(
		             bExpected, true, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
			if ( m_effect.is_null()
			        || !Terrain3DRdContext::enqueueOnRenderThread(
			                callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::serviceStartupWorldPublicationOnRenderThread )
			                        .bind( ( int64_t )nStartupGeneration, ( int64_t )m_nStartupActivationSnapshotGeneration ) ) ) {
				m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
				m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
				++m_nStartupFinalSyncFailureCount;
				m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
				logStartupActivationFailureState( "world_publication_queue", nStartupGeneration );
				t3dScopedLock_t lock( m_diagnosticsMutex );
				m_initializationError = "Final startup activation barrier could not queue unified-world publication preparation";
				return false;
			}
			LOG( INFO, "[Terrain3D][Startup][Activation] WORLD PUBLICATION QUEUED generation=", nStartupGeneration,
			        " snapshot_generation=", m_nStartupActivationSnapshotGeneration );
		}
		return !m_bStartupColdFailed.load( T3D_MEMORY_ORDER_ACQUIRE );
	}

	if ( !isStartupActivationReady( nStartupGeneration ) ) {
		m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec;
		logStartupActivationFailureState( "world_publication_ready", nStartupGeneration );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation barrier unified-world publication did not become ready";
		return false;
	}

	m_nStartupActivationBarrierUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
	m_bStartupActivationPrepared = true;
	LOG( INFO, "[Terrain3D][Startup][Activation] PREPARED generation=", nStartupGeneration,
	        " snapshot_generation=", m_nStartupActivationSnapshotGeneration,
	        " world_publication_generation=", m_nStartupWorldPublicationGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ),
	        " elapsed=", m_nStartupActivationBarrierUsecLast, " us" );
	return true;
}

bool Terrain3DRenderCoordinator::commitStartupActivationCold( const uint32_t nStartupGeneration ) {
	if ( m_bStartupActivationCommitted ) {
		return m_nStartupGeneration == nStartupGeneration && !m_bStartupColdFailed;
	}
	if ( !m_bStartupActivationPrepared || !isStartupActivationReady( nStartupGeneration )
	        || m_pTerrain == NULL || !m_pTerrain->isInitialized() ) {
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
		logStartupActivationFailureState( "commit_prerequisites", nStartupGeneration );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation commit rejected incomplete owner/render state";
		return false;
	}
	m_shaderLibrary.markStartupCommitted( nStartupGeneration );
	if ( t3dStartupDomainGetStage( m_shaderLibrary.getStartupSnapshot() ) != T3D_STARTUP_COMMITTED ) {
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
		logStartupActivationFailureState( "shader_commit", nStartupGeneration );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation commit could not commit shader generation";
		return false;
	}
	m_bStartupActivationCommitted = true;
	updateRenderRoute();
	if ( !m_bRenderEnabled.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		m_bStartupActivationCommitted = false;
		m_bStartupColdFailed = true;
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
		logStartupActivationFailureState( "render_route_enable", nStartupGeneration );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation commit did not enable the render route";
		return false;
	}
	++m_nStartupFinalSyncSuccessCount;
	m_nStartupFinalSyncElapsedUsecLast = m_nStartupFinalSyncBeginUsec != 0u ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec : 0u;
	LOG( INFO, "[Terrain3D][Startup][Activation] COMMITTED generation=", nStartupGeneration,
	        " snapshot_generation=", m_nStartupActivationSnapshotGeneration,
	        " final_sync_elapsed=", m_nStartupFinalSyncElapsedUsecLast, " us" );
	return true;
}

bool Terrain3DRenderCoordinator::prepareStartupCold( const uint32_t nStartupGeneration ) {
	if ( !m_bStartupColdBegun || m_pTerrain == NULL || nStartupGeneration == 0u
	        || m_nStartupGeneration != nStartupGeneration
	        || m_pTerrain->getStartupGenerationInternal() != nStartupGeneration ) {
		return false;
	}
	if ( m_bStartupColdPrepared ) {
		return !m_bStartupColdFailed;
	}
	if ( m_bStartupColdFailed ) {
		return false;
	}
	uint64_t nPublicationPrepareUsec = 0u;
	++m_nStartupPublicationPrepareCount;
	if ( !m_worldRuntime.configureLimitsCold( m_pTerrain->getWorldResourceLimitsInternal() ) ) {
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Unified world limits are invalid or already initialized";
		m_bStartupColdFailed = true;
		return false;
	}
	setUnifiedWorldFramePolicy( ( uint32_t )m_pTerrain->getWorldUploadBudgetKb(),
	        ( uint32_t )m_pTerrain->getWorldClusterUploadBudget(),
	        ( uint32_t )m_pTerrain->getVtRequestBudget(),
	        ( uint32_t )m_pTerrain->getVtAdmissionBudget(),
	        false );
	// shutdown() deliberately destroys retained publication backing. Re-bind the
	// explicit Terrain3D owner before any cold capacity is re-established.
	if ( !m_bRuntimeAllocatorBound || !m_pRuntimeAllocatorZone
	        || !bindRuntimeAllocator( m_pRuntimeAllocatorZone, m_eRuntimeAllocatorOverflowPolicy ) ) {
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "RD publication owner allocator is not bound";
		m_bStartupColdFailed = true;
		return false;
	}
	{
		const uint64_t nPublicationSegmentStartedUsec = t3dRdStartupTicksUsec();
		Terrain3DColdPhaseScope cold_phase_scope;
		m_terrainMegaVT.resetCold();
		m_publicationBuilder.invalidateTerrainMegaVTPersistence();
		if ( !m_publicationBuilder.initializeFixedStorageCold() ) {
			nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
			m_nStartupPublicationPrepareUsecLast = nPublicationPrepareUsec;
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = "RD publication owner could not establish fixed region-map storage";
			m_bStartupColdFailed = true;
			return false;
		}
		nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
	}
	OS *const os = OS::get_singleton();
	const bool bCompareCulling = os && os->get_environment( "TERRAIN3D_RD_COMPARE_CULL" ) == "1";
	const bool bCompareInstanceCulling = bCompareCulling || ( os && os->get_environment( "TERRAIN3D_RD_INSTANCE_COMPARE_CULL" ) == "1" );
	const bool bDirectDebug = os && os->get_environment( "TERRAIN3D_RD_DIRECT_DRAW" ) == "1";
	m_bCompositorDebugEnabled = m_pTerrain->isEditorAuthoringMode() && os && os->get_environment( "TERRAIN3D_RD_DEBUG_COMPOSITOR" ) == "1";
	m_bMotionDebugEnabled = os && os->get_environment( "TERRAIN3D_RD_DEBUG_MOTION" ) == "1";
	m_renderer.setMotionDebugEnabled( m_bMotionDebugEnabled );
	m_instanceRenderer.setMotionDebugEnabled( m_bMotionDebugEnabled );
	m_renderer.setCullingComparisonEnabled( bCompareCulling );
	m_instanceRenderer.setCullingComparisonEnabled( bCompareInstanceCulling );
	m_renderer.setDirectDrawDebugEnabled( bDirectDebug );
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
	const bool bInstanceDirectDebug = true;
#else
	const bool bInstanceDirectDebug = os && os->get_environment( "TERRAIN3D_RD_INSTANCE_DIRECT_DRAW" ) == "1";
#endif
	m_instanceRenderer.setDirectDrawDebugEnabled( bDirectDebug || bInstanceDirectDebug );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nInstanceInitializationAttempts = 0u;
	m_nInstanceInitializationFailures = 0u;
	m_nInstanceInitializationRecoveries = 0u;
	m_nLastInstanceInitializationRenderFrame = UINT64_MAX;
	m_nLastOceanInitializationRenderFrame = UINT64_MAX;
	m_forwardPassPreparer.reset();
	m_bFailureLatched = false;
	m_nFailureLatchTerrainTextureGeneration = 0u;
	m_nFailureLatchMaterialTextureGeneration = 0u;
	const uint64_t nPublicationSegmentStartedUsec = t3dRdStartupTicksUsec();
	if ( !m_pTerrain->getMesher()
	        || !m_renderer.prepareStripParts( m_pTerrain->getMesher()->getClipmapStripParts( LOD0_MESH ),
	                m_pTerrain->getMesher()->getClipmapStripPartCount( LOD0_MESH ),
	                m_pTerrain->getMesher()->getClipmapStripParts( OUTER_RING_MESH ),
	                m_pTerrain->getMesher()->getClipmapStripPartCount( OUTER_RING_MESH ) ) ) {
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Failed to prepare procedural RenderingDevice clipmap strip descriptors";
	}
	initializeOcean();
	// The coordinator owns these retained publication banks. Establish their
	// initial capacity before the first snapshot instead of treating capacity=0
	// as a runtime miss and waiting for the first physics-frame cold service.
	serviceColdCapacity();
	if ( !m_publicationBuilder.hasRequiredInstancePublicationCapacity( *this )
	        || !m_publicationBuilder.hasRequiredInstanceMeshBankCapacity() ) {
		nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
		m_nStartupPublicationPrepareUsecLast = nPublicationPrepareUsec;
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "RD publication owner could not establish initial capacity";
		m_bStartupColdFailed = true;
		return false;
	}
	publishSnapshot();
	// The first cold publication may discover immutable mesh-source sizes that
	// were unavailable before publishSnapshot(). If so, satisfy that owner request
	// immediately and retry before initialization is exposed to READY/physics.
	if ( m_publicationBuilder.isInstancePublicationCapacityPending() || m_publicationBuilder.isInstanceMeshBankCapacityPending() ) {
		serviceColdCapacity();
		if ( m_publicationBuilder.isInstancePublicationCapacityPending() || m_publicationBuilder.isInstanceMeshBankCapacityPending() ) {
			nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
			m_nStartupPublicationPrepareUsecLast = nPublicationPrepareUsec;
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = "RD publication owner could not satisfy discovered initial capacity";
			m_bStartupColdFailed = true;
			return false;
		}
		publishSnapshot();
	}
	nPublicationPrepareUsec += t3dRdStartupTicksUsec() - nPublicationSegmentStartedUsec;
	m_nStartupPublicationPrepareUsecLast = nPublicationPrepareUsec;
	LOG( INFO,
	        "[Terrain3D][Startup][Coordinator] PUBLICATION PREP generation=",
	        m_nStartupGeneration,
	        " elapsed=",
	        m_nStartupPublicationPrepareUsecLast,
	        " us" );
	const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
	const t3dRdMeshSourceSnapshot_t &instanceMeshes = m_publicationBuilder.getCachedInstanceMeshes();
	const bool bHasInstanceVtMaterials = !instanceMeshes.m_vtStableKeys.empty();
	const bool bHasTerrainVtMaterials = assets.is_valid() && assets->usesPackedPbr() && assets->getTextureCount() > 0;
	const bool bHasSharedVtMaterials = bHasInstanceVtMaterials || bHasTerrainVtMaterials;
	String vtError;
	if ( !m_pTerrain->isEditorAuthoringMode() ) {
		if ( bHasSharedVtMaterials && !loadRuntimeVirtualTexturePackageFileCold( &vtError ) ) {
			m_virtualTexture.mark_startup_failed( m_nStartupGeneration );
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = vtError;
			m_bStartupColdFailed = true;
			LOG( ERROR, "[RD instances] Cooked runtime VT package unavailable: ", vtError );
			return false;
		}
	} else if ( Engine::get_singleton() != NULL && Engine::get_singleton()->is_editor_hint() ) {
		if ( assets.is_valid() && bHasSharedVtMaterials && !preparePreviewAuthoringPackageCold( assets, &vtError ) ) {
			LOG( WARN, "[RD instances] Editor VT package unavailable; continuing with neutral material backing: ", vtError );
		}
		m_nPreviewVtObservedMaterialSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( assets );
	}
	m_bStartupColdPrepared = true;
	updateRenderRoute();
	if ( m_bCompositorDebugEnabled ) {
		attachEffectToActiveCamera();
	}
	return true;
}

void Terrain3DRenderCoordinator::shutdown( void ) {
	const bool bStartupBegun = m_bStartupColdBegun.load( T3D_MEMORY_ORDER_ACQUIRE );
	const bool bActivationCommitted = m_bStartupActivationCommitted;
	const t3dRenderLifecycleStatus_t renderStatusBeforeShutdown = snapshotRenderLifecycleStatusOnOwnerThread();
	const bool bRenderTeardownRequired =
	        ( renderStatusBeforeShutdown.m_nReadyMask & T3D_RENDER_READY_RESOURCE_CONSUMERS ) != 0u;

	// P4: external route -> generation invalidation -> child cancellation ->
	// render acknowledgement/drain -> owner reset. Never mutate generation-bound
	// child state before the coordinator generation has been invalidated.
	m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( false );
	}
	const uint32_t nShutdownGeneration = m_nStartupGeneration.exchange( 0u, T3D_MEMORY_ORDER_ACQ_REL );
	const bool bStartupInFlight = nShutdownGeneration != 0u && bStartupBegun && !bActivationCommitted;
	if ( bStartupInFlight ) {
		++m_nStartupCancellationCount;
		m_shaderLibrary.cancelLoadCold( nShutdownGeneration );
		m_virtualTexture.cancel_startup_cold( nShutdownGeneration );
	}
	const bool bCancellationAcknowledged = nShutdownGeneration == 0u
	        || queueStartupCancellationAckCold( nShutdownGeneration, true );

	// The render acknowledgement has now passed every previously queued startup
	// callback for the invalid generation. Owner/control-plane bookkeeping can be
	// reset without racing those callbacks.
	m_bStartupActivationPrepared = false;
	m_bStartupActivationCommitted = false;
	m_nStartupActivationSnapshotGeneration = 0u;
	m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationGeneration.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationPrepareUsecLast.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdBegun.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupColdFailed.store( !bCancellationAcknowledged, T3D_MEMORY_ORDER_RELEASE );

	if ( m_effect.is_valid() ) {
		m_effect->set_enabled( false );
	}
	// Remove the Forward+ publication before RD teardown. detachCoordinator()
	// includes its own render synchronization so an acquired callback cannot retain
	// this coordinator after the bridge pointer is cleared.
	detachForwardBridge();

	// P5: one complete RD destruction implementation, render thread only. There is
	// no owner-thread renderer shutdown fallback. A pristine/CPU-only coordinator
	// has no RD renderer state to schedule, which is important for the initial
	// beginStartupCold()->shutdown() normalization path before the effect exists.
	if ( !bRenderTeardownRequired ) {
		t3dRenderLifecycleStatus_t status{};
		status.m_nStartupGeneration = nShutdownGeneration;
		status.m_eStage = T3D_RENDER_LIFECYCLE_STOPPED;
		publishRenderLifecycleStatusCold( status );
	} else if ( Terrain3DRdContext::isOnRenderThread() ) {
		shutdownRendererLifecycleOnRenderThread( nShutdownGeneration );
		++m_nStartupRenderDrainCount;
	} else if ( m_effect.is_valid()
	        && Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::shutdownRendererLifecycleOnRenderThread )
	                        .bind( ( int64_t )nShutdownGeneration ) ) ) {
		Terrain3DRdContext::forceSync();
		++m_nStartupRenderDrainCount;
	} else {
		RenderingServer *const pServer = RenderingServer::get_singleton();
		t3dRenderLifecycleStatus_t status{};
		status.m_nStartupGeneration = nShutdownGeneration;
		if ( pServer == NULL ) {
			// The engine rendering boundary is already gone. Do not manufacture an
			// owner-thread RD teardown; device/server shutdown owns remaining handles.
			status.m_eStage = T3D_RENDER_LIFECYCLE_STOPPED;
			publishRenderLifecycleStatusCold( status );
		} else {
			// RenderingServer still exists, so failing to schedule the sole render-
			// thread teardown path is an invariant failure. Fail closed and drain any
			// already queued work without touching RD resources on this thread.
			status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
			status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_TEARDOWN_SCHEDULING;
			publishRenderLifecycleStatusCold( status );
			m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
			Terrain3DRdContext::forceSync();
			t3dScopedLock_t lock( m_diagnosticsMutex );
			m_initializationError = "Render-thread renderer teardown could not be scheduled";
		}
	}

	// With successful render teardown (or after the engine rendering boundary has
	// disappeared) queued render work can no longer observe these owner records.
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		++m_nPreviewAsyncLifecycleGeneration;
		if ( m_nPreviewAsyncLifecycleGeneration == 0u ) {
			++m_nPreviewAsyncLifecycleGeneration;
		}
		for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
			m_szPreviewReadbackSlots[iSlot] = t3dPreviewReadbackSlot_t{};
		}
		m_nPendingPreviewReadbackSlot = UINT32_MAX;
	}
	{
		t3dScopedLock_t lock( m_staticScenePublicationMutex );
		m_staticScenePublication = t3dStaticScenePublication_t{};
	}
	if ( m_effect.is_valid() ) {
		m_effect->setCoordinator( NULL );
	}
	detachEffect();
	m_effect.unref();
	m_materialStateBuilder.clear();
	m_shaderLibrary.clear();
	m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bShaderGpuStartupService.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_cachedPreviewMeshes = t3dRdMeshSourceSnapshot_t{};
	m_bCachedPreviewMeshesValid = false;
	m_nPreviewMeshFingerprint = 0u;
	m_nCachedPreviewAssetId = UINT16_MAX;
	m_nPreviewMeshGeneration = 0u;
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
	m_ePreviewVtRepublishResourceClass = T3D_RW_SHARED_RESOURCES_NONE;
	{
		t3dScopedLock_t lock( m_previewStateMutex );
		m_pendingPreviewMeshes = t3dRdMeshSourceSnapshot_t{};
		m_bPendingPreviewMeshesValid = false;
		m_pendingPreviewImage.unref();
		m_pendingPreviewError = String();
	}
	m_publicationBuilder.clearCachedSources();
	resetSnapshotPublication();
	m_bCompositorDebugEnabled = false;
	m_bMotionDebugEnabled = false;
	m_pTerrain = NULL;
}

void Terrain3DRenderCoordinator::serviceShaderStartupCold( void ) {
	const uint32_t nStartupGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		return;
	}
	m_shaderLibrary.serviceLoadCold();
	if ( m_shaderLibrary.isFailed( nStartupGeneration ) ) {
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = m_shaderLibrary.getLastError();
		return;
	}
	if ( !m_bStartupColdPrepared.load( T3D_MEMORY_ORDER_ACQUIRE ) || !m_shaderLibrary.isCpuReady( nStartupGeneration )
	        || m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) || m_effect.is_null() ) {
		return;
	}
	bool expected = false;
	if ( !m_bShaderGpuStartupQueued.compare_exchange_strong( expected, true, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return;
	}
	m_shaderLibrary.markStartupGpuPending( nStartupGeneration );
	if ( !queueStartupGpuServiceOnRenderThread( nStartupGeneration ) ) {
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
		status.m_nStartupGeneration = nStartupGeneration;
		status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_STARTUP_REQUEUE;
		publishRenderLifecycleStatusCold( status );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Failed queuing initial budgeted GPU startup service";
	}
}

bool Terrain3DRenderCoordinator::queueStartupGpuServiceOnRenderThread( const uint32_t nStartupGeneration ) {
	if ( m_effect.is_null() ) {
		return false;
	}
	return Terrain3DRdContext::enqueueOnRenderThread(
	        callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::serviceShaderStartupOnRenderThread )
	                .bind( ( int64_t )nStartupGeneration ) );
}

bool Terrain3DRenderCoordinator::serviceStartupGpuOnRenderThreadBudgeted( const uint32_t nStartupGeneration ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	static const uint32_t s_nWorkUnitBudget = 1u;
	static const uint64_t s_nUploadByteBudget = 32u * 1024u * 1024u;
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		return true;
	}
	{
		t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
		if ( status.m_nStartupGeneration != nStartupGeneration ) {
			status = t3dRenderLifecycleStatus_t{};
			status.m_nStartupGeneration = nStartupGeneration;
			status.m_eStage = T3D_RENDER_LIFECYCLE_STARTING;
			publishRenderLifecycleStatusOnRenderThread( status );
		}
	}
	if ( m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		return true;
	}

	bool bReady = true;
	switch ( ( t3dStartupGpuStage_t )m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		case T3D_STARTUP_GPU_VT_STORAGE:
			bReady = m_sharedVtRuntime.service_shared_vt_storage_cold(
			        Terrain3DRdContext::getShared(), s_nWorkUnitBudget );
			if ( bReady && m_virtualTexture.is_gpu_storage_ready() ) {
				setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_VT_STORAGE );
				++m_nStartupGpuStage;
			}
			break;
		case T3D_STARTUP_GPU_VT_EXECUTABLE:
			bReady = m_sharedVtRuntime.service_shared_vt_executable_cold( s_nWorkUnitBudget );
			if ( bReady && m_virtualTexture.is_gpu_ready() ) {
				setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_VT_EXECUTABLE );
				++m_nStartupGpuStage;
			}
			break;
		case T3D_STARTUP_GPU_VT_PUBLICATION:
			bReady = m_sharedVtRuntime.service_shared_vt_publication_cold( s_nUploadByteBudget, s_nWorkUnitBudget );
			if ( bReady && m_sharedVtRuntime.are_shared_resources_ready() ) {
				setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_VT_PUBLISHED );
				++m_nStartupGpuStage;
			}
			break;
		case T3D_STARTUP_GPU_TERRAIN_RENDERER:
			if ( !m_bStartupTerrainRendererPrepared ) {
				Terrain3DColdPhaseScope cold_phase_scope;
				m_bStartupTerrainRendererPrepared = m_renderer.initialize( m_shaderLibrary.getNear(),
				        m_shaderLibrary.getMiddle(), m_shaderLibrary.getFar(), m_shaderLibrary.getCull(),
				        m_shaderLibrary.getForwardBundle() );
				bReady = m_bStartupTerrainRendererPrepared;
			}
			if ( bReady ) {
				setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_TERRAIN );
				++m_nStartupGpuStage;
			}
			break;
		case T3D_STARTUP_GPU_WORLD_RUNTIME:
			if ( !m_bStartupWorldRuntimePrepared ) {
				Terrain3DColdPhaseScope cold_phase_scope;
				Ref<RDShaderSPIRV> world_terrain_strip[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
				Ref<RDShaderSPIRV> world_mesh[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
				for ( uint32_t i = 0u; i < T3D_RD_FORWARD_SHADER_VARIANT_MAX; ++i ) {
					world_terrain_strip[i] = m_shaderLibrary.getWorldRasterTerrainStrip( ( t3dRdForwardShaderVariant_t )i );
					world_mesh[i] = m_shaderLibrary.getWorldRasterMesh( ( t3dRdForwardShaderVariant_t )i );
				}
				m_bStartupWorldRuntimePrepared = m_worldRuntime.initializeCold( m_shaderLibrary.getWorldHiZReduce(),
				        m_shaderLibrary.getWorldVisibilityReset(), m_shaderLibrary.getWorldVisibilityCull(),
				        m_shaderLibrary.getWorldVisibilityFinalize(), world_terrain_strip,
				        m_shaderLibrary.getWorldRasterTerrainStripShadow(), world_mesh,
				        m_shaderLibrary.getWorldRasterMeshShadow(), m_renderer.usesPersistentFrameBuffer(),
				        m_renderer.usesPersistentCpuBuffers() );
				bReady = m_bStartupWorldRuntimePrepared;
			}
			if ( bReady ) {
				m_bRendererInitialized.store( true, T3D_MEMORY_ORDER_RELEASE );
				setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_WORLD );
				++m_nStartupGpuStage;
			}
			break;
		case T3D_STARTUP_GPU_INSTANCE_RENDERER: {
			Terrain3DColdPhaseScope cold_phase_scope;
			++m_nInstanceInitializationAttempts;
			const bool bInstanceReady = m_instanceRenderer.initialize( m_shaderLibrary.getInstances(),
			        m_shaderLibrary.getHlodCull(), m_shaderLibrary.getInstanceCull(), m_shaderLibrary.getInstanceHiZReduce(),
			        m_shaderLibrary.getInstanceOccluderDepth(), m_shaderLibrary.getForwardBundle() );
			m_bInstanceRendererInitialized.store( bInstanceReady, T3D_MEMORY_ORDER_RELEASE );
			if ( !bInstanceReady ) {
				++m_nInstanceInitializationFailures;
				bReady = false;
			} else {
				setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_INSTANCES );
				++m_nStartupGpuStage;
			}
			break;
		}
		case T3D_STARTUP_GPU_OCEAN_RENDERER: {
			Terrain3DColdPhaseScope cold_phase_scope;
			const bool bOceanReady = m_oceanRenderer.initialize( m_shaderLibrary.getOceanColor(), m_shaderLibrary.getOceanShadow() );
			m_bOceanRendererInitialized.store( bOceanReady, T3D_MEMORY_ORDER_RELEASE );
			if ( !bOceanReady ) {
				bReady = false;
			} else {
				setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_OCEAN );
				++m_nStartupGpuStage;
			}
			break;
		}
		case T3D_STARTUP_GPU_COMPLETE:
			if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
				return true;
			}
			m_shaderLibrary.markStartupGpuReady( nStartupGeneration );
			setRenderLifecycleReadyBitOnRenderThread( nStartupGeneration, T3D_RENDER_READY_SHADER_GPU );
			if ( ( snapshotRenderLifecycleStatusOnOwnerThread().m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED )
			        != T3D_RENDER_READY_STARTUP_REQUIRED ) {
				bReady = false;
				break;
			}
			m_bStartupGpuServiceComplete.store( true, T3D_MEMORY_ORDER_RELEASE );
			++m_nStartupRendererInitializeSuccessCount;
			return true;
		default:
			bReady = false;
			break;
	}
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		return true;
	}
	if ( !bReady ) {
		m_nStartupGpuStage.store( T3D_STARTUP_GPU_FAILED, T3D_MEMORY_ORDER_RELEASE );
		const bool bRecovery = m_bRecoveryInProgress.exchange( false, T3D_MEMORY_ORDER_ACQ_REL );
		const uint64_t nRecoveryRevision = m_nRecoveryActiveRevision.exchange( 0u, T3D_MEMORY_ORDER_ACQ_REL );
		if ( !bRecovery ) {
			m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		}
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		t3dRenderLifecycleStatus_t failure = snapshotRenderLifecycleStatusOnOwnerThread();
		failure.m_nStartupGeneration = nStartupGeneration;
		failure.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		failure.m_nFailureCode = bRecovery ? T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_INITIALIZATION
		                                  : T3D_RENDER_LIFECYCLE_FAILURE_STARTUP_GPU;
		failure.m_nRecoveryRevision = nRecoveryRevision;
		failure.m_nPreparedWorldPublicationGeneration = 0u;
		publishRenderLifecycleStatusOnRenderThread( failure );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		if ( m_initializationError.is_empty() ) m_initializationError = bRecovery
		        ? String( "Budgeted render-thread recovery work unit failed" )
		        : String( "Budgeted render-thread startup work unit failed" );
		return false;
	}
	return true;
}

void Terrain3DRenderCoordinator::serviceShaderStartupOnRenderThread( const uint32_t nStartupGeneration ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		m_nStartupStaleCompletionRejectCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	if ( m_shaderLibrary.isFailed( nStartupGeneration ) || !m_shaderLibrary.isCpuReady( nStartupGeneration ) ) {
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	m_bShaderGpuStartupService.store( true, T3D_MEMORY_ORDER_RELEASE );
	const uint64_t nStartedUsec = t3dRdStartupTicksUsec();
	++m_nStartupGpuServiceCount;
	++m_nStartupRendererInitializeAttemptCount;
	const bool bReady = serviceStartupGpuOnRenderThreadBudgeted( nStartupGeneration );
	m_nStartupGpuServiceUsecLast = t3dRdStartupTicksUsec() - nStartedUsec;
	m_nStartupRendererInitializeUsecLast = m_nStartupGpuServiceUsecLast;
	m_nStartupRendererInitializeUsecTotal += m_nStartupGpuServiceUsecLast;
	m_bShaderGpuStartupService.store( false, T3D_MEMORY_ORDER_RELEASE );
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		m_nStartupStaleCompletionRejectCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	const uint32_t nGpuStage = m_nStartupGpuStage.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( !bReady ) {
		LOG( ERROR, "[Terrain3D][Startup][GPU] FAILED generation=", nStartupGeneration,
		        " stage=", nGpuStage, " callback=", m_nStartupGpuServiceCount );
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}
	LOG( INFO, "[Terrain3D][Startup][GPU] SERVICE generation=", nStartupGeneration,
	        " stage=", nGpuStage, " callback=", m_nStartupGpuServiceCount,
	        " elapsed=", m_nStartupGpuServiceUsecLast, " us" );

	if ( m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {
		const uint64_t nRecoveryRevision = m_nRecoveryActiveRevision.load( T3D_MEMORY_ORDER_ACQUIRE );
		if ( m_bRecoveryInProgress.load( T3D_MEMORY_ORDER_ACQUIRE ) && nRecoveryRevision != 0u ) {
			( void )completeRendererRecoveryOnRenderThread( nStartupGeneration, nRecoveryRevision );
		}
		m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		return;
	}

	// P6: after the first owner-thread kick, each bounded render callback schedules
	// the next bounded unit itself. The queued token remains owned until a terminal
	// state so owner physics cadence cannot insert bubbles between GPU work units.
	if ( queueStartupGpuServiceOnRenderThread( nStartupGeneration ) ) {
		return;
	}

	m_bShaderGpuStartupQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupGpuStage.store( T3D_STARTUP_GPU_FAILED, T3D_MEMORY_ORDER_RELEASE );
	const bool bRecovery = m_bRecoveryInProgress.exchange( false, T3D_MEMORY_ORDER_ACQ_REL );
	m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	if ( !bRecovery ) {
		m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
	m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	publishRenderLifecycleFailureOnRenderThread( nStartupGeneration,
	        bRecovery ? T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_SCHEDULING
	                  : T3D_RENDER_LIFECYCLE_FAILURE_STARTUP_REQUEUE );
	{
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = bRecovery
		        ? String( "Render-thread recovery could not self-requeue the next bounded GPU startup unit" )
		        : String( "Render-thread startup could not self-requeue the next bounded GPU startup unit" );
	}
}

void Terrain3DRenderCoordinator::serviceStartupWorldPublicationOnRenderThread(
        const uint32_t nStartupGeneration, const uint64_t nSnapshotGeneration ) {
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		m_nStartupStaleCompletionRejectCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		return;
	}
	const uint64_t nStartedUsec = t3dRdStartupTicksUsec();
	const t3dRdFrameSnapshotSlot_t *const pPublication = acquireSnapshotOnRenderThread();
	const bool bSnapshotMatches = pPublication != NULL
	        && pPublication->m_snapshot.m_nPublicationGeneration == nSnapshotGeneration;
	const t3dWorldPublicationBank_t *const pWorld = bSnapshotMatches
	        ? m_publicationBuilder.getWorldPublicationBank( pPublication->m_nWorldPublicationBank )
	        : NULL;
	const bool bWorldValid = pWorld != NULL && pWorld->m_publication.m_nGeneration != 0u;
	bool bPrepared = false;
	if ( bWorldValid ) {
		Terrain3DColdPhaseScope cold_phase_scope;
		bPrepared = m_worldRuntime.preparePublicationCold( pWorld->m_publication, 0u );
	}
	const uint64_t nElapsedUsec = t3dRdStartupTicksUsec() - nStartedUsec;
	if ( !isStartupGenerationCurrent( nStartupGeneration ) ) {
		m_nStartupStaleCompletionRejectCount.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED );
		return;
	}
	if ( !bSnapshotMatches || !bWorldValid || !bPrepared || !m_worldRuntime.isReady() ) {
		m_nStartupWorldPublicationPrepareUsecLast.store( nElapsedUsec, T3D_MEMORY_ORDER_RELEASE );
		m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bStartupWorldPublicationPrepared.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bStartupColdFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		publishRenderLifecycleFailureOnRenderThread( nStartupGeneration, T3D_RENDER_LIFECYCLE_FAILURE_WORLD_PUBLICATION );
		++m_nStartupFinalSyncFailureCount;
		m_nStartupFinalSyncElapsedUsecLast = m_nStartupFinalSyncBeginUsec != 0u
		        ? t3dRdStartupTicksUsec() - m_nStartupFinalSyncBeginUsec
		        : 0u;
		LOG( ERROR, "[Terrain3D][Startup][Activation][FAIL][WorldPublication] generation=", nStartupGeneration,
		        " requested_snapshot_generation=", nSnapshotGeneration,
		        " acquired_snapshot_generation=", pPublication ? pPublication->m_snapshot.m_nPublicationGeneration : 0u,
		        " world_valid=", bWorldValid,
		        " world_generation=", pWorld ? pWorld->m_publication.m_nGeneration : 0u,
		        " prepared=", bPrepared,
		        " world_ready=", m_worldRuntime.isReady(),
		        " elapsed=", nElapsedUsec, " us" );
		logStartupActivationFailureState( "world_publication_prepare", nStartupGeneration );
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Final startup activation render-thread unified-world publication preparation failed";
		return;
	}
	m_nStartupWorldPublicationGeneration.store( pWorld->m_publication.m_nGeneration, T3D_MEMORY_ORDER_RELEASE );
	m_nStartupWorldPublicationPrepareUsecLast.store( nElapsedUsec, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationPrepared.store( true, T3D_MEMORY_ORDER_RELEASE );
	m_bStartupWorldPublicationQueued.store( false, T3D_MEMORY_ORDER_RELEASE );
	t3dRenderLifecycleStatus_t lifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( lifecycle.m_nStartupGeneration != nStartupGeneration ) {
		lifecycle = t3dRenderLifecycleStatus_t{};
		lifecycle.m_nStartupGeneration = nStartupGeneration;
	}
	lifecycle.m_nPreparedWorldPublicationGeneration = pWorld->m_publication.m_nGeneration;
	lifecycle.m_eStage = ( lifecycle.m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) == T3D_RENDER_READY_STARTUP_REQUIRED
	        ? T3D_RENDER_LIFECYCLE_READY
	        : T3D_RENDER_LIFECYCLE_FAILED;
	lifecycle.m_nFailureCode = lifecycle.m_eStage == T3D_RENDER_LIFECYCLE_READY
	        ? T3D_RENDER_LIFECYCLE_FAILURE_NONE
	        : T3D_RENDER_LIFECYCLE_FAILURE_WORLD_PUBLICATION;
	publishRenderLifecycleStatusOnRenderThread( lifecycle );
	LOG( INFO, "[Terrain3D][Startup][Activation] WORLD PUBLICATION READY generation=", nStartupGeneration,
	        " snapshot_generation=", nSnapshotGeneration,
	        " world_publication_generation=", pWorld->m_publication.m_nGeneration,
	        " elapsed=", nElapsedUsec, " us" );
}

void Terrain3DRenderCoordinator::update( void ) {
	// Snapshot custom instancing lighting globals on the coordinator thread.
	// The render thread consumes only this small locked POD copy and never calls
	// back into RenderingServer for global shader parameters.
	m_instanceRenderer.setStylizedSunGlobals( t3dRdGlobalVector4( "nomad_sun_shape" ),
	        t3dRdGlobalVector4( "nomad_sun_diffuse" ),
	        t3dRdGlobalVector4( "nomad_sun_specular" ),
	        t3dRdGlobalVector4( "nomad_sun_rim" ) );
	if ( !m_pTerrain ) {
		return;
	}
	serviceShaderStartupCold();
	servicePreviewReadbackWatchdogCold();
	if ( m_pTerrain->isEditorAuthoringMode() && Engine::get_singleton() != NULL && Engine::get_singleton()->is_editor_hint() ) {
		const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
		const uint64_t nMaterialSourceGeneration = t3dRdSharedVtMaterialSourceGeneration( assets );
		if ( assets.is_valid() && nMaterialSourceGeneration != 0u
		        && nMaterialSourceGeneration != m_nPreviewVtObservedMaterialSourceGeneration ) {
			m_nPreviewVtObservedMaterialSourceGeneration = nMaterialSourceGeneration;
			String vtError;
			if ( !preparePreviewAuthoringPackageCold( assets, &vtError ) && !vtError.is_empty() ) {
				LOG( WARN, "[RD instances] Editor VT refresh failed after material source change: ", vtError );
			}
		}
		( void )queuePreviewVtRepublishCold();
	}
	publishSnapshot();
	if ( m_pTerrain->isEditorAuthoringMode() && Engine::get_singleton() != NULL && Engine::get_singleton()->is_editor_hint()
	        && ( m_publicationBuilder.isInstancePublicationCapacityPending()
	                || m_publicationBuilder.isInstanceMeshBankCapacityPending() ) ) {
		// Asset batches discover immutable mesh-bank requirements during publish.
		// Grow and republish immediately in authoring instead of leaving a frame
		// with the old bank and servicing it on the next scheduler tick.
		m_publicationBuilder.serviceColdCapacity( *this );
		publishSnapshot();
	}
	const uint64_t nTerrainTextureGeneration = m_publicationBuilder.getTerrainTextureGeneration();
	const uint64_t nMaterialTextureGeneration = m_publicationBuilder.getMaterialTextureGeneration();
	t3dRenderLifecycleStatus_t recoveryStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	if ( recoveryStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED && !m_bFailureLatched ) {
		m_bFailureLatched = true;
		m_nFailureLatchTerrainTextureGeneration = nTerrainTextureGeneration;
		m_nFailureLatchMaterialTextureGeneration = nMaterialTextureGeneration;
		m_bRenderEnabled.store( false, T3D_MEMORY_ORDER_RELEASE );
		if ( m_effect.is_valid() ) {
			m_effect->set_enabled( false );
		}
	}
	if ( m_bFailureLatched ) {
		const bool bRecoveryReady = recoveryStatus.m_eStage == T3D_RENDER_LIFECYCLE_READY
		        && recoveryStatus.m_nRecoveryRevision != 0u
		        && recoveryStatus.m_nRecoveryRevision == m_nRecoveryQueuedRevision;
		if ( bRecoveryReady ) {
			m_bFailureLatched = false;
			m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
			m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
			m_nRecoveryQueuedRevision = 0u;
			++m_nRendererRecoveryCount;
			LOG( INFO, "[RD terrain] Render-thread recovery READY revision=", recoveryStatus.m_nRecoveryRevision );
		} else if ( recoveryStatus.m_eStage == T3D_RENDER_LIFECYCLE_FAILED ) {
			const bool bRecoveryAttemptFailed = recoveryStatus.m_nRecoveryRevision != 0u
			        && recoveryStatus.m_nRecoveryRevision == m_nRecoveryQueuedRevision;
			if ( bRecoveryAttemptFailed ) {
				m_nFailureLatchTerrainTextureGeneration = nTerrainTextureGeneration;
				m_nFailureLatchMaterialTextureGeneration = nMaterialTextureGeneration;
				m_nRecoveryQueuedRevision = 0u;
			} else {
				const bool bResourceGenerationAdvanced = nTerrainTextureGeneration != m_nFailureLatchTerrainTextureGeneration
				        || nMaterialTextureGeneration != m_nFailureLatchMaterialTextureGeneration;
				if ( bResourceGenerationAdvanced && m_nRecoveryQueuedRevision == 0u ) {
					m_nRecoveryRequestRevision = advanceGeneration( m_nRecoveryRequestRevision );
					if ( queueRendererRecoveryCold( m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ), m_nRecoveryRequestRevision ) ) {
						m_nRecoveryQueuedRevision = m_nRecoveryRequestRevision;
						LOG( INFO, "[RD terrain] Resource generation advanced; queued render recovery revision=", m_nRecoveryQueuedRevision );
					} else {
						m_nFailureLatchTerrainTextureGeneration = nTerrainTextureGeneration;
						m_nFailureLatchMaterialTextureGeneration = nMaterialTextureGeneration;
					}
				}
			}
		}
	}
	updateRenderRoute();
	if ( m_bCompositorDebugEnabled && !m_bFailureLatched ) {
		attachEffectToActiveCamera();
	} else if ( !m_bCompositorDebugEnabled ) {
		detachEffect();
	}
}


bool Terrain3DRenderCoordinator::queueRendererRecoveryCold(
        const uint32_t nStartupGeneration, const uint64_t nRecoveryRevision ) {
	if ( nStartupGeneration == 0u || nRecoveryRevision == 0u || m_effect.is_null() ) {
		return false;
	}
	if ( Terrain3DRdContext::enqueueOnRenderThread(
	             callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::recoverRendererLifecycleOnRenderThread )
	                     .bind( ( int64_t )nStartupGeneration, ( int64_t )nRecoveryRevision ) ) ) {
		return true;
	}
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	status.m_nStartupGeneration = nStartupGeneration;
	status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_SCHEDULING;
	status.m_nRecoveryRevision = nRecoveryRevision;
	publishRenderLifecycleStatusCold( status );
	{
		t3dScopedLock_t lock( m_diagnosticsMutex );
		m_initializationError = "Render-thread renderer recovery could not be scheduled";
	}
	return false;
}

bool Terrain3DRenderCoordinator::completeRendererRecoveryOnRenderThread(
        const uint32_t nStartupGeneration, const uint64_t nRecoveryRevision ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), false );
#endif
	const t3dRdFrameSnapshotSlot_t *const pPublication = acquireSnapshotOnRenderThread();
	const t3dWorldPublicationBank_t *const pWorld = pPublication
	        ? m_publicationBuilder.getWorldPublicationBank( pPublication->m_nWorldPublicationBank )
	        : NULL;
	const bool bWorldPrepared = pWorld != NULL && pWorld->m_publication.m_nGeneration != 0u
	        && m_worldRuntime.preparePublicationCold( pWorld->m_publication, 0u );
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	status.m_nStartupGeneration = nStartupGeneration;
	status.m_nRecoveryRevision = nRecoveryRevision;
	if ( !bWorldPrepared
	        || ( status.m_nReadyMask & T3D_RENDER_READY_STARTUP_REQUIRED ) != T3D_RENDER_READY_STARTUP_REQUIRED ) {
		status.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_INITIALIZATION;
		publishRenderLifecycleStatusOnRenderThread( status );
		m_bRecoveryInProgress.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		return false;
	}
	status.m_nPreparedWorldPublicationGeneration = pWorld->m_publication.m_nGeneration;
	status.m_eStage = T3D_RENDER_LIFECYCLE_READY;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	publishRenderLifecycleStatusOnRenderThread( status );
	m_bRecoveryInProgress.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	return true;
}

void Terrain3DRenderCoordinator::recoverRendererLifecycleOnRenderThread(
        const uint32_t nStartupGeneration, const uint64_t nRecoveryRevision ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	if ( nStartupGeneration == 0u || nRecoveryRevision == 0u
	        || m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) != nStartupGeneration
	        || !m_bStartupActivationCommitted ) {
		return;
	}
	m_bRecoveryInProgress.store( true, T3D_MEMORY_ORDER_RELEASE );
	m_nRecoveryActiveRevision.store( nRecoveryRevision, T3D_MEMORY_ORDER_RELEASE );
	t3dRenderLifecycleStatus_t status = snapshotRenderLifecycleStatusOnOwnerThread();
	status.m_nStartupGeneration = nStartupGeneration;
	status.m_eStage = T3D_RENDER_LIFECYCLE_RECOVERING;
	status.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_NONE;
	status.m_nRecoveryRevision = nRecoveryRevision;
	status.m_nReadyMask &= ~( T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD
	        | T3D_RENDER_READY_INSTANCES | T3D_RENDER_READY_OCEAN );
	publishRenderLifecycleStatusOnRenderThread( status );

	m_worldRuntime.shutdownCold();
	m_oceanRenderer.shutdown();
	m_instanceRenderer.shutdown();
	m_renderer.shutdown();
	resetForwardLifecycle();
	m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bInstanceRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bOceanRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bRendererFailed.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );

	if ( !initializeRendererOnRenderThread( UINT64_MAX )
	        || !completeRendererRecoveryOnRenderThread( nStartupGeneration, nRecoveryRevision ) ) {
		t3dRenderLifecycleStatus_t failure = snapshotRenderLifecycleStatusOnOwnerThread();
		failure.m_nStartupGeneration = nStartupGeneration;
		failure.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		failure.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_RECOVERY_INITIALIZATION;
		failure.m_nRecoveryRevision = nRecoveryRevision;
		publishRenderLifecycleStatusOnRenderThread( failure );
		m_bRecoveryInProgress.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_nRecoveryActiveRevision.store( 0u, T3D_MEMORY_ORDER_RELEASE );
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
}

void Terrain3DRenderCoordinator::shutdownRendererOnRenderThread( void ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	if ( m_bPreviewRendererInitialized ) {
		m_previewRenderer.shutdown();
		m_bPreviewRendererInitialized = false;
	}
	m_worldRuntime.shutdownCold();
	m_sharedVtRuntime.shutdown_cold();
	m_oceanRenderer.shutdown();
	m_instanceRenderer.shutdown();
	m_renderer.shutdown();
	resetForwardLifecycle();
	m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bInstanceRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bOceanRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
	t3dRenderLifecycleStatus_t lifecycle = snapshotRenderLifecycleStatusOnOwnerThread();
	lifecycle.m_nReadyMask &= T3D_RENDER_READY_SHADER_GPU;
	lifecycle.m_nSharedResourceClass = ( uint32_t )T3D_RW_SHARED_RESOURCES_NONE;
	lifecycle.m_nPreparedWorldPublicationGeneration = 0u;
	publishRenderLifecycleStatusOnRenderThread( lifecycle );
}

void Terrain3DRenderCoordinator::shutdownRendererLifecycleOnRenderThread( const uint32_t nShutdownGeneration ) {
#ifdef DEBUG_ENABLED
	ASSERT( Terrain3DRdContext::isOnRenderThread(), VOID );
#endif
	shutdownRendererOnRenderThread();
	t3dRenderLifecycleStatus_t status{};
	status.m_nStartupGeneration = nShutdownGeneration;
	status.m_eStage = T3D_RENDER_LIFECYCLE_STOPPED;
	publishRenderLifecycleStatusOnRenderThread( status );
}

void Terrain3DRenderCoordinator::renderPreviewOnRenderThread( void ) {
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
		if ( m_shaderLibrary.getInstances().is_null() ) {
			m_pendingPreviewError = "RD editor preview base instance shader is unavailable";
			break;
		}
		if ( !ensureSharedVtRuntimeCold( T3D_RW_SHARED_RESOURCES_PREVIEW ) ) {
			m_pendingPreviewError = "RD editor preview shared VT resources are unavailable";
			break;
		}
		if ( !m_bPreviewRendererInitialized ) {
			m_bPreviewRendererInitialized = m_previewRenderer.initializePreview( m_shaderLibrary.getInstances() );
			if ( m_bPreviewRendererInitialized ) {
				setRenderLifecycleReadyBitOnRenderThread(
				        m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ), T3D_RENDER_READY_PREVIEW );
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
		clearRenderLifecycleReadyBitOnRenderThread( T3D_RENDER_READY_PREVIEW );
	}
	context.endRecoverableFailureScope();
}

Ref<Image> Terrain3DRenderCoordinator::failMeshPreview(
        const t3dRdPreviewFailure_t eFailure, const String &rError, t3dRdPreviewFailure_t *const pFailure ) {
	if ( pFailure ) {
		*pFailure = eFailure;
	}
	t3dScopedLock_t lock( m_diagnosticsMutex );
	m_previewLastError = rError;
	m_lastPreviewFailure = eFailure;
	++m_nPreviewFailureCount;
	return Ref<Image>();
}

bool Terrain3DRenderCoordinator::prepareMeshPreviewSourceCold(
        const uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *const pFailure ) {
	if ( pFailure ) {
		*pFailure = T3D_RD_PREVIEW_NONE;
	}
	if ( !m_pTerrain || !m_effect.is_valid() ) {
		( void )failMeshPreview( T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview coordinator is unavailable", pFailure );
		return false;
	}
	// Preview rendering/readback owns separate RD targets. Region-cache uploads do
	// not alias those targets, so global authoring residency is not a correctness
	// prerequisite. Transient deferral is reserved for preview-owned in-flight
	// slots and publication/rebuild seams instead of unrelated terrain streaming.
	// Mesh-list edits can still discover a larger immutable instance mesh source.
	// Do not launch previews against the stale bank while that cold capacity
	// transition is pending.
	if ( m_publicationBuilder.isInstancePublicationCapacityPending() || m_publicationBuilder.isInstanceMeshBankCapacityPending() ) {
		( void )failMeshPreview( T3D_RD_PREVIEW_TRANSIENT_DEFERRED, "RD preview is waiting for instance publication capacity", pFailure );
		return false;
	}
	const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
	if ( rSize.x < 2 || rSize.y < 2 || assets.is_null() || nAssetId >= assets->getMeshCount() ) {
		( void )failMeshPreview( T3D_RD_PREVIEW_ASSET_SOURCE, "RD preview asset or dimensions are invalid", pFailure );
		return false;
	}
	const uint64_t fingerprint = Terrain3DRdMeshCache::computeSourceFingerprintForAsset( assets, nAssetId );
	if ( !m_bCachedPreviewMeshesValid || nAssetId != m_nCachedPreviewAssetId || fingerprint != m_nPreviewMeshFingerprint ) {
		String error;
		m_nPreviewMeshGeneration = advanceGeneration( m_nPreviewMeshGeneration );
		if ( !Terrain3DRdMeshCache::buildSourceSnapshotForAsset(
		             assets, nAssetId, m_cachedPreviewMeshes, fingerprint, m_nPreviewMeshGeneration, &error ) ) {
			( void )failMeshPreview( T3D_RD_PREVIEW_ASSET_SOURCE, error, pFailure );
			return false;
		}
		m_bCachedPreviewMeshesValid = true;
		m_nPreviewMeshFingerprint = fingerprint;
		m_nCachedPreviewAssetId = nAssetId;
	}
	String vtError;
	if ( !preparePreviewAuthoringPackageCold( assets, &vtError ) ) {
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
			( void )failMeshPreview( T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview snapshot copy failed", pFailure );
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

Ref<Image> Terrain3DRenderCoordinator::createMeshPreview(
        const uint16_t nAssetId, const Vector2i &rSize, t3dRdPreviewFailure_t *const pFailure ) {
	t3dScopedLock_t call_lock( m_previewCallMutex );
	{
		t3dScopedLock_t state_lock( m_previewStateMutex );
		for ( uint32_t iSlot = 0u; iSlot < s_nPreviewReadbackSlotCount; ++iSlot ) {
			if ( m_szPreviewReadbackSlots[iSlot].bActive ) {
				return failMeshPreview( T3D_RD_PREVIEW_TRANSIENT_DEFERRED, "RD async preview readback is in flight", pFailure );
			}
		}
	}
	if ( !prepareMeshPreviewSourceCold( nAssetId, rSize, pFailure ) ) {
		return Ref<Image>();
	}
	if ( Terrain3DRdContext::isOnRenderThread() ) {
		renderPreviewOnRenderThread();
	} else {
		if ( m_effect.is_null()
		        || !Terrain3DRdContext::enqueueOnRenderThread(
		                callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::renderPreviewOnRenderThread ) ) ) {
			return failMeshPreview( T3D_RD_PREVIEW_SYSTEMIC_RD, "RD preview render-thread submission failed", pFailure );
		}
		Terrain3DRdContext::forceSync();
	}
	t3dScopedLock_t lock( m_previewStateMutex );
	if ( m_pendingPreviewImage.is_null() ) {
		return failMeshPreview( m_pendingPreviewFailure == T3D_RD_PREVIEW_NONE ? T3D_RD_PREVIEW_SYSTEMIC_RD : m_pendingPreviewFailure,
		        m_pendingPreviewError,
		        pFailure );
	}
	{
		t3dScopedLock_t diagnostics_lock( m_diagnosticsMutex );
		m_previewLastError = String();
		m_lastPreviewFailure = T3D_RD_PREVIEW_NONE;
	}
	return m_pendingPreviewImage;
}

bool Terrain3DRenderCoordinator::requestMeshPreviewAsync( const uint16_t nAssetId,
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
	if ( !prepareMeshPreviewSourceCold( nAssetId, rSize, pFailure ) ) {
		return false;
	}
	const Ref<Terrain3DAssets> assets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
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
	if ( m_effect.is_null()
	        || !Terrain3DRdContext::enqueueOnRenderThread(
	                callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::renderPreviewAsyncOnRenderThread ) ) ) {
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

void Terrain3DRenderCoordinator::dispatchPreviewAsyncCompletionCold(
        const t3dPreviewReadbackSlot_t &rSlot, const t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage ) {
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

void Terrain3DRenderCoordinator::completePreviewAsyncOnRenderThread(
        const uint32_t nSlot, const t3dRdPreviewFailure_t eFailure, const Ref<Image> &rImage ) {
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

void Terrain3DRenderCoordinator::servicePreviewReadbackWatchdogCold( void ) {
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
		( void )queuePreviewVtRepublishCold();
	}
}

void Terrain3DRenderCoordinator::cancelPreviewAsyncRequestsOnRenderThread( void ) {
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

void Terrain3DRenderCoordinator::renderPreviewAsyncOnRenderThread( void ) {
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
		if ( !m_bPendingPreviewMeshesValid || m_shaderLibrary.getInstances().is_null() ) {
			break;
		}
		if ( !ensureSharedVtRuntimeCold( T3D_RW_SHARED_RESOURCES_PREVIEW ) ) {
			break;
		}
		if ( !m_bPreviewRendererInitialized ) {
			m_bPreviewRendererInitialized = m_previewRenderer.initializePreview( m_shaderLibrary.getInstances() );
			if ( m_bPreviewRendererInitialized ) {
				setRenderLifecycleReadyBitOnRenderThread(
				        m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ), T3D_RENDER_READY_PREVIEW );
			}
		}
		if ( !m_bPreviewRendererInitialized
		        || !m_previewRenderer.renderPreviewToTarget( &m_pendingPreviewMeshes, m_nPendingPreviewAssetId, m_pendingPreviewSize ) ) {
			break;
		}
		const Callable callback = callable_mp( m_effect.ptr(), &Terrain3DRdCompositorEffect::consumePreviewReadbackOnRenderThread )
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
		clearRenderLifecycleReadyBitOnRenderThread( T3D_RENDER_READY_PREVIEW );
		completePreviewAsyncOnRenderThread( nSlot, eFailure, Ref<Image>() );
	}
	context.endRecoverableFailureScope();
}

void Terrain3DRenderCoordinator::consumePreviewReadbackOnRenderThread(
        const uint32_t nSlot, const uint64_t nRequestGeneration, const PackedByteArray &rReadbackBytes ) {
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
	servicePreviewVtRepublishOnRenderThread();
}

void Terrain3DRenderCoordinator::render( RenderData *const pRenderData ) {
	if ( !m_bCompositorDebugEnabled || !pRenderData ) {
		return;
	}
	++m_nCallbackCount;
	if ( !initializeRendererOnRenderThread() ) {
		return;
	}
	const t3dRdFrameSnapshotSlot_t *const publication = acquireSnapshotOnRenderThread();
	if ( !publication ) {
		return;
	}
	const t3dRdFrameSnapshot_t &snapshot = publication->m_snapshot;
	const t3dRdRegionMapBank_t *const pRegionMap = m_publicationBuilder.getRegionMapBank( publication->m_nRegionMapBank );
	const t3dRdInstanceMeshBank_t *const pMeshes = m_publicationBuilder.getInstanceMeshBank( publication->m_nInstanceMeshBank );
	const t3dRdInstanceSceneBank_t *const pScene = m_publicationBuilder.getInstanceSceneBank( publication->m_nInstanceSceneBank );
	const bool bHasTerrain = snapshot.m_nPatchCount > 0u && pRegionMap && !pRegionMap->m_bytes.is_empty();
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_RENDER );
	const bool instance_lane_ready = m_bInstanceRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE );
	if ( instance_lane_ready ) {
		( void )m_instanceRenderer.render(
		        pRenderData, pMeshes ? &pMeshes->m_snapshot : NULL, pScene ? &pScene->m_snapshot : NULL, snapshot.m_nCameraCullMask );
	}
	const bool terrain_ok = !bHasTerrain
	        || m_renderer.render(
	                pRenderData, snapshot, pRegionMap->m_bytes, snapshot.m_nRegionMapGeneration, publication->m_bindings, true );
	if ( terrain_ok ) {
		m_nConsecutiveRenderFailures.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	} else {
		const uint32_t failures = m_nConsecutiveRenderFailures.fetch_add( 1u, T3D_MEMORY_ORDER_ACQ_REL ) + 1u;
		// The render thread owns failure teardown. Keep the shared VT/RenderWorld
		// publication authority alive so a later resource-generation recovery can
		// rebuild only the failed terrain/world consumers on the render thread.
		m_worldRuntime.shutdownCold();
		m_oceanRenderer.shutdown();
		m_instanceRenderer.shutdown();
		m_renderer.shutdown();
		m_bInstanceRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bOceanRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		resetForwardLifecycle();
		m_bRendererInitialized.store( false, T3D_MEMORY_ORDER_RELEASE );
		m_bRendererFailed.store( true, T3D_MEMORY_ORDER_RELEASE );
		t3dRenderLifecycleStatus_t failure = snapshotRenderLifecycleStatusOnOwnerThread();
		failure.m_eStage = T3D_RENDER_LIFECYCLE_FAILED;
		failure.m_nFailureCode = T3D_RENDER_LIFECYCLE_FAILURE_RUNTIME_RENDER;
		failure.m_nRecoveryRevision = 0u;
		failure.m_nReadyMask &= ~( T3D_RENDER_READY_TERRAIN | T3D_RENDER_READY_WORLD
		        | T3D_RENDER_READY_INSTANCES | T3D_RENDER_READY_OCEAN );
		publishRenderLifecycleStatusOnRenderThread( failure );
		LOG( ERROR, "[RD terrain] Runtime render failure published to owner failures=", failures,
		        " publication_generation=", failure.m_nPreparedWorldPublicationGeneration );
	}
}

bool Terrain3DRenderCoordinator::publishStaticSceneDatabaseCold( const T3DGeometryDatabase &rGeometry,
        const T3DSceneCell *const pCells, const uint32_t nCellCount,
        const T3DInstanceRecord *const pInstances, const uint32_t nInstanceCount,
        const t3dRenderingMaterialGpu_t *const pMaterials, const uint32_t nMaterialCount ) {
	if ( ( nCellCount != 0u && pCells == NULL ) || ( nInstanceCount != 0u && pInstances == NULL )
	        || ( nMaterialCount != 0u && pMaterials == NULL ) ) {
		return false;
	}
	t3dScopedLock_t lock( m_staticScenePublicationMutex );
	const uint64_t nextGeneration = advanceGeneration( m_staticScenePublication.nGeneration );
	m_staticScenePublication = t3dStaticScenePublication_t{};
	m_staticScenePublication.pGeometry = &rGeometry;
	m_staticScenePublication.pCells = pCells;
	m_staticScenePublication.pInstances = pInstances;
	m_staticScenePublication.pMaterials = pMaterials;
	m_staticScenePublication.nCellCount = nCellCount;
	m_staticScenePublication.nInstanceCount = nInstanceCount;
	m_staticScenePublication.nMaterialCount = nMaterialCount;
	m_staticScenePublication.nGeneration = nextGeneration;
	m_staticScenePublication.bValid = true;
	return true;
}

void Terrain3DRenderCoordinator::releaseStaticSceneDatabaseCold( void ) {
	t3dScopedLock_t lock( m_staticScenePublicationMutex );
	const uint64_t nextGeneration = advanceGeneration( m_staticScenePublication.nGeneration );
	m_staticScenePublication = t3dStaticScenePublication_t{};
	m_staticScenePublication.nGeneration = nextGeneration;
}



void Terrain3DRenderCoordinator::resetForwardLifecycle( void ) {
	m_forwardPassPreparer.reset();
}

bool Terrain3DRenderCoordinator::renderForwardPass( const t3dRdForwardPassContext_t &rContext ) {
	return m_forwardPassPreparer.render( *this, rContext );
}

size_t Terrain3DRenderCoordinator::getRetainedCpuBytes( void ) const {
	return m_diagnosticsPublisher.getRetainedCpuBytes( *this );
}

Dictionary Terrain3DRenderCoordinator::getForwardLaneDiagnostics( void ) const {
	return m_diagnosticsPublisher.getForwardLaneDiagnostics( *this );
}

Dictionary Terrain3DRenderCoordinator::getDiagnostics( void ) const {
	return m_diagnosticsPublisher.getDiagnostics( *this );
}

Dictionary Terrain3DRenderCoordinator::getStartupDiagnostics( void ) const {
	Dictionary result;
	const t3dRenderLifecycleStatus_t renderStatus = snapshotRenderLifecycleStatusOnOwnerThread();
	result["startup_generation"] = ( int64_t )( m_nStartupGeneration );
	result["startup_gpu_stage"] = ( int64_t )( m_nStartupGpuStage );
	result["startup_gpu_service_count"] = ( int64_t )( m_nStartupGpuServiceCount );
	result["startup_gpu_service_usec_last"] = ( int64_t )( m_nStartupGpuServiceUsecLast );
	result["startup_gpu_service_complete"] = m_bStartupGpuServiceComplete.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["startup_stale_completion_reject_count"] = ( int64_t )( m_nStartupStaleCompletionRejectCount.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["startup_cancellation_count"] = ( int64_t )( m_nStartupCancellationCount );
	result["startup_render_drain_count"] = ( int64_t )( m_nStartupRenderDrainCount );
	result["activation_barrier_count"] = ( int64_t )( m_nStartupActivationBarrierCount );
	result["activation_barrier_usec_last"] = ( int64_t )( m_nStartupActivationBarrierUsecLast );
	result["activation_snapshot_generation"] = ( int64_t )( m_nStartupActivationSnapshotGeneration );
	result["activation_world_publication_queued"] = m_bStartupWorldPublicationQueued.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["activation_world_publication_prepared"] = m_bStartupWorldPublicationPrepared.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["activation_world_publication_generation"] = ( int64_t )( m_nStartupWorldPublicationGeneration.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["activation_world_publication_prepare_usec_last"] = ( int64_t )( m_nStartupWorldPublicationPrepareUsecLast.load( T3D_MEMORY_ORDER_ACQUIRE ) );
	result["activation_prepared"] = m_bStartupActivationPrepared;
	result["activation_committed"] = m_bStartupActivationCommitted;
	result["final_sync_attempts"] = ( int64_t )( m_nStartupActivationBarrierCount );
	result["final_sync_successes"] = ( int64_t )( m_nStartupFinalSyncSuccessCount );
	result["final_sync_failures"] = ( int64_t )( m_nStartupFinalSyncFailureCount );
	result["final_sync_elapsed_ms"] = ( double )( m_nStartupFinalSyncElapsedUsecLast ) / 1000.0;
	const t3dStartupDomainSnapshot_t shaderStartup = m_shaderLibrary.getStartupSnapshot();
	const t3dStartupDomainSnapshot_t vtStartup = m_virtualTexture.get_startup_snapshot();
	result["shader_stage"] = ( int64_t )( t3dStartupDomainGetStage( shaderStartup ) );
	result["shader_stage_name"] = String( t3dStartupStageName( t3dStartupDomainGetStage( shaderStartup ) ) );
	result["vt_stage"] = ( int64_t )( t3dStartupDomainGetStage( vtStartup ) );
	result["vt_stage_name"] = String( t3dStartupStageName( t3dStartupDomainGetStage( vtStartup ) ) );
	result["publication_prepare_count"] = ( int64_t )( m_nStartupPublicationPrepareCount );
	result["publication_prepare_usec_last"] = ( int64_t )( m_nStartupPublicationPrepareUsecLast );
	result["renderer_initialize_attempts"] = ( int64_t )( m_nStartupRendererInitializeAttemptCount );
	result["renderer_initialize_successes"] = ( int64_t )( m_nStartupRendererInitializeSuccessCount );
	result["renderer_initialize_usec_last"] = ( int64_t )( m_nStartupRendererInitializeUsecLast );
	result["renderer_initialize_usec_total"] = ( int64_t )( m_nStartupRendererInitializeUsecTotal );
	result["renderer_initialized"] = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_TERRAIN ) != 0u;
	result["shared_vt_resources_ready"] = ( renderStatus.m_nReadyMask & T3D_RENDER_READY_VT_PUBLISHED ) != 0u;
	result["render_lifecycle_generation"] = ( int64_t )renderStatus.m_nStartupGeneration;
	result["render_lifecycle_stage"] = ( int64_t )renderStatus.m_eStage;
	result["render_lifecycle_ready_mask"] = ( int64_t )renderStatus.m_nReadyMask;
	result["render_lifecycle_failure_code"] = ( int64_t )renderStatus.m_nFailureCode;
	result["render_lifecycle_prepared_world_generation"] = ( int64_t )renderStatus.m_nPreparedWorldPublicationGeneration;
	result["render_lifecycle_recovery_revision"] = ( int64_t )renderStatus.m_nRecoveryRevision;
	result["render_lifecycle_revision"] = ( int64_t )renderStatus.m_nRevision;
	result["shader"] = m_shaderLibrary.getDiagnostics();
	result["virtual_texture"] = m_virtualTexture.get_startup_diagnostics();
	return result;
}

void Terrain3DRenderCoordinator::setUnifiedWorldFramePolicy( const uint32_t nWorldUploadKb,
        const uint32_t nWorldClusters,
        const uint32_t nVtRequests,
        const uint32_t nVtAdmissions,
        const bool bBoundaryDeferred ) {
	m_nWorldUploadBudgetKbPolicy.store( MAX( 1u, nWorldUploadKb ), T3D_MEMORY_ORDER_RELEASE );
	m_nWorldClusterBudgetPolicy.store( MAX( 1u, nWorldClusters ), T3D_MEMORY_ORDER_RELEASE );
	m_nVtRequestBudgetPolicy.store( MAX( 1u, nVtRequests ), T3D_MEMORY_ORDER_RELEASE );
	m_nVtAdmissionBudgetPolicy.store( MAX( 1u, nVtAdmissions ), T3D_MEMORY_ORDER_RELEASE );
	m_bWorldBoundaryDeferredPolicy.store( bBoundaryDeferred, T3D_MEMORY_ORDER_RELEASE );
}

uint64_t Terrain3DRenderCoordinator::getWorldGpuBufferBytes( void ) const {
	return m_worldRuntime.getResources().getGpuBufferBytes();
}
uint64_t Terrain3DRenderCoordinator::getWorldHiZGpuBytes( void ) const {
	return m_worldRuntime.getHiZ().getGpuBytes();
}
size_t Terrain3DRenderCoordinator::getWorldRetainedCpuBytes( void ) const {
	return m_worldRuntime.getRetainedCpuBytes();
}
uint64_t Terrain3DRenderCoordinator::getWorldBudgetDeferralCount( void ) const {
	return m_worldRuntime.getPublicationBudgetDeferralCount() + m_worldRuntime.getPublicationBoundaryDeferralCount();
}
uint64_t Terrain3DRenderCoordinator::getVirtualTextureBudgetDeferralCount( void ) const {
	const T3DVTStreamingStats &rStats = m_virtualTexture.get_streaming().Stats;
	return rStats.nBudgetDeferredCount + rStats.nAdmissionBudgetDeferredCount;
}
uint64_t Terrain3DRenderCoordinator::getVirtualTextureCpuBytes( void ) const {
	return m_virtualTexture.get_cpu_image_bytes();
}
bool Terrain3DRenderCoordinator::isEditorAuthoringMode( void ) const {
	return m_pTerrain && m_pTerrain->isEditorAuthoringMode();
}
