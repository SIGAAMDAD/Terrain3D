// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_generation.h"
#include "terrain_3d.h"
#include "core/utility/terrain_3d_sync.h"

#include "Terrain3DConstants.h"
#include "core/utility/logger.h"
#include "core/utility/terrain_3d_integration_helpers.h"
#include "core/utility/terrain_3d_util.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "instances/physics/terrain_3d_instance_occlusion.h"
#include "instances/physics/terrain_3d_instance_physics.h"
#include "lighting/policy/terrain_3d_lighting_policy.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "memory/core/terrain_3d_memory.h"
#include "memory/arena/terrain_3d_zone_allocator.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "simd/core/terrain_3d_parallel.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "streaming/policy/terrain_3d_performance_profile.h"
#include "streaming/policy/terrain_3d_streaming_policy.h"

#include <EASTL/numeric_limits.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>

// Initialize static member variable

// -----------------------------------------------------------------------------
// Optional production viewport VRS profile. The Godot viewport owns its VRS
// attachment; Terrain3D owns only the density texture while attached. The
// project setting is intentionally absent/disabled by default. Unsupported
// renderers and externally configured viewports retain their existing modes.
void Terrain3D::applyViewportVrsProfile(
	void ) {
	if ( m_nVrsViewportId != 0u || !is_inside_tree() || Engine::get_singleton()->is_editor_hint() ) {
		return;
	}
	ProjectSettings *const pSettings = ProjectSettings::get_singleton();
	if ( !pSettings || !pSettings->has_setting( "terrain3d/rendering/vrs_profile" ) ) {
		return;
	}
	const String profile = String( pSettings->get_setting( "terrain3d/rendering/vrs_profile" ) ).to_lower();
	if ( profile != "uniform" && profile != "conservative" ) {
		return;
	}
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer || pServer->get_current_rendering_method() == "gl_compatibility" ) {
		return;
	}
	Viewport *const pViewport = get_viewport();
	if ( !pViewport || pViewport->get_vrs_mode() != Viewport::VRS_DISABLED ) {
		return;
	}
	const int32_t nWidth = 128;
	const int32_t nHeight = 72;
	Ref< Image > image = Image::create( nWidth, nHeight, false, Image::FORMAT_RGB8 );
	if ( image.is_null() || image->is_empty() ) {
		return;
	}
	const Color fullRate( 0.0f, 0.0f, 0.0f );
	const Color halfRate( 85.0f / 255.0f, 85.0f / 255.0f, 0.0f );
	for ( int32_t y = 0; y < nHeight; ++y ) {
		const float fY = ( ( float ) y + 0.5f ) / ( float ) nHeight * 2.0f - 1.0f;
		for ( int32_t x = 0; x < nWidth; ++x ) {
			const float fX = ( ( float ) x + 0.5f ) / ( float ) nWidth * 2.0f - 1.0f;
			// Reserve the central 80% ellipse for full-rate shading.
			const bool bCoarse = profile == "uniform" || ( fX * fX + fY * fY ) > 0.64f;
			image->set_pixel( x, y, bCoarse ? halfRate : fullRate );
		}
	}
	Ref< ImageTexture > density = ImageTexture::create_from_image( image );
	if ( density.is_null() ) {
		return;
	}
	m_hVrsPreviousTexture = pViewport->get_vrs_texture();
	m_nPreviousVrsUpdateMode = ( int32_t ) pViewport->get_vrs_update_mode();
	m_hVrsProfileTexture = density;
	m_nVrsViewportId = pViewport->get_instance_id();
	pViewport->set_vrs_texture( density );
	pViewport->set_vrs_update_mode( Viewport::VRS_UPDATE_ONCE );
	pViewport->set_vrs_mode( Viewport::VRS_TEXTURE );
}

void Terrain3D::releaseViewportVrsProfile(
	void ) {
	if ( m_nVrsViewportId == 0u ) {
		return;
	}
	Object *const pObject = ObjectDB::get_instance( m_nVrsViewportId );
	Viewport *const pViewport = Object::cast_to< Viewport >( pObject );
	if ( pViewport && pViewport->get_vrs_mode() == Viewport::VRS_TEXTURE &&
		pViewport->get_vrs_texture() == m_hVrsProfileTexture ) {
		pViewport->set_vrs_mode( Viewport::VRS_DISABLED );
		pViewport->set_vrs_texture( m_hVrsPreviousTexture );
		pViewport->set_vrs_update_mode( ( Viewport::VRSUpdateMode ) m_nPreviousVrsUpdateMode );
	}
	m_nVrsViewportId = 0u;
	m_hVrsProfileTexture.unref();
	m_hVrsPreviousTexture.unref();
}

// -----------------------------------------------------------------------------
// Private functions

