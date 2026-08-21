// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
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

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

#include "eastl_config.h"
#include "core/logger.h"
#include "terrain_3d.h"
#include "core/terrain_3d_integration_helpers.h"
#include "rendering/terrain_3d_compute.h"
#include "authoring/terrain_3d_editor.h"
#include "memory/terrain_3d_linear_arena.h"
#include "memory/terrain_3d_zone_allocator.h"
#include "lighting/terrain_3d_lightmap_streamer.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "lighting/terrain_3d_lighting_policy.h"
#include "streaming/terrain_3d_node_streamer.h"
#include "simd/terrain_3d_parallel.h"
#include "streaming/terrain_3d_performance_profile.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "streaming/terrain_3d_streaming_policy.h"
#include "streaming/terrain_3d_upload_bridge.h"
#include "core/terrain_3d_util.h"


t3dAdaptiveBudgetLimits_t T3DTerrain::getRuntimeAdaptiveBudgetLimits( void ) const {
	t3dAdaptiveBudgetLimits_t limits;
	limits.minimum.nGpuPages = m_nRuntimeAdaptiveMinGpuPages;
	limits.minimum.nCpuPages = m_nRuntimeAdaptiveMinCpuPages;
	limits.minimum.nInstancePages = m_nRuntimeAdaptiveMinInstancePages;
	limits.minimum.instancerCells = m_nRuntimeAdaptiveMinInstancerCells;
	limits.minimum.nNodeLoadRegions = m_nRuntimeAdaptiveMinNodeLoadRegions;
	limits.minimum.nNodeUnloadRegions = m_nRuntimeAdaptiveMinNodeUnloadRegions;
	limits.maximum.nGpuPages = m_nGpuRegionUploadBudgetLayers;
	limits.maximum.nCpuPages = m_nCpuRegionLoadBudgetLayers;
	limits.maximum.nInstancePages = m_nInstanceRegionLoadBudgetPages;
	limits.maximum.instancerCells = m_nInstancerUpdateCellBudget == 0 ? 65536 : m_nInstancerUpdateCellBudget;
	limits.maximum.nNodeLoadRegions = m_nDynamicNodeLoadRegionBudget;
	limits.maximum.nNodeUnloadRegions = m_nDynamicNodeUnloadRegionBudget;
	limits.nTargetUsec = m_nRuntimeAdaptiveTargetUsec;
	limits.nHardMaxUsec = m_nRuntimeAdaptiveHardMaxUsec;
	limits.nAdjustmentWindowFrames = m_nRuntimeAdaptiveAdjustmentWindowFrames;
	terrain3dAdaptiveSanitizeLimits( limits );
	return limits;
}

t3dAdaptiveBudgetPressure_t T3DTerrain::getRuntimeAdaptiveBudgetPressure( void ) const {
	t3dAdaptiveBudgetPressure_t pressure;
	if ( m_pData ) {
		pressure.bGpuLimited = effectiveGpuRegionCacheEnabled() &&
			( m_pData->getGpuRegionStallFlagsLastTick() &
				TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET ) != 0u;
		pressure.bCpuLimited = effectiveCpuRegionCacheEnabled() &&
			( m_pData->getCpuRegionStallFlagsLastTick() &
				TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET ) != 0u;
		pressure.bInstanceLimited = effectiveInstanceRegionCacheEnabled() &&
			( m_pData->getInstanceRegionStallFlagsLastTick() &
				TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET ) != 0u;
	}
	pressure.bInstancerLimited = effectiveInstancerCellStreamingEnabled() && m_pInstancer &&
		m_pInstancer->isUpdateBudgetLimitedLastFrame();
	pressure.bNodeLoadLimited = effectiveDynamicNodeStreamingEnabled() && m_pNodeStreamer &&
		m_pNodeStreamer->isLoadRegionBudgetLimitedLastTick();
	pressure.bNodeUnloadLimited = effectiveDynamicNodeStreamingEnabled() && m_pNodeStreamer &&
		m_pNodeStreamer->isUnloadRegionBudgetLimitedLastTick();
	return pressure;
}

void T3DTerrain::applyRuntimeAdaptiveWorkBudgets( void ) {
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
	const t3dAdaptiveBudgetValues_t &effective = m_nRuntimeAdaptiveBudgetState.effective;
	if ( m_pData ) {
		m_pData->setRuntimePageBudgetOverrides( effective.nGpuPages, effective.nCpuPages,
			effective.nInstancePages );
	}
	if ( m_pInstancer ) {
		m_pInstancer->setRuntimeUpdateCellBudgetOverride( effective.instancerCells );
	}
	if ( m_pNodeStreamer && effectiveDynamicNodeStreamingEnabled() ) {
		m_pNodeStreamer->setRuntimeLoadRegionBudgetOverride( effective.nNodeLoadRegions );
		m_pNodeStreamer->setRuntimeUnloadRegionBudgetOverride( effective.nNodeUnloadRegions );
	}
}

