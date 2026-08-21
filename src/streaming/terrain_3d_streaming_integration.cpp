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


bool T3DTerrain::isEditorAuthoringModeInternal( void ) const {
	return terrain3dIsEditorAuthoringMode( IS_EDITOR,
		( int ) ( m_editorExecutionMode ) );
}

bool T3DTerrain::isAuthoringResidencyReadbackSafe( void ) const {
	if ( !isEditorAuthoringModeInternal() ) {
		return true;
	}
	if ( !m_bInitialized || !m_pData || !m_pData->isAuthoringGpuReadbackSafe() ) {
		return false;
	}
	if ( m_pNodeStreamer && effectiveDynamicNodeStreamingEnabled() && !m_pNodeStreamer->isResidencyIdle() ) {
		return false;
	}
	if ( m_pLightmapStreamer && m_bLightmapStreamingEnabled && !m_pLightmapStreamer->isResidencyIdle() ) {
		return false;
	}
	return true;
}

bool T3DTerrain::effectiveGpuRegionCacheEnabled( void ) const {
	// GPU paging is authoring-safe and remains essential when inspecting large
	// terrain datasets. Editor mode only pins mutable source and object data.
	return terrain3dIsGpuPagingEffectivelyEnabled( m_bGpuRegionCacheEnabled );
}

bool T3DTerrain::effectiveCpuRegionCacheEnabled( void ) const {
	return terrain3dIsStreamingEffectivelyEnabled( m_bCpuRegionCacheEnabled, IS_EDITOR,
		( int ) ( m_editorExecutionMode ) );
}

bool T3DTerrain::effectiveInstanceRegionCacheEnabled( void ) const {
	return terrain3dIsStreamingEffectivelyEnabled( m_bInstanceRegionCacheEnabled, IS_EDITOR,
		( int ) ( m_editorExecutionMode ) );
}

bool T3DTerrain::effectiveInstancerCellStreamingEnabled( void ) const {
	return terrain3dIsStreamingEffectivelyEnabled( m_bInstancerCellStreamingEnabled, IS_EDITOR,
		( int ) ( m_editorExecutionMode ) );
}

bool T3DTerrain::effectiveDynamicNodeStreamingEnabled( void ) const {
	// Authoring still uses the node streamer, but the unified state map marks every
	// authored region ACTIVE so all authored nodes are loaded and pinned.
	return m_bDynamicNodeStreamingEnabled;
}

void T3DTerrain::invalidateRegionStreamingPolicy( void ) {
	m_bRegionStreamingDirty = true;
}

t3dregionStreamingState_t T3DTerrain::getRegionStreamingStateInternal( const Vector2i &vLocation ) const {
	if ( !m_pData || !m_pData->isRegionLocationValidInternal( vLocation ) ) {
		return TERRAIN3D_REGION_STREAMING_UNLOADED;
	}
	const int map_index = T3DData::getRegionMapIndex( vLocation );
	return map_index >= 0 ? ( t3dregionStreamingState_t ) ( m_szRegionStreamingStates[map_index] ) :
		TERRAIN3D_REGION_STREAMING_UNLOADED;
}

bool T3DTerrain::isRegionStreamingPrefetchInternal( const Vector2i &vLocation ) const {
	if ( !m_pData || !m_pData->isRegionLocationValidInternal( vLocation ) ) {
		return false;
	}
	const int map_index = T3DData::getRegionMapIndex( vLocation );
	return map_index >= 0 && m_szRegionStreamingPrefetch[map_index] != 0u;
}

