// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

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
#include "memory/terrain_3d_zone_allocator.h"
#include "rendering/terrain_3d_compute.h"
#include "simd/terrain_3d_parallel.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "streaming/terrain_3d_performance_profile.h"
#include "streaming/terrain_3d_streaming_policy.h"
#include "terrain_3d.h"

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

t3dAdaptiveBudgetLimits_t Terrain3D::getRuntimeAdaptiveBudgetLimits( void ) const {
	t3dAdaptiveBudgetLimits_t limits;
	limits.m_minimum.m_nGpuPages = m_nRuntimeAdaptiveMinGpuPages;
	limits.m_minimum.m_nCpuPages = m_nRuntimeAdaptiveMinCpuPages;
	limits.m_minimum.m_nInstancePages = m_nRuntimeAdaptiveMinInstancePages;
	limits.m_minimum.m_nInstancerCells = m_nRuntimeAdaptiveMinInstancerCells;
	limits.m_minimum.m_nNodeLoadRegions = m_nRuntimeAdaptiveMinNodeLoadRegions;
	limits.m_minimum.m_nNodeUnloadRegions = m_nRuntimeAdaptiveMinNodeUnloadRegions;
	limits.m_maximum.m_nGpuPages = m_nGpuRegionUploadBudgetLayers;
	limits.m_maximum.m_nCpuPages = m_nCpuRegionLoadBudgetLayers;
	limits.m_maximum.m_nInstancePages = m_nInstanceRegionLoadBudgetPages;
	limits.m_maximum.m_nInstancerCells = m_nInstancerUpdateCellBudget == 0 ? 65536 : m_nInstancerUpdateCellBudget;
	limits.m_maximum.m_nNodeLoadRegions = m_nDynamicNodeLoadRegionBudget;
	limits.m_maximum.m_nNodeUnloadRegions = m_nDynamicNodeUnloadRegionBudget;
	limits.m_nTargetUsec = m_nRuntimeAdaptiveTargetUsec;
	limits.m_nHardMaxUsec = m_nRuntimeAdaptiveHardMaxUsec;
	limits.m_nAdjustmentWindowFrames = m_nRuntimeAdaptiveAdjustmentWindowFrames;
	t3dAdaptiveSanitizeLimits( limits );
	return limits;
}

t3dAdaptiveBudgetPressure_t Terrain3D::getRuntimeAdaptiveBudgetPressure( void ) const {
	t3dAdaptiveBudgetPressure_t pressure;
	if ( m_pData ) {
		pressure.m_bGpuLimited = effectiveGpuRegionCacheEnabled()
		        && ( m_pData->getGpuRegionStallFlagsLastTick() & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET ) != 0u;
		pressure.m_bCpuLimited = effectiveCpuRegionCacheEnabled()
		        && ( m_pData->getCpuRegionStallFlagsLastTick() & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET ) != 0u;
		pressure.m_bInstanceLimited = effectiveInstanceRegionCacheEnabled()
		        && ( m_pData->getInstanceRegionStallFlagsLastTick() & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET ) != 0u;
	}
	pressure.m_bInstancerLimited =
	        effectiveInstancerCellStreamingEnabled() && m_pInstancer && m_pInstancer->isUpdateBudgetLimitedLastFrame();
	pressure.m_bNodeLoadLimited =
	        effectiveDynamicNodeStreamingEnabled() && m_pNodeStreamer && m_pNodeStreamer->isLoadRegionBudgetLimitedLastTick();
	pressure.m_bNodeUnloadLimited =
	        effectiveDynamicNodeStreamingEnabled() && m_pNodeStreamer && m_pNodeStreamer->isUnloadRegionBudgetLimitedLastTick();
	return pressure;
}

void Terrain3D::applyRuntimeAdaptiveWorkBudgets( void ) {
	if ( !m_bRuntimeAdaptiveWorkBudgetsEnabled ) {
		if ( m_pData ) {
			m_pData->clearRuntimePageBudgetOverrides();
		}
		if ( m_pInstancer ) {
			m_pInstancer->setRuntimeUpdateCellBudgetOverride( -1 );
		}
		if ( m_pNodeStreamer ) {
			m_pNodeStreamer->setRuntimeLoadRegionBudgetOverride( -1 );
			m_pNodeStreamer->setRuntimeUnloadRegionBudgetOverride( -1 );
		}
		return;
	}
	const t3dAdaptiveBudgetValues_t &effective = m_nRuntimeAdaptiveBudgetState.m_effective;
	if ( m_pData ) {
		m_pData->setRuntimePageBudgetOverrides( effective.m_nGpuPages, effective.m_nCpuPages, effective.m_nInstancePages );
	}
	if ( m_pInstancer ) {
		m_pInstancer->setRuntimeUpdateCellBudgetOverride( effective.m_nInstancerCells );
	}
	if ( m_pNodeStreamer && effectiveDynamicNodeStreamingEnabled() ) {
		m_pNodeStreamer->setRuntimeLoadRegionBudgetOverride( effective.m_nNodeLoadRegions );
		m_pNodeStreamer->setRuntimeUnloadRegionBudgetOverride( effective.m_nNodeUnloadRegions );
	}
}

void Terrain3D::resetRuntimeAdaptiveWorkBudgets( const bool bClearOverrides ) {
	const t3dAdaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
	t3dAdaptiveReset( limits,
	        m_nRuntimeAdaptiveBudgetState,
	        m_bRuntimeAdaptiveWorkBudgetsEnabled ? TERRAIN3D_ADAPTIVE_REASON_INITIALIZED : TERRAIN3D_ADAPTIVE_REASON_DISABLED );
	m_nRuntimeAdaptiveLastMeasuredUsec = 0u;
	if ( bClearOverrides || !m_bRuntimeAdaptiveWorkBudgetsEnabled ) {
		if ( m_pData ) {
			m_pData->clearRuntimePageBudgetOverrides();
		}
		if ( m_pInstancer ) {
			m_pInstancer->setRuntimeUpdateCellBudgetOverride( -1 );
		}
		if ( m_pNodeStreamer ) {
			m_pNodeStreamer->setRuntimeLoadRegionBudgetOverride( -1 );
			m_pNodeStreamer->setRuntimeUnloadRegionBudgetOverride( -1 );
		}
		return;
	}
	applyRuntimeAdaptiveWorkBudgets();
}

void Terrain3D::updateRuntimeAdaptiveWorkBudgets( void ) {
	if ( !m_bRuntimeAdaptiveWorkBudgetsEnabled ) {
		return;
	}
	const t3dAdaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
	const t3dAdaptiveBudgetPressure_t pressure = getRuntimeAdaptiveBudgetPressure();
	t3dAdaptiveStep( limits, pressure, m_nRuntimeAdaptiveLastMeasuredUsec, m_nRuntimeAdaptiveBudgetState );
	applyRuntimeAdaptiveWorkBudgets();
}