void T3DTerrain::resetRuntimeAdaptiveWorkBudgets( const bool bClearOverrides ) {
	const t3dAdaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
	terrain3dAdaptiveReset( limits, m_nRuntimeAdaptiveBudgetState,
		m_bRuntimeAdaptiveWorkBudgetsEnabled ? TERRAIN3D_ADAPTIVE_REASON_INITIALIZED :
		TERRAIN3D_ADAPTIVE_REASON_DISABLED );
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

void T3DTerrain::updateRuntimeAdaptiveWorkBudgets( void ) {
	if ( !m_bRuntimeAdaptiveWorkBudgetsEnabled ) {
		return;
	}
	const t3dAdaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
	const t3dAdaptiveBudgetPressure_t pressure = getRuntimeAdaptiveBudgetPressure();
	terrain3dAdaptiveStep( limits, pressure, m_nRuntimeAdaptiveLastMeasuredUsec,
		m_nRuntimeAdaptiveBudgetState );
	applyRuntimeAdaptiveWorkBudgets();
}

String T3DTerrain::getRuntimeAdaptiveReasonName( void ) const {
	switch ( m_nRuntimeAdaptiveBudgetState.reason ) {
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

void T3DTerrain::sampleRuntimeProfile( const t3dRuntimeProfileSample_t &rSample ) {
	t3dRuntimeProfileSample_t sample = rSample;
	sample.frameSequence = ++m_nRuntimeProfileSampleCount;
	if ( m_pData ) {
		m_pData->appendRuntimeProfileSample( sample );
	}
	if ( m_pTerrainMesher ) {
		sample.clipmapNearUpdates = ( int ) ( m_pTerrainMesher->getNearUpdatesLastSnap() );
		sample.clipmapMiddleUpdates = ( int ) ( m_pTerrainMesher->getMiddleUpdatesLastSnap() );
		sample.clipmapFarUpdates = ( int ) ( m_pTerrainMesher->getFarUpdatesLastSnap() );
		sample.clipmapTeleports = ( int ) ( m_pTerrainMesher->getTeleportsLastSnap() );
		sample.nClipmapBufferUploads = ( int ) ( m_pTerrainMesher->getBufferUploadsLastSnap() );
	}
	if ( m_pInstancer ) {
		sample.renderCells = m_pInstancer->getRenderCellCount();
		sample.pendingCellUpdates = m_pInstancer->getPendingStreamingUpdateCount();
	}
	if ( m_pNodeStreamer ) {
		sample.nNodeActiveRegions = m_pNodeStreamer->getActiveRegionCount();
		sample.nodeActiveNodes = m_pNodeStreamer->getActiveNodeCount();
		sample.nodeLoads = m_pNodeStreamer->getLoadsLastTick();
		sample.nodeUnloads = m_pNodeStreamer->getUnloadsLastTick();
		sample.nNodeLoadFailures = m_pNodeStreamer->getSceneLoadFailuresLastTick();
	}
	if ( m_pMaterial.is_valid() ) {
		const uint64_t shader_updates = m_pMaterial->getShaderUpdateCount();
		const uint64_t shader_creations = m_pMaterial->getShaderVariantCreationCount();
		sample.shaderUpdates = ( int ) ( shader_updates >= m_nRuntimeProfileShaderUpdatesSeen ?
			shader_updates - m_nRuntimeProfileShaderUpdatesSeen : shader_updates );
		sample.shaderVariantCreations = ( int ) ( shader_creations >= m_runtimeProfileShaderCreationsSeen ?
			shader_creations - m_runtimeProfileShaderCreationsSeen : shader_creations );
		if ( sample.shaderUpdates > 0 || sample.shaderVariantCreations > 0 ) {
			sample.nShaderSourceSubmissionUsec = m_pMaterial->getShaderSourceSubmissionUsecLast();
		}
		if ( sample.shaderUpdates > 0 ) {
			sample.nShaderUpdateUsec = m_pMaterial->getShaderUpdateUsecLast();
		}
		m_nRuntimeProfileShaderUpdatesSeen = shader_updates;
		m_runtimeProfileShaderCreationsSeen = shader_creations;
	}

	if ( sample.gpuCameraMoved != 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_CAMERA_MOVED;
	}
	if ( sample.nGpuUploads > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_GPU_UPLOAD;
	}
	if ( sample.nGpuEvictions > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_GPU_EVICTION;
	}
	if ( sample.gpuMissingRequired > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_GPU_MISSING_REQUIRED;
	}
	if ( sample.nGpuCookedMisses > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_COOKED_PAGE_MISS;
	}
	if ( sample.gpuRuntimePrepares > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_RUNTIME_PAGE_PREPARE;
	}
	if ( sample.clipmapTeleports > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_CLIPMAP_TELEPORT;
	}
	if ( sample.nNodeLoadFailures > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_NODE_LOAD_FAILURE;
	}
	if ( sample.gpuFarRegionsProcessed > 0 || sample.nGpuFarDirtyRegions > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_FAR_ATLAS_WORK;
	}
	if ( sample.cpuLoads > 0 && sample.cpuSelectionPending != 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_CPU_DEMAND_LOAD;
	}
	if ( sample.pendingCellUpdates > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_INSTANCE_BACKLOG;
	}
	if ( sample.shaderUpdates > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_SHADER_UPDATE;
	}
	if ( sample.shaderVariantCreations > 0 ) {
		sample.eventFlags |= TERRAIN3D_PROFILE_EVENT_SHADER_VARIANT_CREATED;
	}
	if ( sample.pendingCellUpdates > 0 && m_nInstancerUpdateCellBudget > 0 &&
		sample.pendingCellUpdates >= m_nInstancerUpdateCellBudget ) {
		sample.stallFlags |= TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET;
	}
	if ( m_nDynamicNodeLoadRegionBudget > 0 && sample.nodeLoads >= m_nDynamicNodeLoadRegionBudget ) {
		sample.stallFlags |= TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET;
	}
	if ( m_nDynamicNodeUnloadRegionBudget > 0 && sample.nodeUnloads >= m_nDynamicNodeUnloadRegionBudget ) {
		sample.stallFlags |= TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET;
	}

	updateCacheThrashEvents( sample );
	m_runtimeProfileLastSample = sample;
	m_runtimeProfileAccumulator.addSample( sample );
	if ( !m_pRuntimeProfileHistory ) {
		m_pRuntimeProfileHistory = memnew( t3dRuntimeProfileHistory_t );
	}
	m_pRuntimeProfileHistory->push( sample );
}

void T3DTerrain::updateCacheThrashEvents( const t3dRuntimeProfileSample_t &rSample ) {
	const bool bStates[3] = {
		rSample.gpuMissingRequired > 0 || ( rSample.nGpuEvictions >= 2 && rSample.nGpuUploads >= 2 ),
		rSample.nCpuEvictions >= 2 && rSample.cpuLoads >= 2,
		rSample.nInstanceEvictions >= 2 && rSample.instanceLoads >= 2,
	};
	const int64_t activity[3] = {
		( int64_t ) ( MAX( rSample.gpuMissingRequired, rSample.nGpuEvictions ) ),
		( int64_t ) ( rSample.nCpuEvictions ), ( int64_t ) ( rSample.nInstanceEvictions ),
	};
	for ( int index = 0; index < 3; ++index ) {
		if ( m_bCacheThrashEventState[( size_t ) ( index )] == bStates[index] ) {
			continue;
		}
		m_bCacheThrashEventState[( size_t ) ( index )] = bStates[index];
		emit_signal( "cache_thrash_state_changed", StringName( TERRAIN3D_CACHE_THRASH_SUBSYSTEM_NAMES[index] ), bStates[index], activity[index] );
	}
}

void T3DTerrain::setRuntimeProfilingEnabled( const bool bEnabled ) {
	if ( bEnabled && !m_pRuntimeProfileHistory ) {
		m_pRuntimeProfileHistory = memnew( t3dRuntimeProfileHistory_t );
	}
	m_bRuntimeProfilingEnabled = bEnabled;
}

Dictionary T3DTerrain::runtimeProfileSampleToDictionary( const t3dRuntimeProfileSample_t &rSample ) const {
	Dictionary sample;
	sample["sequence"] = ( int64_t ) ( rSample.frameSequence );
	sample["physics_total_usec"] = ( int64_t ) ( rSample.nPhysicsTotalUsec );
	sample["data_residency_usec"] = ( int64_t ) ( rSample.nDataResidencyUsec );
	sample["hlod_streaming_usec"] = ( int64_t ) ( rSample.nHlodStreamingUsec );
	sample["node_streaming_usec"] = ( int64_t ) ( rSample.nNodeStreamingUsec );
	sample["lightmap_streaming_usec"] = ( int64_t ) ( rSample.nLightmapStreamingUsec );
	sample["clipmap_usec"] = ( int64_t ) ( rSample.nClipmapUsec );
	sample["ocean_usec"] = ( int64_t ) ( rSample.nOceanUsec );
	sample["instancer_usec"] = ( int64_t ) ( rSample.nInstancerUsec );
	sample["light_sync_usec"] = ( int64_t ) ( rSample.nLightSyncUsec );
	sample["collision_usec"] = ( int64_t ) ( rSample.nCollisionUsec );
	sample["streaming_usec"] = ( int64_t ) ( rSample.nStreamingUsec );
	sample["target_position"] = Vector2( rSample.fTargetX, rSample.fTargetZ );
	sample["movement"] = Vector2( rSample.fMovementX, rSample.fMovementZ );
	sample["target_region"] = Vector2i( rSample.targetRegionX, rSample.targetRegionZ );
	sample["event_flags"] = ( int64_t ) ( rSample.eventFlags );
	sample["stall_flags"] = ( int64_t ) ( rSample.stallFlags );
	sample["events"] = getRuntimeProfileEventNames( rSample.eventFlags );
	sample["stall_reasons"] = getRuntimeProfileStallNames( rSample.stallFlags );
	sample["gpu_resident"] = rSample.gpuResident;
	sample["gpu_capacity"] = rSample.nGpuCapacity;
	sample["gpu_uploads"] = rSample.nGpuUploads;
	sample["gpu_evictions"] = rSample.nGpuEvictions;
	sample["gpu_missing_required"] = rSample.gpuMissingRequired;
	sample["gpu_upload_bytes"] = ( int64_t ) ( rSample.nGpuUploadBytes );
	sample["gpu_requested_pages"] = rSample.nGpuRequestedPages;
	sample["gpu_required_pages"] = rSample.nGpuRequiredPages;
	sample["gpu_guard_pages"] = rSample.nGpuGuardPages;
	sample["gpu_prefetch_pages"] = rSample.nGpuPrefetchPages;
	sample["gpu_hysteresis_pages"] = rSample.nGpuHysteresisPages;
	sample["gpu_prepared_queue"] = rSample.gpuPreparedQueue;
	sample["gpu_prepare_pending_pages"] = rSample.nGpuPreparePendingPages;
	sample["gpu_prepare_job_active"] = rSample.gpuPrepareJobActive != 0;
	sample["gpu_prepare_workers"] = rSample.nGpuPrepareWorkers;
	sample["gpu_prepare_latency_usec"] = ( int64_t ) ( rSample.nGpuPrepareLatencyUsec );
	sample["gpu_prepare_queue_wait_usec"] = ( int64_t ) ( rSample.nGpuPrepareQueueWaitUsec );
	sample["gpu_prepare_file_open_usec"] = ( int64_t ) ( rSample.nGpuPrepareFileOpenUsec );
	sample["gpu_prepare_file_read_usec"] = ( int64_t ) ( rSample.nGpuPrepareFileReadUsec );
	sample["gpu_prepare_validation_usec"] = ( int64_t ) ( rSample.nGpuPrepareValidationUsec );
	sample["gpu_prepare_materialize_usec"] = ( int64_t ) ( rSample.nGpuPrepareMaterializeUsec );
	sample["gpu_prepare_worker_usec"] = ( int64_t ) ( rSample.nGpuPrepareWorkerUsec );
	sample["gpu_prepare_ready_wait_usec"] = ( int64_t ) ( rSample.nGpuPrepareReadyWaitUsec );
	sample["gpu_prepare_publication_usec"] = ( int64_t ) ( rSample.nGpuPreparePublicationUsec );
	sample["gpu_prepare_end_to_end_usec"] = ( int64_t ) ( rSample.nGpuPrepareEndToEndUsec );
	sample["gpu_cache_service_usec"] = ( int64_t ) ( rSample.nGpuCacheServiceUsec );
	sample["gpu_effective_upload_limit"] = rSample.gpuEffectiveUploadLimit;
	sample["gpu_cooked_hits"] = rSample.nGpuCookedHits;
	sample["gpu_cooked_misses"] = rSample.nGpuCookedMisses;
	sample["gpu_runtime_prepares"] = rSample.gpuRuntimePrepares;
	sample["gpu_far_dirty_regions"] = rSample.nGpuFarDirtyRegions;
	sample["gpu_far_regions_processed"] = rSample.gpuFarRegionsProcessed;
	sample["clipmap_near_updates"] = rSample.clipmapNearUpdates;
	sample["clipmap_middle_updates"] = rSample.clipmapMiddleUpdates;
	sample["clipmap_far_updates"] = rSample.clipmapFarUpdates;
	sample["clipmap_teleports"] = rSample.clipmapTeleports;
	sample["clipmap_buffer_uploads"] = rSample.nClipmapBufferUploads;
	sample["cpu_resident"] = rSample.cpuResident;
	sample["cpu_desired"] = rSample.cpuDesired;
	sample["cpu_loads"] = rSample.cpuLoads;
	sample["cpu_evictions"] = rSample.nCpuEvictions;
	sample["cpu_load_bytes"] = ( int64_t ) ( rSample.nCpuLoadBytes );
	sample["cpu_cache_service_usec"] = ( int64_t ) ( rSample.nCpuCacheServiceUsec );
	sample["cpu_selection_pending"] = rSample.cpuSelectionPending != 0;
	sample["cpu_pinned"] = rSample.cpuPinned;
	sample["instance_resident"] = rSample.instanceResident;
	sample["instance_desired"] = rSample.instanceDesired;
	sample["instance_loads"] = rSample.instanceLoads;
	sample["instance_evictions"] = rSample.nInstanceEvictions;
	sample["instance_load_bytes"] = ( int64_t ) ( rSample.nInstanceLoadBytes );
	sample["instance_cache_service_usec"] = ( int64_t ) ( rSample.nInstanceCacheServiceUsec );
	sample["instance_selection_pending"] = rSample.instanceSelectionPending != 0;
	sample["render_cells"] = rSample.renderCells;
	sample["pending_cell_updates"] = rSample.pendingCellUpdates;
	sample["node_active_regions"] = rSample.nNodeActiveRegions;
	sample["node_active_nodes"] = rSample.nodeActiveNodes;
	sample["node_loads"] = rSample.nodeLoads;
	sample["node_unloads"] = rSample.nodeUnloads;
	sample["node_load_failures"] = rSample.nNodeLoadFailures;
	sample["shader_updates"] = rSample.shaderUpdates;
	sample["shader_variant_creations"] = rSample.shaderVariantCreations;
	sample["shader_source_submission_usec"] = ( int64_t ) ( rSample.nShaderSourceSubmissionUsec );
	sample["shader_update_usec"] = ( int64_t ) ( rSample.nShaderUpdateUsec );
	return sample;
}

Array T3DTerrain::getRuntimeProfileEventNames( const uint32_t nFlags ) const {
	Array names;
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_CAMERA_MOVED ) != 0u ) { names.push_back( "camera_moved" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_GPU_UPLOAD ) != 0u ) { names.push_back( "gpu_upload" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_GPU_EVICTION ) != 0u ) { names.push_back( "gpu_eviction" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_GPU_MISSING_REQUIRED ) != 0u ) { names.push_back( "gpu_missing_required" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_COOKED_PAGE_MISS ) != 0u ) { names.push_back( "cooked_page_miss" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_RUNTIME_PAGE_PREPARE ) != 0u ) { names.push_back( "runtime_page_prepare" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_CLIPMAP_TELEPORT ) != 0u ) { names.push_back( "clipmap_teleport" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_NODE_LOAD_FAILURE ) != 0u ) { names.push_back( "node_load_failure" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_FAR_ATLAS_WORK ) != 0u ) { names.push_back( "far_atlas_work" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_CPU_DEMAND_LOAD ) != 0u ) { names.push_back( "cpu_demand_load" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_INSTANCE_BACKLOG ) != 0u ) { names.push_back( "instance_backlog" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_SHADER_UPDATE ) != 0u ) { names.push_back( "shader_update" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_EVENT_SHADER_VARIANT_CREATED ) != 0u ) { names.push_back( "shader_variant_created" ); }
	return names;
}

Array T3DTerrain::getRuntimeProfileStallNames( const uint32_t nFlags ) const {
	Array names;
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET ) != 0u ) { names.push_back( "gpu_upload_layer_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING ) != 0u ) { names.push_back( "gpu_prepare_pending" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE ) != 0u ) { names.push_back( "gpu_no_ready_page" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY ) != 0u ) { names.push_back( "gpu_page_capacity" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET ) != 0u ) { names.push_back( "cpu_load_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET ) != 0u ) { names.push_back( "instance_load_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET ) != 0u ) { names.push_back( "instance_update_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET ) != 0u ) { names.push_back( "node_load_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET ) != 0u ) { names.push_back( "node_unload_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET ) != 0u ) { names.push_back( "gpu_upload_byte_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET ) != 0u ) { names.push_back( "gpu_update_call_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY ) != 0u ) { names.push_back( "gpu_retirement_capacity" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET ) != 0u ) { names.push_back( "cpu_load_byte_budget" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY ) != 0u ) { names.push_back( "cpu_pinned_capacity" ); }
	if ( ( nFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET ) != 0u ) { names.push_back( "instance_load_byte_budget" ); }
	return names;
}