void T3DTerrain::updateRegionStreamingPolicy(
	const Vector3 &rWorldPosition,
	const double fDelta,
	const bool bForce
) {
	if ( !m_pData || m_pData->getRegionMapSizeInternal() <= 0 ) {
		return;
	}
	const int map_size = m_pData->getRegionMapSizeInternal();
	const Vector2i raw_center = m_pData->getRegionLocation( rWorldPosition );
	const t3dstreamingCoord_t policy_center = terrain3dStreamingClampCenter(
		{ raw_center.x, raw_center.y }, map_size );
	const Vector2i center( policy_center.x, policy_center.y );
	const bool authoring = isEditorAuthoringModeInternal();
	if ( authoring && !bForce && !m_bRegionStreamingDirty && m_nRegionStreamingGeneration > 0u ) {
		return;
	}
	const bool target_moved = center != m_regionStreamingCenter;
	const bool world_moved = m_regionStreamingPreviousWorldPosition == V3_MAX ||
		rWorldPosition.distance_squared_to( m_regionStreamingPreviousWorldPosition ) > 0.000001f;
	if ( !bForce && !m_bRegionStreamingDirty && !target_moved && !world_moved &&
		std::abs( m_fRegionStreamingVelocityX ) < 0.001 && std::abs( m_fRegionStreamingVelocityZ ) < 0.001 ) {
		return;
	}

	double instant_x = 0.0;
	double instant_z = 0.0;
	const double delta = MAX( 0.0, fDelta );
	if ( m_regionStreamingPreviousWorldPosition != V3_MAX && delta > 0.000001 && m_nRegionSize > 0 && m_fVertexSpacing > 0.0f ) {
		const double region_world_size = ( double ) ( m_nRegionSize ) * ( double ) ( m_fVertexSpacing );
		instant_x = ( ( double ) ( rWorldPosition.x - m_regionStreamingPreviousWorldPosition.x ) / region_world_size ) / delta;
		instant_z = ( ( double ) ( rWorldPosition.z - m_regionStreamingPreviousWorldPosition.z ) / region_world_size ) / delta;
	}
	const t3dregionStreamingPrediction_t prediction = terrain3dStreamingPredict(
		{ center.x, center.y }, map_size, instant_x, instant_z,
		m_fRegionStreamingVelocityX, m_fRegionStreamingVelocityZ, delta, m_regionStreamingConfig );
	m_fRegionStreamingVelocityX = prediction.fVelocityXRegionsPerSecond;
	m_fRegionStreamingVelocityZ = prediction.fVelocityYRegionsPerSecond;
	m_regionStreamingCenter = center;
	Vector2i prefetch_center( prediction.predictedCenter.x, prediction.predictedCenter.y );
	// Whole-region velocity prediction rounds to zero at ordinary traversal speeds.
	// Add a spatial lead near the current boundary so the entering Active/Hot edge
	// begins asynchronous preparation before the discrete center changes.
	if ( !authoring && m_nRegionSize > 0 && m_fVertexSpacing > 0.0f ) {
		const double region_world_size = ( double ) ( m_nRegionSize ) * ( double ) ( m_fVertexSpacing );
		const double grid_x = ( double ) ( rWorldPosition.x ) / region_world_size;
		const double grid_z = ( double ) ( rWorldPosition.z ) / region_world_size;
		const double local_x = grid_x - std::floor( grid_x );
		const double local_z = grid_z - std::floor( grid_z );
		constexpr double boundary_fraction = 0.25;
		if ( m_fRegionStreamingVelocityX > 0.001 && local_x >= 1.0 - boundary_fraction ) {
			prefetch_center.x = MAX( prefetch_center.x, center.x + 1 );
		} else if ( m_fRegionStreamingVelocityX < -0.001 && local_x <= boundary_fraction ) {
			prefetch_center.x = MIN( prefetch_center.x, center.x - 1 );
		}
		if ( m_fRegionStreamingVelocityZ > 0.001 && local_z >= 1.0 - boundary_fraction ) {
			prefetch_center.y = MAX( prefetch_center.y, center.y + 1 );
		} else if ( m_fRegionStreamingVelocityZ < -0.001 && local_z <= boundary_fraction ) {
			prefetch_center.y = MIN( prefetch_center.y, center.y - 1 );
		}
		const t3dstreamingCoord_t clamped_prefetch = terrain3dStreamingClampCenter(
			{ prefetch_center.x, prefetch_center.y }, map_size );
		prefetch_center = Vector2i( clamped_prefetch.x, clamped_prefetch.y );
	}
	m_regionStreamingPredictedCenter = prefetch_center;
	m_regionStreamingPreviousWorldPosition = rWorldPosition;

	std::fill_n( m_szRegionStreamingStates, TERRAIN3D_MAX_REGION_SLOTS,
		( uint8_t ) ( TERRAIN3D_REGION_STREAMING_UNLOADED ) );
	std::fill_n( m_szRegionStreamingPrefetch, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	m_nRegionStreamingActiveCount = 0;
	m_nRegionStreamingHotCount = 0;
	m_nRegionStreamingColdCount = 0;
	m_nRegionStreamingUnloadedCount = 0;
	m_nRegionStreamingPrefetchCount = 0;

	const Vector2i *locations = m_pData->getRegionSlotsInternal();
	const int count = m_pData->getRegionSlotCountInternal();
	for ( int index = 0; index < count; ++index ) {
		const Vector2i location = locations[index];
		if ( location == V2I_MAX || !m_pData->hasRegion( location ) ) {
			continue;
		}
		const int map_index = T3DData::getRegionMapIndex( location );
		if ( map_index < 0 ) {
			continue;
		}
		t3dregionStreamingState_t state = TERRAIN3D_REGION_STREAMING_ACTIVE;
		if ( !authoring ) {
			state = terrain3dStreamingClassify( { location.x, location.y },
				{ center.x, center.y }, m_regionStreamingConfig );
		}
		m_szRegionStreamingStates[map_index] = ( uint8_t ) ( state );
		switch ( state ) {
			case TERRAIN3D_REGION_STREAMING_ACTIVE: ++m_nRegionStreamingActiveCount; break;
			case TERRAIN3D_REGION_STREAMING_HOT: ++m_nRegionStreamingHotCount; break;
			case TERRAIN3D_REGION_STREAMING_COLD: ++m_nRegionStreamingColdCount; break;
			case TERRAIN3D_REGION_STREAMING_UNLOADED:
			default: ++m_nRegionStreamingUnloadedCount; break;
		}

		if ( !authoring && m_regionStreamingConfig.bPredictionEnabled &&
			m_regionStreamingPredictedCenter != center && state != TERRAIN3D_REGION_STREAMING_ACTIVE ) {
			const t3dregionStreamingState_t predicted_state = terrain3dStreamingClassify(
				{ location.x, location.y }, { m_regionStreamingPredictedCenter.x, m_regionStreamingPredictedCenter.y },
				m_regionStreamingConfig );
			if ( predicted_state > state ) {
				m_szRegionStreamingPrefetch[map_index] = 1u;
				++m_nRegionStreamingPrefetchCount;
			}
		}
	}
	++m_nRegionStreamingGeneration;
	m_bRegionStreamingDirty = false;
}

Dictionary T3DTerrain::getRegionStreamingStats( void ) const {
	Dictionary stats;
	stats["active"] = m_nRegionStreamingActiveCount;
	stats["hot"] = m_nRegionStreamingHotCount;
	stats["cold"] = m_nRegionStreamingColdCount;
	stats["unloaded"] = m_nRegionStreamingUnloadedCount;
	stats["predicted_promotions"] = m_nRegionStreamingPrefetchCount;
	stats["center"] = m_regionStreamingCenter;
	stats["predicted_center"] = m_regionStreamingPredictedCenter;
	stats["velocity_regions_per_second"] = Vector2( ( real_t ) ( m_fRegionStreamingVelocityX ),
		( real_t ) ( m_fRegionStreamingVelocityZ ) );
	stats["generation"] = ( int64_t ) ( m_nRegionStreamingGeneration );
	stats["authoring_bounded_gpu_residency"] = isEditorAuthoringModeInternal();
	return stats;
}

Dictionary T3DTerrain::getRegionStreamingConfigurationStatus( void ) const {
	Dictionary status;
	const int near_lods = MAX( 0, m_nRuntimeNearShaderLods );
	const int middle_lods = MAX( 0, m_nRuntimeNearShaderLods + m_nRuntimeMiddleShaderLods );
	const int required_active_radius = terrain3dGpuCacheCalculateFootprintRadiusRegions(
		m_nMeshSize, near_lods, ( double ) ( m_fVertexSpacing ), ( int ) ( m_nRegionSize ) );
	const int required_hot_radius = terrain3dGpuCacheCalculateFootprintRadiusRegions(
		m_nMeshSize, middle_lods, ( double ) ( m_fVertexSpacing ), ( int ) ( m_nRegionSize ) );
	const bool active_range_valid = m_regionStreamingConfig.activeRadius >= required_active_radius;
	const bool hot_range_valid = m_regionStreamingConfig.hotRadius >= required_hot_radius;
	const int active_side = m_regionStreamingConfig.activeRadius * 2 + 1;
	const int hot_side = m_regionStreamingConfig.hotRadius * 2 + 1;
	const int authored_regions = m_pData ? m_pData->getRegionCount() : TERRAIN3D_MAX_REGION_SLOTS;
	const int required_active_surface_pages = MIN( authored_regions, active_side * active_side );
	const int required_hot_height_pages = MIN( authored_regions, hot_side * hot_side );
	int surface_capacity = isEditorAuthoringModeInternal() ? TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS : TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS;
	int height_capacity = TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS;
	if ( m_pData ) {
		if ( m_pData->getGpuRegionCacheCapacity() > 0 ) {
			surface_capacity = m_pData->getGpuRegionCacheCapacity();
		}
		const Dictionary height_stats = m_pData->getMiddleHeightCacheStats();
		const int mapped_height_capacity = ( int ) ( height_stats.get( "mapped_capacity", 0 ) );
		if ( mapped_height_capacity > 0 ) {
			height_capacity = mapped_height_capacity;
		}
	}
	// Authoring deliberately keeps a bounded physical detail pool. Correctness
	// comes from authoritative CPU data + the far atlas, not from pinning every
	// authored region into the GPU texture arrays.
	const bool active_surface_budget_valid = required_active_surface_pages <= surface_capacity;
	const bool hot_height_budget_valid = isEditorAuthoringModeInternal() || required_hot_height_pages <= height_capacity;
	status["valid"] = active_range_valid && hot_range_valid && active_surface_budget_valid && hot_height_budget_valid &&
		terrain3dStreamingConfigIsValid( m_regionStreamingConfig );
	status["active_radius"] = m_regionStreamingConfig.activeRadius;
	status["hot_radius"] = m_regionStreamingConfig.hotRadius;
	status["cold_radius"] = m_regionStreamingConfig.coldRadius;
	status["unload_guard"] = m_regionStreamingConfig.unloadGuard;
	status["effective_cold_radius_with_guard"] = m_regionStreamingConfig.coldRadius + m_regionStreamingConfig.unloadGuard;
	status["required_active_radius_for_near"] = required_active_radius;
	status["required_hot_radius_for_middle"] = required_hot_radius;
	status["near_coverage_valid"] = active_range_valid;
	status["middle_coverage_valid"] = hot_range_valid;
	status["required_active_surface_pages"] = required_active_surface_pages;
	status["active_surface_capacity"] = surface_capacity;
	status["active_surface_budget_valid"] = active_surface_budget_valid;
	status["required_hot_height_pages"] = required_hot_height_pages;
	status["hot_height_capacity"] = height_capacity;
	status["hot_height_budget_valid"] = hot_height_budget_valid;
	return status;
}

void T3DTerrain::validateRegionStreamingConfiguration( const bool bReport ) const {
	if ( !bReport ) {
		return;
	}
	const Dictionary status = getRegionStreamingConfigurationStatus();
	if ( !( bool ) ( status["valid"] ) ) {
		if ( !( bool ) ( status["near_coverage_valid"] ) || !( bool ) ( status["middle_coverage_valid"] ) ) {
			UtilityFunctions::push_warning( "Terrain3D: Region streaming ranges do not cover the configured Near/Middle render footprints. Increase Active/Hot Radius or reduce the corresponding shader LOD ranges." );
		}
		if ( !( bool ) ( status["active_surface_budget_valid"] ) || !( bool ) ( status["hot_height_budget_valid"] ) ) {
			UtilityFunctions::push_warning( vformat(
				"Terrain3D: Region streaming budgets cannot satisfy the visible ACTIVE/HOT footprint (ACTIVE surface %d/%d pages, HOT height %d/%d pages). Increase the corresponding GPU budgets or reduce the tier radii.",
				( int ) ( status["required_active_surface_pages"] ), ( int ) ( status["active_surface_capacity"] ),
				( int ) ( status["required_hot_height_pages"] ), ( int ) ( status["hot_height_capacity"] ) ) );
		}
	}
}

void T3DTerrain::setStreamingActiveRadius( const int nRadius ) {
	m_regionStreamingConfig.activeRadius = CLAMP( nRadius, 0, 31 );
	m_regionStreamingConfig.hotRadius = MAX( m_regionStreamingConfig.hotRadius, m_regionStreamingConfig.activeRadius );
	m_regionStreamingConfig.coldRadius = MAX( m_regionStreamingConfig.coldRadius, m_regionStreamingConfig.hotRadius );
	invalidateRegionStreamingPolicy();
	validateRegionStreamingConfiguration();
}

void T3DTerrain::setStreamingHotRadius( const int nRadius ) {
	m_regionStreamingConfig.hotRadius = CLAMP( nRadius, m_regionStreamingConfig.activeRadius, 31 );
	m_regionStreamingConfig.coldRadius = MAX( m_regionStreamingConfig.coldRadius, m_regionStreamingConfig.hotRadius );
	invalidateRegionStreamingPolicy();
	validateRegionStreamingConfiguration();
}

void T3DTerrain::setStreamingColdRadius( const int nRadius ) {
	m_regionStreamingConfig.coldRadius = CLAMP( nRadius, m_regionStreamingConfig.hotRadius, 31 );
	invalidateRegionStreamingPolicy();
}

void T3DTerrain::setStreamingUnloadGuard( const int nRegions ) {
	m_regionStreamingConfig.unloadGuard = CLAMP( nRegions, 0, 4 );
	invalidateRegionStreamingPolicy();
}

void T3DTerrain::setStreamingPredictionEnabled( const bool bEnabled ) {
	m_regionStreamingConfig.bPredictionEnabled = bEnabled;
	invalidateRegionStreamingPolicy();
}

void T3DTerrain::setStreamingPredictionLookahead( const real_t fSeconds ) {
	m_regionStreamingConfig.fPredictionLookaheadSeconds = MAX( ( real_t ) ( 0.0 ), fSeconds );
	invalidateRegionStreamingPolicy();
}

void T3DTerrain::setStreamingPredictionMaximumRegions( const int nRegions ) {
	m_regionStreamingConfig.predictionMaximumRegions = MAX( 0, nRegions );
	invalidateRegionStreamingPolicy();
}

void T3DTerrain::applyStreamingPolicy( const bool bForceResidency ) {
	LOG( T3DTerrain::INFO, "[streaming-policy] Applying policy: force_residency=", bForceResidency,
		"; editor_mode=", ( int ) ( m_editorExecutionMode ),
		"; gpu_cache=", effectiveGpuRegionCacheEnabled(),
		"; cpu_cache=", effectiveCpuRegionCacheEnabled(),
		"; instance_cache=", effectiveInstanceRegionCacheEnabled(),
		"; instancer_cells=", effectiveInstancerCellStreamingEnabled(),
		"; dynamic_nodes=", effectiveDynamicNodeStreamingEnabled() );
	if ( m_pData ) {
		// Restore mutable authoring payloads before refreshing GPU residency.
		m_pData->configureCpuRegionCache( effectiveCpuRegionCacheEnabled(), m_nEffectiveCpuRegionCacheBudgetMb,
			m_nCpuRegionLoadBudgetLayers, m_nCpuRegionLoadBudgetMb );
		m_pData->configureInstanceRegionCache( effectiveInstanceRegionCacheEnabled(),
			m_nEffectiveInstanceRegionCacheBudgetMb, m_nInstanceRegionLoadBudgetPages,
			m_nInstanceRegionLoadBudgetMb );
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
		if ( m_bInitialized && bForceResidency ) {
			const Vector3 target = getClipmapTargetPosition();
			updateRegionStreamingPolicy( target, 0.0, true );
			m_pData->updateRegionResidency( target, true );
		}
	}
	if ( m_pInstancer && effectiveInstancerCellStreamingEnabled() ) {
		m_pInstancer->setCellStreamingEnabled( effectiveInstancerCellStreamingEnabled() );
	}
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->setEnabled( effectiveDynamicNodeStreamingEnabled() );
	}
}