String Terrain3D::getRuntimeAdaptiveReasonName( void ) const {
	switch ( m_nRuntimeAdaptiveBudgetState.m_reason ) {
		case TERRAIN3D_ADAPTIVE_REASON_DISABLED:
			return "disabled";
		case TERRAIN3D_ADAPTIVE_REASON_INITIALIZED:
			return "initialized_at_configured_maxima";
		case TERRAIN3D_ADAPTIVE_REASON_HARD_LIMIT:
			return "terrain_time_above_hard_maximum";
		case TERRAIN3D_ADAPTIVE_REASON_BACKLOG_RECOVERY:
			return "frame_slack_reassigned_to_limited_subsystems";
		case TERRAIN3D_ADAPTIVE_REASON_UNDER_TARGET_NO_BACKLOG:
			return "under_target_without_budget_limited_backlog";
		case TERRAIN3D_ADAPTIVE_REASON_STABLE:
		default:
			return "inside_adjustment_window";
	}
}

void Terrain3D::sampleRuntimeProfile( const t3dRuntimeProfileSample_t &rSample ) {
	t3dRuntimeProfileSample_t sample = rSample;
	sample.m_nFrameSequence = ++m_nRuntimeProfileSampleCount;
	if ( m_pData ) {
		m_pData->appendRuntimeProfileSample( sample );
	}
	if ( m_pTerrainMesher ) {
		sample.m_nClipmapNearUpdates = ( int )( m_pTerrainMesher->getNearUpdatesLastSnap() );
		sample.m_nClipmapMiddleUpdates = ( int )( m_pTerrainMesher->getMiddleUpdatesLastSnap() );
		sample.m_nClipmapFarUpdates = ( int )( m_pTerrainMesher->getFarUpdatesLastSnap() );
		sample.m_nClipmapTeleports = ( int )( m_pTerrainMesher->getTeleportsLastSnap() );
		sample.m_nClipmapBufferUploads = ( int )( m_pTerrainMesher->getBufferUploadsLastSnap() );
	}
	if ( m_pInstancer ) {
		sample.m_nRenderCells = m_pInstancer->getRenderCellCount();
		sample.m_nPendingCellUpdates = m_pInstancer->getPendingStreamingUpdateCount();
	}
	if ( m_pNodeStreamer ) {
		sample.m_nNodeActiveRegions = m_pNodeStreamer->getActiveRegionCount();
		sample.m_nNodeActiveNodes = m_pNodeStreamer->getActiveNodeCount();
		sample.m_nNodeLoads = m_pNodeStreamer->getLoadsLastTick();
		sample.m_nNodeUnloads = m_pNodeStreamer->getUnloadsLastTick();
		sample.m_nNodeLoadFailures = m_pNodeStreamer->getSceneLoadFailuresLastTick();
	}
	if ( m_material.is_valid() ) {
		const uint64_t shader_updates = m_material->getShaderUpdateCount();
		const uint64_t shader_creations = m_material->getShaderVariantCreationCount();
		sample.m_nShaderUpdates =
		        ( int )( shader_updates >= m_nRuntimeProfileShaderUpdatesSeen ? shader_updates - m_nRuntimeProfileShaderUpdatesSeen
		                                                                      : shader_updates );
		sample.m_nShaderVariantCreations =
		        ( int )( shader_creations >= m_nRuntimeProfileShaderCreationsSeen ? shader_creations - m_nRuntimeProfileShaderCreationsSeen
		                                                                          : shader_creations );
		if ( sample.m_nShaderUpdates > 0 || sample.m_nShaderVariantCreations > 0 ) {
			sample.m_nShaderSourceSubmissionUsec = m_material->getShaderSourceSubmissionUsecLast();
		}
		if ( sample.m_nShaderUpdates > 0 ) {
			sample.m_nShaderUpdateUsec = m_material->getShaderUpdateUsecLast();
		}
		m_nRuntimeProfileShaderUpdatesSeen = shader_updates;
		m_nRuntimeProfileShaderCreationsSeen = shader_creations;
	}

	if ( sample.m_nGpuCameraMoved != 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_CAMERA_MOVED;
	}
	if ( sample.m_nGpuUploads > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_GPU_UPLOAD;
	}
	if ( sample.m_nGpuEvictions > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_GPU_EVICTION;
	}
	if ( sample.m_nGpuMissingRequired > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_GPU_MISSING_REQUIRED;
	}
	if ( sample.m_nGpuCookedMisses > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_COOKED_PAGE_MISS;
	}
	if ( sample.m_nGpuRuntimePrepares > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_RUNTIME_PAGE_PREPARE;
	}
	if ( sample.m_nClipmapTeleports > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_CLIPMAP_TELEPORT;
	}
	if ( sample.m_nNodeLoadFailures > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_NODE_LOAD_FAILURE;
	}
	if ( sample.m_nGpuFarRegionsProcessed > 0 || sample.m_nGpuFarDirtyRegions > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_FAR_ATLAS_WORK;
	}
	if ( sample.m_nCpuLoads > 0 && sample.m_nCpuSelectionPending != 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_CPU_DEMAND_LOAD;
	}
	if ( sample.m_nPendingCellUpdates > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_INSTANCE_BACKLOG;
	}
	if ( sample.m_nShaderUpdates > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_SHADER_UPDATE;
	}
	if ( sample.m_nShaderVariantCreations > 0 ) {
		sample.m_nEventFlags |= TERRAIN3D_PROFILE_EVENT_SHADER_VARIANT_CREATED;
	}
	if ( sample.m_nPendingCellUpdates > 0 && m_nInstancerUpdateCellBudget > 0
	        && sample.m_nPendingCellUpdates >= m_nInstancerUpdateCellBudget ) {
		sample.m_nStallFlags |= TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET;
	}
	if ( m_nDynamicNodeLoadRegionBudget > 0 && sample.m_nNodeLoads >= m_nDynamicNodeLoadRegionBudget ) {
		sample.m_nStallFlags |= TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET;
	}
	if ( m_nDynamicNodeUnloadRegionBudget > 0 && sample.m_nNodeUnloads >= m_nDynamicNodeUnloadRegionBudget ) {
		sample.m_nStallFlags |= TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET;
	}

	updateCacheThrashEvents( sample );
	m_runtimeProfileLastSample = sample;
	m_runtimeProfileAccumulator.addSample( sample );
	if ( !m_pRuntimeProfileHistory ) {
		m_pRuntimeProfileHistory = memnew( t3dRuntimeProfileHistory_t );
	}
	m_pRuntimeProfileHistory->push( sample );
}

void Terrain3D::updateCacheThrashEvents( const t3dRuntimeProfileSample_t &rSample ) {
	const bool bStates[3] = {
		rSample.m_nGpuMissingRequired > 0 || ( rSample.m_nGpuEvictions >= 2 && rSample.m_nGpuUploads >= 2 ),
		rSample.m_nCpuEvictions >= 2 && rSample.m_nCpuLoads >= 2,
		rSample.m_nInstanceEvictions >= 2 && rSample.m_nInstanceLoads >= 2,
	};
	const int64_t activity[3] = {
		( int64_t )( MAX( rSample.m_nGpuMissingRequired, rSample.m_nGpuEvictions ) ),
		( int64_t )( rSample.m_nCpuEvictions ),
		( int64_t )( rSample.m_nInstanceEvictions ),
	};
	for ( int index = 0; index < 3; ++index ) {
		if ( m_bCacheThrashEventState[( size_t )( index )] == bStates[index] ) {
			continue;
		}
		m_bCacheThrashEventState[( size_t )( index )] = bStates[index];
		emit_signal( "cache_thrash_state_changed", StringName( s_pszCacheThrashSubsystemNames[index] ), bStates[index], activity[index] );
	}
}