void Terrain3D::initializeInternal(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_memoryAccount.get() );
	// Initialization is an explicit owner-controlled rebuild boundary. The phase
	// scopes below label telemetry only; each storage owner decides whether its
	// backing may change during this transition.
	Terrain3DColdPhaseScope		  cold_scope;
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION );
	if ( m_runtimeLifecycleState == TERRAIN3D_RUNTIME_SHUTTING_DOWN ||
		 m_runtimeLifecycleState == TERRAIN3D_RUNTIME_DRAIN_FAILED ) {
		LOG( ERROR,
			 "[lifecycle] Ignoring initialize request while runtime memory is shutting down; state=",
			 ( int ) ( m_runtimeLifecycleState ) );
		set_physics_process( false );
		return;
	}
	LOG( INFO,
		 "[lifecycle] Initialize request: initialized=",
		 m_bInitialized,
		 "; inside_tree=",
		 is_inside_tree(),
		 "; inside_world=",
		 m_bIsInsideWorld,
		 "; data_directory=",
		 m_dataDirectory );
	// Make blank objects if needed
	if ( !m_pData ) {
		LOG( DEBUG, "Creating blank data object" );
		m_pData = memnew( Terrain3DData );
		m_nDataSignalConnections = 0u;
	}
	if ( !initializeRuntimeMemory() ) {
		LOG( ERROR, "[lifecycle] Unable to initialize the fixed runtime memory image" );
		set_physics_process( false );
		return;
	}
	bool bCreatedDefaultMaterial = false;
	if ( m_material.is_null() ) {
		LOG( DEBUG, "Creating blank material" );
		m_material.instantiate();
		bCreatedDefaultMaterial = true;
	}
	// A base profile selected before a default material exists must seed that new
	// material once. Existing/deserialized material resources are never overwritten
	// here; their actual values determine Exact versus Modified conformity.
	if ( m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL &&
		 ( bCreatedDefaultMaterial || m_bPerformanceProfileMaterialPendingApply ) ) {
		m_material->setShaderPerformancePreset( SHADER_PRESET_XBOX_360_AUTHENTIC );
		m_bPerformanceProfileMaterialPendingApply = false;
	}
	connectAutoShaderProfileSignal( m_autoShaderProfile );
	applyAutoShaderProfileMode();
	if ( m_assets.is_null() ) {
		LOG( DEBUG, "Creating blank texture list" );
		m_assets.instantiate();
	}
	if ( !m_pCollision ) {
		LOG( DEBUG, "Creating collision manager" );
		m_pCollision = memnew( Terrain3DCollision );
	}
	if ( !m_pInstancer ) {
		LOG( DEBUG, "Creating instancer" );
		m_pInstancer = memnew( Terrain3DInstancer );
		if ( !m_pInstancer->bindRuntimeAllocator( &m_runtimeMemoryZone,
												  getRuntimeAllocatorOverflowPolicyInternal() ) ) {
			LOG( ERROR, "Failed binding instancer runtime allocator ownership" );
			return;
		}
	}
	if ( !m_pInstanceOcclusion ) {
		LOG( DEBUG, "Creating instance occlusion service" );
		m_pInstanceOcclusion = memnew( Terrain3DInstanceOcclusion );
		if ( !m_pInstanceOcclusion->bindRuntimeAllocator( &m_runtimeMemoryZone,
														  getRuntimeAllocatorOverflowPolicyInternal() ) ) {
			LOG( ERROR, "Failed binding instance occlusion runtime allocator ownership" );
			return;
		}
	}
	if ( !m_pInstancePhysics ) {
		LOG( DEBUG, "Creating instance physics service" );
		m_pInstancePhysics = memnew( Terrain3DInstancePhysics );
		if ( !m_pInstancePhysics->bindRuntimeAllocator( &m_runtimeMemoryZone,
														getRuntimeAllocatorOverflowPolicyInternal() ) ) {
			LOG( ERROR, "Failed binding instance physics runtime allocator ownership" );
			return;
		}
	}
	if ( !m_pNodeStreamer ) {
		LOG( DEBUG, "Creating node streamer" );
		m_pNodeStreamer = memnew( Terrain3DNodeStreamer );
		if ( !m_pNodeStreamer->bindRuntimeAllocator( &m_runtimeMemoryZone ) ) {
			LOG( ERROR, "Failed binding node streamer runtime allocator ownership" );
			return;
		}
	}
	if ( !m_pLightmapStreamer ) {
		LOG( DEBUG, "Creating lightmap streamer" );
		m_pLightmapStreamer = memnew( Terrain3DLightmapStreamer );
	}
	if ( !m_pHlodStreamer ) {
		LOG( DEBUG, "Creating HLOD streamer" );
		m_pHlodStreamer = memnew( Terrain3DHLODStreamer );
		if ( !m_pHlodStreamer->bindRuntimeAllocator( &m_runtimeMemoryZone,
													 getRuntimeAllocatorOverflowPolicyInternal() ) ) {
			LOG( ERROR, "Failed binding HLOD runtime allocator ownership" );
			return;
		}
	}
	// Terrain3DData is retained when the runtime deactivates. Register its
	// owner-lifetime callbacks once, instead of re-creating CallableCustom
	// instances on every initializeInternal() invocation. Godot's custom
	// callable lookup is not a reliable lifecycle guard here.
	if ( ( m_nDataSignalConnections & 0x01u ) == 0u ) {
		LOG( DEBUG, "Connecting _data::region_map_changed signal to updateRegionLabels()" );
		const Error err = m_pData->connect( "region_map_changed", callable_mp( this, &Terrain3D::updateRegionLabels ) );
		if ( err == OK || err == ERR_ALREADY_EXISTS ) {
			m_nDataSignalConnections |= 0x01u;
		}
	}
	// Collision can be reconstructed during a budget change; always signal the
	// persistent Terrain3D owner and forward to its current collision object.
	if ( ( m_nDataSignalConnections & 0x02u ) == 0u ) {
		LOG( DEBUG, "Connecting _data::region_map_changed signal to onRegionMapChanged()" );
		const Error err = m_pData->connect( "region_map_changed", callable_mp( this, &Terrain3D::onRegionMapChanged ) );
		if ( err == OK || err == ERR_ALREADY_EXISTS ) {
			m_nDataSignalConnections |= 0x02u;
		}
	}
	connectResourceSignals( m_assets, m_material );
	refreshPerformanceProfileConformity();
	if ( ( m_nDataSignalConnections & 0x04u ) == 0u ) {
		LOG( DEBUG, "Connecting _data::height_maps_changed signal to updateMesherAabbs()" );
		const Error err = m_pData->connect( "height_maps_changed", callable_mp( this, &Terrain3D::updateMesherAabbs ) );
		if ( err == OK || err == ERR_ALREADY_EXISTS ) {
			m_nDataSignalConnections |= 0x04u;
		}
	}
	// Initialize the system. P05 begins the independent startup domains here and
	// returns; serviceStartup() advances them from the owner physics tick.
	if ( !m_bInitialized && m_bIsInsideWorld && is_inside_tree() &&
		 m_runtimeLifecycleState != TERRAIN3D_RUNTIME_INITIALIZING ) {
		m_nStartupGeneration = t3dNextGeneration( m_nStartupGeneration );
		++m_nStartupAttemptCount;
		m_nStartupStartUsec = Time::get_singleton()->get_ticks_usec();
		m_nStartupEndUsec = 0u;
		m_nStartupTotalUsecLast = 0u;
		m_nStartupDataInitializeUsecLast = 0u;
		m_nStartupOwnerPrepareUsecLast = 0u;
		m_nStartupCoordinatorInitializeUsecLast = 0u;
		m_nStartupOwnerFinalizeUsecLast = 0u;
		m_nStartupServiceTraceCount = 0u;
		m_nStartupServiceTraceLastUsec = 0u;
		m_nStartupServiceTraceSignature = UINT64_MAX;
		m_bStartupSucceededLast = false;
		t3dStartupDomainRequest( m_startupFinalizationState, m_nStartupGeneration, 0u, 3u );
		m_runtimeLifecycleState = TERRAIN3D_RUNTIME_INITIALIZING;
		LOG( INFO, "[Terrain3D][Startup] BEGIN generation=", m_nStartupGeneration );
		if ( !m_hCamera.isValid() && ( IS_EDITOR || !m_clipmapTarget.isValid() ) ) {
			grabCamera();
		}
		m_startupInitialTarget = getClipmapTargetPosition();
		const Error data_error = m_pData->beginStartupCold( this, m_nStartupGeneration );
		if ( data_error != OK ) {
			LOG( ERROR,
				 "[Terrain3D][Startup][Data] BEGIN FAILED generation=",
				 m_nStartupGeneration,
				 " error=",
				 data_error );
			deactivateRuntime( true );
			return;
		}
		if ( !m_pRenderCoordinator ) {
			m_pRenderCoordinator = memnew( Terrain3DRenderCoordinator );
			if ( !m_pRenderCoordinator->bindRuntimeAllocator( getRuntimeMemoryZoneInternal(),
															  getRuntimeAllocatorOverflowPolicyInternal() ) ) {
				LOG( ERROR, "Failed binding render coordinator runtime allocator ownership" );
				deactivateRuntime( true );
				return;
			}
		}
		if ( !m_pRenderCoordinator->beginStartupCold( this, m_nStartupGeneration ) ) {
			LOG( ERROR, "[Terrain3D][Startup][Coordinator] BEGIN FAILED generation=", m_nStartupGeneration );
			deactivateRuntime( true );
			return;
		}
	}

	if ( m_runtimeLifecycleState != TERRAIN3D_RUNTIME_INITIALIZING ) {
		serviceMemoryBudgetPolicy( true );
	}
	update_configuration_warnings();
}

void Terrain3D::runStartupOwnerStage(
	const char *const pszStage,
	void ( Terrain3D::*pStage )( void ) ) {
	const uint64_t nStageStartedUsec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][OwnerPrepare][", pszStage, "] BEGIN generation=", m_nStartupGeneration );
	( this->*pStage )();
	LOG( INFO,
		 "[Terrain3D][Startup][OwnerPrepare][",
		 pszStage,
		 "] END generation=",
		 m_nStartupGeneration,
		 " elapsed=",
		 Time::get_singleton()->get_ticks_usec() - nStageStartedUsec,
		 " us" );
}

void Terrain3D::prepareStartupAssetsStageCold(
	void ) {
	m_assets->initialize( this );
}

void Terrain3D::prepareStartupMemoryPolicyStageCold(
	void ) {
	serviceMemoryBudgetPolicy( true );
	applyRuntimeAdaptiveWorkBudgets();
	normalizeRuntimeTierLayout( true );
}

void Terrain3D::prepareStartupResidencyStageCold(
	void ) {
	updateRegionStreamingPolicy( m_startupInitialTarget, 0.0, true );
	m_pData->serviceColdInstanceResidency( m_startupInitialTarget, true );
	m_pData->updateRegionResidency( m_startupInitialTarget, true );
}

void Terrain3D::prepareStartupMaterialStageCold(
	void ) {
	m_material->initialize( this );
}

void Terrain3D::prepareStartupCollisionStageCold(
	void ) {
	m_pCollision->initialize( this );
}

void Terrain3D::prepareStartupInstancerStageCold(
	void ) {
	m_pInstancer->initialize( this );
}

void Terrain3D::prepareStartupInstanceOcclusionStageCold(
	void ) {
	m_pInstanceOcclusion->initialize( this );
}

void Terrain3D::prepareStartupInstancePhysicsStageCold(
	void ) {
	m_pInstancePhysics->initialize( this );
}

void Terrain3D::prepareStartupNodeStreamerStageCold(
	void ) {
	m_pNodeStreamer->initialize( this );
	m_pNodeStreamer->setEnabled( effectiveDynamicNodeStreamingEnabled() );
	m_pNodeStreamer->setRadiusRegions( m_nDynamicNodeStreamRadius );
	m_pNodeStreamer->setLoadRegionBudget( m_nDynamicNodeLoadRegionBudget );
	m_pNodeStreamer->setLoadNodeBudget( m_nDynamicNodeLoadNodeBudget );
	m_pNodeStreamer->setUnloadRegionBudget( m_nDynamicNodeUnloadRegionBudget );
}