int T3DTerrain::getTotalClipmapLevels( void ) const {
	return CLAMP( m_nMeshLods + m_tessellationLevel, 1, TERRAIN3D_MAX_CLIPMAP_LEVELS );
}

real_t T3DTerrain::lodCountToRange( const int nLodCount ) const {
	if ( nLodCount <= 0 ) {
		return 0.0f;
	}
	const real_t fTessellationScale = ( real_t ) ( ( uint64_t ) ( 1 ) << ( uint64_t ) ( CLAMP( m_tessellationLevel, 0, 6 ) ) );
	const real_t fBaseExtent = ( real_t ) ( m_nMeshSize * 2 + 6 ) * m_fVertexSpacing / fTessellationScale;
	return fBaseExtent * ( real_t ) ( ( uint64_t ) ( 1 ) << ( uint64_t ) ( CLAMP( nLodCount - 1, 0, 15 ) ) );
}

int T3DTerrain::rangeToLodCount( const real_t fRangeM ) const {
	if ( fRangeM <= 0.0f ) {
		return 0;
	}
	const int total_levels = getTotalClipmapLevels();
	for ( int count = 1; count <= total_levels; ++count ) {
		if ( lodCountToRange( count ) >= fRangeM ) {
			return count;
		}
	}
	return total_levels;
}