void Terrain3D::setRuntimeProfilingEnabled( const bool bEnabled ) {
	if ( bEnabled && !m_pRuntimeProfileHistory ) {
		m_pRuntimeProfileHistory = memnew( t3dRuntimeProfileHistory_t );
	}
	m_bRuntimeProfilingEnabled = bEnabled;
}

Dictionary Terrain3D::runtimeProfileSampleToDictionary( const t3dRuntimeProfileSample_t &rSample ) const {
	Dictionary sample;
	sample["sequence"] = ( int64_t )( rSample.m_nFrameSequence );
	sample["physics_total_usec"] = ( int64_t )( rSample.m_nPhysicsTotalUsec );
	sample["data_residency_usec"] = ( int64_t )( rSample.m_nDataResidencyUsec );
	sample["hlod_streaming_usec"] = ( int64_t )( rSample.m_nHlodStreamingUsec );
	sample["node_streaming_usec"] = ( int64_t )( rSample.m_nNodeStreamingUsec );
	sample["lightmap_streaming_usec"] = ( int64_t )( rSample.m_nLightmapStreamingUsec );
	sample["clipmap_usec"] = ( int64_t )( rSample.m_nClipmapUsec );
	sample["ocean_usec"] = ( int64_t )( rSample.m_nOceanUsec );
	sample["instancer_usec"] = ( int64_t )( rSample.m_nInstancerUsec );
	sample["light_sync_usec"] = ( int64_t )( rSample.m_nLightSyncUsec );
	sample["collision_usec"] = ( int64_t )( rSample.m_nCollisionUsec );
	sample["streaming_usec"] = ( int64_t )( rSample.m_nStreamingUsec );
	sample["target_position"] = Vector2( rSample.m_fTargetX, rSample.m_fTargetZ );
	sample["movement"] = Vector2( rSample.m_fMovementX, rSample.m_fMovementZ );
	sample["target_region"] = Vector2i( rSample.m_nTargetRegionX, rSample.m_nTargetRegionZ );
	sample["event_flags"] = ( int64_t )( rSample.m_nEventFlags );
	sample["stall_flags"] = ( int64_t )( rSample.m_nStallFlags );
	sample["events"] = getRuntimeProfileEventNames( rSample.m_nEventFlags );
	sample["stall_reasons"] = getRuntimeProfileStallNames( rSample.m_nStallFlags );
	sample["gpu_resident"] = rSample.m_nGpuResident;
	sample["gpu_capacity"] = rSample.m_nGpuCapacity;
	sample["gpu_uploads"] = rSample.m_nGpuUploads;
	sample["gpu_evictions"] = rSample.m_nGpuEvictions;
	sample["gpu_missing_required"] = rSample.m_nGpuMissingRequired;
	sample["gpu_upload_bytes"] = ( int64_t )( rSample.m_nGpuUploadBytes );
	sample["gpu_requested_pages"] = rSample.m_nGpuRequestedPages;
	sample["gpu_required_pages"] = rSample.m_nGpuRequiredPages;
	sample["gpu_guard_pages"] = rSample.m_nGpuGuardPages;
	sample["gpu_prefetch_pages"] = rSample.m_nGpuPrefetchPages;
	sample["gpu_hysteresis_pages"] = rSample.m_nGpuHysteresisPages;
	sample["gpu_prepared_queue"] = rSample.m_nGpuPreparedQueue;
	sample["gpu_prepare_pending_pages"] = rSample.m_nGpuPreparePendingPages;
	sample["gpu_prepare_job_active"] = rSample.m_nGpuPrepareJobActive != 0;
	sample["gpu_prepare_workers"] = rSample.m_nGpuPrepareWorkers;
	sample["gpu_prepare_latency_usec"] = ( int64_t )( rSample.m_nGpuPrepareLatencyUsec );
	sample["gpu_prepare_queue_wait_usec"] = ( int64_t )( rSample.m_nGpuPrepareQueueWaitUsec );
	sample["gpu_prepare_file_open_usec"] = ( int64_t )( rSample.m_nGpuPrepareFileOpenUsec );
	sample["gpu_prepare_file_read_usec"] = ( int64_t )( rSample.m_nGpuPrepareFileReadUsec );
	sample["gpu_prepare_validation_usec"] = ( int64_t )( rSample.m_nGpuPrepareValidationUsec );
	sample["gpu_prepare_materialize_usec"] = ( int64_t )( rSample.m_nGpuPrepareMaterializeUsec );
	sample["gpu_prepare_worker_usec"] = ( int64_t )( rSample.m_nGpuPrepareWorkerUsec );
	sample["gpu_prepare_ready_wait_usec"] = ( int64_t )( rSample.m_nGpuPrepareReadyWaitUsec );
	sample["gpu_prepare_publication_usec"] = ( int64_t )( rSample.m_nGpuPreparePublicationUsec );
	sample["gpu_prepare_end_to_end_usec"] = ( int64_t )( rSample.m_nGpuPrepareEndToEndUsec );
	sample["gpu_cache_service_usec"] = ( int64_t )( rSample.m_nGpuCacheServiceUsec );
	sample["gpu_effective_upload_limit"] = rSample.m_nGpuEffectiveUploadLimit;
	sample["gpu_cooked_hits"] = rSample.m_nGpuCookedHits;
	sample["gpu_cooked_misses"] = rSample.m_nGpuCookedMisses;
	sample["gpu_runtime_prepares"] = rSample.m_nGpuRuntimePrepares;
	sample["gpu_far_dirty_regions"] = rSample.m_nGpuFarDirtyRegions;
	sample["gpu_far_regions_processed"] = rSample.m_nGpuFarRegionsProcessed;
	sample["clipmap_near_updates"] = rSample.m_nClipmapNearUpdates;
	sample["clipmap_middle_updates"] = rSample.m_nClipmapMiddleUpdates;
	sample["clipmap_far_updates"] = rSample.m_nClipmapFarUpdates;
	sample["clipmap_teleports"] = rSample.m_nClipmapTeleports;
	sample["clipmap_buffer_uploads"] = rSample.m_nClipmapBufferUploads;
	sample["cpu_resident"] = rSample.m_nCpuResident;
	sample["cpu_desired"] = rSample.m_nCpuDesired;
	sample["cpu_loads"] = rSample.m_nCpuLoads;
	sample["cpu_evictions"] = rSample.m_nCpuEvictions;
	sample["cpu_load_bytes"] = ( int64_t )( rSample.m_nCpuLoadBytes );
	sample["cpu_cache_service_usec"] = ( int64_t )( rSample.m_nCpuCacheServiceUsec );
	sample["cpu_selection_pending"] = rSample.m_nCpuSelectionPending != 0;
	sample["cpu_pinned"] = rSample.m_nCpuPinned;
	sample["instance_resident"] = rSample.m_nInstanceResident;
	sample["instance_desired"] = rSample.m_nInstanceDesired;
	sample["instance_loads"] = rSample.m_nInstanceLoads;
	sample["instance_evictions"] = rSample.m_nInstanceEvictions;
	sample["instance_load_bytes"] = ( int64_t )( rSample.m_nInstanceLoadBytes );
	sample["instance_cache_service_usec"] = ( int64_t )( rSample.m_nInstanceCacheServiceUsec );
	sample["instance_selection_pending"] = rSample.m_nInstanceSelectionPending != 0;
	sample["render_cells"] = rSample.m_nRenderCells;
	sample["pending_cell_updates"] = rSample.m_nPendingCellUpdates;
	sample["node_active_regions"] = rSample.m_nNodeActiveRegions;
	sample["node_active_nodes"] = rSample.m_nNodeActiveNodes;
	sample["node_loads"] = rSample.m_nNodeLoads;
	sample["node_unloads"] = rSample.m_nNodeUnloads;
	sample["node_load_failures"] = rSample.m_nNodeLoadFailures;
	sample["shader_updates"] = rSample.m_nShaderUpdates;
	sample["shader_variant_creations"] = rSample.m_nShaderVariantCreations;
	sample["shader_source_submission_usec"] = ( int64_t )( rSample.m_nShaderSourceSubmissionUsec );
	sample["shader_update_usec"] = ( int64_t )( rSample.m_nShaderUpdateUsec );
	return sample;
}