void Terrain3D::prepareStartupLightmapStreamerStageCold(
	void ) {
	m_pLightmapStreamer->initialize( this );
	m_pLightmapStreamer->setResidencyBudgetMb( m_nEffectiveLightmapResidencyBudgetMb );
	m_pLightmapStreamer->setEnabled( m_bLightmapStreamingEnabled );
	m_pLightmapStreamer->setRadiusRegions( m_nLightmapStreamRadius );
	m_pLightmapStreamer->setLoadBudget( m_nLightmapLoadBudget );
	m_pLightmapStreamer->setUnloadBudget( m_nLightmapUnloadBudget );
}

void Terrain3D::prepareStartupHLODStreamerStageCold(
	void ) {
	m_pHlodStreamer->setManifestPath( m_hlodManifestPath );
	m_pHlodStreamer->setEnabled( m_bHlodEnabled );
	m_pHlodStreamer->setTargetErrorPixels( m_fHlodTargetErrorPixels );
	m_pHlodStreamer->setHysteresis( m_fHlodHysteresis );
	m_pHlodStreamer->setMaxLevel( m_nHlodMaxLevel );
	m_pHlodStreamer->setLoadBudget( m_nHlodLoadBudget );
	m_pHlodStreamer->setUnloadBudget( m_nHlodUnloadBudget );
	m_pHlodStreamer->setMemoryBudgetMb( m_nEffectiveHlodMemoryBudgetMb );
	m_pHlodStreamer->setMaxShadowLevel( m_nHlodMaxShadowLevel );
	m_pHlodStreamer->setForcedDebugLevel( m_nHlodForcedDebugLevel );
	m_pHlodStreamer->initialize( this );
}

void Terrain3D::prepareStartupTerrainMesherStageCold(
	void ) {
	setupTerrainMesher( m_startupInitialTarget );
}

void Terrain3D::prepareStartupOceanMesherStageCold(
	void ) {
	setupOceanMesher( m_startupInitialTarget );
}

bool Terrain3D::prepareStartupOwnerCold(
	void ) {
	if ( m_runtimeLifecycleState != TERRAIN3D_RUNTIME_INITIALIZING || m_pData == NULL ||
		 !m_pData->isStartupCommitted( m_nStartupGeneration ) ) {
		return false;
	}
	const uint64_t owner_prepare_started_usec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][OwnerPrepare] BEGIN generation=", m_nStartupGeneration );

	t3dStartupDomainSetStartUsecIfZero( m_startupFinalizationState, owner_prepare_started_usec );
	t3dStartupDomainSetStage( m_startupFinalizationState, T3D_STARTUP_PREPARING );
	// Finalize the aggregate owner-controlled memory grants after the terrain
	// repository has been loaded and before tier normalization. GPU page minimums
	// depend on the real 128 m footprint and active topology; normalizing against
	// the constructor-time provisional grant can otherwise collapse Near before
	// the memory coordinator has had a chance to establish functional capacity.
	// Runtime-page validation includes the material source signature, and that
	// signature includes owner-dependent Terrain3D settings. Bind/initialize the
	// asset owner before the first cooked-page residency pass so startup never
	// validates a freshly cooked page against a provisional ownerless signature.
	runStartupOwnerStage( "Assets", &Terrain3D::prepareStartupAssetsStageCold );

	runStartupOwnerStage( "MemoryPolicy", &Terrain3D::prepareStartupMemoryPolicyStageCold );

	// Establish the authoritative residency map before the tier materials take
	// their initial sampler bindings. Physical GPU publication remains bounded
	// in authoring as well as runtime; CPU data and the shared FAR V1 source remain the
	// whole-world authority. Force the first reconciliation even when the actual
	// center/generation equals the zero/default cached state.
	runStartupOwnerStage( "Residency", &Terrain3D::prepareStartupResidencyStageCold );

	runStartupOwnerStage( "Material", &Terrain3D::prepareStartupMaterialStageCold );
	if ( !m_material->isRenderShaderReady() ) {
		m_nStartupOwnerPrepareUsecLast = Time::get_singleton()->get_ticks_usec() - owner_prepare_started_usec;
		LOG( ERROR,
			 "[Terrain3D][Startup][OwnerPrepare][Material] FAIL generation=",
			 m_nStartupGeneration,
			 " reason=invalid_render_shader elapsed=",
			 m_nStartupOwnerPrepareUsecLast,
			 " us" );
		return false;
	}
	runStartupOwnerStage( "Collision", &Terrain3D::prepareStartupCollisionStageCold );
	m_pInstancer->configureInitialPolicy( { m_nInstancerCellSize,
											effectiveInstancerCellStreamingEnabled(),
											m_bInstancerSingleLodBuffers,
											m_fInstancerCellStreamingMargin,
											m_fInstancerCellStreamingUpdateDistance,
											( uint32_t ) ( MAX( m_nInstancerUpdateCellBudget, 0 ) ) } );
	runStartupOwnerStage( "Instancer", &Terrain3D::prepareStartupInstancerStageCold );
	runStartupOwnerStage( "InstanceOcclusion", &Terrain3D::prepareStartupInstanceOcclusionStageCold );
	runStartupOwnerStage( "InstancePhysics", &Terrain3D::prepareStartupInstancePhysicsStageCold );
	runStartupOwnerStage( "NodeStreamer", &Terrain3D::prepareStartupNodeStreamerStageCold );
	runStartupOwnerStage( "LightmapStreamer", &Terrain3D::prepareStartupLightmapStreamerStageCold );
	if ( m_hlodManifestPath.is_empty() && !m_dataDirectory.is_empty() ) {
		m_hlodManifestPath = m_dataDirectory.path_join( "terrain3d_hlod_manifest.tres" );
	}
	runStartupOwnerStage( "HLODStreamer", &Terrain3D::prepareStartupHLODStreamerStageCold );
	// Construct all CPU clipmaps against the same target used for residency, then
	// let the coordinator publish. This prevents its first snapshot from racing
	// an origin-centered/default mesher state. Ocean topology is also ready before
	// initializeOcean() runs.
	runStartupOwnerStage( "TerrainMesher", &Terrain3D::prepareStartupTerrainMesherStageCold );
	runStartupOwnerStage( "OceanMesher", &Terrain3D::prepareStartupOceanMesherStageCold );
	m_nStartupOwnerPrepareUsecLast = Time::get_singleton()->get_ticks_usec() - owner_prepare_started_usec;
	LOG( INFO,
		 "[Terrain3D][Startup][OwnerPrepare] END generation=",
		 m_nStartupGeneration,
		 " elapsed=",
		 m_nStartupOwnerPrepareUsecLast,
		 " us" );
	return true;
}

bool Terrain3D::finalizeStartupOwnerCold(
	void ) {
	const t3dStartupDomainSnapshot_t finalizationSnapshot = t3dStartupDomainSnapshot( m_startupFinalizationState );
	if ( m_runtimeLifecycleState != TERRAIN3D_RUNTIME_INITIALIZING || finalizationSnapshot.m_nCompletedUnits < 2u ||
		 m_pRenderCoordinator == NULL ) {
		return false;
	}
	const uint64_t owner_finalize_started_usec = Time::get_singleton()->get_ticks_usec();
	if ( !m_pRenderCoordinator->hasStartupActivationSnapshot( m_nStartupGeneration ) ) {
		updateDisplacementBuffer();
		// Freeze the final clipmap/ocean/streaming target exactly once while the owner
		// is still non-rendering. The render thread may need later ticks to upload the
		// resulting unified-world publication before activation can commit.
		synchronizeClipmapNow( getClipmapTargetPosition() );
	}
	if ( !m_pRenderCoordinator->prepareStartupActivationCold( m_nStartupGeneration ) ) {
		m_nStartupOwnerFinalizeUsecLast = Time::get_singleton()->get_ticks_usec() - owner_finalize_started_usec;
		return false;
	}
	if ( !m_pRenderCoordinator->isStartupActivationPrepared( m_nStartupGeneration ) ) {
		m_nStartupOwnerFinalizeUsecLast = Time::get_singleton()->get_ticks_usec() - owner_finalize_started_usec;
		return true;
	}

	// Owner activation is the only point at which updateRenderRoute() may expose
	// the prepared snapshot. The coordinator keeps its own activation gate closed
	// until commitStartupActivationCold() immediately below.
	m_bInitialized = true;
	m_runtimeLifecycleState = TERRAIN3D_RUNTIME_ACTIVE;
	if ( !m_pRenderCoordinator->commitStartupActivationCold( m_nStartupGeneration ) ) {
		m_bInitialized = false;
		m_runtimeLifecycleState = TERRAIN3D_RUNTIME_INITIALIZING;
		m_nStartupOwnerFinalizeUsecLast = Time::get_singleton()->get_ticks_usec() - owner_finalize_started_usec;
		return false;
	}
	// V1 pages and shader state are committed. Authoring Texture2D resources
	// are no longer an authority in cooked runtime. Preserve scalar identities.
	if ( m_bFreeEditorTextures && !IS_EDITOR && m_assets.is_valid() && !isEditorAuthoringModeInternal() &&
		 isGpuRegionCookedPagesEnabled() && m_assets->usesPackedPbr() ) {
		( void ) m_assets->publishRuntimeMaterialViewCold();
	}
	updateRegionLabels();
	m_nStartupOwnerFinalizeUsecLast = Time::get_singleton()->get_ticks_usec() - owner_finalize_started_usec;
	m_nStartupEndUsec = Time::get_singleton()->get_ticks_usec();
	m_nStartupTotalUsecLast = m_nStartupEndUsec - m_nStartupStartUsec;
	m_bStartupSucceededLast = true;
	t3dStartupDomainSetCompletedUnits( m_startupFinalizationState, 3u );
	t3dStartupDomainFinish( m_startupFinalizationState, T3D_STARTUP_COMMITTED, m_nStartupEndUsec );
	LOG( INFO,
		 "[Terrain3D][Startup] ACTIVE generation=",
		 m_nStartupGeneration,
		 " elapsed=",
		 m_nStartupTotalUsecLast,
		 " us owner_finalize=",
		 m_nStartupOwnerFinalizeUsecLast,
		 " us" );
	LOG( INFO,
		 "[lifecycle] Main subsystems ready: regions=",
		 m_pData->getRegionCount(),
		 "; region_size=",
		 ( int ) ( T3D_REGION_SIZE ),
		 "; mesh_lods=",
		 m_nMeshLods,
		 "; tessellation=",
		 m_nTessellationLevel );
	// Preview work is queued on the next idle turn after the owner and GPU
	// activation domains have committed. It never blocks startup finalization.
	if ( IS_EDITOR && m_assets.is_valid() ) {
		m_assets->resumePendingMeshThumbnails();
	}
	return true;
}