void T3DTerrain::normalizeRuntimeTierLayout( const bool bSyncDetailLods ) {
	const int total_levels = getTotalClipmapLevels();
	m_nRuntimeNearShaderLods = CLAMP( m_nRuntimeNearShaderLods, 0, total_levels );
	m_nRuntimeMiddleShaderLods = CLAMP( m_nRuntimeMiddleShaderLods, 0,
		total_levels - m_nRuntimeNearShaderLods );
	m_nGpuRegionDetailLods = CLAMP( m_nGpuRegionDetailLods, 0, total_levels );
	// A detail value of zero intentionally means every clipmap level. It already
	// covers the near tier and must not be rewritten to the near count.
	if ( bSyncDetailLods && m_bRuntimeSyncDetailLods && m_nGpuRegionDetailLods > 0 &&
		m_nRuntimeNearShaderLods > m_nGpuRegionDetailLods ) {
		m_nGpuRegionDetailLods = m_nRuntimeNearShaderLods;
	}
	if ( m_pData && m_pData->isInitializedFor( this ) && m_pData->getRegionCount() > 0 && !m_bDeferGpuRegionCacheReconfigure ) {
		const int effective_detail_lods = m_nGpuRegionDetailLods <= 0 ? total_levels : m_nGpuRegionDetailLods;
		const int required_pages = m_pData->calculateGpuRegionRequiredCorePagesForLods( effective_detail_lods );
		const int capacity = m_pData->calculateGpuRegionCapacityForLods( effective_detail_lods );
		if ( required_pages > capacity ) {
			int supported_lods = 0;
			for ( int lods = 1; lods <= total_levels; ++lods ) {
				const int candidate_required = m_pData->calculateGpuRegionRequiredCorePagesForLods( lods );
				const int candidate_capacity = m_pData->calculateGpuRegionCapacityForLods( lods );
				if ( candidate_required > candidate_capacity ) {
					break;
				}
				supported_lods = lods;
			}
			m_nGpuRegionDetailLods = supported_lods;
			if ( m_bRuntimeSyncDetailLods && m_nRuntimeNearShaderLods > supported_lods ) {
				m_nRuntimeNearShaderLods = supported_lods;
				m_nRuntimeMiddleShaderLods = CLAMP( m_nRuntimeMiddleShaderLods, 0,
					total_levels - m_nRuntimeNearShaderLods );
			}
			LOG( T3DTerrain::WARN, "Clamped detailed terrain range to ", supported_lods,
				" clipmap levels because the configured GPU page capacity cannot cover the requested footprint" );
		}
	}
}

void T3DTerrain::applyRuntimeTierLayout( const bool bForceFullSnap ) {
	normalizeRuntimeTierLayout( true );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierLayout( m_nRuntimeNearShaderLods, m_nRuntimeMiddleShaderLods,
			m_nRuntimeClipmapUpdateBudget, bForceFullSnap );
	}
	if ( m_pData && effectiveGpuRegionCacheEnabled() && !m_bDeferGpuRegionCacheReconfigure ) {
		m_pData->configureGpuRegionCache( true, m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setEditorRuntimeStreamingPreview( const bool bEnabled ) {
	if ( bEnabled ) {
		setEditorNodeStreamingPreview( false );
	}
	setEditorExecutionMode( bEnabled ? EDITOR_EXECUTION_RUNTIME_PROFILE : EDITOR_EXECUTION_AUTHORING );
}

void T3DTerrain::setEditorNodeStreamingPreview( const bool bEnabled ) {
	if ( m_bEditorNodeStreamingPreview == bEnabled ) {
		return;
	}
	m_bEditorNodeStreamingPreview = bEnabled;
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->invalidateResidency();
	}
	emit_signal( "editor_execution_mode_changed", ( int ) ( m_editorExecutionMode ) );
}

void T3DTerrain::setEditorHlodPreview( const bool bEnabled ) {
	if ( m_bEditorHlodPreview == bEnabled ) { return; }
	m_bEditorHlodPreview = bEnabled;
	if ( m_pHlodStreamer ) {
		if ( bEnabled ) { m_pHlodStreamer->reloadManifest(); }
		else { m_pHlodStreamer->suspend(); }
	}
	if ( m_pNodeStreamer ) { m_pNodeStreamer->invalidateResidency(); }
	emit_signal( "editor_execution_mode_changed", ( int ) m_editorExecutionMode );
}

void T3DTerrain::setEditorExecutionMode( const t3deditorExecutionMode_t mode ) {
	if ( !terrain3dEditorExecutionModeIsValid( ( int ) ( mode ) ) ) {
		return;
	}
	if ( mode == EDITOR_EXECUTION_RUNTIME_PROFILE ) {
		m_bEditorNodeStreamingPreview = false;
		m_bEditorHlodPreview = false;
	}
	if ( m_editorExecutionMode == mode ) {
		return;
	}
	m_editorExecutionMode = mode;
	applyStreamingPolicy( true );
	if ( m_pAssets.is_valid() && m_pAssets->isInitializedFor( this ) ) {
		// Runtime Profile is a real execution-mode transition, not just a shader
		// preview. Rebuild the exact bounded/full material representation selected
		// by the new mode after GPU residency has established its priority set.
		m_pAssets->configureRuntimeMaterialCache();
	}
	if ( m_pCollision ) {
		m_pCollision->build();
	}
	if ( m_pMaterial.is_valid() && m_pMaterial->isInitialized() ) {
		m_pMaterial->update( T3DMaterial::FULL_REBUILD );
	}
	if ( m_bInitialized ) {
		const Vector3 target = getClipmapTargetPosition();
		if ( m_pNodeStreamer && effectiveDynamicNodeStreamingEnabled() ) {
			m_pNodeStreamer->updateResidency( target, true );
		}
		if ( m_pLightmapStreamer && m_bLightmapStreamingEnabled ) {
			m_pLightmapStreamer->updateResidency( target, true );
		}
		if ( m_pHlodStreamer && m_bHlodEnabled && mode == EDITOR_EXECUTION_RUNTIME_PROFILE ) {
			m_pHlodStreamer->updateResidency( target, true );
		}
		setupTerrainMesher();
	}
	notify_property_list_changed();
	emit_signal( "editor_execution_mode_changed", ( int ) ( m_editorExecutionMode ) );
}

void T3DTerrain::setGpuRegionCacheEnabled( const bool bEnabled ) {
	if ( m_bGpuRegionCacheEnabled == bEnabled ) {
		return;
	}
	m_bGpuRegionCacheEnabled = bEnabled;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bDeferGpuRegionCacheReconfigure ) {
		return;
	}
	if ( m_pData ) {
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
		if ( m_bInitialized && effectiveGpuRegionCacheEnabled() ) {
			m_pData->updateGpuRegionResidency( getClipmapTargetPosition(), false );
		}
	}
}