Array Terrain3D::getRuntimeProfileEventNames( const uint32_t nFlags ) const {
	Array names;
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_CAMERA_MOVED ) != 0u ) {
		names.push_back( "camera_moved" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_GPU_UPLOAD ) != 0u ) {
		names.push_back( "gpu_upload" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_GPU_EVICTION ) != 0u ) {
		names.push_back( "gpu_eviction" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_GPU_MISSING_REQUIRED ) != 0u ) {
		names.push_back( "gpu_missing_required" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_COOKED_PAGE_MISS ) != 0u ) {
		names.push_back( "cooked_page_miss" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_RUNTIME_PAGE_PREPARE ) != 0u ) {
		names.push_back( "runtime_page_prepare" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_CLIPMAP_TELEPORT ) != 0u ) {
		names.push_back( "clipmap_teleport" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_NODE_LOAD_FAILURE ) != 0u ) {
		names.push_back( "node_load_failure" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_FAR_ATLAS_WORK ) != 0u ) {
		names.push_back( "far_atlas_work" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_CPU_DEMAND_LOAD ) != 0u ) {
		names.push_back( "cpu_demand_load" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_INSTANCE_BACKLOG ) != 0u ) {
		names.push_back( "instance_backlog" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_SHADER_UPDATE ) != 0u ) {
		names.push_back( "shader_update" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_SHADER_VARIANT_CREATED ) != 0u ) {
		names.push_back( "shader_variant_created" );
	}
	return names;
}

Array Terrain3D::getRuntimeProfileStallNames( const uint32_t nFlags ) const {
	Array names;
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET ) != 0u ) {
		names.push_back( "gpu_upload_layer_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING ) != 0u ) {
		names.push_back( "gpu_prepare_pending" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE ) != 0u ) {
		names.push_back( "gpu_no_ready_page" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY ) != 0u ) {
		names.push_back( "gpu_page_capacity" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET ) != 0u ) {
		names.push_back( "cpu_load_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET ) != 0u ) {
		names.push_back( "instance_load_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET ) != 0u ) {
		names.push_back( "instance_update_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET ) != 0u ) {
		names.push_back( "node_load_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET ) != 0u ) {
		names.push_back( "node_unload_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET ) != 0u ) {
		names.push_back( "gpu_upload_byte_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET ) != 0u ) {
		names.push_back( "gpu_update_call_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY ) != 0u ) {
		names.push_back( "gpu_retirement_capacity" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET ) != 0u ) {
		names.push_back( "cpu_load_byte_budget" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY ) != 0u ) {
		names.push_back( "cpu_pinned_capacity" );
	}
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET ) != 0u ) {
		names.push_back( "instance_load_byte_budget" );
	}
	return names;
}

Array Terrain3D::getRuntimeProfileProblems( void ) const {
	Array problems;
	const t3dRuntimeProfileSample_t &sample = m_runtimeProfileLastSample;
	if ( sample.m_nGpuMissingRequired > 0 ) {
		t3dAppendRuntimeProblem( problems,
		        "error",
		        "gpu_required_pages_missing",
		        "gpu_streaming",
		        "Required detailed terrain pages are not resident.",
		        sample.m_nGpuMissingRequired );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "gpu_upload_budget_saturated",
		        "gpu_streaming",
		        "GPU publication stopped at the current layer budget.",
		        sample.m_nGpuEffectiveUploadLimit );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "gpu_upload_byte_budget",
		        "gpu_streaming",
		        "GPU publication stopped at the configured byte budget.",
		        ( int64_t )( sample.m_nGpuUploadBytes ) );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "gpu_update_call_budget",
		        "gpu_streaming",
		        "GPU publication stopped at the texture-update call budget.",
		        sample.m_nGpuUploads );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "gpu_prepare_pending",
		        "gpu_streaming",
		        "A required page is still being prepared by the worker pool.",
		        sample.m_nGpuPreparePendingPages );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "gpu_no_ready_page",
		        "gpu_streaming",
		        "A required page had no prepared cooked payload ready for publication.",
		        sample.m_nGpuPreparedQueue );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "gpu_retirement_capacity",
		        "gpu_streaming",
		        "No staging layer is free because physical pages are mapped or retiring.",
		        sample.m_nGpuResident );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "error",
		        "gpu_page_capacity_exhausted",
		        "gpu_streaming",
		        "Mapped GPU page capacity is exhausted while required pages are missing.",
		        sample.m_nGpuCapacity );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "cpu_load_budget",
		        "cpu_streaming",
		        "CPU region loading stopped at the page budget.",
		        sample.m_nCpuLoads );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "cpu_load_byte_budget",
		        "cpu_streaming",
		        "CPU region loading stopped at the byte budget.",
		        ( int64_t )( sample.m_nCpuLoadBytes ) );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "cpu_pinned_capacity",
		        "cpu_streaming",
		        "CPU cache replacement could not find an evictable region slot.",
		        sample.m_nCpuPinned );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "instance_load_budget",
		        "instance_streaming",
		        "Instance page loading stopped at the page budget.",
		        sample.m_nInstanceLoads );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "instance_load_byte_budget",
		        "instance_streaming",
		        "Instance page loading stopped at the byte budget.",
		        ( int64_t )( sample.m_nInstanceLoadBytes ) );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "instance_update_budget",
		        "instancer",
		        "Instance cell publication remains queued at the per-frame cell budget.",
		        sample.m_nPendingCellUpdates );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "node_load_budget",
		        "node_streaming",
		        "Dynamic node loading reached the region budget.",
		        sample.m_nNodeLoads );
	}
	if ( ( sample.m_nStallFlags & TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "node_unload_budget",
		        "node_streaming",
		        "Dynamic node unloading reached the region budget.",
		        sample.m_nNodeUnloads );
	}
	if ( sample.m_nGpuCookedMisses > 0 ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "cooked_page_miss",
		        "gpu_streaming",
		        "Cooked runtime pages were unavailable and required fallback preparation.",
		        sample.m_nGpuCookedMisses );
	}
	if ( sample.m_nPendingCellUpdates > 0 ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "instance_cell_backlog",
		        "instancer",
		        "Instance cell updates remain queued after this tick.",
		        sample.m_nPendingCellUpdates );
	}
	if ( sample.m_nNodeLoadFailures > 0 ) {
		t3dAppendRuntimeProblem( problems,
		        "error",
		        "node_load_failure",
		        "node_streaming",
		        "One or more streamed node scenes failed to load.",
		        sample.m_nNodeLoadFailures );
	}
	if ( sample.m_nGpuFarDirtyRegions > 0 ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "far_atlas_backlog",
		        "far_atlas",
		        "Far-atlas region patches remain dirty.",
		        sample.m_nGpuFarDirtyRegions );
	}
	if ( sample.m_nShaderUpdates > 0 ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "shader_update",
		        "shaders",
		        "Terrain shader source was submitted or rebound during this sampled tick.",
		        ( int64_t )( sample.m_nShaderUpdateUsec ) );
	}
	if ( sample.m_nShaderVariantCreations > 0 ) {
		t3dAppendRuntimeProblem( problems,
		        "info",
		        "shader_variant_created",
		        "shaders",
		        "One or more shader variants were created during this sampled tick.",
		        sample.m_nShaderVariantCreations );
	}
	if ( sample.m_nPhysicsTotalUsec >= 2000u ) {
		t3dAppendRuntimeProblem( problems,
		        "warning",
		        "terrain_frame_over_2ms",
		        "frame",
		        "Terrain3D physics work exceeded 2 ms on the latest sampled tick.",
		        ( int64_t )( sample.m_nPhysicsTotalUsec ) );
	}
	return problems;
}