void Terrain3D::serviceStartup(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_memoryAccount.get() );
	if ( m_runtimeLifecycleState != TERRAIN3D_RUNTIME_INITIALIZING || m_bInitialized || m_nStartupGeneration == 0u ) {
		return;
	}
	const uint32_t nGeneration = m_nStartupGeneration;
	++m_nStartupServiceTraceCount;
	if ( m_pRenderCoordinator ) {
		m_pRenderCoordinator->serviceStartupCold( nGeneration );
		if ( m_pRenderCoordinator->isStartupFailed( nGeneration ) ) {
			// Snapshot the original failure before cancelStartupCold() resets the GPU
			// generation, shader domain, and render lifecycle publication.
			m_pRenderCoordinator->logStartupFailureDiagnosticsCold( "owner_coordinator_service_failed", nGeneration );
			LOG( ERROR, "[Terrain3D][Startup][Coordinator] SERVICE FAILED generation=", nGeneration,
				 " service_call=", m_nStartupServiceTraceCount,
				 " elapsed=", Time::get_singleton()->get_ticks_usec() - m_nStartupStartUsec, " us" );
			deactivateRuntime( true );
			return;
		}
	}
	const t3dStartupDomainSnapshot_t rDataState = m_pData->getStartupSnapshot();
	const t3dStartupDomainSnapshot_t rFinalState = t3dStartupDomainSnapshot( m_startupFinalizationState );
	const t3dStartupDomainSnapshot_t rShaderState = m_pRenderCoordinator ?
		m_pRenderCoordinator->getShaderStartupState() : t3dStartupDomainSnapshot_t{};
	const t3dStartupDomainSnapshot_t rVtState = m_pRenderCoordinator ?
		m_pRenderCoordinator->getVirtualTextureStartupState() : t3dStartupDomainSnapshot_t{};
	const uint64_t nNowUsec = Time::get_singleton()->get_ticks_usec();
	const uint64_t nSignature = ( uint64_t ) rDataState.m_eStage |
		( ( uint64_t ) rFinalState.m_eStage << 8u ) |
		( ( uint64_t ) rFinalState.m_nCompletedUnits << 16u ) |
		( ( uint64_t ) rShaderState.m_eStage << 24u ) |
		( ( uint64_t ) rVtState.m_eStage << 32u );
	if ( nSignature != m_nStartupServiceTraceSignature || m_nStartupServiceTraceLastUsec == 0u ||
		 nNowUsec - m_nStartupServiceTraceLastUsec >= 1000000u ) {
		m_nStartupServiceTraceSignature = nSignature;
		m_nStartupServiceTraceLastUsec = nNowUsec;
		LOG( INFO, "[Terrain3D][Startup][OwnerService] PROGRESS generation=", nGeneration,
			 " call=", m_nStartupServiceTraceCount,
			 " elapsed=", nNowUsec - m_nStartupStartUsec, " us",
			 " data=", t3dStartupStageName( rDataState.m_eStage ),
			 " data_units=", rDataState.m_nCompletedUnits, "/", rDataState.m_nTotalUnits,
			 " owner=", t3dStartupStageName( rFinalState.m_eStage ),
			 " owner_units=", rFinalState.m_nCompletedUnits, "/", rFinalState.m_nTotalUnits,
			 " shader=", t3dStartupStageName( rShaderState.m_eStage ),
			 " shader_units=", rShaderState.m_nCompletedUnits, "/", rShaderState.m_nTotalUnits,
			 " vt=", t3dStartupStageName( rVtState.m_eStage ),
			 " vt_units=", rVtState.m_nCompletedUnits, "/", rVtState.m_nTotalUnits );
	}
	const t3dStartupStage_t			 eDataStage = t3dStartupDomainGetStage( rDataState );
	if ( rDataState.m_nGeneration != nGeneration || eDataStage == T3D_STARTUP_CANCELLED ) {
		LOG( WARN, "[Terrain3D][Startup][OwnerService][Data] CANCEL generation=", nGeneration,
			 " actual_generation=", rDataState.m_nGeneration,
			 " stage=", t3dStartupStageName( eDataStage ) );
		deactivateRuntime( false );
		return;
	}
	if ( eDataStage == T3D_STARTUP_FAILED ) {
		LOG( ERROR, "[Terrain3D][Startup][OwnerService][Data] FAILED generation=", nGeneration,
			 " units=", rDataState.m_nCompletedUnits, "/", rDataState.m_nTotalUnits );
		deactivateRuntime( true );
		return;
	}
	if ( eDataStage == T3D_STARTUP_PREPARING ) {
		const Error error = m_pData->prepareStartupCold( nGeneration );
		if ( error != OK ) {
			LOG( ERROR, "[Terrain3D][Startup][Data] PREPARE FAILED generation=", nGeneration, " error=", error );
			deactivateRuntime( true );
		}
		return;
	}
	if ( eDataStage == T3D_STARTUP_CPU_READY ) {
		const Error error = m_pData->commitStartupCold( nGeneration );
		if ( error != OK ) {
			LOG( ERROR, "[Terrain3D][Startup][Data] COMMIT FAILED generation=", nGeneration, " error=", error );
			deactivateRuntime( true );
			return;
		}
		m_pData->finalizeStartupDerivedCold( nGeneration );
		m_nStartupDataInitializeUsecLast = Time::get_singleton()->get_ticks_usec() - m_nStartupStartUsec;
		LOG( INFO,
			 "[Terrain3D][Startup][Data] COMMITTED generation=",
			 nGeneration,
			 " elapsed=",
			 m_nStartupDataInitializeUsecLast,
			 " us" );
		return;
	}
	if ( eDataStage != T3D_STARTUP_COMMITTED ) {
		return;
	}
	const t3dStartupDomainSnapshot_t finalizationSnapshot = t3dStartupDomainSnapshot( m_startupFinalizationState );
	if ( finalizationSnapshot.m_nCompletedUnits == 0u ) {
		if ( !prepareStartupOwnerCold() ) {
			LOG( ERROR, "[Terrain3D][Startup][OwnerService][OwnerPrepare] FAILED generation=", nGeneration );
			deactivateRuntime( true );
			return;
		}
		t3dStartupDomainSetCompletedUnits( m_startupFinalizationState, 1u );
		return;
	}
	if ( finalizationSnapshot.m_nCompletedUnits == 1u ) {
		const uint64_t coordinator_started_usec = Time::get_singleton()->get_ticks_usec();
		setupRenderCoordinator();
		m_nStartupCoordinatorInitializeUsecLast = Time::get_singleton()->get_ticks_usec() - coordinator_started_usec;
		if ( !m_pRenderCoordinator || m_pRenderCoordinator->isStartupFailed( nGeneration ) ||
			 !m_pRenderCoordinator->isStartupPrepared( nGeneration ) ) {
			if ( m_pRenderCoordinator ) {
				m_pRenderCoordinator->logStartupFailureDiagnosticsCold( "coordinator_prepare", nGeneration );
			}
			LOG( ERROR, "[Terrain3D][Startup][OwnerService][CoordinatorPrepare] FAILED generation=", nGeneration,
				 " elapsed=", m_nStartupCoordinatorInitializeUsecLast, " us" );
			deactivateRuntime( true );
			return;
		}
		t3dStartupDomainSetCompletedUnits( m_startupFinalizationState, 2u );
		t3dStartupDomainSetStage( m_startupFinalizationState, T3D_STARTUP_GPU_PENDING );
		LOG( INFO,
			 "[Terrain3D][Startup][Coordinator] PREPARED generation=",
			 nGeneration,
			 " elapsed=",
			 m_nStartupCoordinatorInitializeUsecLast,
			 " us" );
		return;
	}
	// Advance the initial terrain footprint while rendering is still gated.
	// The first residency pass only queues a fraction of the pages; without this
	// service, preparation and upload resume only after the scene becomes active.
	// Keep the ordinary byte/layer limits and retained tracked-batch completion.
	m_pData->updateGpuRegionResidency( m_startupInitialTarget, false );
	if ( m_pRenderCoordinator ) {
		m_pRenderCoordinator->serviceStartupCold( nGeneration );
		const t3dStartupDomainSnapshot_t rShaderState = m_pRenderCoordinator->getShaderStartupState();
		const t3dStartupDomainSnapshot_t rVtState = m_pRenderCoordinator->getVirtualTextureStartupState();
		const t3dStartupStage_t			 eShaderStage = t3dStartupDomainGetStage( rShaderState );
		const t3dStartupStage_t			 eVtStage = t3dStartupDomainGetStage( rVtState );
		if ( rShaderState.m_nGeneration != nGeneration || rVtState.m_nGeneration != nGeneration ||
			 eShaderStage == T3D_STARTUP_CANCELLED || eVtStage == T3D_STARTUP_CANCELLED ) {
			LOG( WARN, "[Terrain3D][Startup][OwnerService][GPU] CANCEL generation=", nGeneration,
				 " shader_gen=", rShaderState.m_nGeneration, " shader=", t3dStartupStageName( eShaderStage ),
				 " vt_gen=", rVtState.m_nGeneration, " vt=", t3dStartupStageName( eVtStage ) );
			deactivateRuntime( false );
			return;
		}
		if ( eShaderStage == T3D_STARTUP_FAILED || eVtStage == T3D_STARTUP_FAILED ) {
			m_pRenderCoordinator->logStartupFailureDiagnosticsCold( "gpu_domain_failed", nGeneration );
			LOG( ERROR,
				 "[Terrain3D][Startup][GPU] FAILED generation=", nGeneration,
				 " shader_stage=", ( int ) eShaderStage,
				 " vt_stage=", ( int ) eVtStage );
			deactivateRuntime( true );
			return;
		}
		const bool bShaderGpuReady = eShaderStage == T3D_STARTUP_GPU_READY || eShaderStage == T3D_STARTUP_COMMITTED;
		if ( !bShaderGpuReady || eVtStage != T3D_STARTUP_COMMITTED ) {
			return;
		}
	}
	t3dStartupDomainSetStage( m_startupFinalizationState, T3D_STARTUP_GPU_READY );
	if ( !finalizeStartupOwnerCold() ) {
		if ( m_pRenderCoordinator ) {
			m_pRenderCoordinator->logStartupFailureDiagnosticsCold( "owner_finalization_failed", nGeneration );
		}
		LOG( ERROR, "[Terrain3D][Startup][OwnerService][Activation] FAILED generation=", nGeneration );
		deactivateRuntime( true );
	}
}