void T3DTerrain::setGpuRegionCacheBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 1, 128 );
	if ( m_nGpuRegionCacheBudgetMb == budget_mb ) {
		return;
	}
	m_nGpuRegionCacheBudgetMb = budget_mb;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( !m_bGpuRegionCacheBudgetAuto && m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void T3DTerrain::setGpuRegionUploadBudgetLayers( const int nLayers ) {
	const int layers = CLAMP( nLayers, 1, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	if ( m_nGpuRegionUploadBudgetLayers == layers ) {
		return;
	}
	m_nGpuRegionUploadBudgetLayers = layers;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setGpuRegionUploadBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 1, 64 );
	if ( m_nGpuRegionUploadBudgetMb == budget_mb ) {
		return;
	}
	m_nGpuRegionUploadBudgetMb = budget_mb;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setGpuRegionSparePages( const int nPages ) {
	const int pages = CLAMP( nPages, TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS,
		TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS - 1 );
	if ( m_nGpuRegionSparePages == pages ) {
		return;
	}
	m_nGpuRegionSparePages = pages;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setGpuRegionRetireFrames( const int nFrames ) {
	const int frames = CLAMP( nFrames, 1, 16 );
	if ( m_nGpuRegionRetireFrames == frames ) {
		return;
	}
	m_nGpuRegionRetireFrames = frames;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setGpuRegionDetailLods( const int nLods ) {
	const int lods = CLAMP( nLods, 0, getTotalClipmapLevels() );
	if ( m_nGpuRegionDetailLods == lods ) {
		return;
	}
	m_nGpuRegionDetailLods = lods;
	refreshPerformanceProfileConformity();
	normalizeRuntimeTierLayout( false );
	if ( m_bRuntimeSyncDetailLods && m_nGpuRegionDetailLods > 0 &&
		m_nRuntimeNearShaderLods > m_nGpuRegionDetailLods ) {
		m_nRuntimeNearShaderLods = m_nGpuRegionDetailLods;
		m_nRuntimeMiddleShaderLods = CLAMP( m_nRuntimeMiddleShaderLods, 0,
			getTotalClipmapLevels() - m_nRuntimeNearShaderLods );
	}
	applyRuntimeTierLayout( true );
	notify_property_list_changed();
}

void T3DTerrain::setGpuRegionDetailRangeEndM( const real_t fRangeM ) {
	setGpuRegionDetailLods( rangeToLodCount( MAX( fRangeM, ( real_t ) ( 0.0f ) ) ) );
}

real_t T3DTerrain::getGpuRegionDetailRangeEndM( void ) const {
	const int detail_lods = m_nGpuRegionDetailLods <= 0 ? getTotalClipmapLevels() : m_nGpuRegionDetailLods;
	return lodCountToRange( detail_lods );
}

int T3DTerrain::getGpuRegionMaxSupportedDetailLods( void ) const {
	if ( !m_pData ) {
		return getTotalClipmapLevels();
	}
	int supported = 0;
	for ( int lods = 1; lods <= getTotalClipmapLevels(); ++lods ) {
		if ( m_pData->calculateGpuRegionRequiredCorePagesForLods( lods ) >
			m_pData->calculateGpuRegionCapacityForLods( lods ) ) {
			break;
		}
		supported = lods;
	}
	return supported;
}

real_t T3DTerrain::getGpuRegionMaxSupportedDetailRangeM( void ) const {
	return lodCountToRange( getGpuRegionMaxSupportedDetailLods() );
}

int T3DTerrain::getGpuRegionRequiredCorePages( void ) const {
	return m_pData ? m_pData->getGpuRegionRequiredCorePages() : 0;
}

int T3DTerrain::getGpuRegionMappedCapacity( void ) const {
	return m_pData ? m_pData->getGpuRegionCacheCapacity() : 0;
}

bool T3DTerrain::isGpuRegionDetailRangeValid( void ) const {
	if ( !m_pData || !m_bGpuRegionCacheEnabled ) {
		return true;
	}
	return getGpuRegionRequiredCorePages() <= getGpuRegionMappedCapacity();
}

void T3DTerrain::setGpuRegionGuardRegions( const int nRegions ) {
	// Serialized compatibility only. Spatial guard behavior is owned by
	// streaming_unload_guard in the unified region policy.
	m_nGpuRegionGuardRegions = CLAMP( nRegions, 0, 4 );
}

void T3DTerrain::setGpuRegionPrefetchRows( const int nRows ) {
	// Serialized compatibility only. Prediction is owned by the unified streaming
	// policy and supports diagonal, speed-sensitive lookahead.
	m_nGpuRegionPrefetchRows = CLAMP( nRows, 0, 4 );
}

void T3DTerrain::setGpuRegionHysteresisTicks( const int nTicks ) {
	const int ticks = CLAMP( nTicks, 0, 240 );
	if ( m_nGpuRegionHysteresisTicks == ticks ) {
		return;
	}
	m_nGpuRegionHysteresisTicks = ticks;
	refreshPerformanceProfileConformity();
	if ( m_pData && effectiveGpuRegionCacheEnabled() && m_bInitialized ) {
		m_pData->updateGpuRegionResidency( getClipmapTargetPosition(), true );
	}
}

void T3DTerrain::setGpuRegionColorResolutionDivisor( const int nDivisor ) {
	const int divisor = nDivisor <= 1 ? 1 : ( nDivisor <= 2 ? 2 : 4 );
	if ( m_nGpuRegionColorResolutionDivisor == divisor ) {
		return;
	}
	m_nGpuRegionColorResolutionDivisor = divisor;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setGpuRegionDerivedResolutionDivisor( const int nDivisor ) {
	const int divisor = nDivisor <= 1 ? 1 : ( nDivisor <= 2 ? 2 : 4 );
	if ( m_nGpuRegionDerivedResolutionDivisor == divisor ) {
		return;
	}
	m_nGpuRegionDerivedResolutionDivisor = divisor;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setGpuMiddleHeightResolutionDivisor( const int nDivisor ) {
	const int divisor = nDivisor <= 1 ? 1 : ( nDivisor <= 2 ? 2 : 4 );
	if ( m_nGpuMiddleHeightResolutionDivisor == divisor ) {
		return;
	}
	m_nGpuMiddleHeightResolutionDivisor = divisor;
	refreshPerformanceProfileConformity();
	markMemoryBudgetDirty();
	if ( m_pData && m_bInitialized ) {
		m_pData->resetRuntimeStreamingCaches();
	}
}

void T3DTerrain::setEditorGpuRegionCachePages( const int nPages ) {
	const int pages = CLAMP( nPages, 16, TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS );
	if ( m_nEditorGpuRegionCachePages == pages ) {
		return;
	}
	m_nEditorGpuRegionCachePages = pages;
	refreshPerformanceProfileConformity();
	markMemoryBudgetDirty();
	if ( m_pData && m_bInitialized && isEditorAuthoringMode() ) {
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setRuntimeMaterialStreamingEnabled( const bool bEnabled ) {
	if ( m_bRuntimeMaterialStreamingEnabled == bEnabled ) {
		return;
	}
	m_bRuntimeMaterialStreamingEnabled = bEnabled;
	refreshPerformanceProfileConformity();
	markMemoryBudgetDirty();
	if ( m_pAssets.is_valid() && m_bInitialized ) {
		m_pAssets->configureRuntimeMaterialCache();
	}
}

void T3DTerrain::setRuntimeMaterialCacheLayers( const int nLayers ) {
	const int layers = CLAMP( nLayers, 4, TERRAIN3D_MAX_TEXTURE_ASSETS );
	if ( m_nRuntimeMaterialCacheLayers == layers ) {
		return;
	}
	m_nRuntimeMaterialCacheLayers = layers;
	refreshPerformanceProfileConformity();
	markMemoryBudgetDirty();
	if ( m_pAssets.is_valid() && m_bInitialized ) {
		m_pAssets->configureRuntimeMaterialCache();
	}
}

void T3DTerrain::setRuntimeMaterialUploadBudgetLayers( const int nLayers ) {
	const int layers = CLAMP( nLayers, 1, TERRAIN3D_MAX_TEXTURE_ASSETS );
	if ( m_nRuntimeMaterialUploadBudgetLayers == layers ) {
		return;
	}
	m_nRuntimeMaterialUploadBudgetLayers = layers;
	refreshPerformanceProfileConformity();
}

void T3DTerrain::setRuntimeReleaseSourceMaterialTextures( const bool bEnabled ) {
	if ( m_bRuntimeReleaseSourceMaterialTextures == bEnabled ) {
		return;
	}
	m_bRuntimeReleaseSourceMaterialTextures = bEnabled;
	refreshPerformanceProfileConformity();
	if ( bEnabled && m_pAssets.is_valid() && m_bInitialized ) {
		m_pAssets->configureRuntimeMaterialCache();
	}
}

void T3DTerrain::setGpuFarQualityDivisor( const int nDivisor ) {
	const int nDivisorValue = nDivisor <= 1 ? 1 : ( nDivisor <= 2 ? 2 : ( nDivisor <= 4 ? 4 : 8 ) );
	if ( m_nGpuFarQualityDivisor == nDivisorValue ) {
		return;
	}
	m_nGpuFarQualityDivisor = nDivisorValue;
	refreshPerformanceProfileConformity();
	markMemoryBudgetDirty();
	if ( m_pData ) {
		// Far quality is a surface-quality control. Geometry follows the first Far
		// mesh lattice and must remain stable while surface quality changes.
		m_pData->invalidateGpuFarSurface();
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
	if ( m_pMaterial.is_valid() && m_pMaterial->isInitializedFor( this ) ) {
		m_pMaterial->update( T3DMaterial::UNIFORMS_ONLY );
	}
}

void T3DTerrain::setFarMaterialResolution( const int nResolution ) {
	const int resolution = nResolution <= 64 ? 64 : ( nResolution <= 128 ? 128 : 256 );
	if ( m_nFarMaterialResolution == resolution ) {
		return;
	}
	m_nFarMaterialResolution = resolution;
	if ( m_pAssets.is_valid() ) {
		m_pAssets->invalidateRuntimeMaterialCache();
	}
	refreshPerformanceProfileConformity();
}

void T3DTerrain::setFarControlResolution( const int nResolution ) {
	const int resolution = nResolution <= 64 ? 64 : ( nResolution <= 128 ? 128 : 256 );
	if ( m_nFarControlResolution == resolution ) {
		return;
	}
	m_nFarControlResolution = resolution;
	markMemoryBudgetDirty();
	if ( m_pData ) {
		m_pData->invalidateGpuFarSurface();
	}
	refreshPerformanceProfileConformity();
}

int T3DTerrain::getGpuFarHeightAtlasSide( void ) const {
	// Geometry quality follows the first Far mesh lattice. Surface-quality settings
	// are intentionally excluded so they can never erase representable terrain shape.
	const int first_far_lod = CLAMP( m_nRuntimeNearShaderLods + m_nRuntimeMiddleShaderLods, 0, 30 );
	const int source_spacing = 1 << first_far_lod;
	const int required_side = MAX( 1, TERRAIN3D_ACTIVE_WORLD_TEXELS / source_spacing );
	return CLAMP( required_side, TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE, TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
}

int T3DTerrain::getGpuFarSurfaceAtlasSide( void ) const {
	return m_nFarControlResolution * TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
}

uint64_t T3DTerrain::getGpuFarAtlasBytes( void ) const {
	const uint64_t height_side = ( uint64_t ) ( getGpuFarHeightAtlasSide() );
	const uint64_t surface_side = ( uint64_t ) ( getGpuFarSurfaceAtlasSide() );
	return height_side * height_side * 2u + surface_side * surface_side * 4u;
}

void T3DTerrain::setGpuRegionCookedPagesEnabled( const bool bEnabled ) {
	if ( m_bGpuRegionCookedPagesEnabled == bEnabled ) {
		return;
	}
	m_bGpuRegionCookedPagesEnabled = bEnabled;
	refreshPerformanceProfileConformity();
}

void T3DTerrain::setGpuRegionCookOnSave( const bool bEnabled ) {
	if ( m_bGpuRegionCookOnSave == bEnabled ) {
		return;
	}
	m_bGpuRegionCookOnSave = bEnabled;
	refreshPerformanceProfileConformity();
}

void T3DTerrain::setCpuRegionCacheEnabled( const bool bEnabled ) {
	if ( m_bCpuRegionCacheEnabled == bEnabled ) {
		return;
	}
	m_bCpuRegionCacheEnabled = bEnabled;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureCpuRegionCache( effectiveCpuRegionCacheEnabled(), m_nEffectiveCpuRegionCacheBudgetMb,
			m_nCpuRegionLoadBudgetLayers, m_nCpuRegionLoadBudgetMb );
		if ( m_bInitialized && effectiveCpuRegionCacheEnabled() ) {
			m_pData->updateCpuRegionResidency( getClipmapTargetPosition(), false );
		}
	}
}

void T3DTerrain::setCpuRegionCacheBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 1, 128 );
	if ( m_nCpuRegionCacheBudgetMb == budget_mb ) {
		return;
	}
	m_nCpuRegionCacheBudgetMb = budget_mb;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( !m_bCpuRegionCacheBudgetAuto && m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void T3DTerrain::setCpuRegionLoadBudgetLayers( const int nLayers ) {
	const int layers = CLAMP( nLayers, 1, TERRAIN3D_CPU_CACHE_MAX_LAYERS );
	if ( m_nCpuRegionLoadBudgetLayers == layers ) {
		return;
	}
	m_nCpuRegionLoadBudgetLayers = layers;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureCpuRegionCache( effectiveCpuRegionCacheEnabled(), m_nEffectiveCpuRegionCacheBudgetMb,
			m_nCpuRegionLoadBudgetLayers, m_nCpuRegionLoadBudgetMb );
	}
}

void T3DTerrain::setCpuRegionLoadBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 1, 64 );
	if ( m_nCpuRegionLoadBudgetMb == budget_mb ) {
		return;
	}
	m_nCpuRegionLoadBudgetMb = budget_mb;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureCpuRegionCache( effectiveCpuRegionCacheEnabled(), m_nEffectiveCpuRegionCacheBudgetMb,
			m_nCpuRegionLoadBudgetLayers, m_nCpuRegionLoadBudgetMb );
	}
}

void T3DTerrain::setInstanceRegionCacheEnabled( const bool bEnabled ) {
	if ( m_bInstanceRegionCacheEnabled == bEnabled ) {
		return;
	}
	m_bInstanceRegionCacheEnabled = bEnabled;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureInstanceRegionCache( effectiveInstanceRegionCacheEnabled(),
			m_nEffectiveInstanceRegionCacheBudgetMb, m_nInstanceRegionLoadBudgetPages,
			m_nInstanceRegionLoadBudgetMb );
	}
}

void T3DTerrain::setInstanceRegionCacheBudgetMb( const int nBudgetMb ) {
	const int budget = CLAMP( nBudgetMb, 1, 128 );
	if ( m_nInstanceRegionCacheBudgetMb == budget ) {
		return;
	}
	m_nInstanceRegionCacheBudgetMb = budget;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( !m_bInstanceRegionCacheBudgetAuto && m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void T3DTerrain::setInstanceRegionLoadBudgetPages( const int nPages ) {
	const int pages = CLAMP( nPages, 1, TERRAIN3D_INSTANCE_CACHE_MAX_PAGES );
	if ( m_nInstanceRegionLoadBudgetPages == pages ) {
		return;
	}
	m_nInstanceRegionLoadBudgetPages = pages;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureInstanceRegionCache( effectiveInstanceRegionCacheEnabled(),
			m_nEffectiveInstanceRegionCacheBudgetMb, m_nInstanceRegionLoadBudgetPages,
			m_nInstanceRegionLoadBudgetMb );
	}
}

void T3DTerrain::setInstanceRegionLoadBudgetMb( const int nBudgetMb ) {
	const int budget = CLAMP( nBudgetMb, 1, 64 );
	if ( m_nInstanceRegionLoadBudgetMb == budget ) {
		return;
	}
	m_nInstanceRegionLoadBudgetMb = budget;
	refreshPerformanceProfileConformity();
	if ( m_pData ) {
		m_pData->configureInstanceRegionCache( effectiveInstanceRegionCacheEnabled(),
			m_nEffectiveInstanceRegionCacheBudgetMb, m_nInstanceRegionLoadBudgetPages,
			m_nInstanceRegionLoadBudgetMb );
	}
}

void T3DTerrain::setInstancerCellSize( const int nCellSize ) {
	if ( nCellSize < TERRAIN3D_INSTANCE_MIN_CELL_SIZE || nCellSize > TERRAIN3D_INSTANCE_MAX_CELL_SIZE ||
		( nCellSize & ( nCellSize - 1 ) ) != 0 || ( int ) ( m_nRegionSize ) % nCellSize != 0 ) {
		LOG( T3DTerrain::ERROR, "Instancer cell size must be a power of two from ", TERRAIN3D_INSTANCE_MIN_CELL_SIZE,
			" through ", TERRAIN3D_INSTANCE_MAX_CELL_SIZE, " and divide region size ", ( int ) ( m_nRegionSize ) );
		return;
	}
	if ( m_nInstancerCellSize == nCellSize ) {
		return;
	}
	if ( m_pInstancer ) {
		m_pInstancer->setCellSize( nCellSize );
		if ( m_pInstancer->getCellSize() != nCellSize ) {
			return;
		}
	}
	m_nInstancerCellSize = nCellSize;
	refreshPerformanceProfileConformity();
	if ( m_pMaterial.is_valid() && m_pMaterial->isInitialized() ) {
		m_pMaterial->update();
	}
}

void T3DTerrain::setInstancerCellStreamingEnabled( const bool bEnabled ) {
	if ( m_bInstancerCellStreamingEnabled == bEnabled ) {
		return;
	}
	m_bInstancerCellStreamingEnabled = bEnabled;
	refreshPerformanceProfileConformity();
	if ( m_pInstancer ) {
		m_pInstancer->setCellStreamingEnabled( effectiveInstancerCellStreamingEnabled() );
	}
}

void T3DTerrain::setInstancerSingleLodBuffers( const bool bEnabled ) {
	if ( m_bInstancerSingleLodBuffers == bEnabled ) {
		return;
	}
	m_bInstancerSingleLodBuffers = bEnabled;
	refreshPerformanceProfileConformity();
	if ( m_pInstancer ) {
		m_pInstancer->setSingleLodInstanceBuffers( bEnabled );
	}
}

void T3DTerrain::setInstancerCellStreamingMargin( const real_t fMargin ) {
	const real_t margin = CLAMP( fMargin, 0.0f, 1024.0f );
	if ( Math::is_equal_approx( m_fInstancerCellStreamingMargin, margin ) ) {
		return;
	}
	m_fInstancerCellStreamingMargin = margin;
	refreshPerformanceProfileConformity();
	if ( m_pInstancer ) {
		m_pInstancer->setCellStreamingMargin( margin );
	}
}

void T3DTerrain::setInstancerCellStreamingUpdateDistance( const real_t fDistance ) {
	const real_t distance = CLAMP( fDistance, 1.0f, 256.0f );
	if ( Math::is_equal_approx( m_fInstancerCellStreamingUpdateDistance, distance ) ) {
		return;
	}
	m_fInstancerCellStreamingUpdateDistance = distance;
	refreshPerformanceProfileConformity();
	if ( m_pInstancer ) {
		m_pInstancer->setCellStreamingStep( distance );
	}
}

void T3DTerrain::setInstancerUpdateCellBudget( const int nBudget ) {
	const int budget = CLAMP( nBudget, 0, 65536 );
	if ( m_nInstancerUpdateCellBudget == budget ) {
		return;
	}
	m_nInstancerUpdateCellBudget = budget;
	refreshPerformanceProfileConformity();
	if ( m_pInstancer ) {
		m_pInstancer->setUpdateCellBudget( budget );
	}
}

void T3DTerrain::setDynamicNodeStreamingEnabled( const bool bEnabled ) {
	if ( m_bDynamicNodeStreamingEnabled == bEnabled ) {
		return;
	}
	m_bDynamicNodeStreamingEnabled = bEnabled;
	refreshPerformanceProfileConformity();
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->setEnabled( effectiveDynamicNodeStreamingEnabled() );
	}
}

void T3DTerrain::setDynamicNodeStreamRadius( const int nRadius ) {
	const int radius = CLAMP( nRadius, 0, 16 );
	if ( m_nDynamicNodeStreamRadius == radius ) {
		return;
	}
	m_nDynamicNodeStreamRadius = radius;
	refreshPerformanceProfileConformity();
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->setRadiusRegions( radius );
	}
}

void T3DTerrain::setDynamicNodeLoadRegionBudget( const int nBudget ) {
	const int budget = CLAMP( nBudget, 0, 64 );
	if ( m_nDynamicNodeLoadRegionBudget == budget ) {
		return;
	}
	m_nDynamicNodeLoadRegionBudget = budget;
	refreshPerformanceProfileConformity();
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->setLoadRegionBudget( budget );
	}
}

void T3DTerrain::setDynamicNodeLoadNodeBudget( const int nBudget ) {
	const int budget = CLAMP( nBudget, 0, 4096 );
	if ( m_nDynamicNodeLoadNodeBudget == budget ) {
		return;
	}
	m_nDynamicNodeLoadNodeBudget = budget;
	refreshPerformanceProfileConformity();
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->setLoadNodeBudget( budget );
	}
}

void T3DTerrain::setDynamicNodeUnloadRegionBudget( const int nBudget ) {
	const int budget = CLAMP( nBudget, 0, 64 );
	if ( m_nDynamicNodeUnloadRegionBudget == budget ) {
		return;
	}
	m_nDynamicNodeUnloadRegionBudget = budget;
	refreshPerformanceProfileConformity();
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->setUnloadRegionBudget( budget );
	}
}