Array T3DTerrain::getRuntimeProfileProblems( void ) const {
	Array problems;
	const t3dRuntimeProfileSample_t &sample = m_runtimeProfileLastSample;
	if ( sample.gpuMissingRequired > 0 ) {
		t3dAppendRuntimeProblem( problems, "error", "gpu_required_pages_missing", "gpu_streaming",
			"Required detailed terrain pages are not resident.", sample.gpuMissingRequired );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "warning", "gpu_upload_budget_saturated", "gpu_streaming",
			"GPU publication stopped at the current layer budget.", sample.gpuEffectiveUploadLimit );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "warning", "gpu_upload_byte_budget", "gpu_streaming",
			"GPU publication stopped at the configured byte budget.", ( int64_t ) ( sample.nGpuUploadBytes ) );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "warning", "gpu_update_call_budget", "gpu_streaming",
			"GPU publication stopped at the texture-update call budget.", sample.nGpuUploads );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "info", "gpu_prepare_pending", "gpu_streaming",
			"A required page is still being prepared by the worker pool.", sample.nGpuPreparePendingPages );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "warning", "gpu_no_ready_page", "gpu_streaming",
			"A required page had no prepared cooked payload ready for publication.", sample.gpuPreparedQueue );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "warning", "gpu_retirement_capacity", "gpu_streaming",
			"No staging layer is free because physical pages are mapped or retiring.", sample.gpuResident );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "error", "gpu_page_capacity_exhausted", "gpu_streaming",
			"Mapped GPU page capacity is exhausted while required pages are missing.", sample.nGpuCapacity );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "info", "cpu_load_budget", "cpu_streaming",
			"CPU region loading stopped at the page budget.", sample.cpuLoads );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "info", "cpu_load_byte_budget", "cpu_streaming",
			"CPU region loading stopped at the byte budget.", ( int64_t ) ( sample.nCpuLoadBytes ) );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "warning", "cpu_pinned_capacity", "cpu_streaming",
			"CPU cache replacement could not find an evictable region slot.", sample.cpuPinned );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "info", "instance_load_budget", "instance_streaming",
			"Instance page loading stopped at the page budget.", sample.instanceLoads );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "info", "instance_load_byte_budget", "instance_streaming",
			"Instance page loading stopped at the byte budget.", ( int64_t ) ( sample.nInstanceLoadBytes ) );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "warning", "instance_update_budget", "instancer",
			"Instance cell publication remains queued at the per-frame cell budget.", sample.pendingCellUpdates );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "info", "node_load_budget", "node_streaming",
			"Dynamic node loading reached the region budget.", sample.nodeLoads );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET ) != 0u ) {
		t3dAppendRuntimeProblem( problems, "info", "node_unload_budget", "node_streaming",
			"Dynamic node unloading reached the region budget.", sample.nodeUnloads );
	}
	if ( sample.nGpuCookedMisses > 0 ) {
		t3dAppendRuntimeProblem( problems, "warning", "cooked_page_miss", "gpu_streaming",
			"Cooked runtime pages were unavailable and required fallback preparation.", sample.nGpuCookedMisses );
	}
	if ( sample.pendingCellUpdates > 0 ) {
		t3dAppendRuntimeProblem( problems, "warning", "instance_cell_backlog", "instancer",
			"Instance cell updates remain queued after this tick.", sample.pendingCellUpdates );
	}
	if ( sample.nNodeLoadFailures > 0 ) {
		t3dAppendRuntimeProblem( problems, "error", "node_load_failure", "node_streaming",
			"One or more streamed node scenes failed to load.", sample.nNodeLoadFailures );
	}
	if ( sample.nGpuFarDirtyRegions > 0 ) {
		t3dAppendRuntimeProblem( problems, "info", "far_atlas_backlog", "far_atlas",
			"Far-atlas region patches remain dirty.", sample.nGpuFarDirtyRegions );
	}
	if ( sample.shaderUpdates > 0 ) {
		t3dAppendRuntimeProblem( problems, "info", "shader_update", "shaders",
			"Terrain shader source was submitted or rebound during this sampled tick.",
			( int64_t ) ( sample.nShaderUpdateUsec ) );
	}
	if ( sample.shaderVariantCreations > 0 ) {
		t3dAppendRuntimeProblem( problems, "info", "shader_variant_created", "shaders",
			"One or more shader variants were created during this sampled tick.",
			sample.shaderVariantCreations );
	}
	if ( sample.nPhysicsTotalUsec >= 2000u ) {
		t3dAppendRuntimeProblem( problems, "warning", "terrain_frame_over_2ms", "frame",
			"Terrain3D physics work exceeded 2 ms on the latest sampled tick.", ( int64_t ) ( sample.nPhysicsTotalUsec ) );
	}
	return problems;
}