void Terrain3D::onRegionMapChanged(
	void ) {
	// The collision consumer has a shorter lifetime than the data owner.
	if ( m_pCollision ) {
		m_pCollision->onRegionMapChanged();
	}
}

void Terrain3D::deactivateRuntime(
	const bool bStartupFailure ) {
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN );
	const bool bWasInitializing = m_runtimeLifecycleState == TERRAIN3D_RUNTIME_INITIALIZING && !m_bStartupSucceededLast;
	const uint32_t nShutdownGeneration = m_nStartupGeneration.load( T3D_MEMORY_ORDER_ACQUIRE );
	LOG( INFO,
		 "[Terrain3D][Lifecycle][Deactivate] generation=", nShutdownGeneration,
		 " state=", ( int ) m_runtimeLifecycleState,
		 " startup_failure=", bStartupFailure,
		 " initialized=", m_bInitialized,
		 " startup_succeeded=", m_bStartupSucceededLast,
		 " inside_tree=", is_inside_tree(),
		 " inside_world=", m_bIsInsideWorld );
	if ( bWasInitializing && nShutdownGeneration != 0u ) {
		if ( m_pData ) {
			m_pData->cancelStartupCold( nShutdownGeneration );
		}
		if ( m_pRenderCoordinator ) {
			m_pRenderCoordinator->cancelStartupCold( nShutdownGeneration );
		}
		m_nStartupEndUsec = Time::get_singleton()->get_ticks_usec();
		m_nStartupTotalUsecLast = m_nStartupStartUsec != 0u ? m_nStartupEndUsec - m_nStartupStartUsec : 0u;
		t3dStartupDomainFinish( m_startupFinalizationState,
								bStartupFailure ? T3D_STARTUP_FAILED : T3D_STARTUP_CANCELLED,
								m_nStartupEndUsec );
		if ( !bStartupFailure ) {
			++m_nStartupCancellationCount;
		}
		LOG( bStartupFailure ? ERROR : INFO,
			 bStartupFailure ? "[Terrain3D][Startup] FAILED generation=" : "[Terrain3D][Startup] CANCELLED generation=",
			 nShutdownGeneration,
			 " elapsed=",
			 m_nStartupTotalUsecLast,
			 " us" );
		uint32_t nInvalidatedGeneration = nShutdownGeneration + 1u;
		if ( nInvalidatedGeneration == 0u ) {
			nInvalidatedGeneration = 1u;
		}
		m_nStartupGeneration.store( nInvalidatedGeneration, T3D_MEMORY_ORDER_RELEASE );
		++m_nStartupGenerationInvalidationCount;
	}
	if ( m_runtimeLifecycleState == TERRAIN3D_RUNTIME_DEACTIVATING ||
		 m_runtimeLifecycleState == TERRAIN3D_RUNTIME_DETACHED ) {
		LOG( DEBUG,
			 "[lifecycle] Ignoring duplicate runtime deactivation; state=",
			 ( int ) ( m_runtimeLifecycleState ) );
		return;
	}
	if ( m_runtimeLifecycleState == TERRAIN3D_RUNTIME_SHUTTING_DOWN ||
		 m_runtimeLifecycleState == TERRAIN3D_RUNTIME_DRAIN_FAILED ) {
		return;
	}

	m_runtimeLifecycleState = TERRAIN3D_RUNTIME_DEACTIVATING;
	m_bInitialized = false;
	set_physics_process( false );

	// Scene/world detach destroys runtime consumers, but the fixed allocator image
	// backs object-lifetime repository/container storage and therefore must survive
	// until NOTIFICATION_PREDELETE. This path is deliberately idempotent because
	// Godot commonly delivers both EXIT_TREE and EXIT_WORLD for one scene switch.
	destroyRenderCoordinator();
	destroyTerrainMesher();
	destroyOceanMesher();
	destroyHlodStreamer();
	destroyInstancePhysics();
	destroyInstanceOcclusion();
	destroyLightmapStreamer();
	destroyNodeStreamer( true );
	destroyInstancer();
	destroyCollision();
	destroyDisplacementBuffer();
	if ( m_pAuthoringHooks ) {
		m_pAuthoringHooks->releaseRuntimeAllocatorStorage();
	}
	disconnectResourceSignals( m_assets, m_material );
	if ( m_assets.is_valid() ) {
		m_assets->uninitialize( this );
	}
	if ( m_material.is_valid() ) {
		m_material->uninitialize( this );
	}
	if ( m_pData ) {
		m_pData->deactivateRuntimeStorage( this );
	}

	// Detach preserves this Terrain3D instance's private runtime zone. Retained
	// runtime containers carry explicit allocator->zone ownership, so another
	// Terrain3D instance can activate independently without changing their route.

	m_runtimeLifecycleState = m_bRuntimeMemoryReady ? TERRAIN3D_RUNTIME_DETACHED : TERRAIN3D_RUNTIME_COLD;
}