void T3DTerrain::setLightmapStreamingEnabled( const bool bEnabled ) {
	if ( m_bLightmapStreamingEnabled == bEnabled ) {
		return;
	}
	m_bLightmapStreamingEnabled = bEnabled;
	markMemoryBudgetDirty();
	if ( m_pLightmapStreamer ) {
		m_pLightmapStreamer->setEnabled( bEnabled );
	}
}

void T3DTerrain::setLightmapStreamRadius( const int nRadius ) {
	const int radius = CLAMP( nRadius, 0, 16 );
	if ( m_nLightmapStreamRadius == radius ) {
		return;
	}
	m_nLightmapStreamRadius = radius;
	if ( m_pLightmapStreamer ) {
		m_pLightmapStreamer->setRadiusRegions( radius );
	}
}

void T3DTerrain::setLightmapLoadBudget( const int nBudget ) {
	const int budget = CLAMP( nBudget, 0, 64 );
	if ( m_nLightmapLoadBudget == budget ) {
		return;
	}
	m_nLightmapLoadBudget = budget;
	if ( m_pLightmapStreamer ) {
		m_pLightmapStreamer->setLoadBudget( budget );
	}
}

void T3DTerrain::setLightmapUnloadBudget( const int nBudget ) {
	const int budget = CLAMP( nBudget, 0, 64 );
	if ( m_nLightmapUnloadBudget == budget ) {
		return;
	}
	m_nLightmapUnloadBudget = budget;
	if ( m_pLightmapStreamer ) {
		m_pLightmapStreamer->setUnloadBudget( budget );
	}
}