Array T3DTerrain::getRuntimeProfileHistory( const int nMaxSamples ) const {
	Array history;
	if ( !m_pRuntimeProfileHistory ) {
		return history;
	}
	const size_t available = m_pRuntimeProfileHistory->nCount;
	const size_t requested = nMaxSamples <= 0 ? available : ( size_t ) ( MIN( nMaxSamples, ( int ) ( available ) ) );
	const size_t start = available - requested;
	history.resize( ( int ) ( requested ) );
	for ( size_t index = 0u; index < requested; ++index ) {
		history[( int ) ( index )] = runtimeProfileSampleToDictionary( m_pRuntimeProfileHistory->getOldest( start + index ) );
	}
	return history;
}

Array T3DTerrain::getRuntimeProfileHistorySince( const int64_t nAfterSequence, const int nMaxSamples ) const {
	Array history;
	if ( !m_pRuntimeProfileHistory ) {
		return history;
	}
	const size_t available = m_pRuntimeProfileHistory->nCount;
	if ( available == 0u || nMaxSamples == 0 ) {
		return history;
	}
	size_t first_new = available;
	for ( size_t index = 0u; index < available; ++index ) {
		if ( ( int64_t ) ( m_pRuntimeProfileHistory->getOldest( index ).frameSequence ) > nAfterSequence ) {
			first_new = index;
			break;
		}
	}
	if ( first_new >= available ) {
		return history;
	}
	const size_t new_count = available - first_new;
	const size_t requested = nMaxSamples < 0 ? new_count : ( size_t ) ( MIN( nMaxSamples, ( int ) ( new_count ) ) );
	const size_t start = available - requested;
	history.resize( ( int ) ( requested ) );
	for ( size_t index = 0u; index < requested; ++index ) {
		history[( int ) ( index )] = runtimeProfileSampleToDictionary( m_pRuntimeProfileHistory->getOldest( start + index ) );
	}
	return history;
}

