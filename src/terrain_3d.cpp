// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d.h"
#include "core/terrain_3d_sync.h"

#include "Terrain3DConstants.h"
#include "authoring/terrain_3d_editor.h"
#include "core/logger.h"
#include "core/terrain_3d_integration_helpers.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_lighting_policy.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "memory/terrain_3d_linear_arena.h"
#include "memory/terrain_3d_memory.h"
#include "memory/terrain_3d_zone_allocator.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"
#include "rendering/terrain_3d_compute.h"
#include "simd/terrain_3d_parallel.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "streaming/terrain_3d_performance_profile.h"
#include "streaming/terrain_3d_streaming_policy.h"

#include <EASTL/numeric_limits.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>

// Initialize static member variable
t3dDebugLevel_t Terrain3D::s_debugLevel{ ERROR };

// -----------------------------------------------------------------------------
// Private functions

void Terrain3D::initializeInternal( void ) {
	// Initialization is an explicit owner-controlled rebuild boundary. The phase
	// scopes below label telemetry only; each storage owner decides whether its
	// backing may change during this transition.
	Terrain3DColdPhaseScope cold_scope;
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION );
	if ( m_runtimeLifecycleState == TERRAIN3D_RUNTIME_SHUTTING_DOWN || m_runtimeLifecycleState == TERRAIN3D_RUNTIME_DRAIN_FAILED ) {
		LOG( ERROR,
		        "[lifecycle] Ignoring initialize request while runtime memory is shutting down; state=",
		        ( int )( m_runtimeLifecycleState ) );
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
	if ( m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL
	        && ( bCreatedDefaultMaterial || m_bPerformanceProfileMaterialPendingApply ) ) {
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
		if ( !m_pInstancer->bindRuntimeAllocator( &m_runtimeMemoryZone, getRuntimeAllocatorOverflowPolicyInternal() ) ) {
			LOG( ERROR, "Failed binding instancer runtime allocator ownership" );
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
		if ( !m_pHlodStreamer->bindRuntimeAllocator( &m_runtimeMemoryZone, getRuntimeAllocatorOverflowPolicyInternal() ) ) {
			LOG( ERROR, "Failed binding HLOD runtime allocator ownership" );
			return;
		}
	}
	// Connect signals
	// Any region was changed, update region labels
	if ( !m_pData->is_connected( "region_map_changed", callable_mp( this, &Terrain3D::updateRegionLabels ) ) ) {
		LOG( DEBUG, "Connecting _data::region_map_changed signal to set_show_region_locations()" );
		m_pData->connect( "region_map_changed", callable_mp( this, &Terrain3D::updateRegionLabels ) );
	}
	// Region layout changes are coalesced and update the existing dynamic
	// collision pool instead of rebuilding every shape per editor event.
	if ( !m_pData->is_connected( "region_map_changed", callable_mp( m_pCollision, &Terrain3DCollision::onRegionMapChanged ) ) ) {
		LOG( DEBUG, "Connecting _data::region_map_changed signal to on_region_map_changed()" );
		m_pData->connect( "region_map_changed", callable_mp( m_pCollision, &Terrain3DCollision::onRegionMapChanged ) );
	}
	connectResourceSignals( m_assets, m_material );
	refreshPerformanceProfileConformity();
	// Height map was regenerated, update aabbs
	if ( !m_pData->is_connected( "height_maps_changed", callable_mp( this, &Terrain3D::updateMesherAabbs ) ) ) {
		LOG( DEBUG, "Connecting _data::height_maps_changed signal to update_aabbs()" );
		m_pData->connect( "height_maps_changed", callable_mp( this, &Terrain3D::updateMesherAabbs ) );
	}
	// Initialize the system
	if ( !m_bInitialized && m_bIsInsideWorld && is_inside_tree() ) {
		m_runtimeLifecycleState = TERRAIN3D_RUNTIME_INITIALIZING;
		LOG( INFO,
		        "[lifecycle] Initializing main subsystems: gpu_cache=",
		        effectiveGpuRegionCacheEnabled(),
		        "; cpu_cache=",
		        effectiveCpuRegionCacheEnabled(),
		        "; instance_cache=",
		        effectiveInstanceRegionCacheEnabled() );
		// Resolve one authoritative startup target before residency selection, CPU
		// clipmap placement, or RD snapshot publication. A Terrain3D node can enter
		// the editor world before the editor plugin has handed it a Camera3D; without
		// this acquisition the mesher silently initializes every LOD around world
		// origin and the first GPU snapshot preserves that stale placement.
		if ( !m_hCamera.isValid() && ( IS_EDITOR || !m_clipmapTarget.isValid() ) ) {
			grabCamera();
		}
		const Vector3 initial_target = getClipmapTargetPosition();

		const Error data_error = m_pData->initialize( this );
		if ( data_error != OK ) {
			LOG( ERROR,
			        "[lifecycle] Terrain data activation failed; dependent runtime systems will remain inactive. "
			        "Error: ",
			        data_error,
			        "; state=",
			        m_pData->getRuntimeState() );
			deactivateRuntime();
			return;
		}
		// Finalize the aggregate owner-controlled memory grants after the terrain
		// repository has been loaded and before tier normalization. GPU page minimums
		// depend on the real 128 m footprint and active topology; normalizing against
		// the constructor-time provisional grant can otherwise collapse Near before
		// the memory coordinator has had a chance to establish functional capacity.
		// Runtime-page validation includes the material source signature, and that
		// signature includes owner-dependent Terrain3D settings. Bind/initialize the
		// asset owner before the first cooked-page residency pass so startup never
		// validates a freshly cooked page against a provisional ownerless signature.
		m_assets->initialize( this );

		serviceMemoryBudgetPolicy( true );
		applyRuntimeAdaptiveWorkBudgets();
		normalizeRuntimeTierLayout( true );

		// Establish the authoritative residency map before the tier materials take
		// their initial sampler bindings. Physical GPU publication remains bounded
		// in authoring as well as runtime; CPU data and the far atlas remain the
		// whole-world authority. Force the first reconciliation even when the actual
		// center/generation equals the zero/default cached state.
		updateRegionStreamingPolicy( initial_target, 0.0, true );
		m_pData->serviceColdInstanceResidency( initial_target, true );
		m_pData->updateRegionResidency( initial_target, true );

		// Asset arrays are already initialized above so cooked-page source signatures
		// are authoritative before residency. Build the whole-world far atlases before
		// tier materials take their initial sampler bindings.
		m_pData->ensureFarAtlases();
		m_material->initialize( this );
		if ( !m_material->isRenderShaderReady() ) {
			LOG( ERROR,
			        "[lifecycle] Terrain material failed to produce a valid render shader; "
			        "aborting terrain activation before render meshes are created." );
			deactivateRuntime();
			return;
		}
		m_pCollision->initialize( this );
		m_pInstancer->m_nCellSize = m_nInstancerCellSize;
		m_pInstancer->m_bCellStreamingEnabled = effectiveInstancerCellStreamingEnabled();
		m_pInstancer->m_bSingleLodInstanceBuffers = m_bInstancerSingleLodBuffers;
		m_pInstancer->m_fCellStreamingMargin = m_fInstancerCellStreamingMargin;
		m_pInstancer->m_fCellStreamingStep = m_fInstancerCellStreamingUpdateDistance;
		m_pInstancer->m_nUpdateCellBudget = ( uint32_t )( MAX( m_nInstancerUpdateCellBudget, 0 ) );
		m_pInstancer->initialize( this );
		m_pNodeStreamer->initialize( this );
		m_pNodeStreamer->setEnabled( effectiveDynamicNodeStreamingEnabled() );
		m_pNodeStreamer->setRadiusRegions( m_nDynamicNodeStreamRadius );
		m_pNodeStreamer->setLoadRegionBudget( m_nDynamicNodeLoadRegionBudget );
		m_pNodeStreamer->setLoadNodeBudget( m_nDynamicNodeLoadNodeBudget );
		m_pNodeStreamer->setUnloadRegionBudget( m_nDynamicNodeUnloadRegionBudget );
		m_pLightmapStreamer->initialize( this );
		m_pLightmapStreamer->setResidencyBudgetMb( m_nEffectiveLightmapResidencyBudgetMb );
		m_pLightmapStreamer->setEnabled( m_bLightmapStreamingEnabled );
		m_pLightmapStreamer->setRadiusRegions( m_nLightmapStreamRadius );
		m_pLightmapStreamer->setLoadBudget( m_nLightmapLoadBudget );
		m_pLightmapStreamer->setUnloadBudget( m_nLightmapUnloadBudget );
		if ( m_hlodManifestPath.is_empty() && !m_dataDirectory.is_empty() ) {
			m_hlodManifestPath = m_dataDirectory.path_join( "terrain3d_hlod_manifest.tres" );
		}
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
		// Construct all CPU clipmaps against the same target used for residency, then
		// let the coordinator publish. This prevents its first snapshot from racing
		// an origin-centered/default mesher state. Ocean topology is also ready before
		// initializeOcean() runs.
		setupTerrainMesher( initial_target );
		setupOceanMesher( initial_target );
		setupRenderCoordinator();
		updateDisplacementBuffer();
		m_bInitialized = true;
		m_runtimeLifecycleState = TERRAIN3D_RUNTIME_ACTIVE;
		updateRegionLabels();
		// setupTerrainMesher() snaps during construction and setupRenderCoordinator()
		// immediately publishes that state. Re-synchronize once after the complete
		// activation sequence so a camera/target that became valid while the other
		// subsystems were initializing cannot leave the GPU with a stale origin-
		// centered snapshot. Do not merely invalidate the mesher here: that creates
		// a window where the CPU patch state is empty until a later physics tick.
		synchronizeClipmapNow( getClipmapTargetPosition() );
		LOG( INFO,
		        "[lifecycle] Main subsystems ready: regions=",
		        m_pData->getRegionCount(),
		        "; region_size=",
		        ( int )( s_nRegionSize ),
		        "; mesh_lods=",
		        m_nMeshLods,
		        "; tessellation=",
		        m_nTessellationLevel );
	}
	serviceMemoryBudgetPolicy( true );
	update_configuration_warnings();
}

void Terrain3D::deactivateRuntime( void ) {
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN );
	if ( m_runtimeLifecycleState == TERRAIN3D_RUNTIME_DEACTIVATING || m_runtimeLifecycleState == TERRAIN3D_RUNTIME_DETACHED ) {
		LOG( DEBUG, "[lifecycle] Ignoring duplicate runtime deactivation; state=", ( int )( m_runtimeLifecycleState ) );
		return;
	}
	if ( m_runtimeLifecycleState == TERRAIN3D_RUNTIME_SHUTTING_DOWN || m_runtimeLifecycleState == TERRAIN3D_RUNTIME_DRAIN_FAILED ) {
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
	destroyLightmapStreamer();
	destroyNodeStreamer( true );
	destroyInstancer();
	destroyCollision();
	destroyDisplacementBuffer();
	if ( m_pEditor ) {
		m_pEditor->releaseRuntimeAllocatorStorage();
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
 * This is a proxy for _process(delta) called by _notification() due to
 * https://github.com/godotengine/godot-cpp/issues/1022
 */
void Terrain3D::physicsProcess( const double fDelta ) {
	if ( !m_bInitialized ) {
		return;
	}
	serviceMemoryBudgetPolicy( false );
	if ( !m_hCamera.isValid() ) {
		LOG( DEBUG, "Camera is null, getting the current one" );
		grabCamera();
	}
	// Resolve the frame target before sealing allocation. Cold services use this
	// same position so materialized ownership and the following hot selection are
	// guaranteed to describe one residency frame.
	const Vector3 clipmap_target_position = getClipmapTargetPosition();
	updateRegionStreamingPolicy( clipmap_target_position, fDelta, false );
	if ( m_pData ) {
		m_pData->serviceColdInstanceResidency( clipmap_target_position, false );
	}
	if ( m_pInstancer ) {
		m_pInstancer->serviceColdCapacity();
	}
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->serviceColdRebuild();
	}
	if ( m_pCollision ) {
		m_pCollision->serviceColdRebuild();
	}
	if ( m_pRenderCoordinator ) {
		m_pRenderCoordinator->serviceColdCapacity();
	}
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_HOT_FRAME );
	if ( m_bComputeDisplacementFallbackRequested.exchange( false, T3D_MEMORY_ORDER_ACQ_REL ) ) {
		m_lastBufferPosition = s_v2Max;
	}

	t3dRuntimeProfileSample_t profile_sample;
	const bool bProfileFrame = m_bRuntimeProfilingEnabled;
	const bool bAdaptiveFrame = m_bRuntimeAdaptiveWorkBudgetsEnabled;
	const bool bMeasureFrame = bProfileFrame || bAdaptiveFrame;
	const uint64_t frame_started_usec = bMeasureFrame ? Time::get_singleton()->get_ticks_usec() : 0u;
	uint64_t phase_started_usec = frame_started_usec;
	m_nRuntimeAdaptiveControllerUsecLast = 0u;
	if ( bAdaptiveFrame ) {
		updateRuntimeAdaptiveWorkBudgets();
		const uint64_t adaptive_completed_usec = Time::get_singleton()->get_ticks_usec();
		m_nRuntimeAdaptiveControllerUsecLast = adaptive_completed_usec - frame_started_usec;
		if ( bProfileFrame ) {
			phase_started_usec = adaptive_completed_usec;
		}
	}

	// Meshing, displacement, streaming, and ocean snapping use the target that
	// was resolved for the cold residency phase above.
	++m_iRuntimePhysicsFrameIndex;
	const Vector2i target_region = m_pData ? m_pData->getRegionLocation( clipmap_target_position ) : s_v2iMax;
	if ( target_region != m_runtimeBoundaryTargetRegion ) {
		m_runtimeBoundaryTargetRegion = target_region;
		m_nRuntimeBoundaryTransitionFrame = m_iRuntimePhysicsFrameIndex;
	}
	const uint64_t boundary_age_frames = m_iRuntimePhysicsFrameIndex - m_nRuntimeBoundaryTransitionFrame;
	m_bRuntimeBoundaryNodeDeferredLastTick = t3dBoundaryShouldDefer( m_bRuntimeBoundaryWorkPhasingEnabled,
	        effectiveDynamicNodeStreamingEnabled(),
	        boundary_age_frames,
	        m_nRuntimeBoundaryNodeDelayFrames,
	        m_nRuntimeBoundaryNodeDeferralStreak );
	m_bRuntimeBoundaryInstancerDeferredLastTick = t3dBoundaryShouldDefer( m_bRuntimeBoundaryWorkPhasingEnabled,
	        effectiveInstancerCellStreamingEnabled(),
	        boundary_age_frames,
	        m_nRuntimeBoundaryInstancerDelayFrames,
	        m_nRuntimeBoundaryInstancerDeferralStreak );
	m_nRuntimeBoundaryNodeDeferralStreak = m_bRuntimeBoundaryNodeDeferredLastTick ? m_nRuntimeBoundaryNodeDeferralStreak + 1 : 0;
	m_nRuntimeBoundaryInstancerDeferralStreak =
	        m_bRuntimeBoundaryInstancerDeferredLastTick ? m_nRuntimeBoundaryInstancerDeferralStreak + 1 : 0;
	if ( bProfileFrame ) {
		profile_sample.m_fTargetX = ( float )( clipmap_target_position.x );
		profile_sample.m_fTargetZ = ( float )( clipmap_target_position.z );
		if ( m_nRuntimeProfileSampleCount > 0u ) {
			profile_sample.m_fMovementX = profile_sample.m_fTargetX - m_runtimeProfileLastSample.m_fTargetX;
			profile_sample.m_fMovementZ = profile_sample.m_fTargetZ - m_runtimeProfileLastSample.m_fTargetZ;
		}
		profile_sample.m_nTargetRegionX = target_region.x;
		profile_sample.m_nTargetRegionZ = target_region.y;
	}
	if ( m_pData ) {
		m_pData->updateRegionResidency( clipmap_target_position, false );
		if ( m_assets.is_valid() && m_assets->isRuntimeMaterialCacheActive() ) {
			uint8_t material_ids[s_nMaxTextureAssets];
			memset( material_ids, 0, sizeof( material_ids ) );
			const int material_count = m_pData->fillGpuMaterialPriorityIds( material_ids, s_nMaxTextureAssets );
			m_assets->updateRuntimeMaterialResidency( material_ids, material_count );
		}
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nDataResidencyUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	const bool hlod_active =
	        m_bHlodEnabled && ( !Engine::get_singleton()->is_editor_hint() || !isEditorAuthoringModeInternal() || m_bEditorHlodPreview );
	if ( m_pHlodStreamer ) {
		if ( hlod_active ) {
			m_pHlodStreamer->updateResidency( clipmap_target_position, false );
		} else {
			m_pHlodStreamer->suspend();
		}
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nHlodStreamingUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_pNodeStreamer && effectiveDynamicNodeStreamingEnabled() ) {
		if ( m_bRuntimeBoundaryNodeDeferredLastTick ) {
			m_pNodeStreamer->deferUpdateForFrame();
			++m_nRuntimeBoundaryNodeDeferralsTotal;
		} else {
			m_pNodeStreamer->updateResidency( clipmap_target_position, false );
		}
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nNodeStreamingUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_pLightmapStreamer && m_bLightmapStreamingEnabled ) {
		m_pLightmapStreamer->updateResidency( clipmap_target_position, false );
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nLightmapStreamingUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_nTessellationLevel > 0 ) {
		if ( m_pTerrainMesher && m_material.is_valid() ) {
			const Vector2 target_position_2d = t3dVector3ToVector2( clipmap_target_position );
			const real_t fTessellationDensity = 1.0f / ( real_t )( 1u << ( uint32_t )m_nTessellationLevel );
			const real_t fBufferVertexSpacing = s_fRegionVertexSpacing * fTessellationDensity;
			const real_t fUpdateDistance = MAX( fBufferVertexSpacing, m_fDisplacementUpdateDistance );
			const bool bMoved =
			        MAX( abs( m_lastBufferPosition.x - target_position_2d.x ), abs( m_lastBufferPosition.y - target_position_2d.y ) )
			        >= fUpdateDistance;
			const uint64_t nPageTableGeneration = m_pData ? m_pData->getPhysicalRegionMapGenerationInternal() : 0u;
			const bool bPageTableChanged = nPageTableGeneration != m_nComputeDisplacementPageTableGeneration;
			const bool bDisplacementDirty = m_bComputeDisplacementDirty.load( T3D_MEMORY_ORDER_ACQUIRE );

			// Geometry snapping and displacement servicing are separate lifecycles.
			// A failed displacement request must not force the mesh to resnap every
			// frame, but it must remain dirty so it is retried without camera motion.
			if ( bMoved ) {
				m_lastBufferPosition = target_position_2d;
				m_pTerrainMesher->snap( clipmap_target_position );
			}
			if ( bMoved || bPageTableChanged || bDisplacementDirty ) {
				if ( tryUpdateComputeDisplacement( clipmap_target_position ) ) {
					m_nComputeDisplacementPageTableGeneration = nPageTableGeneration;
					m_bComputeDisplacementDirty.store( false, T3D_MEMORY_ORDER_RELEASE );
				} else {
					m_bComputeDisplacementDirty.store( true, T3D_MEMORY_ORDER_RELEASE );
				}
			}
		}
	} else if ( m_pTerrainMesher ) {
		m_pTerrainMesher->snap( clipmap_target_position );
	}
	if ( m_pRenderCoordinator ) {
		m_pRenderCoordinator->update();
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nClipmapUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_bOceanEnabled && m_pOceanMesher ) {
		m_pOceanMesher->snap( clipmap_target_position );
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nOceanUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	const bool bInstanceCellStreaming = effectiveInstancerCellStreamingEnabled();
	const bool bInstanceTargetTracking = bInstanceCellStreaming || m_bInstancerSingleLodBuffers;
	if ( m_pInstancer && bInstanceTargetTracking ) {
		if ( bInstanceCellStreaming && m_bRuntimeBoundaryInstancerDeferredLastTick ) {
			m_pInstancer->deferStreamingSelectionForFrame();
			++m_nRuntimeBoundaryInstancerDeferralsTotal;
		} else {
			m_pInstancer->updateStreaming( clipmap_target_position );
		}
	}
	if ( m_pInstancer ) {
		m_pInstancer->servicePendingUpdates();
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nInstancerUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	syncLightShaderParameters();
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.m_nLightSyncUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_pCollision ) {
		m_pCollision->servicePendingRegionMapUpdate();
		if ( m_pCollision->isEnabled() ) {
			m_pCollision->update();
		}
	}
	if ( bMeasureFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		const uint64_t measured_usec = now - frame_started_usec;
		if ( bAdaptiveFrame ) {
			m_nRuntimeAdaptiveLastMeasuredUsec = measured_usec;
		}
		if ( bProfileFrame ) {
			profile_sample.m_nCollisionUsec = now - phase_started_usec;
			profile_sample.m_nPhysicsTotalUsec = measured_usec;
			profile_sample.m_nStreamingUsec = profile_sample.m_nDataResidencyUsec + profile_sample.m_nHlodStreamingUsec
			        + profile_sample.m_nNodeStreamingUsec + profile_sample.m_nLightmapStreamingUsec + profile_sample.m_nInstancerUsec;
			sampleRuntimeProfile( profile_sample );
		}
	}
}

/**
 * If running in the editor, grab the first editor viewport camera.
 * The edited_scene_root is excluded in case the user already has a Camera3D in their scene.
 */
void Terrain3D::grabCamera( void ) {
	if ( IS_EDITOR ) {
		EditorInterface *const pEditor = EditorInterface::get_singleton();
		SubViewport *const pViewport = pEditor ? pEditor->get_editor_viewport_3d( 0 ) : NULL;
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

void Terrain3D::buildContainers( void ) {
	if ( m_pLabelParent ) {
		return;
	}
	m_pLabelParent = memnew( Node3D );
	m_pLabelParent->set_name( "Labels" );
	add_child( m_pLabelParent, true );
}

void Terrain3D::destroyContainers( void ) {
	if ( m_pLabelParent ) {
		memdelete( m_pLabelParent );
		m_pLabelParent = NULL;
	}
}

const t3dBakeRegionSnapshot_t *t3dBakeSnapshot_t::getRegion( const int32_t iX, const int32_t iZ ) const {
	const Vector2i regionLocation = t3dRegionCoordinateFromGrid( Vector2i( iX, iZ ) );
	const int32_t map_index = Terrain3DData::getRegionMapIndex( regionLocation );
	if ( map_index < 0 ) {
		return NULL;
	}
	const int32_t region_index = m_szRegionLookup[map_index];
	if ( region_index < 0 || region_index >= ( int32_t )m_regions.size() ) {
		return NULL;
	}
	return &m_regions[( size_t )( region_index )];
}

void t3dBakeSnapshot_t::getSample( const int32_t iX, const int32_t iZ, uint32_t &nControl, real_t &fHeight ) const {
	const t3dBakeRegionSnapshot_t *pRegion = getRegion( iX, iZ );
	if ( !pRegion || !pRegion->m_pHeightBytes ) {
		nControl = UINT32_MAX;
		fHeight = NAN;
		return;
	}
	const int32_t local_x = t3dRegionLocalFromGrid( iX );
	const int32_t local_z = t3dRegionLocalFromGrid( iZ );
	const int32_t index = local_z * s_nRegionSize + local_x;
	fHeight = ( real_t )( t3dRegionCodecReadHeightBytes( pRegion->m_pHeightBytes, index, Image::FORMAT_R16 ) );
	if ( pRegion->m_pControlBytes ) {
		uint32_t control = s_nRegionDefaultControl;
		memcpy( &control, pRegion->m_pControlBytes + ( size_t )( index ) * sizeof( uint32_t ), sizeof( uint32_t ) );
		nControl = control;
	} else {
		nControl = s_nRegionDefaultControl;
	}
	return;
}

bool t3dBakeSnapshot_t::getMinimumRect(
        const int32_t iX, const int32_t iZ, const int32_t nWidth, const int32_t nHeight, real_t &fHeight ) const {
	if ( nWidth <= 0 || nHeight <= 0 ) {
		return false;
	}
	real_t fMinimum = eastl::numeric_limits<real_t>::infinity();
	for ( int32_t row = 0; row < nHeight; ++row ) {
		const int32_t global_z = iZ + row;
		int32_t copied = 0;
		while ( copied < nWidth ) {
			const int32_t global_x = iX + copied;
			const t3dBakeRegionSnapshot_t *pRegion = getRegion( global_x, global_z );
			if ( !pRegion || !pRegion->m_pHeightBytes ) {
				return false;
			}
			const int32_t local_x = t3dRegionLocalFromGrid( global_x );
			const int32_t local_z = t3dRegionLocalFromGrid( global_z );
			const int32_t run = MIN( nWidth - copied, s_nRegionSize - local_x );
			const int32_t index = local_z * s_nRegionSize + local_x;
			const uint16_t *pHeight = ( const uint16_t * )( pRegion->m_pHeightBytes ) + index;
			const uint32_t *pControl = pRegion->m_pControlBytes ? ( const uint32_t * )( pRegion->m_pControlBytes ) + index : NULL;
			for ( int32_t i = 0; i < run; ++i ) {
				const uint32_t sample_control = pControl ? pControl[i] : s_nRegionDefaultControl;
				if ( t3dIsHole( sample_control ) ) {
					return false;
				}
				fMinimum = MIN( fMinimum, ( real_t )( t3dRegionCodecDecodeHeightU16( pHeight[i] ) ) );
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
        const int32_t nLod, const t3dHeightFilter_t filter, const int32_t iX, const int32_t iZ, uint32_t &nControl ) const {
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	real_t fHeight = NAN;
	getSample( iX, iZ, nControl, fHeight );
	if ( t3dIsHole( nControl ) ) {
		fHeight = NAN;
	}

	if ( filter == HEIGHT_FILTER_MINIMUM && !getMinimumRect( iX - step / 2, iZ - step / 2, step, step, fHeight ) ) {
		fHeight = NAN;
	}

	return Vector3( ( real_t )( iX )*m_fVertexSpacing, fHeight, ( real_t )( iZ )*m_fVertexSpacing );
}

// Generates two triangles: Top 124, Bottom 143
//		1  __  2
//		  |\ |
//		  | \|
//		3  --  4
// p_require_nav is false for the runtime baker, which ignores navigation.
// -----------------------------------------------------------------------------
// Public functions

Terrain3D::Terrain3D( void ) {
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

Terrain3D::~Terrain3D( void ) {
	Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_SHUTDOWN );
	if ( m_pRuntimeProfileHistory ) {
		memdelete( m_pRuntimeProfileHistory );
		m_pRuntimeProfileHistory = NULL;
	}
	// PREDELETE owns the normal final teardown. Keep this as a last-resort fallback
	// for destruction paths that bypass the notification sequence.
	if ( m_bRuntimeMemoryReady && m_runtimeLifecycleState != TERRAIN3D_RUNTIME_DRAIN_FAILED && !shutdownRuntimeMemoryImage() ) {
		LOG( ERROR,
		        "[lifecycle] Terrain destroyed with live runtime allocator storage; live allocations=",
		        m_runtimeMemoryZone.liveAllocations() );
	}
}

void Terrain3D::setDebugLevel( const t3dDebugLevel_t level ) {
	SET_IF_DIFF( s_debugLevel, CLAMP( level, ERROR, EXTREME ) );
	LOG( INFO, "Setting debug level: ", s_debugLevel );
}

#define T3D_OCEAN_MODE_SETTER( NAME, TYPE, FIELD )                                                                                         \
	void Terrain3D::NAME( const TYPE valueArg ) {                                                                                          \
		if ( FIELD == valueArg ) {                                                                                                         \
			return;                                                                                                                        \
		}                                                                                                                                  \
		FIELD = valueArg;                                                                                                                  \
		m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM;                                                                                    \
		markOceanPublicationDirty();                                                                                                       \
		updateOceanShaderVariant();                                                                                                        \
	}

T3D_OCEAN_MODE_SETTER( setOceanRefractionMode, t3dOceanRefractionMode_t, m_oceanRefractionMode )
T3D_OCEAN_MODE_SETTER( setOceanFoamMode, t3dOceanFoamMode_t, m_oceanFoamMode )
T3D_OCEAN_MODE_SETTER( setOceanScatteringMode, t3dOceanScatteringMode_t, m_oceanScatteringMode )
T3D_OCEAN_MODE_SETTER( setOceanShadingMode, t3dOceanShadingMode_t, m_oceanShadingMode )
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
void Terrain3D::_notification( const int nWhat ) {
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
			// Reload editor textures - Also see READY
			if ( m_bFreeEditorTextures && !IS_EDITOR && m_assets.is_valid() && !m_assets->get_path().is_empty()
			        && !m_assets->get_path().contains( "Terrain3DAssets" ) ) {
				LOG( INFO, "free_editor_textures enabled, reloading Assets path: ", m_assets->get_path() );
				m_assets = ResourceLoader::get_singleton()->load( m_assets->get_path(), "", ResourceLoader::CACHE_MODE_IGNORE );
			}
			initializeInternal(); // Rebuild anything freed: meshes, collision, instancer
			set_physics_process( true );
			break;
		}

		case NOTIFICATION_READY: {
			// Node is ready
			LOG( INFO, "NOTIFICATION_READY" );
			// Clear editor textures - also see ENTER_TREE
			if ( m_bFreeEditorTextures && !IS_EDITOR && m_assets.is_valid() ) {
				if ( m_assets->get_path().contains( "Terrain3DAssets" ) ) {
					LOG( WARN,
					        "free_editor_textures requires `Assets` be saved to a file. Do so, or disable the former "
					        "to turn off this warning" );
				} else {
					LOG( INFO, "free_editor_textures enabled, clearing texture assets" );
					m_assets->clearTextures();
				}
			}
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
			deactivateRuntime();
			destroyMousePicking();
			break;
		}

		case NOTIFICATION_EXIT_WORLD: {
			// Node3D unregistered from current World3D
			// Sent on scene changes
			LOG( INFO, "NOTIFICATION_EXIT_WORLD" );
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
			if ( m_pEditor ) {
				m_pEditor->releaseRuntimeAllocatorStorage();
			}

			m_assets.unref();
			m_material.unref();
			if ( m_pData ) {
				if ( !m_pData->releaseRuntimeAllocatorStorageInternal() ) {
					LOG( ERROR, "[lifecycle] Terrain data retained allocator-backed storage during final teardown" );
				}
				memdelete( m_pData );
				m_pData = NULL;
			}
			destroyLabels();
			destroyContainers();
			if ( m_pRuntimeProfileHistory ) {
				memdelete( m_pRuntimeProfileHistory );
				m_pRuntimeProfileHistory = NULL;
			}

			if ( !shutdownRuntimeMemoryImage() ) {
				LOG( ERROR, "[lifecycle] Fixed runtime memory image still has live allocations after object-lifetime teardown" );
			}
			break;
		}

		default:
			break;
	}
}