Dictionary T3DTerrain::getLightmapStreamingStats( void ) const {
	return m_pLightmapStreamer ? m_pLightmapStreamer->getStats() : Dictionary();
}

void T3DTerrain::setRegionLightmapOutputDirectory( const String &rDirectory ) {
	String directory = rDirectory.strip_edges().trim_suffix( "/" );
	if ( directory.is_empty() ) {
		directory = terrain3dDefaultRegionLightmapOutputDirectory( m_dataDirectory );
	}
	m_regionLightmapOutputDirectory = directory;
}

void T3DTerrain::setRegionLightmapProxyLod( const int nLod ) {
	m_nRegionLightmapProxyLod = CLAMP( nLod, 0, 8 );
}

void T3DTerrain::setRegionLightmapContextRadius( const int nRadius ) {
	m_nRegionLightmapContextRadius = CLAMP( nRadius, 0, 4 );
}

void T3DTerrain::setRegionLightmapTexelSize( const real_t fTexelSize ) {
	m_fRegionLightmapTexelSize = CLAMP( fTexelSize, 0.05f, 16.0f );
}

void T3DTerrain::setRegionLightmapBakeQuality( const int nQuality ) {
	m_nRegionLightmapBakeQuality = CLAMP( nQuality, 0, 3 );
}