Array Terrain3D::getRuntimeProfileHistory( const int nMaxSamples ) const {
	Array history;
	if ( !m_pRuntimeProfileHistory ) {
		return history;
	}
	const size_t available = m_pRuntimeProfileHistory->m_nCount;
	const size_t requested = nMaxSamples <= 0 ? available : ( size_t )( MIN( nMaxSamples, ( int )( available ) ) );
	const size_t start = available - requested;
	history.resize( ( int )( requested ) );
	for ( size_t index = 0u; index < requested; ++index ) {
		history[( int )( index )] = runtimeProfileSampleToDictionary( m_pRuntimeProfileHistory->getOldest( start + index ) );
	}
	return history;
}

Array Terrain3D::getRuntimeProfileHistorySince( const int64_t nAfterSequence, const int nMaxSamples ) const {
	Array history;
	if ( !m_pRuntimeProfileHistory ) {
		return history;
	}
	const size_t available = m_pRuntimeProfileHistory->m_nCount;
	if ( available == 0u || nMaxSamples == 0 ) {
		return history;
	}
	size_t first_new = available;
	for ( size_t index = 0u; index < available; ++index ) {
		if ( ( int64_t )( m_pRuntimeProfileHistory->getOldest( index ).m_nFrameSequence ) > nAfterSequence ) {
			first_new = index;
			break;
		}
	}
	if ( first_new >= available ) {
		return history;
	}
	const size_t new_count = available - first_new;
	const size_t requested = nMaxSamples < 0 ? new_count : ( size_t )( MIN( nMaxSamples, ( int )( new_count ) ) );
	const size_t start = available - requested;
	history.resize( ( int )( requested ) );
	for ( size_t index = 0u; index < requested; ++index ) {
		history[( int )( index )] = runtimeProfileSampleToDictionary( m_pRuntimeProfileHistory->getOldest( start + index ) );
	}
	return history;
}