/**
 * Hot-frame work is owned by Terrain3DRuntimeFrameScheduler. Terrain3D remains
 * the Node lifecycle/public facade and forwards the physics notification.
 */
void Terrain3D::physicsProcess(
	const double fDelta ) {
	Terrain3DMemoryAccountScope memory_scope( m_memoryAccount.get() );
	if ( m_runtimeLifecycleState == TERRAIN3D_RUNTIME_INITIALIZING ) {
		serviceStartup();
	}
	if ( m_bInitialized ) {
		m_runtimeFrameScheduler.tick( runtimeFrameServices(), fDelta );
	}
}

/**
 * If running in the editor, grab the first editor viewport camera.
 * The edited_scene_root is excluded in case the user already has a Camera3D in their scene.
 */
void Terrain3D::grabCamera(
	void ) {
	if ( IS_EDITOR ) {
		EditorInterface *const pEditor = EditorInterface::get_singleton();
		SubViewport *const	   pViewport = pEditor ? pEditor->get_editor_viewport_3d( 0 ) : NULL;
		m_hCamera.setTarget( pViewport ? pViewport->get_camera_3d() : NULL );
		LOG( DEBUG, "Grabbing the first editor viewport camera: ", m_hCamera.getTarget() );
	} else {
		Viewport *const pViewport = get_viewport();
		m_hCamera.setTarget( pViewport ? pViewport->get_camera_3d() : NULL );
		LOG( DEBUG, "Grabbing the in-game viewport camera: ", m_hCamera.getTarget() );
	}
	if ( !m_hCamera.isValid() && !m_clipmapTarget.isValid() ) {
		set_physics_process( false ); // No target to follow, disable snapping until one set
		LOG( ERROR,
			 "Cannot find clipmap target or active camera. LODs won't be updated. Set manually with "
			 "set_clipmap_target() or setCamera()" );
	}
}

void Terrain3D::buildContainers(
	void ) {
	if ( m_pLabelParent ) {
		return;
	}
	m_pLabelParent = memnew( Node3D );
	m_pLabelParent->set_name( "Labels" );
	add_child( m_pLabelParent, true );
}

void Terrain3D::destroyContainers(
	void ) {
	if ( m_pLabelParent ) {
		memdelete( m_pLabelParent );
		m_pLabelParent = NULL;
	}
}

const t3dBakeRegionSnapshot_t *t3dBakeSnapshot_t::getRegion(
	const int32_t iX,
	const int32_t iZ ) const {
	const Vector2i regionLocation = t3dRegionCoordinateFromGrid( Vector2i( iX, iZ ) );
	const int32_t  map_index = Terrain3DData::getRegionMapIndex( regionLocation );
	if ( map_index < 0 ) {
		return NULL;
	}
	const int32_t region_index = m_szRegionLookup[map_index];
	if ( region_index < 0 || region_index >= ( int32_t ) m_regions.size() ) {
		return NULL;
	}
	return &m_regions[( size_t ) ( region_index )];
}

void t3dBakeSnapshot_t::getSample(
	const int32_t iX,
	const int32_t iZ,
	uint32_t	 &nControl,
	real_t		 &fHeight ) const {
	const t3dBakeRegionSnapshot_t *pRegion = getRegion( iX, iZ );
	if ( !pRegion || !pRegion->m_pHeightBytes ) {
		nControl = UINT32_MAX;
		fHeight = NAN;
		return;
	}
	const int32_t local_x = t3dRegionLocalFromGrid( iX );
	const int32_t local_z = t3dRegionLocalFromGrid( iZ );
	const int32_t index = local_z * T3D_REGION_SIZE + local_x;
	fHeight = ( real_t ) ( t3dRegionCodecReadHeightBytes( pRegion->m_pHeightBytes, index, Image::FORMAT_R16 ) );
	if ( pRegion->m_pControlBytes ) {
		uint32_t control = T3D_REGION_DEFAULT_CONTROL;
		memcpy( &control, pRegion->m_pControlBytes + ( size_t ) ( index ) * sizeof( uint32_t ), sizeof( uint32_t ) );
		nControl = control;
	} else {
		nControl = T3D_REGION_DEFAULT_CONTROL;
	}
	return;
}

bool t3dBakeSnapshot_t::getMinimumRect(
	const int32_t iX,
	const int32_t iZ,
	const int32_t nWidth,
	const int32_t nHeight,
	real_t		 &fHeight ) const {
	if ( nWidth <= 0 || nHeight <= 0 ) {
		return false;
	}
	real_t fMinimum = eastl::numeric_limits< real_t >::infinity();
	for ( int32_t row = 0; row < nHeight; ++row ) {
		const int32_t global_z = iZ + row;
		int32_t		  copied = 0;
		while ( copied < nWidth ) {
			const int32_t				   global_x = iX + copied;
			const t3dBakeRegionSnapshot_t *pRegion = getRegion( global_x, global_z );
			if ( !pRegion || !pRegion->m_pHeightBytes ) {
				return false;
			}
			const int32_t	local_x = t3dRegionLocalFromGrid( global_x );
			const int32_t	local_z = t3dRegionLocalFromGrid( global_z );
			const int32_t	run = MIN( nWidth - copied, T3D_REGION_SIZE - local_x );
			const int32_t	index = local_z * T3D_REGION_SIZE + local_x;
			const uint8_t *const pHeight = pRegion->m_pHeightBytes + ( size_t ) index * sizeof( uint16_t );
			const uint8_t *const pControl =
				pRegion->m_pControlBytes ? pRegion->m_pControlBytes + ( size_t ) index * sizeof( uint32_t ) : NULL;
			for ( int32_t i = 0; i < run; ++i ) {
				const uint32_t sample_control = pControl ? t3dReadNativeUnaligned< uint32_t >( pControl + ( size_t ) i * sizeof( uint32_t ) )
							 : T3D_REGION_DEFAULT_CONTROL;
				if ( t3dIsHole( sample_control ) ) {
					return false;
				}
				fMinimum = MIN( fMinimum, ( real_t ) ( t3dRegionCodecReadHeightBytes( pHeight, i, Image::FORMAT_R16 ) ) );
			}
			copied += run;
		}
	}
	if ( !isfinite( fMinimum ) ) {
		return false;
	}
	fHeight = fMinimum;
	return true;
}

Vector3 t3dBakeSnapshot_t::getMeshVertex(
	const int32_t			nLod,
	const t3dHeightFilter_t filter,
	const int32_t			iX,
	const int32_t			iZ,
	uint32_t			   &nControl ) const {
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	real_t		  fHeight = NAN;
	getSample( iX, iZ, nControl, fHeight );
	if ( t3dIsHole( nControl ) ) {
		fHeight = NAN;
	}

	if ( filter == HEIGHT_FILTER_MINIMUM && !getMinimumRect( iX - step / 2, iZ - step / 2, step, step, fHeight ) ) {
		fHeight = NAN;
	}

	return Vector3( ( real_t ) ( iX ) *m_fVertexSpacing, fHeight, ( real_t ) ( iZ ) *m_fVertexSpacing );
}

// Generates two triangles: Top 124, Bottom 143
//		1  __  2
//		  |\ |
//		  | \|
//		3  --  4
// p_require_nav is false for the runtime baker, which ignores navigation.
// -----------------------------------------------------------------------------
// Public functions

Terrain3D::Terrain3D(
	void ) {
	// The account must accept authoring allocations before any runtime arena,
	// texture payload or stream worker is created. No scene property is modified.
	const t3dMemoryProfileLimits_t memory_profile = resolveMemoryProfileLimits( m_editorExecutionMode );
	if ( m_memoryAccount.get() ) {
		m_memoryAccount.get()->setLimit( uint64_t( memory_profile.m_nTotalBudgetMb ) * T3D_MEMORY_MIB );
		m_memoryAccount.get()->setEditorAuthoringPolicy( memory_profile.m_bEditorAuthoring );
	}
	LOG( INFO, "Terrain3D v", m_version, " - https://github.com/TokisanGames/Terrain3D" );
	LOG( INFO, "[build] native_build_id=", s_pszNativeBuildId );
	// Process the command line
	PackedStringArray args = OS::get_singleton()->get_cmdline_args();
	for ( int i = args.size() - 1; i >= 0; i-- ) {
		String arg = args[i];
		if ( arg.begins_with( "--terrain3d-debug=" ) ) {
			String value = arg.rsplit( "=" )[1];
			if ( value == "ERROR" ) {
				setDebugLevel( ERROR );
			} else if ( value == "INFO" ) {
				setDebugLevel( INFO );
			} else if ( value == "DEBUG" ) {
				setDebugLevel( DEBUG );
			} else if ( value == "EXTREME" ) {
				setDebugLevel( EXTREME );
			}
		}
	}
}