Dictionary T3DTerrain::getRuntimeProfileAggregate( void ) const {
	Dictionary aggregate;
	const t3dRuntimeProfileAccumulator_t &stats = m_runtimeProfileAccumulator;
	aggregate["sampled_ticks"] = ( int64_t ) ( stats.nSampledTicks );

	Dictionary timing;
	Dictionary physics;
	physics["total_usec"] = ( int64_t ) ( stats.nPhysicsTotalUsec );
	physics["peak_usec"] = ( int64_t ) ( stats.nPhysicsPeakUsec );
	physics["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nPhysicsTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["physics"] = physics;
	Dictionary data_residency;
	data_residency["total_usec"] = ( int64_t ) ( stats.nDataResidencyTotalUsec );
	data_residency["peak_usec"] = ( int64_t ) ( stats.nDataResidencyPeakUsec );
	data_residency["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nDataResidencyTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["data_residency"] = data_residency;
	Dictionary hlod_streaming;
	hlod_streaming["total_usec"] = ( int64_t ) ( stats.nHlodStreamingTotalUsec );
	hlod_streaming["peak_usec"] = ( int64_t ) ( stats.nHlodStreamingPeakUsec );
	hlod_streaming["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nHlodStreamingTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["hlod_streaming"] = hlod_streaming;
	Dictionary node_streaming;
	node_streaming["total_usec"] = ( int64_t ) ( stats.nNodeStreamingTotalUsec );
	node_streaming["peak_usec"] = ( int64_t ) ( stats.nNodeStreamingPeakUsec );
	node_streaming["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nNodeStreamingTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["node_streaming"] = node_streaming;
	Dictionary clipmap_timing;
	clipmap_timing["total_usec"] = ( int64_t ) ( stats.nClipmapTotalUsec );
	clipmap_timing["peak_usec"] = ( int64_t ) ( stats.nClipmapPeakUsec );
	clipmap_timing["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nClipmapTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["clipmap"] = clipmap_timing;
	Dictionary ocean;
	ocean["total_usec"] = ( int64_t ) ( stats.nOceanTotalUsec );
	ocean["peak_usec"] = ( int64_t ) ( stats.nOceanPeakUsec );
	ocean["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nOceanTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["ocean"] = ocean;
	Dictionary instancer_timing;
	instancer_timing["total_usec"] = ( int64_t ) ( stats.nInstancerTotalUsec );
	instancer_timing["peak_usec"] = ( int64_t ) ( stats.nInstancerPeakUsec );
	instancer_timing["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nInstancerTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["instancer"] = instancer_timing;
	Dictionary light_sync;
	light_sync["total_usec"] = ( int64_t ) ( stats.nLightSyncTotalUsec );
	light_sync["peak_usec"] = ( int64_t ) ( stats.nLightSyncPeakUsec );
	light_sync["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nLightSyncTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["light_sync"] = light_sync;
	Dictionary collision;
	collision["total_usec"] = ( int64_t ) ( stats.nCollisionTotalUsec );
	collision["peak_usec"] = ( int64_t ) ( stats.nCollisionPeakUsec );
	collision["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nCollisionTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["collision"] = collision;
	Dictionary streaming;
	streaming["total_usec"] = ( int64_t ) ( stats.nStreamingTotalUsec );
	streaming["peak_usec"] = ( int64_t ) ( stats.nStreamingPeakUsec );
	streaming["average_usec"] = stats.nSampledTicks > 0u ?
		( double ) ( stats.nStreamingTotalUsec ) / ( double ) ( stats.nSampledTicks ) : 0.0;
	timing["streaming"] = streaming;
	aggregate["timing"] = timing;

	Dictionary gpu;
	gpu["uploads"] = ( int64_t ) ( stats.nGpuUploadsTotal );
	gpu["evictions"] = ( int64_t ) ( stats.nGpuEvictionsTotal );
	gpu["upload_bytes"] = ( int64_t ) ( stats.nGpuUploadBytesTotal );
	gpu["missing_required"] = ( int64_t ) ( stats.nGpuMissingRequiredTotal );
	gpu["resident_peak"] = stats.gpuResidentPeak;
	gpu["prepared_pages"] = ( int64_t ) ( stats.nGpuPreparedPagesTotal );
	gpu["prepare_workers_peak"] = stats.gpuPrepareWorkersPeak;
	gpu["effective_upload_limit_peak"] = stats.gpuEffectiveUploadLimitPeak;
	gpu["camera_move_ticks"] = ( int64_t ) ( stats.nGpuCameraMoveTicks );
	gpu["distance_travelled_m"] = stats.fDistanceTravelledM;
	gpu["cooked_hits"] = ( int64_t ) ( stats.nGpuCookedHitsTotal );
	gpu["cooked_misses"] = ( int64_t ) ( stats.nGpuCookedMissesTotal );
	gpu["runtime_prepares"] = ( int64_t ) ( stats.nGpuRuntimePreparesTotal );
	const uint64_t cooked_attempts = stats.nGpuCookedHitsTotal + stats.nGpuCookedMissesTotal;
	gpu["cooked_hit_ratio"] = cooked_attempts > 0u ?
		( double ) ( stats.nGpuCookedHitsTotal ) / ( double ) ( cooked_attempts ) : -1.0;
	gpu["upload_bytes_per_m"] = stats.fDistanceTravelledM > 0.0 ?
		( double ) ( stats.nGpuUploadBytesTotal ) / stats.fDistanceTravelledM : 0.0;
	gpu["evictions_per_km"] = stats.fDistanceTravelledM > 0.0 ?
		( double ) ( stats.nGpuEvictionsTotal ) * 1000.0 / stats.fDistanceTravelledM : 0.0;
	aggregate["gpu"] = gpu;

	Dictionary clipmap;
	clipmap["near_updates"] = ( int64_t ) ( stats.nClipmapNearUpdatesTotal );
	clipmap["middle_updates"] = ( int64_t ) ( stats.nClipmapMiddleUpdatesTotal );
	clipmap["far_updates"] = ( int64_t ) ( stats.nClipmapFarUpdatesTotal );
	clipmap["teleports"] = ( int64_t ) ( stats.nClipmapTeleportsTotal );
	clipmap["teleports_peak"] = stats.clipmapTeleportsPeak;
	clipmap["buffer_uploads"] = ( int64_t ) ( stats.nClipmapBufferUploadsTotal );
	clipmap["buffer_uploads_peak"] = stats.clipmapBufferUploadsPeak;
	aggregate["clipmap"] = clipmap;

	Dictionary cpu;
	cpu["loads"] = ( int64_t ) ( stats.nCpuLoadsTotal );
	cpu["evictions"] = ( int64_t ) ( stats.nCpuEvictionsTotal );
	cpu["load_bytes"] = ( int64_t ) ( stats.nCpuLoadBytesTotal );
	cpu["resident_peak"] = stats.cpuResidentPeak;
	aggregate["cpu"] = cpu;

	Dictionary instances;
	instances["loads"] = ( int64_t ) ( stats.nInstanceLoadsTotal );
	instances["evictions"] = ( int64_t ) ( stats.nInstanceEvictionsTotal );
	instances["load_bytes"] = ( int64_t ) ( stats.nInstanceLoadBytesTotal );
	instances["resident_peak"] = stats.instanceResidentPeak;
	instances["render_cells_peak"] = stats.renderCellsPeak;
	instances["pending_cell_updates_peak"] = stats.pendingCellUpdatesPeak;
	aggregate["instances"] = instances;

	Dictionary nodes;
	nodes["loads"] = ( int64_t ) ( stats.nNodeLoadsTotal );
	nodes["unloads"] = ( int64_t ) ( stats.nNodeUnloadsTotal );
	nodes["load_failures"] = ( int64_t ) ( stats.nNodeLoadFailuresTotal );
	nodes["active_regions_peak"] = stats.nodeActiveRegionsPeak;
	nodes["active_nodes_peak"] = stats.nodeActiveNodesPeak;
	aggregate["nodes"] = nodes;
	return aggregate;
}

void T3DTerrain::setRuntimeAdaptiveWorkBudgetsEnabled( const bool bEnabled ) {
	if ( m_bRuntimeAdaptiveWorkBudgetsEnabled == bEnabled ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_bRuntimeAdaptiveWorkBudgetsEnabled = bEnabled;
	resetRuntimeAdaptiveWorkBudgets( !bEnabled );
	notify_property_list_changed();
}

void T3DTerrain::setRuntimeAdaptiveTargetMs( const real_t fTargetMs ) {
	const uint64_t target_usec = ( uint64_t ) ( CLAMP( fTargetMs, ( real_t ) ( 0.05f ), ( real_t ) ( 50.0f ) ) * 1000.0f );
	if ( m_nRuntimeAdaptiveTargetUsec == target_usec ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveTargetUsec = target_usec;
	m_nRuntimeAdaptiveHardMaxUsec = MAX( m_nRuntimeAdaptiveHardMaxUsec, m_nRuntimeAdaptiveTargetUsec );
	m_nRuntimeAdaptiveBudgetState.overBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.underBudgetStreak = 0;
}

void T3DTerrain::setRuntimeAdaptiveHardMaxMs( const real_t fHardMaxMs ) {
	const uint64_t hard_max_usec = ( uint64_t ) ( CLAMP( fHardMaxMs, ( real_t ) ( 0.05f ), ( real_t ) ( 100.0f ) ) * 1000.0f );
	const uint64_t normalized = MAX( hard_max_usec, m_nRuntimeAdaptiveTargetUsec );
	if ( m_nRuntimeAdaptiveHardMaxUsec == normalized ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveHardMaxUsec = normalized;
	m_nRuntimeAdaptiveBudgetState.overBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.underBudgetStreak = 0;
}

void T3DTerrain::setRuntimeAdaptiveAdjustmentWindowFrames( const int nFrames ) {
	const int frames = CLAMP( nFrames, 1, 120 );
	if ( m_nRuntimeAdaptiveAdjustmentWindowFrames == frames ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveAdjustmentWindowFrames = frames;
	m_nRuntimeAdaptiveBudgetState.overBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.underBudgetStreak = 0;
}

void T3DTerrain::setRuntimeAdaptiveMinGpuPages( const int nPages ) {
	const int pages = CLAMP( nPages, 0, 64 );
	if ( m_nRuntimeAdaptiveMinGpuPages == pages ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinGpuPages = pages;
}

void T3DTerrain::setRuntimeAdaptiveMinCpuPages( const int nPages ) {
	const int pages = CLAMP( nPages, 0, 64 );
	if ( m_nRuntimeAdaptiveMinCpuPages == pages ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinCpuPages = pages;
}

void T3DTerrain::setRuntimeAdaptiveMinInstancePages( const int nPages ) {
	const int pages = CLAMP( nPages, 0, 64 );
	if ( m_nRuntimeAdaptiveMinInstancePages == pages ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinInstancePages = pages;
}

void T3DTerrain::setRuntimeAdaptiveMinInstancerCells( const int nCells ) {
	const int cells = CLAMP( nCells, 0, 65536 );
	if ( m_nRuntimeAdaptiveMinInstancerCells == cells ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinInstancerCells = cells;
}

void T3DTerrain::setRuntimeAdaptiveMinNodeLoadRegions( const int nRegions ) {
	const int regions = CLAMP( nRegions, 0, 64 );
	if ( m_nRuntimeAdaptiveMinNodeLoadRegions == regions ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinNodeLoadRegions = regions;
}

void T3DTerrain::setRuntimeAdaptiveMinNodeUnloadRegions( const int nRegions ) {
	const int regions = CLAMP( nRegions, 0, 64 );
	if ( m_nRuntimeAdaptiveMinNodeUnloadRegions == regions ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeAdaptiveMinNodeUnloadRegions = regions;
}

void T3DTerrain::setRuntimeBoundaryWorkPhasingEnabled( const bool bEnabled ) {
	if ( m_bRuntimeBoundaryWorkPhasingEnabled == bEnabled ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_bRuntimeBoundaryWorkPhasingEnabled = bEnabled;
	m_bRuntimeBoundaryNodeDeferredLastTick = false;
	m_bRuntimeBoundaryInstancerDeferredLastTick = false;
	m_runtimeBoundaryNodeDeferralStreak = 0;
	m_runtimeBoundaryInstancerDeferralStreak = 0;
}

void T3DTerrain::setRuntimeBoundaryNodeDelayFrames( const int nFrames ) {
	const int frames = CLAMP( nFrames, 0, 8 );
	if ( m_nRuntimeBoundaryNodeDelayFrames == frames ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeBoundaryNodeDelayFrames = frames;
}

void T3DTerrain::setRuntimeBoundaryInstancerDelayFrames( const int nFrames ) {
	const int frames = CLAMP( nFrames, 0, 8 );
	if ( m_nRuntimeBoundaryInstancerDelayFrames == frames ) {
		return;
	}
	refreshPerformanceProfileConformity();
	m_nRuntimeBoundaryInstancerDelayFrames = frames;
}

Dictionary T3DTerrain::getRuntimeBoundaryPhasingStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bRuntimeBoundaryWorkPhasingEnabled;
	stats["target_region"] = m_runtimeBoundaryTargetRegion;
	stats["frames_since_boundary"] = ( int64_t ) ( m_iRuntimePhysicsFrameIndex - m_nRuntimeBoundaryTransitionFrame );
	stats["node_delay_frames"] = m_nRuntimeBoundaryNodeDelayFrames;
	stats["instancer_delay_frames"] = m_nRuntimeBoundaryInstancerDelayFrames;
	stats["node_deferred_last_tick"] = m_bRuntimeBoundaryNodeDeferredLastTick;
	stats["instancer_deferred_last_tick"] = m_bRuntimeBoundaryInstancerDeferredLastTick;
	stats["node_deferral_streak"] = m_runtimeBoundaryNodeDeferralStreak;
	stats["instancer_deferral_streak"] = m_runtimeBoundaryInstancerDeferralStreak;
	stats["node_deferrals_total"] = ( int64_t ) ( m_nRuntimeBoundaryNodeDeferralsTotal );
	stats["instancer_deferrals_total"] = ( int64_t ) ( m_nRuntimeBoundaryInstancerDeferralsTotal );
	return stats;
}

Dictionary T3DTerrain::getRuntimeAdaptiveBudgetStats( void ) const {
	const t3dAdaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
	const t3dAdaptiveBudgetPressure_t pressure = getRuntimeAdaptiveBudgetPressure();
	Dictionary stats;
	stats["enabled"] = m_bRuntimeAdaptiveWorkBudgetsEnabled;
	stats["target_usec"] = ( int64_t ) ( limits.nTargetUsec );
	stats["hard_max_usec"] = ( int64_t ) ( limits.nHardMaxUsec );
	stats["last_measured_usec"] = ( int64_t ) ( m_nRuntimeAdaptiveLastMeasuredUsec );
	stats["controller_usec_last"] = ( int64_t ) ( m_nRuntimeAdaptiveControllerUsecLast );
	stats["adjustment_window_frames"] = limits.nAdjustmentWindowFrames;
	stats["over_budget_streak"] = m_nRuntimeAdaptiveBudgetState.overBudgetStreak;
	stats["under_budget_streak"] = m_nRuntimeAdaptiveBudgetState.underBudgetStreak;
	stats["adjustment_count"] = ( int64_t ) ( m_nRuntimeAdaptiveBudgetState.nAdjustmentCount );
	stats["reason"] = getRuntimeAdaptiveReasonName();

	Dictionary configured;
	configured["gpu_pages"] = limits.maximum.nGpuPages;
	configured["cpu_pages"] = limits.maximum.nCpuPages;
	configured["instance_pages"] = limits.maximum.nInstancePages;
	configured["instancer_cells"] = limits.maximum.instancerCells;
	configured["node_load_regions"] = limits.maximum.nNodeLoadRegions;
	configured["node_unload_regions"] = limits.maximum.nNodeUnloadRegions;
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
	minimum["gpu_pages"] = limits.minimum.nGpuPages;
	minimum["cpu_pages"] = limits.minimum.nCpuPages;
	minimum["instance_pages"] = limits.minimum.nInstancePages;
	minimum["instancer_cells"] = limits.minimum.instancerCells;
	minimum["node_load_regions"] = limits.minimum.nNodeLoadRegions;
	minimum["node_unload_regions"] = limits.minimum.nNodeUnloadRegions;
	stats["minimum"] = minimum;

	t3dAdaptiveBudgetState_t displayed_state = m_nRuntimeAdaptiveBudgetState;
	if ( !m_bRuntimeAdaptiveWorkBudgetsEnabled ) {
		displayed_state.effective = limits.maximum;
	} else {
		terrain3dAdaptiveClampState( limits, displayed_state );
	}
	Dictionary effective;
	effective["gpu_pages"] = displayed_state.effective.nGpuPages;
	effective["cpu_pages"] = displayed_state.effective.nCpuPages;
	effective["instance_pages"] = displayed_state.effective.nInstancePages;
	effective["instancer_cells"] = displayed_state.effective.instancerCells;
	effective["node_load_regions"] = displayed_state.effective.nNodeLoadRegions;
	effective["node_unload_regions"] = displayed_state.effective.nNodeUnloadRegions;
	stats["effective"] = effective;

	Dictionary limited;
	limited["gpu"] = pressure.bGpuLimited;
	limited["cpu"] = pressure.bCpuLimited;
	limited["instance"] = pressure.bInstanceLimited;
	limited["instancer"] = pressure.bInstancerLimited;
	limited["node_load"] = pressure.bNodeLoadLimited;
	limited["node_unload"] = pressure.bNodeUnloadLimited;
	stats["limited"] = limited;
	return stats;
}

void T3DTerrain::setPerformanceTarget( const t3dPerformanceTarget_t target ) {
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
	if ( m_pMaterial.is_valid() ) {
		m_pMaterial->setShaderPerformancePreset( T3DMaterial::SHADER_PRESET_XBOX_360_AUTHENTIC );
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
	if ( m_nRegionSize != SIZE_128 || !Math::is_equal_approx( m_fVertexSpacing, ( real_t ) ( 1.0f ) ) ) {
		m_performanceTarget = PERFORMANCE_TARGET_CUSTOM;
		m_bPerformanceProfileMaterialPendingApply = false;
		LOG( T3DTerrain::ERROR, "Xbox 360 performance target requires 128-sample regions at 1 metre vertex spacing" );
	}
	refreshPerformanceProfileConformity();
	notify_property_list_changed();
}

String T3DTerrain::getPerformanceProfileStatusName( void ) const {
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

int T3DTerrain::getPerformanceProfileVersion( void ) const {
	return ( int ) ( TERRAIN3D_PROFILE_VERSION );
}

String T3DTerrain::getPerformanceProfileSignature( void ) const {
	return String::num_uint64( terrain_3d_profile::signature(), 16, true );
}

Dictionary T3DTerrain::getPerformanceProfileManifest( void ) const {
	Dictionary manifest;
	manifest["key"] = TERRAIN3D_PROFILE_KEY;
	manifest["display_name"] = TERRAIN3D_PROFILE_DISPLAY_NAME;
	manifest["version"] = ( int ) ( TERRAIN3D_PROFILE_VERSION );
	manifest["signature"] = getPerformanceProfileSignature();
	manifest["target"] = ( int ) ( PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL );
	Array settings;
	settings.resize( ( int ) ( terrain_3d_profile::settingCount() ) );
	for ( size_t i = 0; i < terrain_3d_profile::settingCount(); ++i ) {
		const terrain_3d_profile::t3dSetting_t &setting = terrain_3d_profile::XBOX360_EXPERIMENTAL_SETTINGS[i];
		Dictionary row;
		row["owner"] = t3dPerformanceProfileOwnerName( setting.owner );
		row["property"] = setting.pszProperty;
		row["key"] = t3dPerformanceProfileOwnerName( setting.owner ) + "." + setting.pszProperty;
		row["value"] = t3dPerformanceProfileValue( setting );
		settings[( int ) ( i )] = row;
	}
	manifest["settings"] = settings;
	return manifest;
}

Array T3DTerrain::getPerformanceProfileDiff( void ) const {
	Array differences;
	if ( m_performanceTarget != PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ) {
		return differences;
	}
	for ( const terrain_3d_profile::t3dSetting_t &setting : terrain_3d_profile::XBOX360_EXPERIMENTAL_SETTINGS ) {
		const Object *pOwner = this;
		if ( setting.owner == terrain_3d_profile::OWNER_MATERIAL ) {
			if ( m_pMaterial.is_null() ) {
				Dictionary row;
				row["owner"] = "T3DMaterial";
				row["property"] = setting.pszProperty;
				row["key"] = String( "T3DMaterial." ) + setting.pszProperty;
				row["current"] = Variant();
				row["expected"] = t3dPerformanceProfileValue( setting );
				differences.push_back( row );
				continue;
			}
			pOwner = m_pMaterial.ptr();
		}
		const Variant current = pOwner->get( StringName( setting.pszProperty ) );
		if ( t3dPerformanceProfileValueMatches( current, setting ) ) {
			continue;
		}
		Dictionary row;
		row["owner"] = t3dPerformanceProfileOwnerName( setting.owner );
		row["property"] = setting.pszProperty;
		row["key"] = t3dPerformanceProfileOwnerName( setting.owner ) + "." + setting.pszProperty;
		row["current"] = current;
		row["expected"] = t3dPerformanceProfileValue( setting );
		differences.push_back( row );
	}
	return differences;
}

void T3DTerrain::resetPerformanceProfile( void ) {
	if ( m_performanceTarget != PERFORMANCE_TARGET_CUSTOM ) {
		setPerformanceTarget( m_performanceTarget );
	}
}