void T3DTerrain::setRegionLightmapShadowmaskMode( const int nMode ) {
	m_nRegionLightmapShadowmaskMode = CLAMP( nMode, 0, 2 );
}

void T3DTerrain::setHlodEnabled( const bool bEnabled ) {
	if ( m_bHlodEnabled == bEnabled ) { return; }
	m_bHlodEnabled = bEnabled; markMemoryBudgetDirty();
	if ( m_pHlodStreamer ) { m_pHlodStreamer->setEnabled( bEnabled ); }
}
void T3DTerrain::setHlodManifestPath( const String &rPath ) {
	const String value = rPath.strip_edges(); if ( m_hlodManifestPath == value ) { return; }
	m_hlodManifestPath = value; if ( m_pHlodStreamer ) { m_pHlodStreamer->setManifestPath( value ); }
}
void T3DTerrain::setHlodTargetErrorPixels( const real_t fPixels ) {
	m_fHlodTargetErrorPixels = CLAMP( fPixels, 0.25f, 64.0f ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setTargetErrorPixels( m_fHlodTargetErrorPixels ); }
}
void T3DTerrain::setHlodHysteresis( const real_t fRatio ) {
	m_fHlodHysteresis = CLAMP( fRatio, 0.0f, 0.75f ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setHysteresis( m_fHlodHysteresis ); }
}
void T3DTerrain::setHlodMaxLevel( const int nLevel ) {
	m_nHlodMaxLevel = CLAMP( nLevel, 1, 6 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setMaxLevel( m_nHlodMaxLevel ); }
}
void T3DTerrain::setHlodLoadBudget( const int nBudget ) {
	m_nHlodLoadBudget = CLAMP( nBudget, 0, 64 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setLoadBudget( m_nHlodLoadBudget ); }
}
void T3DTerrain::setHlodUnloadBudget( const int nBudget ) {
	m_nHlodUnloadBudget = CLAMP( nBudget, 0, 64 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setUnloadBudget( m_nHlodUnloadBudget ); }
}
void T3DTerrain::setHlodMemoryBudgetMb( const int nBudgetMb ) {
	m_nHlodMemoryBudgetMb = CLAMP( nBudgetMb, 1, 4096 ); markMemoryBudgetDirty();
	if ( !m_bHlodMemoryBudgetAuto && m_bInitialized ) { serviceMemoryBudgetPolicy( true ); }
}
void T3DTerrain::setHlodMaxShadowLevel( const int nLevel ) {
	m_nHlodMaxShadowLevel = CLAMP( nLevel, 0, 6 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setMaxShadowLevel( m_nHlodMaxShadowLevel ); }
}
void T3DTerrain::setHlodForcedDebugLevel( const int nLevel ) {
	m_nHlodForcedDebugLevel = CLAMP( nLevel, -1, 6 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setForcedDebugLevel( m_nHlodForcedDebugLevel ); }
}
bool T3DTerrain::reloadHlodManifest( void ) {
	return m_pHlodStreamer && m_pHlodStreamer->reloadManifest();
}
Dictionary T3DTerrain::getHlodStreamingStats( void ) const {
	return m_pHlodStreamer ? m_pHlodStreamer->getStats() : Dictionary();
}
Array T3DTerrain::getHlodDebugEntries( void ) const {
	return m_pHlodStreamer ? m_pHlodStreamer->getDebugEntries() : Array();
}
bool T3DTerrain::isHlodRegionSuppressed( const Vector2i &rRegion, const int nMinimumLevel ) const {
	return m_pHlodStreamer && m_pHlodStreamer->isRegionSuppressed( rRegion, nMinimumLevel );
}
bool T3DTerrain::isHlodRegionDetailRequired( const Vector2i &rRegion ) const {
	return !m_pHlodStreamer || m_pHlodStreamer->isRegionDetailRequired( rRegion );
}
bool T3DTerrain::isHlodRegionManaged( const Vector2i &rRegion ) const {
	return m_pHlodStreamer && m_pHlodStreamer->isRegionManaged( rRegion );
}
uint8_t T3DTerrain::getHlodInstancerMinimumLevel( const Vector2i &rRegion, const int nMeshId ) const {
	return m_pHlodStreamer ? m_pHlodStreamer->getInstancerMinimumLevel( rRegion, nMeshId ) : 0;
}
uint8_t T3DTerrain::getHlodNodeMinimumLevel( const Vector2i &rRegion, const int64_t nSpawnId ) const {
	return m_pHlodStreamer ? m_pHlodStreamer->getNodeMinimumLevel( rRegion, nSpawnId ) : 0;
}
void T3DTerrain::onHlodRegionRepresentationChanged( const Vector2i &rRegion ) {
	if ( m_pInstancer ) { m_pInstancer->updateMmis( -1, rRegion, false ); }
	if ( m_pNodeStreamer ) { m_pNodeStreamer->onHlodRepresentationChanged( rRegion ); }
}
bool T3DTerrain::isHlodDetailReady( const Vector2i &rRegionMin, const Vector2i &rRegionMax ) const {
	if ( m_pHlodStreamer ) {
		for ( int z = rRegionMin.y; z <= rRegionMax.y; ++z ) {
			for ( int x = rRegionMin.x; x <= rRegionMax.x; ++x ) {
				const Vector2i region( x, z );
				if ( m_pHlodStreamer->isRegionManaged( region ) &&
					getRegionStreamingStateInternal( region ) < TERRAIN3D_REGION_STREAMING_HOT ) {
					return false;
				}
			}
		}
	}
	const bool instances_ready = !m_pInstancer || m_pInstancer->areHlodDetailRegionsReady( rRegionMin, rRegionMax );
	const bool nodes_ready = !m_pNodeStreamer || m_pNodeStreamer->areHlodVisualRegionsReady( rRegionMin, rRegionMax );
	return instances_ready && nodes_ready;
}

int64_t T3DTerrain::addStreamedScene(
	const Ref<PackedScene> &rScene, const Transform3D &rGlobalTransform,
	const Dictionary &rState, const int nFlags, const int nHlodMinLevel
) {
	return m_pNodeStreamer ? m_pNodeStreamer->addStreamedScene( rScene, rGlobalTransform, rState, nFlags, nHlodMinLevel ) : 0;
}
int64_t T3DTerrain::addStreamedScenePath(
	const String &rScenePath, const Transform3D &rGlobalTransform,
	const Dictionary &rState, const int nFlags, const int nHlodMinLevel
) {
	return m_pNodeStreamer ? m_pNodeStreamer->addStreamedScenePath( rScenePath, rGlobalTransform, rState, nFlags, nHlodMinLevel ) : 0;
}
bool T3DTerrain::removeStreamedScene( const int64_t nSpawnId ) {
	return m_pNodeStreamer && m_pNodeStreamer->removeStreamedScene( nSpawnId );
}

int T3DTerrain::allocateStreamOwnerId( void ) {
	const int minimum_owner_id = 0x40000000;
	const int maximum_owner_id = INT32_MAX - 1;
	if ( m_nNextStreamOwnerId < minimum_owner_id || m_nNextStreamOwnerId > maximum_owner_id ) {
		m_nNextStreamOwnerId = minimum_owner_id;
	}
	const int owner_id = m_nNextStreamOwnerId;
	m_nNextStreamOwnerId = owner_id >= maximum_owner_id ? minimum_owner_id : owner_id + 1;
	return owner_id;
}

void T3DTerrain::setNextStreamOwnerId( const int nOwnerId ) {
	m_nNextStreamOwnerId = CLAMP( nOwnerId, 0x40000000, INT32_MAX - 1 );
}

Dictionary T3DTerrain::getNodeStreamingStats( void ) const {
	return m_pNodeStreamer ? m_pNodeStreamer->getStats() : Dictionary();
}