Terrain3D::~Terrain3D(
	void ) {
	releaseViewportVrsProfile();
	if ( m_pAuthoringHooks ) {
		m_pAuthoringHooks->onTerrainOwnerDetached();
		m_pAuthoringHooks = NULL;
	}
	m_pEditor = NULL;
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN );
	if ( m_pRuntimeProfileHistory ) {
		memdelete( m_pRuntimeProfileHistory );
		m_pRuntimeProfileHistory = NULL;
	}
	// PREDELETE owns the normal final teardown. Keep this as a last-resort fallback
	// for destruction paths that bypass the notification sequence.
	if ( m_bRuntimeMemoryReady && m_runtimeLifecycleState != TERRAIN3D_RUNTIME_DRAIN_FAILED &&
		 !shutdownRuntimeMemoryImage() ) {
		LOG( ERROR,
			 "[lifecycle] Terrain destroyed with live runtime allocator storage; live allocations=",
			 m_runtimeMemoryZone.liveAllocations() );
	}
}

void Terrain3D::setDebugLevel(
	const t3dDebugLevel_t level ) {
	Terrain3DLogPolicy::setLevel( level );
	LOG( INFO, "Setting debug level: ", Terrain3DLogPolicy::getLevel() );
}

#define T3D_OCEAN_MODE_SETTER( NAME, TYPE, FIELD )                                                                     \
	void Terrain3D::NAME( const TYPE valueArg ) {                                                                      \
		if ( FIELD == valueArg ) {                                                                                     \
			return;                                                                                                    \
		}                                                                                                              \
		FIELD = valueArg;                                                                                              \
		m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM;                                                                \
		markOceanPublicationDirty();                                                                                   \
	}

T3D_OCEAN_MODE_SETTER(
	setOceanRefractionMode,
	t3dOceanRefractionMode_t,
	m_oceanRefractionMode )
T3D_OCEAN_MODE_SETTER(
	setOceanFoamMode,
	t3dOceanFoamMode_t,
	m_oceanFoamMode )
T3D_OCEAN_MODE_SETTER(
	setOceanScatteringMode,
	t3dOceanScatteringMode_t,
	m_oceanScatteringMode )
T3D_OCEAN_MODE_SETTER(
	setOceanShadingMode,
	t3dOceanShadingMode_t,
	m_oceanShadingMode )
#undef T3D_OCEAN_MODE_SETTER

/* Returns the point a ray intersects the ground using either raymarching or the GPU depth texture
 *	p_src_pos (camera position)
 *	p_direction (camera direction looking at the terrain)
 *  p_gpu_mode - false: use raymarching, true: use GPU mode
 * Returns Vec3(NAN) on error or vec3(3.402823466e+38F) on no intersection. Test w/ if (var.x < 3.4e38)
 */
/* Returns the results of a physics raycast, optionally excluding the terrain
 *	p_src_pos (ray start position)
 *	p_direction (ray direction * magnitude), relative to src_pos
 */
/**
 * Generates a static ArrayMesh for the terrain.
 * p_lod (0-8): Determines the granularity of the generated mesh.
 * p_filter: Controls how vertices' Y coordinates are generated from the height map.
 *  HEIGHT_FILTER_NEAREST: Samples the height map in a 'nearest neighbour' fashion.
 *  HEIGHT_FILTER_MINIMUM: Samples a range of heights around each vertex and returns the lowest.
 *   This takes longer than ..._NEAREST, but can be used to create occluders, since it can guarantee the
 *   generated mesh will not extend above or outside the clipmap at any LOD.
 */
/**
 * Generates an ArrayOccluder3D directly. Unlike bake_mesh(), this path does not calculate normals,
 * tangents, or create an intermediate ArrayMesh that the editor immediately throws away.
 */
/**
 * Generates source geometry faces for input to nav mesh baking. Geometry is only generated where there
 * are no holes and the terrain has been painted as navigable.
 * p_global_aabb: If non-empty, geometry will be generated only within this AABB. If empty, geometry
 *  will be generated for the entire terrain.
 * p_require_nav: If true, this function will only generate geometry for terrain marked navigable.
 *  Otherwise, geometry is generated for the entire terrain within the AABB (which can be useful for
 *  dynamic and/or runtime nav mesh baking).
 */
// -----------------------------------------------------------------------------
// Protected functions

// Notifications are defined in individual classes: Object, Node, Node3D
// Listed below in order of operation
void Terrain3D::_notification(
	const int nWhat ) {
	switch ( nWhat ) {
		/// Startup notifications

	case NOTIFICATION_POSTINITIALIZE: {
		// Object initialized, before script is attached
		LOG( INFO, "NOTIFICATION_POSTINITIALIZE" );
		buildContainers();
		break;
	}

	case NOTIFICATION_ENTER_WORLD: {
		// Node3D registered to new World3D resource
		// Sent on scene changes
		LOG( INFO, "NOTIFICATION_ENTER_WORLD" );
		m_bIsInsideWorld = true;
		// ENTER_WORLD and ENTER_TREE can be delivered in different relative
		// orders during editor scene-tab transitions. Initialization is
		// idempotent and must run after both conditions become true.
		initializeInternal();
		if ( is_inside_tree() ) {
			applyViewportVrsProfile();
			set_physics_process( true );
		}
		break;
	}

	case NOTIFICATION_ENTER_TREE: {
		// Node entered a SceneTree
		// Sent on scene changes
		LOG( INFO, "NOTIFICATION_ENTER_TREE" );
		set_as_top_level( true ); // Don't inherit transforms from parent. Global only.
		set_notify_transform( true );
		set_meta( "_edit_lock_", true );
		setupMousePicking();
		setupDisplacementBuffer();
		initializeInternal(); // Rebuild anything freed: meshes, collision, instancer
		applyViewportVrsProfile();
		set_physics_process( true );
		break;
	}

	case NOTIFICATION_READY: {
		// Node is ready
		LOG( INFO, "NOTIFICATION_READY" );
		break;
	}

		/// Game Loop notifications

	case NOTIFICATION_PHYSICS_PROCESS: {
		// Node is processing one physics frame
		physicsProcess( get_physics_process_delta_time() );
		break;
	}

	case NOTIFICATION_TRANSFORM_CHANGED: {
		// Node3D or parent transform changed
		if ( get_transform() != Transform3D() ) {
			set_transform( Transform3D() );
		}
		break;
	}

	case NOTIFICATION_VISIBILITY_CHANGED: {
		// Node3D visibility changed
		LOG( INFO, "NOTIFICATION_VISIBILITY_CHANGED" );
		if ( m_pTerrainMesher ) {
			m_pTerrainMesher->update();
		}
		if ( m_pOceanMesher ) {
			m_pOceanMesher->update();
		}
		if ( m_pInstancer ) {
			if ( !is_visible_in_tree() ) {
				m_pInstancer->suspendRendering();
			} else {
				m_pInstancer->updateMmis( -1, s_v2iMax, true );
			}
		}
		break;
	}

	case NOTIFICATION_EXTENSION_RELOADED: {
		// Object finished hot reloading
		LOG( INFO, "NOTIFICATION_EXTENSION_RELOADED" );
		break;
	}

	case NOTIFICATION_EDITOR_PRE_SAVE: {
		// Editor Node is about to save the current scene
		LOG( INFO, "NOTIFICATION_EDITOR_PRE_SAVE" );
		if ( m_dataDirectory.is_empty() ) {
			LOG( ERROR, "Data directory is empty. Set it to save regions to disk." );
		} else if ( !m_pData ) {
			LOG( DEBUG, "Save requested, but no valid data object. Skipping" );
		} else {
			m_pData->saveDirectory( m_dataDirectory );
		}
		if ( !m_material.is_valid() ) {
			LOG( DEBUG, "Save requested, but no valid material. Skipping" );
		} else {
			m_material->save();
		}
		if ( !m_assets.is_valid() ) {
			LOG( DEBUG, "Save requested, but no valid texture list. Skipping" );
		} else {
			m_assets->save();
		}
		break;
	}

	case NOTIFICATION_EDITOR_POST_SAVE: {
		// Editor Node finished saving current scene
		break;
	}

	case NOTIFICATION_CRASH: {
		// Godot's crash handler reports engine is about to crash
		// Only works on desktop if the crash handler is enabled
		LOG( INFO, "NOTIFICATION_CRASH" );
		break;
	}

		/// Shut down notifications

	case NOTIFICATION_EXIT_TREE: {
		// Node is about to exit a SceneTree
		// Sent on scene changes
		LOG( INFO, "NOTIFICATION_EXIT_TREE" );
		releaseViewportVrsProfile();
		deactivateRuntime();
		destroyMousePicking();
		break;
	}

	case NOTIFICATION_EXIT_WORLD: {
		// Node3D unregistered from current World3D
		// Sent on scene changes
		LOG( INFO, "NOTIFICATION_EXIT_WORLD" );
		releaseViewportVrsProfile();
		// Rendering/physics RIDs and process-static terrain storage belong to the
		// active World3D. Preserve the loaded repository, but release every runtime
		// owner so another scene can activate before this node is destroyed.
		deactivateRuntime();
		m_bIsInsideWorld = false;
		break;
	}

	case NOTIFICATION_PREDELETE: {
		// Object is about to be deleted. Unlike scene detach, this is the point
		// where object-lifetime owners are destroyed and the fixed memory image may
		// finally enter DRAINING.
		LOG( INFO, "NOTIFICATION_PREDELETE" );
		disconnectAutoShaderProfileSignal( m_autoShaderProfile );
		deactivateRuntime();
		m_runtimeLifecycleState = TERRAIN3D_RUNTIME_SHUTTING_DOWN;

		destroyRenderCoordinator( true );
		destroyTerrainMesher( true );
		destroyOceanMesher( true );
		destroyHlodStreamer();
		destroyLightmapStreamer();
		destroyNodeStreamer( false );
		destroyInstancer();
		destroyCollision( true );
		if ( m_pAuthoringHooks ) {
			m_pAuthoringHooks->releaseRuntimeAllocatorStorage();
		}

		m_assets.unref();
		m_material.unref();
		if ( m_pData ) {
			if ( !m_pData->releaseRuntimeAllocatorStorageInternal() ) {
				LOG( ERROR, "[lifecycle] Terrain data retained allocator-backed storage during final teardown" );
			}
			memdelete( m_pData );
			m_pData = NULL;
			m_nDataSignalConnections = 0u;
		}
		destroyLabels();
		destroyContainers();
		if ( m_pRuntimeProfileHistory ) {
			memdelete( m_pRuntimeProfileHistory );
			m_pRuntimeProfileHistory = NULL;
		}

		if ( !shutdownRuntimeMemoryImage() ) {
			LOG( ERROR,
				 "[lifecycle] Fixed runtime memory image still has live allocations after object-lifetime teardown" );
		}
		if ( m_pAuthoringHooks ) {
			m_pAuthoringHooks->onTerrainOwnerDetached();
			m_pAuthoringHooks = NULL;
		}
		m_pEditor = NULL;
		break;
	}

	default:
		break;
	}
}