Dictionary Terrain3D::getRuntimeProfileAggregate( void ) const {
	Dictionary aggregate;
	const t3dRuntimeProfileAccumulator_t &stats = m_runtimeProfileAccumulator;
	aggregate["sampled_ticks"] = ( int64_t )( stats.m_nSampledTicks );

	Dictionary timing;
	Dictionary physics;
	physics["total_usec"] = ( int64_t )( stats.m_nPhysicsTotalUsec );
	physics["peak_usec"] = ( int64_t )( stats.m_nPhysicsPeakUsec );
	physics["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nPhysicsTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["physics"] = physics;
	Dictionary data_residency;
	data_residency["total_usec"] = ( int64_t )( stats.m_nDataResidencyTotalUsec );
	data_residency["peak_usec"] = ( int64_t )( stats.m_nDataResidencyPeakUsec );
	data_residency["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nDataResidencyTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["data_residency"] = data_residency;
	Dictionary hlod_streaming;
	hlod_streaming["total_usec"] = ( int64_t )( stats.m_nHlodStreamingTotalUsec );
	hlod_streaming["peak_usec"] = ( int64_t )( stats.m_nHlodStreamingPeakUsec );
	hlod_streaming["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nHlodStreamingTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["hlod_streaming"] = hlod_streaming;
	Dictionary node_streaming;
	node_streaming["total_usec"] = ( int64_t )( stats.m_nNodeStreamingTotalUsec );
	node_streaming["peak_usec"] = ( int64_t )( stats.m_nNodeStreamingPeakUsec );
	node_streaming["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nNodeStreamingTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["node_streaming"] = node_streaming;
	Dictionary clipmap_timing;
	clipmap_timing["total_usec"] = ( int64_t )( stats.m_nClipmapTotalUsec );
	clipmap_timing["peak_usec"] = ( int64_t )( stats.m_nClipmapPeakUsec );
	clipmap_timing["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nClipmapTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["clipmap"] = clipmap_timing;
	Dictionary ocean;
	ocean["total_usec"] = ( int64_t )( stats.m_nOceanTotalUsec );
	ocean["peak_usec"] = ( int64_t )( stats.m_nOceanPeakUsec );
	ocean["average_usec"] = stats.m_nSampledTicks > 0u ? ( double )( stats.m_nOceanTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["ocean"] = ocean;
	Dictionary instancer_timing;
	instancer_timing["total_usec"] = ( int64_t )( stats.m_nInstancerTotalUsec );
	instancer_timing["peak_usec"] = ( int64_t )( stats.m_nInstancerPeakUsec );
	instancer_timing["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nInstancerTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["instancer"] = instancer_timing;
	Dictionary light_sync;
	light_sync["total_usec"] = ( int64_t )( stats.m_nLightSyncTotalUsec );
	light_sync["peak_usec"] = ( int64_t )( stats.m_nLightSyncPeakUsec );
	light_sync["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nLightSyncTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["light_sync"] = light_sync;
	Dictionary collision;
	collision["total_usec"] = ( int64_t )( stats.m_nCollisionTotalUsec );
	collision["peak_usec"] = ( int64_t )( stats.m_nCollisionPeakUsec );
	collision["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nCollisionTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["collision"] = collision;
	Dictionary streaming;
	streaming["total_usec"] = ( int64_t )( stats.m_nStreamingTotalUsec );
	streaming["peak_usec"] = ( int64_t )( stats.m_nStreamingPeakUsec );
	streaming["average_usec"] =
	        stats.m_nSampledTicks > 0u ? ( double )( stats.m_nStreamingTotalUsec ) / ( double )( stats.m_nSampledTicks ) : 0.0;
	timing["streaming"] = streaming;
	aggregate["timing"] = timing;

	Dictionary gpu;
	gpu["uploads"] = ( int64_t )( stats.m_nGpuUploadsTotal );
	gpu["evictions"] = ( int64_t )( stats.m_nGpuEvictionsTotal );
	gpu["upload_bytes"] = ( int64_t )( stats.m_nGpuUploadBytesTotal );
	gpu["missing_required"] = ( int64_t )( stats.m_nGpuMissingRequiredTotal );
	gpu["resident_peak"] = stats.m_nGpuResidentPeak;
	gpu["prepared_pages"] = ( int64_t )( stats.m_nGpuPreparedPagesTotal );
	gpu["prepare_workers_peak"] = stats.m_nGpuPrepareWorkersPeak;
	gpu["effective_upload_limit_peak"] = stats.m_nGpuEffectiveUploadLimitPeak;
	gpu["camera_move_ticks"] = ( int64_t )( stats.m_nGpuCameraMoveTicks );
	gpu["distance_travelled_m"] = stats.m_fDistanceTravelledM;
	gpu["cooked_hits"] = ( int64_t )( stats.m_nGpuCookedHitsTotal );
	gpu["cooked_misses"] = ( int64_t )( stats.m_nGpuCookedMissesTotal );
	gpu["runtime_prepares"] = ( int64_t )( stats.m_nGpuRuntimePreparesTotal );
	const uint64_t cooked_attempts = stats.m_nGpuCookedHitsTotal + stats.m_nGpuCookedMissesTotal;
	gpu["cooked_hit_ratio"] = cooked_attempts > 0u ? ( double )( stats.m_nGpuCookedHitsTotal ) / ( double )( cooked_attempts ) : -1.0;
	gpu["upload_bytes_per_m"] =
	        stats.m_fDistanceTravelledM > 0.0 ? ( double )( stats.m_nGpuUploadBytesTotal ) / stats.m_fDistanceTravelledM : 0.0;
	gpu["evictions_per_km"] =
	        stats.m_fDistanceTravelledM > 0.0 ? ( double )( stats.m_nGpuEvictionsTotal ) * 1000.0 / stats.m_fDistanceTravelledM : 0.0;
	aggregate["gpu"] = gpu;

	Dictionary clipmap;
	clipmap["near_updates"] = ( int64_t )( stats.m_nClipmapNearUpdatesTotal );
	clipmap["middle_updates"] = ( int64_t )( stats.m_nClipmapMiddleUpdatesTotal );
	clipmap["far_updates"] = ( int64_t )( stats.m_nClipmapFarUpdatesTotal );
	clipmap["teleports"] = ( int64_t )( stats.m_nClipmapTeleportsTotal );
	clipmap["teleports_peak"] = stats.m_nClipmapTeleportsPeak;
	clipmap["buffer_uploads"] = ( int64_t )( stats.m_nClipmapBufferUploadsTotal );
	clipmap["buffer_uploads_peak"] = stats.m_nClipmapBufferUploadsPeak;
	aggregate["clipmap"] = clipmap;

	Dictionary cpu;
	cpu["loads"] = ( int64_t )( stats.m_nCpuLoadsTotal );
	cpu["evictions"] = ( int64_t )( stats.m_nCpuEvictionsTotal );
	cpu["load_bytes"] = ( int64_t )( stats.m_nCpuLoadBytesTotal );
	cpu["resident_peak"] = stats.m_nCpuResidentPeak;
	aggregate["cpu"] = cpu;

	Dictionary instances;
	instances["loads"] = ( int64_t )( stats.m_nInstanceLoadsTotal );
	instances["evictions"] = ( int64_t )( stats.m_nInstanceEvictionsTotal );
	instances["load_bytes"] = ( int64_t )( stats.m_nInstanceLoadBytesTotal );
	instances["resident_peak"] = stats.m_nInstanceResidentPeak;
	instances["render_cells_peak"] = stats.m_nRenderCellsPeak;
	instances["pending_cell_updates_peak"] = stats.m_nPendingCellUpdatesPeak;
	aggregate["instances"] = instances;

	Dictionary nodes;
	nodes["loads"] = ( int64_t )( stats.m_nNodeLoadsTotal );
	nodes["unloads"] = ( int64_t )( stats.m_nNodeUnloadsTotal );
	nodes["load_failures"] = ( int64_t )( stats.m_nNodeLoadFailuresTotal );
	nodes["active_regions_peak"] = stats.m_nNodeActiveRegionsPeak;
	nodes["active_nodes_peak"] = stats.m_nNodeActiveNodesPeak;
	aggregate["nodes"] = nodes;
	return aggregate;
}

void Terrain3D::setRuntimeAdaptiveWorkBudgetsEnabled( const bool bEnabled ) {
	if ( m_bRuntimeAdaptiveWorkBudgetsEnabled == bEnabled ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_bRuntimeAdaptiveWorkBudgetsEnabled = bEnabled;
	resetRuntimeAdaptiveWorkBudgets( !bEnabled );
	notify_property_list_changed();
}

void Terrain3D::setRuntimeAdaptiveTargetMs( const real_t fTargetMs ) {
	const uint64_t target_usec = ( uint64_t )( CLAMP( fTargetMs, ( real_t )( 0.05f ), ( real_t )( 50.0f ) ) * 1000.0f );
	if ( m_nRuntimeAdaptiveTargetUsec == target_usec ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveTargetUsec = target_usec;
	m_nRuntimeAdaptiveHardMaxUsec = MAX( m_nRuntimeAdaptiveHardMaxUsec, m_nRuntimeAdaptiveTargetUsec );
	m_nRuntimeAdaptiveBudgetState.m_nOverBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.m_nUnderBudgetStreak = 0;
}

void Terrain3D::setRuntimeAdaptiveHardMaxMs( const real_t fHardMaxMs ) {
	const uint64_t hard_max_usec = ( uint64_t )( CLAMP( fHardMaxMs, ( real_t )( 0.05f ), ( real_t )( 100.0f ) ) * 1000.0f );
	const uint64_t normalized = MAX( hard_max_usec, m_nRuntimeAdaptiveTargetUsec );
	if ( m_nRuntimeAdaptiveHardMaxUsec == normalized ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveHardMaxUsec = normalized;
	m_nRuntimeAdaptiveBudgetState.m_nOverBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.m_nUnderBudgetStreak = 0;
}

void Terrain3D::setRuntimeAdaptiveAdjustmentWindowFrames( const int nFrames ) {
	const int frames = CLAMP( nFrames, 1, 120 );
	if ( m_nRuntimeAdaptiveAdjustmentWindowFrames == frames ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveAdjustmentWindowFrames = frames;
	m_nRuntimeAdaptiveBudgetState.m_nOverBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.m_nUnderBudgetStreak = 0;
}

void Terrain3D::setRuntimeAdaptiveMinGpuPages( const int nPages ) {
	const int pages = CLAMP( nPages, 0, 64 );
	if ( m_nRuntimeAdaptiveMinGpuPages == pages ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinGpuPages = pages;
}

void Terrain3D::setRuntimeAdaptiveMinCpuPages( const int nPages ) {
	const int pages = CLAMP( nPages, 0, 64 );
	if ( m_nRuntimeAdaptiveMinCpuPages == pages ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinCpuPages = pages;
}

void Terrain3D::setRuntimeAdaptiveMinInstancePages( const int nPages ) {
	const int pages = CLAMP( nPages, 0, 64 );
	if ( m_nRuntimeAdaptiveMinInstancePages == pages ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinInstancePages = pages;
}

void Terrain3D::setRuntimeAdaptiveMinInstancerCells( const int nCells ) {
	const int cells = CLAMP( nCells, 0, 65536 );
	if ( m_nRuntimeAdaptiveMinInstancerCells == cells ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinInstancerCells = cells;
}

void Terrain3D::setRuntimeAdaptiveMinNodeLoadRegions( const int nRegions ) {
	const int regions = CLAMP( nRegions, 0, 64 );
	if ( m_nRuntimeAdaptiveMinNodeLoadRegions == regions ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinNodeLoadRegions = regions;
}

void Terrain3D::setRuntimeAdaptiveMinNodeUnloadRegions( const int nRegions ) {
	const int regions = CLAMP( nRegions, 0, 64 );
	if ( m_nRuntimeAdaptiveMinNodeUnloadRegions == regions ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinNodeUnloadRegions = regions;
}

void Terrain3D::setRuntimeBoundaryWorkPhasingEnabled( const bool bEnabled ) {
	if ( m_bRuntimeBoundaryWorkPhasingEnabled == bEnabled ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_bRuntimeBoundaryWorkPhasingEnabled = bEnabled;
	m_bRuntimeBoundaryNodeDeferredLastTick = false;
	m_bRuntimeBoundaryInstancerDeferredLastTick = false;
	m_nRuntimeBoundaryNodeDeferralStreak = 0;
	m_nRuntimeBoundaryInstancerDeferralStreak = 0;
}

void Terrain3D::setRuntimeBoundaryNodeDelayFrames( const int nFrames ) {
	const int frames = CLAMP( nFrames, 0, 8 );
	if ( m_nRuntimeBoundaryNodeDelayFrames == frames ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeBoundaryNodeDelayFrames = frames;
}

void Terrain3D::setRuntimeBoundaryInstancerDelayFrames( const int nFrames ) {
	const int frames = CLAMP( nFrames, 0, 8 );
	if ( m_nRuntimeBoundaryInstancerDelayFrames == frames ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeBoundaryInstancerDelayFrames = frames;
}

Dictionary Terrain3D::getRuntimeBoundaryPhasingStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bRuntimeBoundaryWorkPhasingEnabled;
	stats["target_region"] = m_runtimeBoundaryTargetRegion;
	stats["frames_since_boundary"] = ( int64_t )( m_iRuntimePhysicsFrameIndex - m_nRuntimeBoundaryTransitionFrame );
	stats["node_delay_frames"] = m_nRuntimeBoundaryNodeDelayFrames;
	stats["instancer_delay_frames"] = m_nRuntimeBoundaryInstancerDelayFrames;
	stats["node_deferred_last_tick"] = m_bRuntimeBoundaryNodeDeferredLastTick;
	stats["instancer_deferred_last_tick"] = m_bRuntimeBoundaryInstancerDeferredLastTick;
	stats["node_deferral_streak"] = m_nRuntimeBoundaryNodeDeferralStreak;
	stats["instancer_deferral_streak"] = m_nRuntimeBoundaryInstancerDeferralStreak;
	stats["node_deferrals_total"] = ( int64_t )( m_nRuntimeBoundaryNodeDeferralsTotal );
	stats["instancer_deferrals_total"] = ( int64_t )( m_nRuntimeBoundaryInstancerDeferralsTotal );
	return stats;
}

Dictionary Terrain3D::getRuntimeAdaptiveBudgetStats( void ) const {
	const t3dAdaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
	const t3dAdaptiveBudgetPressure_t pressure = getRuntimeAdaptiveBudgetPressure();
	Dictionary stats;
	stats["enabled"] = m_bRuntimeAdaptiveWorkBudgetsEnabled;
	stats["target_usec"] = ( int64_t )( limits.m_nTargetUsec );
	stats["hard_max_usec"] = ( int64_t )( limits.m_nHardMaxUsec );
	stats["last_measured_usec"] = ( int64_t )( m_nRuntimeAdaptiveLastMeasuredUsec );
	stats["controller_usec_last"] = ( int64_t )( m_nRuntimeAdaptiveControllerUsecLast );
	stats["adjustment_window_frames"] = limits.m_nAdjustmentWindowFrames;
	stats["over_budget_streak"] = m_nRuntimeAdaptiveBudgetState.m_nOverBudgetStreak;
	stats["under_budget_streak"] = m_nRuntimeAdaptiveBudgetState.m_nUnderBudgetStreak;
	stats["adjustment_count"] = ( int64_t )( m_nRuntimeAdaptiveBudgetState.m_nAdjustmentCount );
	stats["reason"] = getRuntimeAdaptiveReasonName();

	Dictionary configured;
	configured["gpu_pages"] = limits.m_maximum.m_nGpuPages;
	configured["cpu_pages"] = limits.m_maximum.m_nCpuPages;
	configured["instance_pages"] = limits.m_maximum.m_nInstancePages;
	configured["instancer_cells"] = limits.m_maximum.m_nInstancerCells;
	configured["node_load_regions"] = limits.m_maximum.m_nNodeLoadRegions;
	configured["node_unload_regions"] = limits.m_maximum.m_nNodeUnloadRegions;
	stats["configured_maximum"] = configured;

	Dictionary requested_minimum;
	requested_minimum["gpu_pages"] = m_nRuntimeAdaptiveMinGpuPages;
	requested_minimum["cpu_pages"] = m_nRuntimeAdaptiveMinCpuPages;
	requested_minimum["instance_pages"] = m_nRuntimeAdaptiveMinInstancePages;
	requested_minimum["instancer_cells"] = m_nRuntimeAdaptiveMinInstancerCells;
	requested_minimum["node_load_regions"] = m_nRuntimeAdaptiveMinNodeLoadRegions;
	requested_minimum["node_unload_regions"] = m_nRuntimeAdaptiveMinNodeUnloadRegions;
	stats["requested_minimum"] = requested_minimum;

	Dictionary minimum;
	minimum["gpu_pages"] = limits.m_minimum.m_nGpuPages;
	minimum["cpu_pages"] = limits.m_minimum.m_nCpuPages;
	minimum["instance_pages"] = limits.m_minimum.m_nInstancePages;
	minimum["instancer_cells"] = limits.m_minimum.m_nInstancerCells;
	minimum["node_load_regions"] = limits.m_minimum.m_nNodeLoadRegions;
	minimum["node_unload_regions"] = limits.m_minimum.m_nNodeUnloadRegions;
	stats["minimum"] = minimum;

	t3dAdaptiveBudgetState_t displayed_state = m_nRuntimeAdaptiveBudgetState;
	if ( !m_bRuntimeAdaptiveWorkBudgetsEnabled ) {
		displayed_state.m_effective = limits.m_maximum;
	} else {
		t3dAdaptiveClampState( limits, displayed_state );
	}
	Dictionary effective;
	effective["gpu_pages"] = displayed_state.m_effective.m_nGpuPages;
	effective["cpu_pages"] = displayed_state.m_effective.m_nCpuPages;
	effective["instance_pages"] = displayed_state.m_effective.m_nInstancePages;
	effective["instancer_cells"] = displayed_state.m_effective.m_nInstancerCells;
	effective["node_load_regions"] = displayed_state.m_effective.m_nNodeLoadRegions;
	effective["node_unload_regions"] = displayed_state.m_effective.m_nNodeUnloadRegions;
	stats["effective"] = effective;

	Dictionary limited;
	limited["gpu"] = pressure.m_bGpuLimited;
	limited["cpu"] = pressure.m_bCpuLimited;
	limited["instance"] = pressure.m_bInstanceLimited;
	limited["instancer"] = pressure.m_bInstancerLimited;
	limited["node_load"] = pressure.m_bNodeLoadLimited;
	limited["node_unload"] = pressure.m_bNodeUnloadLimited;
	stats["limited"] = limited;
	return stats;
}

void Terrain3D::setPerformanceTargetStorage( const t3dPerformanceTarget_t target ) {
	if ( target < PERFORMANCE_TARGET_CUSTOM || target > PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ) {
		return;
	}
	m_performanceTarget = target;
	if ( m_bInitialized ) {
		refreshPerformanceProfileConformity();
		notify_property_list_changed();
	}
}

void Terrain3D::setPerformanceTarget( const t3dPerformanceTarget_t target ) {
	if ( target < PERFORMANCE_TARGET_CUSTOM || target > PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ) {
		return;
	}
	if ( target == PERFORMANCE_TARGET_CUSTOM ) {
		m_performanceTarget = PERFORMANCE_TARGET_CUSTOM;
		m_bPerformanceProfileMaterialPendingApply = false;
		refreshPerformanceProfileConformity();
		notify_property_list_changed();
		return;
	}

	// Reapplying the selected base profile is the reset-to-profile operation.
	// Every owned value comes from the authoritative native manifest.
	m_bApplyingPerformanceTarget = true;
	m_performanceTarget = target;
	m_bPerformanceProfileMaterialPendingApply = false;
	m_bDeferGpuRegionCacheReconfigure = true;
	applyPerformanceProfileToTerrain();
	resetRuntimeAdaptiveWorkBudgets( true );
	normalizeRuntimeTierLayout( true );
	if ( m_material.is_valid() ) {
		m_material->setShaderPerformancePreset( SHADER_PRESET_XBOX_360_AUTHENTIC );
	} else {
		m_bPerformanceProfileMaterialPendingApply = true;
	}
	m_bDeferGpuRegionCacheReconfigure = false;
	serviceMemoryBudgetPolicy( true );
	applyStreamingPolicy( true );
	normalizeRuntimeTierLayout( true );
	if ( m_bInitialized ) {
		setupTerrainMesher();
	}
	m_bApplyingPerformanceTarget = false;
	if ( s_nRegionSize != SIZE_128 ) {
		m_performanceTarget = PERFORMANCE_TARGET_CUSTOM;
		m_bPerformanceProfileMaterialPendingApply = false;
		LOG( ERROR, "Xbox 360 performance target requires 128-sample regions at 1 metre vertex spacing" );
	}
	refreshPerformanceProfileConformity();
	notify_property_list_changed();
}

String Terrain3D::getPerformanceProfileStatusName( void ) const {
	switch ( m_performanceProfileStatus ) {
		case PERFORMANCE_PROFILE_EXACT:
			return "Exact";
		case PERFORMANCE_PROFILE_MODIFIED:
			return "Modified";
		case PERFORMANCE_PROFILE_CUSTOM:
		default:
			return "Custom";
	}
}

int Terrain3D::getPerformanceProfileVersion( void ) const {
	return ( int )( s_nProfileVersion );
}

String Terrain3D::getPerformanceProfileSignature( void ) const {
	return String::num_uint64( t3dPerformanceProfileSignature(), 16, true );
}

Dictionary Terrain3D::getPerformanceProfileManifest( void ) const {
	Dictionary manifest;
	manifest["key"] = s_pszProfileKey;
	manifest["display_name"] = s_pszProfileDisplayName;
	manifest["version"] = ( int )( s_nProfileVersion );
	manifest["signature"] = getPerformanceProfileSignature();
	manifest["target"] = ( int )( PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL );
	Array settings;
	settings.resize( ( int )( t3dPerformanceProfileSettingCount() ) );
	for ( size_t i = 0; i < t3dPerformanceProfileSettingCount(); ++i ) {
		const t3dPerformanceProfileSetting_t &setting = g_szXbox360ExperimentalSettings[i];
		Dictionary row;
		row["owner"] = t3dPerformanceProfileOwnerName( setting.m_owner );
		row["property"] = setting.m_pszProperty;
		row["key"] = t3dPerformanceProfileOwnerName( setting.m_owner ) + String( "." ) + String( setting.m_pszProperty );
		row["value"] = t3dPerformanceProfileValue( setting );
		settings[( int )( i )] = row;
	}
	manifest["settings"] = settings;
	return manifest;
}

Array Terrain3D::getPerformanceProfileDiff( void ) const {
	Array differences;
	if ( m_performanceTarget != PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ) {
		return differences;
	}
	for ( size_t iSetting = 0u; iSetting < T3D_ARRAY_COUNT( g_szXbox360ExperimentalSettings ); iSetting++ ) {
		const t3dPerformanceProfileSetting_t &setting = ( g_szXbox360ExperimentalSettings )[iSetting];
		const Object *pOwner = this;
		if ( setting.m_owner == T3D_PROFILE_OWNER_MATERIAL ) {
			if ( m_material.is_null() ) {
				Dictionary row;
				row["owner"] = "Terrain3DMaterial";
				row["property"] = setting.m_pszProperty;
				row["key"] = String( "Terrain3DMaterial." ) + setting.m_pszProperty;
				row["current"] = Variant();
				row["expected"] = t3dPerformanceProfileValue( setting );
				differences.push_back( row );
				continue;
			}
			pOwner = m_material.ptr();
		}
		const Variant current = pOwner->get( StringName( setting.m_pszProperty ) );
		if ( t3dPerformanceProfileValueMatches( current, setting ) ) {
			continue;
		}
		Dictionary row;
		row["owner"] = t3dPerformanceProfileOwnerName( setting.m_owner );
		row["property"] = setting.m_pszProperty;
		row["key"] = t3dPerformanceProfileOwnerName( setting.m_owner ) + String( "." ) + String( setting.m_pszProperty );
		row["current"] = current;
		row["expected"] = t3dPerformanceProfileValue( setting );
		differences.push_back( row );
	}
	return differences;
}

void Terrain3D::resetPerformanceProfile( void ) {
	if ( m_performanceTarget != PERFORMANCE_TARGET_CUSTOM ) {
		setPerformanceTarget( m_performanceTarget );
	}
}