// M39 extracted non-template triangle-buffer implementation.
void t3dTriangleBuffer_s::bindExternal(
	Vector3 *const pVertices,
	Vector2 *const pUvs,
	const size_t   nCapacity ) {
	m_pExternalVertices = pVertices;
	m_pExternalUvs = pUvs;
	m_nExternalCapacity = nCapacity;
	m_nExternalSize = 0u;
	m_bOverflowed = false;
}

void t3dTriangleBuffer_s::reserve(
	const size_t nCapacity,
	const bool	 bUvs ) {
	m_ownedVertices.reserve( nCapacity );
	if ( bUvs ) {
		m_ownedUvs.reserve( nCapacity );
	}
}

void t3dTriangleBuffer_s::appendTriangle(
	const Vector3 &rA,
	const Vector3 &rB,
	const Vector3 &rC,
	const bool	   bUvs ) {
	if ( m_pExternalVertices ) {
		if ( m_nExternalSize + 3u > m_nExternalCapacity ) {
			m_bOverflowed = true;
			return;
		}
		m_pExternalVertices[m_nExternalSize] = rA;
		m_pExternalVertices[m_nExternalSize + 1u] = rB;
		m_pExternalVertices[m_nExternalSize + 2u] = rC;
		if ( bUvs && m_pExternalUvs ) {
			m_pExternalUvs[m_nExternalSize] = Vector2( rA.x, rA.z );
			m_pExternalUvs[m_nExternalSize + 1u] = Vector2( rB.x, rB.z );
			m_pExternalUvs[m_nExternalSize + 2u] = Vector2( rC.x, rC.z );
		}
		m_nExternalSize += 3u;
		return;
	}

	m_ownedVertices.push_back( rA );
	m_ownedVertices.push_back( rB );
	m_ownedVertices.push_back( rC );
	if ( bUvs ) {
		m_ownedUvs.push_back( Vector2( rA.x, rA.z ) );
		m_ownedUvs.push_back( Vector2( rB.x, rB.z ) );
		m_ownedUvs.push_back( Vector2( rC.x, rC.z ) );
	}
}

uint32_t Terrain3D::getMegaVTTexelsPerMeterInternal(
	void ) const {
	return m_pRenderCoordinator ? m_pRenderCoordinator->getMegaVTTexelsPerMeterCold() : 2u;
}

void Terrain3D::setEnvironmentForcing(
    float fRain, float fSnow, float fDry, const Vector2 &rWind,
    float fGust, float fDeltaTime, bool bEnabled ) {
    if ( !m_pRenderCoordinator ) {
        return;
    }
    t3dEnvironmentForcing_t forcing{};
    forcing.m_fRainRate = isfinite( fRain ) ? CLAMP( fRain, 0.0f, 1.0f ) : 0.0f;
    forcing.m_fSnowRate = isfinite( fSnow ) ? CLAMP( fSnow, 0.0f, 1.0f ) : 0.0f;
    forcing.m_fDryRate = isfinite( fDry ) ? CLAMP( fDry, 0.0f, 1.0f ) : 0.0f;
    forcing.m_fWindX = isfinite( ( double ) rWind.x ) ? ( float ) rWind.x : 0.0f;
    forcing.m_fWindZ = isfinite( ( double ) rWind.y ) ? ( float ) rWind.y : 0.0f;
    forcing.m_fGustStrength = isfinite( fGust ) ? MAX( fGust, 0.0f ) : 0.0f;
    forcing.m_fDeltaTime = isfinite( fDeltaTime ) ? CLAMP( fDeltaTime, 0.0f, 0.1f ) : 0.0f;
    forcing.m_nFlags = bEnabled ? 1u : 0u;
    m_pRenderCoordinator->setEnvironmentForcingOwner( forcing );
}

uint32_t Terrain3D::submitEnvironmentImpulses(
    const PackedByteArray &rPackedImpulses ) {
    if ( !m_pRenderCoordinator || rPackedImpulses.size() == 0u ||
         ( rPackedImpulses.size() % sizeof( t3dEnvironmentImpulse_t ) ) != 0u ) {
        return 0u;
    }
    const uint64_t nCount = ( uint64_t ) rPackedImpulses.size() / sizeof( t3dEnvironmentImpulse_t );
    if ( nCount > UINT32_MAX ) {
        return 0u;
    }
    return m_pRenderCoordinator->submitEnvironmentImpulsesOwner(
        ( const t3dEnvironmentImpulse_t * ) rPackedImpulses.ptr(), ( uint32_t ) nCount );
}

uint64_t Terrain3D::getEnvironmentDroppedImpulses() const {
    return m_pRenderCoordinator ? m_pRenderCoordinator->getEnvironmentDroppedCountOwner() : 0u;
}

// ENV-P07: runtime climate persistence belongs to the existing publication owner.
// External weather/gameplay provides forcing; export/import are explicit save operations.
bool Terrain3D::setEnvironmentRegionClimate( const Vector2i &rRegion, const Vector3 &rClimate ) {
    return m_pRenderCoordinator && m_pRenderCoordinator->setEnvironmentRegionClimateOwner( rRegion, rClimate );
}

Vector3 Terrain3D::getEnvironmentRegionClimate( const Vector2i &rRegion ) const {
    return m_pRenderCoordinator ? m_pRenderCoordinator->getEnvironmentRegionClimateOwner( rRegion ) : Vector3();
}

PackedByteArray Terrain3D::exportEnvironmentRegionClimate() const {
    return m_pRenderCoordinator ? m_pRenderCoordinator->exportEnvironmentRegionClimateOwner() : PackedByteArray();
}

bool Terrain3D::importEnvironmentRegionClimate( const PackedByteArray &rData ) {
    return m_pRenderCoordinator && m_pRenderCoordinator->importEnvironmentRegionClimateOwner( rData );
}

uint64_t Terrain3D::getPreparedHlodSourceGenerationCold( void ) const
{
	return m_pRenderCoordinator ? m_pRenderCoordinator->getPreparedHlodSourceGenerationCold() : 0u;
}
