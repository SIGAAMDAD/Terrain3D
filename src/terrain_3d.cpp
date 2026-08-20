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
#include "streaming/terrain_3d_streaming_policy.h"
#include "streaming/terrain_3d_upload_bridge.h"
#include "core/terrain_3d_util.h"

namespace {


inline int32_t terrain3dDivideFloor( const int32_t nValue, const int32_t nDivisor ) {
	int32_t quotient = nValue / nDivisor;
	if ( nValue % nDivisor < 0 ) {
		--quotient;
	}
	return quotient;
}

void terrain3dAppendRuntimeProblem(
	Array &rProblems, const String &rSeverity, const String &rCode,
	const String &rSubsystem, const String &rMessage, const int64_t nValue
) {
	Dictionary problem;
	problem["severity"] = rSeverity;
	problem["code"] = rCode;
	problem["subsystem"] = rSubsystem;
	problem["message"] = rMessage;
	problem["value"] = nValue;
	rProblems.push_back( problem );
}

Variant terrain3dPerformanceProfileValue( const terrain_3d_profile::t3dsetting_t &rSetting ) {
	switch ( rSetting.kind ) {
		case terrain_3d_profile::VALUE_INT:
			return ( int64_t ) ( rSetting.intValue );
		case terrain_3d_profile::VALUE_FLOAT:
			return rSetting.fFloatValue;
		case terrain_3d_profile::VALUE_BOOL:
			return rSetting.bBoolValue;
	}
	return Variant();
}

bool terrain3dPerformanceProfileValueMatches(
	const Variant &rCurrent,
	const terrain_3d_profile::t3dsetting_t &rSetting
) {
	switch ( rSetting.kind ) {
		case terrain_3d_profile::VALUE_INT:
			return ( int64_t ) ( rCurrent ) == rSetting.intValue;
		case terrain_3d_profile::VALUE_FLOAT:
			return Math::is_equal_approx( ( double ) ( rCurrent ), rSetting.fFloatValue );
		case terrain_3d_profile::VALUE_BOOL:
			return ( bool ) ( rCurrent ) == rSetting.bBoolValue;
	}
	return false;
}

String terrain3dPerformanceProfileOwnerName( const terrain_3d_profile::t3downer_t owner ) {
	return owner == terrain_3d_profile::OWNER_MATERIAL ? "Terrain3DMaterial" : "Terrain3D";
}

#define TERRAIN3D_ALIGN_BAKE_TASK_OFFSET( value ) \
	( ( ( value ) + TERRAIN3D_BAKE_TASK_ALIGNMENT_ELEMENTS - 1u ) & ~( TERRAIN3D_BAKE_TASK_ALIGNMENT_ELEMENTS - 1u ) )

} // namespace

// Initialize static member variable
T3DTerrain::t3ddebugLevel_t T3DTerrain::s_debugLevel{T3DTerrain::ERROR};

///////////////////////////
// Private Functions
///////////////////////////

void T3DTerrain::connectResourceSignals(
	const Ref<T3DAssets> &rAssets,
	const Ref<T3DMaterial> &rMaterial
) {
	if ( rMaterial.is_null() ) {
		return;
	}
	const Callable maps_callback = callable_mp( this, &T3DTerrain::onDataMapsChanged );
	if ( m_pData && !m_pData->is_connected( "maps_changed", maps_callback ) ) {
		m_pData->connect( "maps_changed", maps_callback );
	}
	const Callable page_table_callback = callable_mp( this, &T3DTerrain::onGpuRegionPageTableChanged );
	if ( m_pData && !m_pData->is_connected( "gpu_region_page_table_changed", page_table_callback ) ) {
		m_pData->connect( "gpu_region_page_table_changed", page_table_callback );
	}
	const Callable textures_callback = callable_mp( this, &T3DTerrain::onAssetsTexturesChanged );
	if ( rAssets.is_valid() && !rAssets->is_connected( "textures_changed", textures_callback ) ) {
		rAssets->connect( "textures_changed", textures_callback );
	}
	const Callable material_callback = callable_mp( this, &T3DTerrain::onProfileOwnedResourceChanged );
	if ( !rMaterial->is_connected( "changed", material_callback ) ) {
		rMaterial->connect( "changed", material_callback );
	}
}

void T3DTerrain::disconnectResourceSignals(
	const Ref<T3DAssets> &rAssets,
	const Ref<T3DMaterial> &rMaterial
) {
	if ( rMaterial.is_null() ) {
		return;
	}
	const Callable maps_callback = callable_mp( this, &T3DTerrain::onDataMapsChanged );
	if ( m_pData && m_pData->is_connected( "maps_changed", maps_callback ) ) {
		m_pData->disconnect( "maps_changed", maps_callback );
	}
	const Callable page_table_callback = callable_mp( this, &T3DTerrain::onGpuRegionPageTableChanged );
	if ( m_pData && m_pData->is_connected( "gpu_region_page_table_changed", page_table_callback ) ) {
		m_pData->disconnect( "gpu_region_page_table_changed", page_table_callback );
	}
	const Callable textures_callback = callable_mp( this, &T3DTerrain::onAssetsTexturesChanged );
	if ( rAssets.is_valid() && rAssets->is_connected( "textures_changed", textures_callback ) ) {
		rAssets->disconnect( "textures_changed", textures_callback );
	}
	const Callable material_callback = callable_mp( this, &T3DTerrain::onProfileOwnedResourceChanged );
	if ( rMaterial->is_connected( "changed", material_callback ) ) {
		rMaterial->disconnect( "changed", material_callback );
	}
}

void T3DTerrain::connectAutoShaderProfileSignal( const Ref<Resource> &rProfile ) {
	if ( rProfile.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &T3DTerrain::onAutoShaderProfileChanged );
	if ( !rProfile->is_connected( "changed", callback ) ) {
		rProfile->connect( "changed", callback );
	}
}

void T3DTerrain::disconnectAutoShaderProfileSignal( const Ref<Resource> &rProfile ) {
	if ( rProfile.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &T3DTerrain::onAutoShaderProfileChanged );
	if ( rProfile->is_connected( "changed", callback ) ) {
		rProfile->disconnect( "changed", callback );
	}
}

void T3DTerrain::applyAutoShaderProfileMode( void ) {
	if ( m_pMaterial.is_null() || m_pAutoShaderProfile.is_null() ) {
		return;
	}

	const Variant mode_value = m_pAutoShaderProfile->get( "mode" );
	if ( mode_value.get_type() != Variant::INT ) {
		return;
	}

	m_pMaterial->setAutoShaderEnabled( ( int ) ( mode_value ) == TERRAIN3D_AUTOSHADER_MODE_LEGACY_RUNTIME );
}

void T3DTerrain::onAutoShaderProfileChanged( void ) {
	applyAutoShaderProfileMode();
}

void T3DTerrain::onDataMapsChanged( void ) {
	// Signals can arrive from an inactive editor scene whose resource objects
	// have already transferred to the newly active Terrain3D. Only the current
	// owner may rebind shared material state.
	if ( m_pMaterial.is_valid() && m_pMaterial->isInitializedFor( this ) ) {
		m_pMaterial->update( T3DMaterial::REGION_ARRAYS );
	}
}

void T3DTerrain::onGpuRegionPageTableChanged( void ) {
	if ( m_pMaterial.is_valid() && m_pMaterial->isInitializedFor( this ) ) {
		m_pMaterial->updateRegionPageTable();
	}
}

void T3DTerrain::onAssetsTexturesChanged( void ) {
	if ( m_pAssets.is_valid() && m_pAssets->isInitializedFor( this ) && m_pMaterial.is_valid() &&
		m_pMaterial->isInitializedFor( this ) ) {
		m_pMaterial->update( T3DMaterial::TEXTURE_ARRAYS );
		if ( m_pData ) {
			m_pData->invalidateGpuFarSurface();
		}
	}
}

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

Dictionary T3DTerrain::getHeightSourceDiagnostics( const Vector3 &rWorldPosition ) const {
	Dictionary diagnostics;
	const Vector2i region = m_pData ? m_pData->getRegionLocation( rWorldPosition ) : V2I_MAX;
	const t3dregionStreamingState_t state = getRegionStreamingStateInternal( region );
	const Vector3 target = getClipmapTargetPosition();
	const real_t distance = MAX( Math::abs( rWorldPosition.x - target.x ), Math::abs( rWorldPosition.z - target.z ) );
	const int lod_count = rangeToLodCount( distance );
	const int mesh_lod = MAX( 0, lod_count - 1 );
	const int first_far_lod = CLAMP( m_nRuntimeNearShaderLods + m_nRuntimeMiddleShaderLods, 0, 30 );
	const real_t tessellation_scale = ( real_t ) ( ( uint64_t ) ( 1 ) << ( uint64_t ) ( CLAMP( m_tessellationLevel, 0, 6 ) ) );
	const real_t mesh_spacing = m_fVertexSpacing * ( real_t ) ( ( uint64_t ) ( 1 ) << ( uint64_t ) ( CLAMP( mesh_lod, 0, 30 ) ) ) /
		tessellation_scale;
	const real_t far_source_spacing = m_fVertexSpacing * ( real_t ) ( TERRAIN3D_ACTIVE_WORLD_TEXELS ) /
		( real_t ) ( MAX( 1, getGpuFarHeightAtlasSide() ) );

	String height_source = "none";
	real_t source_spacing = 0.0f;
	if ( isEditorAuthoringModeInternal() || state == TERRAIN3D_REGION_STREAMING_ACTIVE ) {
		height_source = "near_region_height";
		source_spacing = m_fVertexSpacing;
	} else if ( state == TERRAIN3D_REGION_STREAMING_HOT ) {
		height_source = "middle_height_cache";
		source_spacing = m_fVertexSpacing * ( real_t ) ( isEditorAuthoringModeInternal() ? 1 : getGpuMiddleHeightResolutionDivisor() );
	} else if ( state == TERRAIN3D_REGION_STREAMING_COLD ) {
		height_source = "far_height_lattice";
		source_spacing = far_source_spacing;
	}

	diagnostics["world_position"] = rWorldPosition;
	diagnostics["region"] = region;
	diagnostics["streaming_state"] = ( int ) ( state );
	diagnostics["streaming_state_name"] = String( terrain3dStreamingStateName( state ) );
	diagnostics["render_mesh_lod"] = mesh_lod;
	diagnostics["mesh_vertex_spacing"] = mesh_spacing;
	diagnostics["first_far_mesh_lod"] = first_far_lod;
	diagnostics["first_far_mesh_spacing"] = m_fVertexSpacing * ( real_t ) ( ( uint64_t ) ( 1 ) << ( uint64_t ) ( first_far_lod ) ) /
		tessellation_scale;
	diagnostics["height_source"] = height_source;
	diagnostics["height_source_spacing"] = source_spacing;
	diagnostics["far_height_atlas_side"] = getGpuFarHeightAtlasSide();
	diagnostics["far_surface_atlas_side"] = getGpuFarSurfaceAtlasSide();
	diagnostics["surface_gpu_resident"] = m_pData && m_pData->isGpuSurfaceRegionResident( region );
	diagnostics["middle_height_resident"] = m_pData && m_pData->isMiddleHeightRegionResident( region );
	diagnostics["geometry_source_invariant_valid"] = state == TERRAIN3D_REGION_STREAMING_UNLOADED ||
		source_spacing <= MAX( mesh_spacing, m_fVertexSpacing ) + 0.0001f;
	return diagnostics;
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

t3dadaptiveBudgetLimits_t T3DTerrain::getRuntimeAdaptiveBudgetLimits( void ) const {
	t3dadaptiveBudgetLimits_t limits;
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

t3dadaptiveBudgetPressure_t T3DTerrain::getRuntimeAdaptiveBudgetPressure( void ) const {
	t3dadaptiveBudgetPressure_t pressure;
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
	const t3dadaptiveBudgetValues_t &effective = m_nRuntimeAdaptiveBudgetState.effective;
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
	const t3dadaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
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
	const t3dadaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
	const t3dadaptiveBudgetPressure_t pressure = getRuntimeAdaptiveBudgetPressure();
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

void T3DTerrain::refreshPerformanceProfileConformity( void ) {
	if ( m_bApplyingPerformanceTarget ) {
		return;
	}

	t3dperformanceProfileStatus_t next_status = PERFORMANCE_PROFILE_CUSTOM;
	if ( m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ) {
		next_status = PERFORMANCE_PROFILE_EXACT;
		for ( const terrain_3d_profile::t3dsetting_t &setting : terrain_3d_profile::XBOX360_EXPERIMENTAL_SETTINGS ) {
			Object *pOwner = this;
			if ( setting.owner == terrain_3d_profile::OWNER_MATERIAL ) {
				if ( m_pMaterial.is_null() ) {
					next_status = PERFORMANCE_PROFILE_MODIFIED;
					break;
				}
				pOwner = m_pMaterial.ptr();
			}
			const Variant current = pOwner->get( StringName( setting.pszProperty ) );
			if ( !terrain3dPerformanceProfileValueMatches( current, setting ) ) {
				next_status = PERFORMANCE_PROFILE_MODIFIED;
				break;
			}
		}
	}

	if ( m_performanceProfileStatus != next_status ) {
		m_performanceProfileStatus = next_status;
		notify_property_list_changed();
		emit_signal( "performance_profile_state_changed", ( int ) ( m_performanceProfileStatus ) );
	}
}

void T3DTerrain::onProfileOwnedResourceChanged( void ) {
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
}

void T3DTerrain::applyPerformanceProfileToTerrain( void ) {
	for ( const terrain_3d_profile::t3dsetting_t &setting : terrain_3d_profile::XBOX360_EXPERIMENTAL_SETTINGS ) {
		if ( setting.owner != terrain_3d_profile::OWNER_TERRAIN ) {
			continue;
		}
		set( StringName( setting.pszProperty ), terrain3dPerformanceProfileValue( setting ) );
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

void T3DTerrain::sampleRuntimeProfile( const t3druntimeProfileSample_t &rSample ) {
	t3druntimeProfileSample_t sample = rSample;
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
		m_pRuntimeProfileHistory = memnew( t3druntimeProfileHistory_t );
	}
	m_pRuntimeProfileHistory->push( sample );
}

void T3DTerrain::updateCacheThrashEvents( const t3druntimeProfileSample_t &rSample ) {
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

void T3DTerrain::initializeInternal( void ) {
	LOG( T3DTerrain::INFO, "[lifecycle] Initialize request: initialized=", m_bInitialized,
		"; inside_tree=", is_inside_tree(), "; inside_world=", m_bIsInsideWorld,
		"; data_directory=", m_dataDirectory );
	// Make blank objects if needed
	if ( !m_pData ) {
		LOG( T3DTerrain::DEBUG, "Creating blank data object" );
		m_pData = memnew( T3DData );
	}
	if ( !initializeRuntimeMemory() ) {
		LOG( T3DTerrain::ERROR, "[lifecycle] Unable to initialize the fixed runtime memory image" );
		set_physics_process( false );
		return;
	}
	bool bCreatedDefaultMaterial = false;
	if ( m_pMaterial.is_null() ) {
		LOG( T3DTerrain::DEBUG, "Creating blank material" );
		m_pMaterial.instantiate();
		bCreatedDefaultMaterial = true;
	}
	// A base profile selected before a default material exists must seed that new
	// material once. Existing/deserialized material resources are never overwritten
	// here; their actual values determine Exact versus Modified conformity.
	if ( m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL &&
		( bCreatedDefaultMaterial || m_bPerformanceProfileMaterialPendingApply ) ) {
		m_pMaterial->setShaderPerformancePreset(
			T3DMaterial::SHADER_PRESET_XBOX_360_AUTHENTIC );
		m_bPerformanceProfileMaterialPendingApply = false;
	}
	connectAutoShaderProfileSignal( m_pAutoShaderProfile );
	applyAutoShaderProfileMode();
	if ( m_pAssets.is_null() ) {
		LOG( T3DTerrain::DEBUG, "Creating blank texture list" );
		m_pAssets.instantiate();
	}
	if ( !m_pCollision ) {
		LOG( T3DTerrain::DEBUG, "Creating collision manager" );
		m_pCollision = memnew( T3DCollision );
	}
	if ( !m_pInstancer ) {
		LOG( T3DTerrain::DEBUG, "Creating instancer" );
		m_pInstancer = memnew( T3DInstancer );
	}
	if ( !m_pNodeStreamer ) {
		LOG( T3DTerrain::DEBUG, "Creating node streamer" );
		m_pNodeStreamer = memnew( T3DNodeStreamer );
	}
	if ( !m_pLightmapStreamer ) {
		LOG( T3DTerrain::DEBUG, "Creating lightmap streamer" );
		m_pLightmapStreamer = memnew( T3DLightmapStreamer );
	}
	if ( !m_pHlodStreamer ) {
		LOG( T3DTerrain::DEBUG, "Creating HLOD streamer" );
		m_pHlodStreamer = memnew( T3DHLODStreamer );
	}
	// Connect signals
	// Any region was changed, update region labels
	if ( !m_pData->is_connected( "region_map_changed", callable_mp( this, &T3DTerrain::updateRegionLabels ) ) ) {
		LOG( T3DTerrain::DEBUG, "Connecting _data::region_map_changed signal to set_show_region_locations()" );
		m_pData->connect( "region_map_changed", callable_mp( this, &T3DTerrain::updateRegionLabels ) );
	}
	// Region layout changes are coalesced and update the existing dynamic
	// collision pool instead of rebuilding every shape per editor event.
	if ( !m_pData->is_connected( "region_map_changed",
							 callable_mp( m_pCollision, &T3DCollision::onRegionMapChanged ) ) ) {
		LOG( T3DTerrain::DEBUG, "Connecting _data::region_map_changed signal to on_region_map_changed()" );
		m_pData->connect( "region_map_changed", callable_mp( m_pCollision, &T3DCollision::onRegionMapChanged ) );
	}
	connectResourceSignals( m_pAssets, m_pMaterial );
	refreshPerformanceProfileConformity();
	// Height map was regenerated, update aabbs
	if ( !m_pData->is_connected( "height_maps_changed", callable_mp( this, &T3DTerrain::updateMesherAabbs ) ) ) {
		LOG( T3DTerrain::DEBUG, "Connecting _data::height_maps_changed signal to update_aabbs()" );
		m_pData->connect( "height_maps_changed", callable_mp( this, &T3DTerrain::updateMesherAabbs ) );
	}
	// Initialize the system
	if ( !m_bInitialized && m_bIsInsideWorld && is_inside_tree() ) {
		LOG( T3DTerrain::INFO, "[lifecycle] Initializing main subsystems: gpu_cache=",
			effectiveGpuRegionCacheEnabled(), "; cpu_cache=", effectiveCpuRegionCacheEnabled(),
			"; instance_cache=", effectiveInstanceRegionCacheEnabled() );
		const Error data_error = m_pData->initialize( this );
		if ( data_error != OK ) {
			LOG( T3DTerrain::ERROR, "[lifecycle] Terrain data activation failed; dependent runtime systems will remain inactive. Error: ",
				data_error, "; state=", m_pData->getRuntimeState() );
			set_physics_process( false );
			return;
		}
		// Establish the authoritative residency map before the tier materials take
		// their initial sampler bindings. Physical GPU publication remains bounded
		// in authoring as well as runtime; CPU data and the far atlas remain the
		// whole-world authority.
		const Vector3 initial_target = getClipmapTargetPosition();
		updateRegionStreamingPolicy( initial_target, 0.0, true );
		m_pData->updateRegionResidency( initial_target, true );

		// Asset arrays and the whole-world far atlases must exist before the tier
		// materials take their initial sampler bindings. Initializing the material
		// first leaves Middle/Far bound to their empty fallbacks until an unrelated
		// material rebuild happens.
		m_pAssets->initialize( this );
		m_pData->ensureFarAtlases();
		m_pMaterial->initialize( this );
		if ( !m_pMaterial->isRenderShaderReady() ) {
			LOG( T3DTerrain::ERROR, "[lifecycle] Terrain material failed to produce a valid render shader; "
				"aborting terrain activation before render meshes are created." );
			m_pMaterial->uninitialize( this );
			m_pAssets->uninitialize( this );
			m_pData->deactivateRuntimeStorage( this );
			set_physics_process( false );
			return;
		}
		m_pCollision->initialize( this );
		m_pInstancer->m_nCellSize = m_nInstancerCellSize;
		m_pInstancer->m_bCellStreamingEnabled = effectiveInstancerCellStreamingEnabled();
		m_pInstancer->m_bSingleLodInstanceBuffers = m_bInstancerSingleLodBuffers;
		m_pInstancer->m_fCellStreamingMargin = m_fInstancerCellStreamingMargin;
		m_pInstancer->m_fCellStreamingStep = m_fInstancerCellStreamingUpdateDistance;
		m_pInstancer->m_nUpdateCellBudget = ( uint32_t ) ( MAX( m_nInstancerUpdateCellBudget, 0 ) );
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
		applyRuntimeAdaptiveWorkBudgets();
		setupTerrainMesher();
		setupOceanMesher();
		updateDisplacementBuffer();
		m_bInitialized = true;
		updateRegionLabels();
		snap();
		LOG( T3DTerrain::INFO, "[lifecycle] Main subsystems ready: regions=", m_pData->getRegionCount(),
			"; region_size=", ( int ) ( m_nRegionSize ), "; mesh_lods=", m_nMeshLods,
			"; tessellation=", m_tessellationLevel );
	}
	serviceMemoryBudgetPolicy( true );
	update_configuration_warnings();
}

void T3DTerrain::deactivateRuntime( void ) {
	set_physics_process( false );
	destroyTerrainMesher();
	destroyOceanMesher();
	destroyHlodStreamer();
	destroyLightmapStreamer();
	destroyNodeStreamer( true );
	destroyInstancer();
	destroyCollision();
	disconnectResourceSignals( m_pAssets, m_pMaterial );
	if ( m_pAssets.is_valid() ) {
		m_pAssets->uninitialize( this );
	}
	if ( m_pMaterial.is_valid() ) {
		m_pMaterial->uninitialize( this );
	}
	if ( m_pData ) {
		m_pData->deactivateRuntimeStorage( this );
	}
	m_bInitialized = false;
}

/**
 * This is a proxy for _process(delta) called by _notification() due to
 * https://github.com/godotengine/godot-cpp/issues/1022
 */
void T3DTerrain::physicsProcess( const double fDelta ) {
	if ( !m_bInitialized ) {
		return;
	}
	serviceMemoryBudgetPolicy( false );
	if ( m_bComputeDisplacementFallbackRequested.exchange( false, std::memory_order_acq_rel ) ) {
		m_lastBufferPosition = V2_MAX;
	}
	if ( !m_camera.isValid() ) {
		LOG( T3DTerrain::DEBUG, "Camera is null, getting the current one" );
		grabCamera();
	}

	t3druntimeProfileSample_t profile_sample;
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

	// Resolve the target once. Meshing, displacement, streaming, and ocean
	// snapping must observe one coherent position for this physics tick.
	const Vector3 clipmap_target_position = getClipmapTargetPosition();
	++m_iRuntimePhysicsFrameIndex;
	const Vector2i target_region = m_pData ? m_pData->getRegionLocation( clipmap_target_position ) : V2I_MAX;
	if ( target_region != m_runtimeBoundaryTargetRegion ) {
		m_runtimeBoundaryTargetRegion = target_region;
		m_nRuntimeBoundaryTransitionFrame = m_iRuntimePhysicsFrameIndex;
	}
	const uint64_t boundary_age_frames = m_iRuntimePhysicsFrameIndex - m_nRuntimeBoundaryTransitionFrame;
	m_bRuntimeBoundaryNodeDeferredLastTick = terrain3dBoundaryShouldDefer(
		m_bRuntimeBoundaryWorkPhasingEnabled, effectiveDynamicNodeStreamingEnabled(),
		boundary_age_frames, m_nRuntimeBoundaryNodeDelayFrames, m_runtimeBoundaryNodeDeferralStreak );
	m_bRuntimeBoundaryInstancerDeferredLastTick = terrain3dBoundaryShouldDefer(
		m_bRuntimeBoundaryWorkPhasingEnabled, effectiveInstancerCellStreamingEnabled(),
		boundary_age_frames, m_nRuntimeBoundaryInstancerDelayFrames,
		m_runtimeBoundaryInstancerDeferralStreak );
	m_runtimeBoundaryNodeDeferralStreak = m_bRuntimeBoundaryNodeDeferredLastTick ?
		m_runtimeBoundaryNodeDeferralStreak + 1 : 0;
	m_runtimeBoundaryInstancerDeferralStreak = m_bRuntimeBoundaryInstancerDeferredLastTick ?
		m_runtimeBoundaryInstancerDeferralStreak + 1 : 0;
	if ( bProfileFrame ) {
		profile_sample.fTargetX = ( float ) ( clipmap_target_position.x );
		profile_sample.fTargetZ = ( float ) ( clipmap_target_position.z );
		if ( m_nRuntimeProfileSampleCount > 0u ) {
			profile_sample.fMovementX = profile_sample.fTargetX - m_runtimeProfileLastSample.fTargetX;
			profile_sample.fMovementZ = profile_sample.fTargetZ - m_runtimeProfileLastSample.fTargetZ;
		}
		profile_sample.targetRegionX = target_region.x;
		profile_sample.targetRegionZ = target_region.y;
	}
	if ( m_pData ) {
		updateRegionStreamingPolicy( clipmap_target_position, fDelta, false );
		m_pData->updateRegionResidency( clipmap_target_position, false );
		if ( m_pAssets.is_valid() && m_pAssets->isRuntimeMaterialCacheActive() ) {
			uint8_t material_ids[TERRAIN3D_MAX_TEXTURE_ASSETS] = {};
			const int material_count = m_pData->fillGpuMaterialPriorityIds(
				material_ids, TERRAIN3D_MAX_TEXTURE_ASSETS );
			m_pAssets->updateRuntimeMaterialResidency( material_ids, material_count );
		}
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.nDataResidencyUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	const bool hlod_active = m_bHlodEnabled && ( !Engine::get_singleton()->is_editor_hint() || !isEditorAuthoringModeInternal() || m_bEditorHlodPreview );
	if ( m_pHlodStreamer ) {
		if ( hlod_active ) { m_pHlodStreamer->updateResidency( clipmap_target_position, false ); }
		else { m_pHlodStreamer->suspend(); }
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.nHlodStreamingUsec = now - phase_started_usec;
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
		profile_sample.nNodeStreamingUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_pLightmapStreamer && m_bLightmapStreamingEnabled ) {
		m_pLightmapStreamer->updateResidency( clipmap_target_position, false );
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.nLightmapStreamingUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_tessellationLevel > 0 ) {
		if ( m_pTerrainMesher && m_pDBufferVp && m_pMaterial.is_valid() ) {
			const Vector2 target_position_2d = v3v2( clipmap_target_position );
			const real_t fTessellationDensity = 1.0f / ( real_t ) ( 1u << ( uint32_t ) m_tessellationLevel );
			const real_t fBufferVertexSpacing = m_fVertexSpacing * fTessellationDensity;
			const real_t fUpdateDistance = MAX( fBufferVertexSpacing, m_fDisplacementUpdateDistance );
			if ( MAX( std::abs( m_lastBufferPosition.x - target_position_2d.x ),
						std::abs( m_lastBufferPosition.y - target_position_2d.y ) ) >= fUpdateDistance ) {
				m_lastBufferPosition = target_position_2d;
				if ( !tryUpdateComputeDisplacement( clipmap_target_position ) ) {
					RS->material_set_param( m_pMaterial->getBufferMaterialRid(), "_target_pos",
										   clipmap_target_position );
					m_pDBufferVp->set_update_mode( SubViewport::UPDATE_ONCE );
				}
				m_pTerrainMesher->snap( clipmap_target_position );
			}
		}
	} else if ( m_pTerrainMesher ) {
		m_pTerrainMesher->snap( clipmap_target_position );
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.nClipmapUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_bOceanEnabled && m_pOceanMesher ) {
		m_pOceanMesher->snap( clipmap_target_position );
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.nOceanUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_pInstancer && effectiveInstancerCellStreamingEnabled() ) {
		if ( m_bRuntimeBoundaryInstancerDeferredLastTick ) {
			m_pInstancer->deferStreamingSelectionForFrame();
			++m_nRuntimeBoundaryInstancerDeferralsTotal;
		} else {
			m_pInstancer->updateStreaming( clipmap_target_position );
		}
	}
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.nInstancerUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	syncLightShaderParameters();
	if ( bProfileFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		profile_sample.nLightSyncUsec = now - phase_started_usec;
		phase_started_usec = now;
	}
	if ( m_pCollision && m_pCollision->isEnabled() ) {
		m_pCollision->update();
	}
	if ( bMeasureFrame ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		const uint64_t measured_usec = now - frame_started_usec;
		if ( bAdaptiveFrame ) {
			m_nRuntimeAdaptiveLastMeasuredUsec = measured_usec;
		}
		if ( bProfileFrame ) {
			profile_sample.nCollisionUsec = now - phase_started_usec;
			profile_sample.nPhysicsTotalUsec = measured_usec;
			profile_sample.nStreamingUsec = profile_sample.nDataResidencyUsec + profile_sample.nHlodStreamingUsec +
				profile_sample.nNodeStreamingUsec + profile_sample.nLightmapStreamingUsec + profile_sample.nInstancerUsec;
			sampleRuntimeProfile( profile_sample );
		}
	}
}

void T3DTerrain::syncLightShaderParameters( void ) {
	DirectionalLight3D *const pLight = m_lightTarget.isValid() ? cast_to<DirectionalLight3D>( m_lightTarget.ptr() ) : nullptr;
	if ( !pLight ) {
		m_bLightStateValid = false;
		return;
	}
	const Color color = pLight->get_color() * pLight->get_param( DirectionalLight3D::PARAM_ENERGY );
	const Vector3 direction = pLight->get_global_basis().get_column( 2 ).normalized();
	const bool bColorChanged = !m_bLightStateValid || !m_lastLightColor.is_equal_approx( color );
	const bool bDirectionChanged = !m_bLightStateValid || !m_lastLightDirection.is_equal_approx( direction );
	if ( !bColorChanged && !bDirectionChanged ) {
		return;
	}

	if ( m_pMaterial.is_valid() ) {
		if ( bColorChanged ) {
			m_pMaterial->setShaderParam( "_light_color", color );
		}
		if ( bDirectionChanged ) {
			m_pMaterial->setShaderParam( "_light_direction", direction );
		}
	}
	if ( m_pOceanMaterial.is_valid() ) {
		ShaderMaterial *const pOceanShaderMaterial = Object::cast_to<ShaderMaterial>( m_pOceanMaterial.ptr() );
		if ( pOceanShaderMaterial ) {
			if ( bColorChanged ) {
				pOceanShaderMaterial->set_shader_parameter( "_light_color", color );
			}
			if ( bDirectionChanged ) {
				pOceanShaderMaterial->set_shader_parameter( "_light_direction", direction );
			}
		}
	}
	m_lastLightColor = color;
	m_lastLightDirection = direction;
	m_bLightStateValid = true;
}

/**
 * If running in the editor, grab the first editor viewport camera.
 * The edited_scene_root is excluded in case the user already has a Camera3D in their scene.
 */
void T3DTerrain::grabCamera( void ) {
	if ( IS_EDITOR ) {
		m_camera.setTarget( EditorInterface::get_singleton()->get_editor_viewport_3d( 0 )->get_camera_3d() );
		LOG( T3DTerrain::DEBUG, "Grabbing the first editor viewport camera: ", m_camera.getTarget() );
	} else {
		m_camera.setTarget( get_viewport()->get_camera_3d() );
		LOG( T3DTerrain::DEBUG, "Grabbing the in-game viewport camera: ", m_camera.getTarget() );
	}
	if ( !m_camera.isValid() && !m_clipmapTarget.isValid() ) {
		set_physics_process( false ); // No target to follow, disable snapping until one set
		LOG( T3DTerrain::ERROR, "Cannot find clipmap target or active camera. LODs won't be updated. Set manually with "
				   "set_clipmap_target() or setCamera()" );
	}
}

void T3DTerrain::destroyCollision( const bool bFinal ) {
	LOG( T3DTerrain::INFO, "Destroying Collision" );
	if ( m_pCollision ) {
		m_pCollision->destroy();
	}
	if ( bFinal && m_pCollision ) {
		memdelete( m_pCollision );
		m_pCollision = nullptr;
	}
}

void T3DTerrain::setupTerrainMesher( void ) {
	if ( !m_pTerrainMesher ) {
		LOG( T3DTerrain::DEBUG, "Creating mesher" );
		m_pTerrainMesher = memnew( T3DMesher );
	}
	const eastl::array<RID, 3> materials = {
		m_pMaterial->getMaterialRidForTier( T3DMaterial::SHADER_TIER_NEAR ),
		m_pMaterial->getMaterialRidForTier( T3DMaterial::SHADER_TIER_MIDDLE ),
		m_pMaterial->getMaterialRidForTier( T3DMaterial::SHADER_TIER_FAR ),
	};
	const eastl::array<RID, 3> shadow_materials = {
		m_pMaterial->getShadowMaterialRidForTier( T3DMaterial::SHADER_TIER_NEAR ),
		m_pMaterial->getShadowMaterialRidForTier( T3DMaterial::SHADER_TIER_MIDDLE ),
		m_pMaterial->getShadowMaterialRidForTier( T3DMaterial::SHADER_TIER_FAR ),
	};
	const bool bTieringActive = isRuntimeShaderTieringActive();
	const int clipmap_levels = CLAMP( m_nMeshLods + m_tessellationLevel, 1, TERRAIN3D_MAX_CLIPMAP_LEVELS );
	const int near_lods = bTieringActive ? m_nRuntimeNearShaderLods : clipmap_levels;
	const int middle_lods = bTieringActive ? m_nRuntimeMiddleShaderLods : 0;
	const int update_budget = bTieringActive ? m_nRuntimeClipmapUpdateBudget : TERRAIN3D_MAX_CLIPMAP_LEVELS;
	const bool bStateMeshBatching = m_pMaterial.is_valid() && !m_pMaterial->isShaderOverrideEnabled();
	const eastl::array<bool, 3> tier_shadow_enabled = {
		m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled
	};
	const eastl::array<int, 3> tier_shadow_lod_counts = {
		m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount
	};
	m_pTerrainMesher->initialize( this, m_nMeshSize, m_nMeshLods, m_tessellationLevel, m_fVertexSpacing, materials, shadow_materials,
		near_lods, middle_lods, update_budget, m_renderLayers, m_castShadows, m_giMode, m_nShadowLodCount,
		tier_shadow_enabled, tier_shadow_lod_counts, bStateMeshBatching );
}

void T3DTerrain::destroyTerrainMesher( const bool bFinal ) {
	LOG( T3DTerrain::INFO, "Destroying terrain mesher" );
	if ( !m_pTerrainMesher ) {
		return;
	}
	if ( bFinal ) {
		memdelete( m_pTerrainMesher );
		m_pTerrainMesher = nullptr;
	} else {
		m_pTerrainMesher->destroy();
	}
}

void T3DTerrain::setupOceanMesher( void ) {
	if ( m_bOceanEnabled ) {
		m_bLightStateValid = false;
		// t3dsettings_t can be deserialized before the default material is loaded. Build
		// the requested compile-time variant before the mesher captures its RID.
		updateOceanShaderVariant();
		if ( !m_pOceanMesher ) {
			LOG( T3DTerrain::DEBUG, "Creating mesher" );
			m_pOceanMesher = memnew( T3DMesher );
		}
		const RID ocean_rid = m_pOceanMaterial.is_valid() ? m_pOceanMaterial->get_rid() : RID();
		const eastl::array<RID, 3> ocean_materials = { ocean_rid, ocean_rid, ocean_rid };
		m_pOceanMesher->initialize(
			this, m_nOceanMeshSize, m_nOceanMeshLods, m_oceanTessellationLevel, m_fOceanVertexSpacing,
			ocean_materials, ocean_materials, m_nOceanMeshLods + m_oceanTessellationLevel, 0,
			TERRAIN3D_MAX_CLIPMAP_LEVELS, m_oceanRenderLayers, m_oceanCastShadows,
			m_oceanGiMode, m_nOceanMeshLods + m_oceanTessellationLevel, { true, true, true },
			{ -1, -1, -1 }, false );
		m_pOceanMesher->updateAabbs( m_fOceanCullMargin, V2_ZERO );
		if ( m_pOceanMaterial.is_valid() ) {
			ShaderMaterial *const pOceanShaderMat = Object::cast_to<ShaderMaterial>( m_pOceanMaterial.ptr() );
			if ( pOceanShaderMat ) {
				pOceanShaderMat->set_shader_parameter( "_mesh_size", m_nOceanMeshSize );
				pOceanShaderMat->set_shader_parameter( "_vertex_spacing", m_fOceanVertexSpacing );
				pOceanShaderMat->set_shader_parameter( "_vertex_density", 1.0f / m_fOceanVertexSpacing );
				pOceanShaderMat->set_shader_parameter( "_subdiv", pow( 2.f, ( real_t ) ( m_oceanTessellationLevel ) ) );
			}
		}
	}
}

void T3DTerrain::destroyOceanMesher( const bool bFinal ) {
	LOG( T3DTerrain::INFO, "Destroying ocean mesher" );
	if ( !m_pOceanMesher ) {
		return;
	}
	if ( bFinal ) {
		memdelete( m_pOceanMesher );
		m_pOceanMesher = nullptr;
	} else {
		m_pOceanMesher->destroy();
	}
}

void T3DTerrain::setupDisplacementBuffer( void ) {
	if ( !is_inside_tree() ) {
		LOG( T3DTerrain::ERROR, "Not inside the tree, skipping displacement buffer setup" );
		return;
	}
	destroyDisplacementBuffer();
	LOG( T3DTerrain::INFO, "t3dsetting_t up displacement buffer" );
	m_pDBufferVp = memnew( SubViewport );
	m_pDBufferVp->set_name( "DBufferViewport" );
	add_child( m_pDBufferVp, true );
	m_pDBufferVp->set_size( Vector2i( 2, 2 ) );
	m_pDBufferVp->set_disable_3d( true );
	m_pDBufferVp->set_use_hdr_2d( true );
	m_pDBufferVp->set_update_mode( SubViewport::UPDATE_ONCE );
	m_pDBufferVp->set_disable_input( true );
	m_pDBufferVp->set_default_canvas_item_texture_filter( Viewport::DEFAULT_CANVAS_ITEM_TEXTURE_FILTER_NEAREST );

	m_pDBufferRect = memnew( ColorRect );
	m_pDBufferRect->set_name( "DBufferRect" );
	m_pDBufferVp->add_child( m_pDBufferRect, true );
	m_pDBufferRect->set_anchors_preset( Control::PRESET_FULL_RECT );
}

void T3DTerrain::updateDisplacementBuffer( void ) {
	if ( !m_pDBufferVp ) {
		return;
	}
	if ( m_tessellationLevel == 0 ) {
		invalidateComputeDisplacementRequest();
		m_pDBufferVp->set_size( V2I_ZERO );
		m_pDBufferRect->set_size( V2I_ZERO );
	} else {
		m_pDBufferVp->set_size( Vector2i( m_nMeshSize * 4 * m_tessellationLevel, m_nMeshSize * 4 ) );
		m_pDBufferRect->set_size( Vector2i( m_nMeshSize * 4 * m_tessellationLevel, m_nMeshSize * 4 ) );
		LOG( T3DTerrain::INFO, "Updating displacement buffer to Size: ", m_pDBufferVp->get_size() );
		if ( m_pMaterial.is_valid() && m_pMaterial->getMaterialRid().is_valid() ) {
			// Keep a valid raster fallback bound until the asynchronous render-thread
			// compute dispatch publishes its renderer-owned storage texture.
			RS->canvas_item_set_material( m_pDBufferRect->get_canvas_item(), m_pMaterial->getBufferMaterialRid() );
			RS->material_set_param( m_pMaterial->getMaterialRid(), "_displacement_buffer",
								   m_pDBufferVp->get_texture()->get_rid() );
			tryUpdateComputeDisplacement( getClipmapTargetPosition() );
		}
	}
}

bool T3DTerrain::tryUpdateComputeDisplacement( const Vector3 &rTargetPosition ) {
	m_bComputeDisplacementActive.store( false, std::memory_order_release );
	if ( !m_bComputeDisplacementEnabled.load( std::memory_order_acquire ) ||
		m_bComputeDisplacementFailed.load( std::memory_order_acquire ) ||
		m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) || !m_pData || m_pMaterial.is_null() ||
		!m_pMaterial->getMaterialRid().is_valid() ) {
		return false;
	}
	T3DCompute *const pCompute = m_pData->getComputeBackend();
	T3DCompute::t3ddisplacementRequest_t request;
	if ( !pCompute || !pCompute->prepareDisplacementRequest( this, rTargetPosition, request ) ) {
		invalidateComputeDisplacementRequest();
		return false;
	}
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		m_computeDisplacementRequest = std::move( request );
		++m_computeDisplacementRequestVersion;
	}
	queueComputeDisplacementRender();
	return true;
}

void T3DTerrain::invalidateComputeDisplacementRequest( void ) {
	m_bComputeDisplacementActive.store( false, std::memory_order_release );
	std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
	m_computeDisplacementRequest = T3DCompute::t3ddisplacementRequest_t{};
	++m_computeDisplacementRequestVersion;
}

void T3DTerrain::queueComputeDisplacementRender( void ) {
	if ( m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
		return;
	}
	bool bExpected = false;
	if ( !m_bComputeDisplacementDispatchPending.compare_exchange_strong( bExpected, true, std::memory_order_acq_rel,
																		std::memory_order_acquire ) ) {
		return;
	}
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( !pServer ) {
		m_bComputeDisplacementDispatchPending.store( false, std::memory_order_release );
		m_bComputeDisplacementFailed.store( true, std::memory_order_release );
		m_bComputeDisplacementFallbackRequested.store( true, std::memory_order_release );
		return;
	}
	if ( pServer->is_on_render_thread() ) {
		renderUpdateComputeDisplacement();
	} else {
		pServer->call_on_render_thread( callable_mp( this, &T3DTerrain::renderUpdateComputeDisplacement ) );
	}
}

void T3DTerrain::renderUpdateComputeDisplacement( void ) {
	T3DCompute::t3ddisplacementRequest_t request;
	uint64_t request_version = 0u;
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		request = m_computeDisplacementRequest;
		request_version = m_computeDisplacementRequestVersion;
	}

	bool bSuccess = false;
	RID texture;
	T3DCompute *const pCompute = m_pData ? m_pData->getComputeBackendIfCreated() : nullptr;
	if ( !m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) && pCompute ) {
		bSuccess = pCompute->dispatchDisplacementRequest( request, texture ) && texture.is_valid();
	}
	bool bRequestCurrent = false;
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		bRequestCurrent = m_computeDisplacementRequestVersion == request_version;
	}
	if ( bSuccess && bRequestCurrent && m_bComputeDisplacementEnabled.load( std::memory_order_acquire ) &&
		!m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
		RenderingServer *const pServer = RenderingServer::get_singleton();
		if ( pServer ) {
			pServer->material_set_param( request.materialRid, "_displacement_buffer", texture );
			m_bComputeDisplacementActive.store( true, std::memory_order_release );
		}
	} else if ( !bSuccess && !m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
		m_bComputeDisplacementActive.store( false, std::memory_order_release );
		m_bComputeDisplacementFailed.store( true, std::memory_order_release );
		m_bComputeDisplacementFallbackRequested.store( true, std::memory_order_release );
	}

	m_bComputeDisplacementDispatchPending.store( false, std::memory_order_release );
	bool bNewerRequest = false;
	bool bNewerRequestValid = false;
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		bNewerRequest = m_computeDisplacementRequestVersion != request_version;
		bNewerRequestValid = m_computeDisplacementRequest.valid();
	}
	if ( bNewerRequest && bNewerRequestValid && m_bComputeDisplacementEnabled.load( std::memory_order_acquire ) &&
		!m_bComputeDisplacementFailed.load( std::memory_order_acquire ) &&
		!m_bComputeDisplacementTearingDown.load( std::memory_order_acquire ) ) {
		queueComputeDisplacementRender();
	}
}

void T3DTerrain::renderShutdownComputeDisplacement( void ) {
	T3DCompute *const pCompute = m_pData ? m_pData->getComputeBackendIfCreated() : nullptr;
	if ( pCompute ) {
		pCompute->shutdownDisplacementResources();
	}
}

void T3DTerrain::setComputeDisplacementEnabled( const bool bEnabled ) {
	const bool bOldEnabled = m_bComputeDisplacementEnabled.exchange( bEnabled, std::memory_order_acq_rel );
	if ( bOldEnabled == bEnabled ) {
		return;
	}
	invalidateComputeDisplacementRequest();
	m_bComputeDisplacementFailed.store( false, std::memory_order_release );
	m_bComputeDisplacementFallbackRequested.store( false, std::memory_order_release );
	m_lastBufferPosition = V2_MAX;
	if ( m_bInitialized ) {
		updateDisplacementBuffer();
	}
}

void T3DTerrain::setDisplacementUpdateDistance( const real_t fDistance ) {
	SET_IF_DIFF( m_fDisplacementUpdateDistance, CLAMP( fDistance, 0.0f, 16.0f ) );
	m_lastBufferPosition = V2_MAX;
}

void T3DTerrain::buildContainers( void ) {
	if ( m_pLabelParent ) {
		return;
	}
	m_pLabelParent = memnew( Node3D );
	m_pLabelParent->set_name( "Labels" );
	add_child( m_pLabelParent, true );
}

void T3DTerrain::destroyContainers( void ) {
	if ( m_pLabelParent ) {
		memdelete( m_pLabelParent );
		m_pLabelParent = nullptr;
	}
}

void T3DTerrain::destroyLabels( void ) {
	if ( !m_pLabelParent ) {
		return;
	}
	const Array labels = m_pLabelParent->get_children();
	LOG( T3DTerrain::DEBUG, "Destroying ", labels.size(), " region labels" );
	for ( const Variant &var : labels ) {
		Node *const pLabel = cast_to<Node>( var );
		if ( pLabel ) {
			memdelete( pLabel );
		}
	}
}

void T3DTerrain::destroyDisplacementBuffer( void ) {
	LOG( T3DTerrain::INFO, "[compute] Draining displacement work and releasing buffer resources" );
	m_bComputeDisplacementTearingDown.store( true, std::memory_order_release );
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && m_pData && m_pData->getComputeBackendIfCreated() ) {
		if ( pServer->is_on_render_thread() ) {
			renderShutdownComputeDisplacement();
		} else {
			pServer->call_on_render_thread( callable_mp( this, &T3DTerrain::renderShutdownComputeDisplacement ) );
			// Lifecycle-only synchronization: all queued displacement work and its
			// renderer-owned resources must be gone before the compute backend or node dies.
			pServer->force_sync();
		}
	}
	m_bComputeDisplacementDispatchPending.store( false, std::memory_order_release );
	m_bComputeDisplacementActive.store( false, std::memory_order_release );
	m_bComputeDisplacementFailed.store( false, std::memory_order_release );
	m_bComputeDisplacementFallbackRequested.store( false, std::memory_order_release );
	{
		std::lock_guard<std::mutex> lock( m_computeDisplacementMutex );
		m_computeDisplacementRequest = T3DCompute::t3ddisplacementRequest_t{};
		++m_computeDisplacementRequestVersion;
	}
	m_bComputeDisplacementTearingDown.store( false, std::memory_order_release );
	LOG( T3DTerrain::DEBUG, "Freeing d_buffer_rect" );
	if ( m_pDBufferRect ) {
		memdelete( m_pDBufferRect );
		m_pDBufferRect = nullptr;
	}
	LOG( T3DTerrain::DEBUG, "Freeing d_buffer_vp" );
	if ( m_pDBufferVp ) {
		memdelete( m_pDBufferVp );
		m_pDBufferVp = nullptr;
	}
}

void T3DTerrain::setupMousePicking( void ) {
	if ( !is_inside_tree() ) {
		LOG( T3DTerrain::ERROR, "Not inside the tree, skipping mouse setup" );
		return;
	}
	LOG( T3DTerrain::INFO, "t3dsetting_t up mouse picker and get_intersection viewport, camera & screen quad" );
	m_pMouseVp = memnew( SubViewport );
	m_pMouseVp->set_name( "MouseViewport" );
	add_child( m_pMouseVp, true );
	m_pMouseVp->set_size( V2I( 2 ) );
	m_pMouseVp->set_scaling_3d_mode( Viewport::SCALING_3D_MODE_BILINEAR );
	m_pMouseVp->set_update_mode( SubViewport::UPDATE_ONCE );
	m_pMouseVp->set_disable_input( true );
	m_pMouseVp->set_canvas_cull_mask( 0 );
	m_pMouseVp->set_use_hdr_2d( true );
	m_pMouseVp->set_anisotropic_filtering_level( Viewport::ANISOTROPY_DISABLED );
	m_pMouseVp->set_default_canvas_item_texture_filter( Viewport::DEFAULT_CANVAS_ITEM_TEXTURE_FILTER_NEAREST );
	m_pMouseVp->set_positional_shadow_atlas_size( 0 );
	m_pMouseVp->set_positional_shadow_atlas_quadrant_subdiv( 0, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED );
	m_pMouseVp->set_positional_shadow_atlas_quadrant_subdiv( 1, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED );
	m_pMouseVp->set_positional_shadow_atlas_quadrant_subdiv( 2, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED );
	m_pMouseVp->set_positional_shadow_atlas_quadrant_subdiv( 3, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED );

	m_pMouseCam = memnew( Camera3D );
	m_pMouseCam->set_name( "MouseCamera" );
	m_pMouseVp->add_child( m_pMouseCam, true );
	Ref<Environment> pEnv;
	pEnv.instantiate();
	pEnv->set_tonemapper( Environment::TONE_MAPPER_LINEAR );
	m_pMouseCam->set_environment( pEnv );
	Ref<Compositor> pComp;
	pComp.instantiate();
	m_pMouseCam->set_compositor( pComp );
	m_pMouseCam->set_projection( Camera3D::PROJECTION_ORTHOGONAL );
	m_pMouseCam->set_size( 0.1f );
	m_pMouseCam->set_far( 100000.f );

	m_pMouseQuad = memnew( MeshInstance3D );
	m_pMouseQuad->set_name( "MouseQuad" );
	m_pMouseCam->add_child( m_pMouseQuad, true );
	Ref<QuadMesh> pQuad;
	pQuad.instantiate();
	pQuad->set_size( V2( 0.1f ) );
	m_pMouseQuad->set_mesh( pQuad );
	const String shader_code = String(
#include "shaders/gpu_depth.glsl"
	);
	Ref<Shader> pShader;
	pShader.instantiate();
	pShader->set_code( shader_code );
	Ref<ShaderMaterial> pShaderMaterial;
	pShaderMaterial.instantiate();
	pShaderMaterial->set_shader( pShader );
	m_pMouseQuad->set_surface_override_material( 0, pShaderMaterial );
	m_pMouseQuad->set_position( Vector3( 0.f, 0.f, -0.5f ) );

	// Set terrain, terrain shader, mouse camera, and screen quad to mouse layer
	const uint32_t force_update_layer = m_mouseLayer;
	m_mouseLayer = 0u;
	setMouseLayer( force_update_layer );
}

void T3DTerrain::destroyMousePicking( void ) {
	LOG( T3DTerrain::DEBUG, "Freeing mouse_quad" );
	if ( m_pMouseQuad ) {
		memdelete( m_pMouseQuad );
		m_pMouseQuad = nullptr;
	}
	LOG( T3DTerrain::DEBUG, "Freeing mouse_cam" );
	if ( m_pMouseCam ) {
		memdelete( m_pMouseCam );
		m_pMouseCam = nullptr;
	}
	LOG( T3DTerrain::DEBUG, "Freeing mouse_vp" );
	if ( m_pMouseVp ) {
		memdelete( m_pMouseVp );
		m_pMouseVp = nullptr;
	}
}

void T3DTerrain::destroyInstancer( void ) {
	LOG( T3DTerrain::INFO, "Destroying Instancer" );
	if ( m_pInstancer ) {
		memdelete( m_pInstancer );
		m_pInstancer = nullptr;
	}
}

void T3DTerrain::destroyNodeStreamer( const bool bCaptureState ) {
	if ( m_pNodeStreamer ) {
		m_pNodeStreamer->destroy( bCaptureState );
		memdelete( m_pNodeStreamer );
		m_pNodeStreamer = nullptr;
	}
}

void T3DTerrain::destroyLightmapStreamer( void ) {
	if ( m_pLightmapStreamer ) {
		m_pLightmapStreamer->destroy();
		memdelete( m_pLightmapStreamer );
		m_pLightmapStreamer = nullptr;
	}
}

void T3DTerrain::destroyHlodStreamer( void ) {
	if ( m_pHlodStreamer ) {
		m_pHlodStreamer->destroy();
		memdelete( m_pHlodStreamer );
		m_pHlodStreamer = nullptr;
	}
}

bool T3DTerrain::bakeRegionSnapshotLess( const t3dbakeRegionSnapshot_t &rA, const t3dbakeRegionSnapshot_t &rB ) {
	if ( rA.location.y != rB.location.y ) {
		return rA.location.y < rB.location.y;
	}
	return rA.location.x < rB.location.x;
}

const T3DTerrain::t3dbakeRegionSnapshot_t *T3DTerrain::t3dbakeSnapshot_t::getRegion( const int32_t iX, const int32_t iZ ) const {
	if ( nRegionSize <= 0 ) {
		return nullptr;
	}

	const Vector2i regionLocation(
		terrain3dDivideFloor( iX, nRegionSize ),
		terrain3dDivideFloor( iZ, nRegionSize )
	);
	const int32_t map_index = T3DData::getRegionMapIndex( regionLocation );
	if ( map_index < 0 ) {
		return nullptr;
	}
	const int32_t region_index = szRegionLookup[map_index];
	if ( region_index < 0 || region_index >= ( int32_t )regions.size() ) {
		return nullptr;
	}
	return &regions[( size_t ) ( region_index )];
}

void T3DTerrain::t3dbakeSnapshot_t::getSample(
	const int32_t iX, const int32_t iZ, uint32_t &nControl,
	real_t &fHeight
) const {
	const t3dbakeRegionSnapshot_t *pRegion = getRegion( iX, iZ );
	if ( !pRegion || !pRegion->pHeightBytes ) {
		nControl = UINT32_MAX;
		fHeight = NAN;
		return;
	}
	const int32_t local_x = iX - pRegion->location.x * nRegionSize;
	const int32_t local_z = iZ - pRegion->location.y * nRegionSize;
	if ( local_x < 0 || local_z < 0 || local_x >= nRegionSize || local_z >= nRegionSize ) {
		nControl = UINT32_MAX;
		fHeight = NAN;
		return;
	}
	const int32_t index = local_z * nRegionSize + local_x;
	fHeight = ( real_t ) ( Terrain3DRegionCodec::readHeightBytes( pRegion->pHeightBytes, index, Image::FORMAT_R16 ) );
	if ( pRegion->pControlBytes ) {
		uint32_t control = TERRAIN3D_REGION_DEFAULT_CONTROL;
		std::memcpy( &control, pRegion->pControlBytes + ( size_t ) ( index ) * sizeof( uint32_t ), sizeof( uint32_t ) );
		nControl = control;
	} else {
		nControl = TERRAIN3D_REGION_DEFAULT_CONTROL;
	}
	return;
}

bool T3DTerrain::t3dbakeSnapshot_t::getMinimumRect(
	const int32_t iX, const int32_t iZ, const int32_t nWidth,
	const int32_t nHeight, real_t &fHeight
) const {
	if ( nRegionSize <= 0 || nWidth <= 0 || nHeight <= 0 ) {
		return false;
	}
	real_t fMinimum = std::numeric_limits<real_t>::infinity();
	for ( int32_t row = 0; row < nHeight; ++row ) {
		const int32_t global_z = iZ + row;
		int32_t copied = 0;
		while ( copied < nWidth ) {
			const int32_t global_x = iX + copied;
			const t3dbakeRegionSnapshot_t *pRegion = getRegion( global_x, global_z );
			if ( !pRegion || !pRegion->pHeightBytes ) {
				return false;
			}
			const int32_t local_x = global_x - pRegion->location.x * nRegionSize;
			const int32_t local_z = global_z - pRegion->location.y * nRegionSize;
			if ( local_x < 0 || local_z < 0 || local_x >= nRegionSize || local_z >= nRegionSize ) {
				return false;
			}
			const int32_t run = MIN( nWidth - copied, nRegionSize - local_x );
			const int32_t index = local_z * nRegionSize + local_x;
			const uint16_t *pHeight = ( const uint16_t * ) ( pRegion->pHeightBytes ) + index;
			const uint32_t *pControl = pRegion->pControlBytes
				? ( const uint32_t * ) ( pRegion->pControlBytes ) + index
				: nullptr;
			for ( int32_t i = 0; i < run; ++i ) {
				const uint32_t sample_control = pControl ? pControl[i] : TERRAIN3D_REGION_DEFAULT_CONTROL;
				if ( isHole( sample_control ) ) {
					return false;
				}
				fMinimum = MIN( fMinimum, ( real_t ) ( Terrain3DRegionCodec::decodeHeightU16( pHeight[i] ) ) );
			}
			copied += run;
		}
	}
	if ( !std::isfinite( fMinimum ) ) {
		return false;
	}
	fHeight = fMinimum;
	return true;
}

Vector3 T3DTerrain::t3dbakeSnapshot_t::getMeshVertex(
	const int32_t nLod, const T3DData::t3dheightFilter_t filter,
	const int32_t iX, const int32_t iZ, uint32_t &nControl
) const {
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	real_t fHeight = NAN;
	getSample( iX, iZ, nControl, fHeight );
	if ( isHole( nControl ) ) {
		fHeight = NAN;
	}

	if ( filter == T3DData::HEIGHT_FILTER_MINIMUM &&
		!getMinimumRect( iX - step / 2, iZ - step / 2, step, step, fHeight ) ) {
		fHeight = NAN;
	}

	return Vector3( ( real_t ) ( iX ) * fVertexSpacing, fHeight, ( real_t ) ( iZ ) * fVertexSpacing );
}

T3DTerrain::t3dbakeSnapshot_t T3DTerrain::createBakeSnapshot( void ) const {
	t3dbakeSnapshot_t snapshot;
	snapshot.nRegionSize = ( int32_t )m_nRegionSize;
	snapshot.fVertexSpacing = m_fVertexSpacing;
	std::fill_n( snapshot.szRegionLookup, TERRAIN3D_MAX_REGION_SLOTS, ( int32_t ) ( -1 ) );

	const TypedArray<Vector2i> region_locations = m_pData->getRegionLocations();
	snapshot.regions.reserve( region_locations.size() );
	const int64_t expected_height_bytes =
		( int64_t ) ( snapshot.nRegionSize ) * ( int64_t ) ( snapshot.nRegionSize ) * ( int64_t ) ( sizeof( uint16_t ) );
	const int64_t expected_control_bytes =
		( int64_t ) ( snapshot.nRegionSize ) * ( int64_t ) ( snapshot.nRegionSize ) * ( int64_t ) ( sizeof( uint32_t ) );

	for ( const Vector2i regionLocation : region_locations ) {
		if ( !m_pData->ensureCpuRegionResident( regionLocation ) ) {
			LOG( T3DTerrain::ERROR, "Skipping terrain region that could not be paged for bake snapshot: ", regionLocation );
			continue;
		}
		T3DRegion *const pRegion = m_pData->getRegionPtr( regionLocation );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}

		const Ref<Image> pHeightMap = pRegion->getHeightMap();
		const Ref<Image> pControlMap = pRegion->getControlMap();
		if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
			pHeightMap->get_width() != snapshot.nRegionSize || pHeightMap->get_height() != snapshot.nRegionSize ||
			( pControlMap.is_valid() && ( pControlMap->get_format() != Image::FORMAT_RF ||
				pControlMap->get_width() != snapshot.nRegionSize || pControlMap->get_height() != snapshot.nRegionSize ) ) ) {
			LOG( T3DTerrain::ERROR, "Skipping invalid terrain region during bake snapshot: ", regionLocation );
			continue;
		}

		t3dbakeRegionSnapshot_t region_snapshot;
		region_snapshot.location = regionLocation;
		region_snapshot.heightData = pHeightMap->get_data();
		if ( pControlMap.is_valid() ) {
			region_snapshot.controlData = pControlMap->get_data();
		}
		if ( region_snapshot.heightData.size() < expected_height_bytes ||
			( pControlMap.is_valid() && region_snapshot.controlData.size() < expected_control_bytes ) ) {
			LOG( T3DTerrain::ERROR, "Skipping terrain region with incomplete map data during bake snapshot: ", regionLocation );
			continue;
		}

		const int32_t map_index = T3DData::getRegionMapIndex( regionLocation );
		if ( map_index < 0 ) {
			continue;
		}
		snapshot.regions.push_back( std::move( region_snapshot ) );
	}

	// Keep adjacent terrain regions adjacent in the snapshot vector. Whole-terrain
	// bake tasks iterate this order, so spatial sorting improves reuse of neighboring
	// height/control payloads and makes worker task construction deterministic even
	// when the resource's active-region order reflects edit history.
	std::sort( snapshot.regions.begin(), snapshot.regions.end(), bakeRegionSnapshotLess );
	for ( size_t region_index = 0; region_index < snapshot.regions.size(); ++region_index ) {
		const int32_t map_index = T3DData::getRegionMapIndex( snapshot.regions[region_index].location );
		if ( map_index >= 0 ) {
			snapshot.szRegionLookup[map_index] = ( int32_t ) ( region_index );
		}
	}

	return snapshot;
}

void T3DTerrain::runBakeTaskRange( void *const pContext, const size_t nBegin, const size_t nEnd ) {
	t3dbakeTaskContext_t *context = ( t3dbakeTaskContext_t * ) ( pContext );
	for ( size_t task_index = nBegin; task_index < nEnd; ++task_index ) {
		const t3dbakeRowTask_t &task = ( *context->pTasks )[task_index];
		t3dtriangleBuffer_t &buffer = ( *context->pBuffers )[task_index];
		for ( int32_t z = task.zStart; z < task.zEnd; z += context->step ) {
			for ( int32_t x = task.xStart; x < task.xEnd; x += context->step ) {
				context->pTerrain->generateTrianglePair( buffer, context->bGenerateUvs, context->lod, context->filter,
					context->bRequireNav, x, z, context->pSnapshot );
			}
		}
	}
}

Vector3 T3DTerrain::getTriangleVertex(
	const t3dbakeSnapshot_t *pSnapshot, const int32_t nLod,
	const T3DData::t3dheightFilter_t filter, const int32_t iX, const int32_t iZ,
	const Vector3 &rPosition, uint32_t &nControl
) const {
	if ( pSnapshot ) {
		return pSnapshot->getMeshVertex( nLod, filter, iX, iZ, nControl );
	}
	const Vector3 vertex = m_pData->getMeshVertex( nLod, filter, rPosition );
	nControl = m_pData->getControl( rPosition );
	return vertex;
}

void T3DTerrain::generateTriangles(
	PackedVector3Array &rVertices, PackedVector2Array *const pUvs, const int32_t nLod,
	const T3DData::t3dheightFilter_t filter, const bool bRequireNav,
	const AABB &rGlobalAabb
) const {
	ERR_FAIL_COND( m_pData == nullptr );
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	const bool generate_uvs = pUvs != nullptr;
	eastl::vector<t3dtriangleBuffer_t> buffers;
	T3DLinearArena bake_arena;
	Terrain3DArenaBuffer<Vector3> bake_vertex_slab;
	Terrain3DArenaBuffer<Vector2> bake_uv_slab;

	// Whole-terrain bakes are split into row chunks. This parallelizes even a single large region,
	// while keeping each worker's output isolated and preserving deterministic merge order.
	if ( !rGlobalAabb.has_volume() ) {


		const int32_t nRegionSize = ( int32_t )m_nRegionSize;
		const int32_t rows_per_task = 16;
		t3dbakeSnapshot_t snapshot = createBakeSnapshot();
		for ( t3dbakeRegionSnapshot_t &region : snapshot.regions ) {
			region.pHeightBytes = region.heightData.ptr();
			region.pControlBytes = region.controlData.is_empty() ? nullptr : region.controlData.ptr();
		}
		eastl::vector<t3dbakeRowTask_t> tasks;
		tasks.reserve( snapshot.regions.size() * MAX( 1, nRegionSize / ( step * rows_per_task ) ) );

		size_t total_output_capacity = 0u;
		for ( const t3dbakeRegionSnapshot_t &region : snapshot.regions ) {
			const Vector2i region_loc = region.location;
			const Vector2i region_pos = region_loc * nRegionSize;
			const int32_t region_z_end = region_pos.y + nRegionSize;
			for ( int32_t z = region_pos.y; z < region_z_end; z += step * rows_per_task ) {
				const int32_t z_end = MIN( z + step * rows_per_task, region_z_end );
				const size_t column_count = ( size_t ) ( ( nRegionSize + step - 1 ) / step );
				const size_t row_count = ( size_t ) ( ( z_end - z + step - 1 ) / step );
				ERR_FAIL_COND_MSG( column_count != 0u && row_count > std::numeric_limits<size_t>::max() / column_count,
					"Terrain bake task dimensions overflow size_t." );
				const size_t quad_count = column_count * row_count;
				ERR_FAIL_COND_MSG( quad_count > std::numeric_limits<size_t>::max() / 6u,
					"Terrain bake triangle capacity overflows size_t." );
				const size_t output_capacity = quad_count * 6u;
				// Isolate adjacent worker slabs on independent cacheline boundaries. The
				// merge uses each buffer's written size, so padding remains invisible.
				ERR_FAIL_COND_MSG( total_output_capacity > std::numeric_limits<size_t>::max() -
					( TERRAIN3D_BAKE_TASK_ALIGNMENT_ELEMENTS - 1u ),
					"Terrain bake task alignment overflows size_t." );
				total_output_capacity = TERRAIN3D_ALIGN_BAKE_TASK_OFFSET( total_output_capacity );
				ERR_FAIL_COND_MSG( output_capacity > std::numeric_limits<size_t>::max() - total_output_capacity,
					"Terrain bake output capacity overflows size_t." );
				tasks.push_back( {region_pos.x, region_pos.x + nRegionSize, z, z_end, total_output_capacity,
							 output_capacity} );
				total_output_capacity += output_capacity;
			}
		}

		T3DLinearArenaLayout bake_layout;
		const bool bLayoutValid = bake_layout.addArray<Vector3>( total_output_capacity ) &&
			( !generate_uvs || bake_layout.addArray<Vector2>( total_output_capacity ) );
		ERR_FAIL_COND_MSG( !bLayoutValid || !bake_layout.valid() || !bake_arena.configure( bake_layout.size() ),
			"Terrain bake scratch arena allocation failed." );
		ERR_FAIL_COND_MSG( !bake_vertex_slab.allocateUninitialized( bake_arena, total_output_capacity ),
			"Terrain bake vertex slab allocation did not match its precomputed layout." );
		if ( generate_uvs ) {
			ERR_FAIL_COND_MSG( !bake_uv_slab.allocateUninitialized( bake_arena, total_output_capacity ),
				"Terrain bake UV slab allocation did not match its precomputed layout." );
		}
		ERR_FAIL_COND_MSG( bake_arena.used() != bake_layout.size(),
			"Terrain bake scratch arena consumption did not match its precomputed layout." );
		buffers.resize( tasks.size() );
		for ( size_t task_index = 0; task_index < tasks.size(); ++task_index ) {
			const t3dbakeRowTask_t &task = tasks[task_index];
			buffers[task_index].bindExternal( bake_vertex_slab.data() + task.iOutputOffset,
				generate_uvs ? bake_uv_slab.data() + task.iOutputOffset : nullptr, task.nOutputCapacity );
		}
		if ( !tasks.empty() ) {
#if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
			const uint32_t worker_count = 1u;
#else
			const uint32_t hardware_threads = MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
			const uint32_t worker_count = std::min( {hardware_threads, ( uint32_t )tasks.size(), 32u} );
#endif
			const T3DParallelExecutor executor{( int32_t ) ( worker_count )};
			t3dbakeTaskContext_t task_context;
			task_context.pTerrain = this;
			task_context.pSnapshot = &snapshot;
			task_context.pTasks = &tasks;
			task_context.pBuffers = &buffers;
			task_context.step = step;
			task_context.lod = nLod;
			task_context.filter = filter;
			task_context.bGenerateUvs = generate_uvs;
			task_context.bRequireNav = bRequireNav;

			LOG( T3DTerrain::INFO, "Generating terrain bake geometry with up to ", worker_count, " persistent worker tasks across ",
				tasks.size(), " chunks" );
			executor.run( tasks.size(), 1u, runBakeTaskRange, &task_context );
		}

		for ( const t3dtriangleBuffer_t &buffer : buffers ) {
			ERR_FAIL_COND_MSG( buffer.bOverflowed,
				"Terrain bake output exceeded its precomputed slab capacity. This indicates a bake sizing invariant failure." );
		}
	} else {
		// AABB generation is primarily used by runtime navigation baking. Keep it serial because these
		// requests are generally small and thread setup costs can exceed the geometry work.
		t3dtriangleBuffer_t buffer;
		const int32_t z_start = ( int32_t )Math::ceil( rGlobalAabb.position.z / m_fVertexSpacing );
		const int32_t z_end = ( int32_t )Math::floor( rGlobalAabb.get_end().z / m_fVertexSpacing ) + 1;
		const int32_t x_start = ( int32_t )Math::ceil( rGlobalAabb.position.x / m_fVertexSpacing );
		const int32_t x_end = ( int32_t )Math::floor( rGlobalAabb.get_end().x / m_fVertexSpacing ) + 1;
		const size_t column_count = ( size_t ) ( MAX( 0, x_end - x_start + step - 1 ) / step );
		const size_t row_count = ( size_t ) ( MAX( 0, z_end - z_start + step - 1 ) / step );
		buffer.reserve( column_count * row_count * 6u, generate_uvs );

		for ( int32_t z = z_start; z < z_end; z += step ) {
			for ( int32_t x = x_start; x < x_end; x += step ) {
				const real_t fHeight =
					m_pData->getHeight( Vector3( ( real_t ) ( x ) * m_fVertexSpacing, 0.f, ( real_t ) ( z ) * m_fVertexSpacing ) );
				if ( fHeight >= rGlobalAabb.position.y && fHeight <= rGlobalAabb.get_end().y ) {
					generateTrianglePair( buffer, generate_uvs, nLod, filter, bRequireNav, x, z );
				}
			}
		}
		buffers.push_back( std::move( buffer ) );
	}

	size_t vertex_count = 0;
	size_t uv_count = 0;
	for ( const t3dtriangleBuffer_t &buffer : buffers ) {
		vertex_count += buffer.size();
		uv_count += buffer.uvSize();
	}
	ERR_FAIL_COND( generate_uvs && vertex_count != uv_count );
	ERR_FAIL_COND( vertex_count > ( size_t ) ( std::numeric_limits<int32_t>::max() ) );

	rVertices.resize( ( int64_t )vertex_count );
	if ( vertex_count > 0 ) {
		Vector3 *const pVertexWrite = rVertices.ptrw();
		size_t vertex_offset = 0;
		for ( const t3dtriangleBuffer_t &buffer : buffers ) {
			if ( buffer.size() > 0u ) {
				std::copy_n( buffer.vertexData(), buffer.size(), pVertexWrite + vertex_offset );
			}
			vertex_offset += buffer.size();
		}
	}

	if ( generate_uvs ) {
		pUvs->resize( ( int64_t )uv_count );
		if ( uv_count > 0 ) {
			Vector2 *const pUvWrite = pUvs->ptrw();
			size_t uv_offset = 0;
			for ( const t3dtriangleBuffer_t &buffer : buffers ) {
				if ( buffer.uvSize() > 0u ) {
					std::copy_n( buffer.uvData(), buffer.uvSize(), pUvWrite + uv_offset );
				}
				uv_offset += buffer.uvSize();
			}
		}
	}
}

// Generates two triangles: Top 124, Bottom 143
//		1  __  2
//		  |\ |
//		  | \|
//		3  --  4
// p_require_nav is false for the runtime baker, which ignores navigation.
void T3DTerrain::generateTrianglePair(
	t3dtriangleBuffer_t &rBuffer, const bool bGenerateUvs, const int32_t nLod,
	const T3DData::t3dheightFilter_t filter, const bool bRequireNav,
	const int32_t x, const int32_t z, const t3dbakeSnapshot_t *pSnapshot
) const {
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	const Vector3 xz = Vector3( x, 0.0f, z ) * m_fVertexSpacing;
	const Vector3 xsz = Vector3( x + step, 0.0f, z ) * m_fVertexSpacing;
	const Vector3 xzs = Vector3( x, 0.0f, z + step ) * m_fVertexSpacing;
	const Vector3 xszs = Vector3( x + step, 0.0f, z + step ) * m_fVertexSpacing;
	uint32_t ctrl1 = UINT32_MAX;
	uint32_t ctrl2 = UINT32_MAX;
	uint32_t ctrl3 = UINT32_MAX;
	uint32_t ctrl4 = UINT32_MAX;
	const Vector3 v1 = getTriangleVertex( pSnapshot, nLod, filter, x, z, xz, ctrl1 );
	const bool bNan1 = std::isnan( v1.y );
	if ( bNan1 ) {
		return;
	}
	Vector3 v2 = getTriangleVertex( pSnapshot, nLod, filter, x + step, z, xsz, ctrl2 );
	Vector3 v3 = getTriangleVertex( pSnapshot, nLod, filter, x, z + step, xzs, ctrl3 );
	Vector3 v4 = getTriangleVertex( pSnapshot, nLod, filter, x + step, z + step, xszs, ctrl4 );
	const bool bNan2 = std::isnan( v2.y );
	const bool bNan3 = std::isnan( v3.y );
	const bool bNan4 = std::isnan( v4.y );
	if ( bNan2 ) {
		v2.y = v1.y;
	}
	if ( bNan3 ) {
		v3.y = v1.y;
	}
	if ( bNan4 ) {
		if ( !bNan2 ) {
			v4.y = v2.y;
		} else if ( !bNan3 ) {
			v4.y = v3.y;
		} else {
			v4.y = v1.y;
		}
	}
	const bool bHole1 = ctrl1 != UINT32_MAX && isHole( ctrl1 );
	const bool bHole2 = ctrl2 != UINT32_MAX && isHole( ctrl2 );
	const bool bHole3 = ctrl3 != UINT32_MAX && isHole( ctrl3 );
	const bool bHole4 = ctrl4 != UINT32_MAX && isHole( ctrl4 );
	const bool bNav1 = ctrl1 != UINT32_MAX && isNav( ctrl1 );
	const bool bNav2 = ( ctrl2 != UINT32_MAX && isNav( ctrl2 ) ) || ( bNan2 && bNav1 );
	const bool bNav3 = ( ctrl3 != UINT32_MAX && isNav( ctrl3 ) ) || ( bNan3 && bNav1 );
	const bool bNav4 = ( ctrl4 != UINT32_MAX && isNav( ctrl4 ) ) || ( bNan4 && bNav1 );

	if ( !( bHole1 || bHole4 || bHole3 ) && ( !bRequireNav || ( bNav1 && bNav4 && bNav3 ) ) ) {
		rBuffer.appendTriangle( v1, v4, v3, bGenerateUvs );
	}
	if ( !( bHole1 || bHole2 || bHole4 ) && ( !bRequireNav || ( bNav1 && bNav2 && bNav4 ) ) ) {
		rBuffer.appendTriangle( v1, v2, v4, bGenerateUvs );
	}
}

///////////////////////////
// Public Functions
///////////////////////////

T3DTerrain::T3DTerrain( void ) {
	LOG( T3DTerrain::INFO, "Terrain3D v", m_version, " - https://github.com/TokisanGames/Terrain3D" );
	// Process the command line
	PackedStringArray args = OS::get_singleton()->get_cmdline_args();
	for ( int i = args.size() - 1; i >= 0; i-- ) {
		String arg = args[i];
		if ( arg.begins_with( "--terrain3d-debug=" ) ) {
			String value = arg.rsplit( "=" )[1];
			if ( value == "ERROR" ) {
				setDebugLevel( T3DTerrain::ERROR );
			} else if ( value == "INFO" ) {
				setDebugLevel( T3DTerrain::INFO );
			} else if ( value == "DEBUG" ) {
				setDebugLevel( T3DTerrain::DEBUG );
			} else if ( value == "EXTREME" ) {
				setDebugLevel( T3DTerrain::EXTREME );
			}
		}
	}
}

T3DTerrain::~T3DTerrain( void ) {
	if ( m_pRuntimeProfileHistory ) {
		memdelete( m_pRuntimeProfileHistory );
		m_pRuntimeProfileHistory = nullptr;
	}
	if ( T3DZoneAllocatorRouter::zone() == &m_runtimeMemoryZone ) {
		T3DZoneAllocatorRouter::install( nullptr );
	}
	if ( m_runtimeMemoryZone.state() == T3DMemoryZone::ACTIVE ) {
		m_runtimeMemoryZone.beginDraining();
	}
}

bool T3DTerrain::initializeRuntimeMemory( void ) {
	if ( m_bRuntimeMemoryReady ) {
		if ( m_pData && m_pData->isActiveStorageOwnerInternal() ) return true;
		const t3dmemoryArenaSpan_t active_span = m_runtimeMemoryArena.resolve( m_runtimeActiveHeightSlice );
		return m_pData && active_span.valid() && m_pData->bindActiveStorageInternal(
			( int ) ( m_nRegionSize ), active_span.data, active_span.size );
	}
	// Arbitrary-lifetime worker/container storage is isolated in the M2 zone.
	// Active/resident fixed page banks retain separate binding work until their
	// P09 immutable-publication topology is enabled; they must never use this lock.
	const size_t active_bytes = terrain3dActiveStorageRequiredBytes( ( int ) ( m_nRegionSize ) );
	const size_t zone_bytes = 16u * TERRAIN3D_MEMORY_MIB;
	if ( active_bytes == 0u || active_bytes > std::numeric_limits<size_t>::max() - zone_bytes -
		2u * TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT ) return false;
	T3DMemoryArenaLayout layout( active_bytes + zone_bytes + 2u * TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT );
	if ( !layout.reserveLow( active_bytes, TERRAIN3D_ACTIVE_STORAGE_ALIGNMENT, m_runtimeActiveHeightSlice ) ||
		!layout.reserveLow( zone_bytes, TERRAIN3D_ARENA_DEFAULT_BASE_ALIGNMENT, m_runtimeZoneSlice ) ||
		!layout.seal() || !m_runtimeMemoryArena.initialize( layout ) || !m_runtimeMemoryArena.commit() ) {
		return false;
	}
	const t3dmemoryArenaSpan_t active_span = m_runtimeMemoryArena.resolve( m_runtimeActiveHeightSlice );
	const t3dmemoryArenaSpan_t zone_span = m_runtimeMemoryArena.resolve( m_runtimeZoneSlice );
	if ( !m_pData || !active_span.valid() || !m_pData->bindActiveStorageInternal( ( int ) ( m_nRegionSize ), active_span.data, active_span.size ) ||
		!zone_span.valid() || !m_runtimeMemoryZone.initialize( zone_span.data, zone_span.size ) ||
		!m_runtimeMemoryArena.seal() ) {
		m_runtimeMemoryArena.release();
		return false;
	}
	T3DZoneAllocatorRouter::install( &m_runtimeMemoryZone );
	m_bRuntimeMemoryReady = true;
	return true;
}

void T3DTerrain::setRuntimeProfilingEnabled( const bool bEnabled ) {
	if ( bEnabled && !m_pRuntimeProfileHistory ) {
		m_pRuntimeProfileHistory = memnew( t3druntimeProfileHistory_t );
	}
	m_bRuntimeProfilingEnabled = bEnabled;
}

void T3DTerrain::setDebugLevel( const t3ddebugLevel_t level ) {
	SET_IF_DIFF( s_debugLevel, CLAMP( level, T3DTerrain::ERROR, T3DTerrain::EXTREME ) );
	LOG( T3DTerrain::INFO, "t3dsetting_t debug level: ", s_debugLevel );
}

static String terrain3dDefaultRegionLightmapOutputDirectory( const String &rDataDirectory ) {
	const String directory = rDataDirectory.strip_edges().trim_suffix( "/" );
	return directory.is_empty() ? String() : directory.path_join( "lighting" );
}

void T3DTerrain::setDataDirectory( const String dir ) {
	if ( m_dataDirectory == dir ) {
		if ( m_regionLightmapOutputDirectory.is_empty() && !dir.is_empty() ) {
			m_regionLightmapOutputDirectory = terrain3dDefaultRegionLightmapOutputDirectory( dir );
		}
		return;
	}
	const String old_dir = m_dataDirectory;
	const String old_lightmap_default = terrain3dDefaultRegionLightmapOutputDirectory( old_dir );
	const bool bFollowLightmapDefault = m_regionLightmapOutputDirectory.is_empty() ||
		m_regionLightmapOutputDirectory == old_lightmap_default;
	LOG( T3DTerrain::INFO, "[data-transition] Requested data directory change: ", old_dir, " -> ", dir,
		"; initialized=", m_bInitialized, "; inside_tree=", is_inside_tree(),
		"; inside_world=", m_bIsInsideWorld );

	if ( !m_pData ) {
		m_dataDirectory = dir;
		if ( bFollowLightmapDefault ) {
			m_regionLightmapOutputDirectory = terrain3dDefaultRegionLightmapOutputDirectory( dir );
		}
		initializeInternal();
		update_configuration_warnings();
		return;
	}

	const bool bHasRegionFiles = !dir.is_empty() && !Util::getFiles( dir, "terrain3d*.res" ).is_empty();
	const bool bClearsRepository = !old_dir.is_empty() && dir.is_empty();
	const bool bReplacesRepository = bHasRegionFiles;
	if ( !bClearsRepository && !bReplacesRepository ) {
		// Assigning a new empty directory is a save-target change. Preserve the
		// current in-memory world instead of destroying it.
		m_dataDirectory = dir;
		if ( bFollowLightmapDefault ) {
			m_regionLightmapOutputDirectory = terrain3dDefaultRegionLightmapOutputDirectory( dir );
		}
		LOG( T3DTerrain::INFO, "[data-transition] Updated save target without replacing terrain data" );
		update_configuration_warnings();
		return;
	}

	const bool bWasInitialized = m_bInitialized;
	const bool bWasPhysicsProcessing = is_physics_processing();
	if ( bWasInitialized ) {
		// A directory replacement invalidates generated texture RIDs, region slot
		// mappings, collision shapes, streamed nodes, and instance buffers. None of
		// their live consumers may observe the repository half way through that
		// transition. In particular, the displacement compute path can have a queued
		// render-thread request that contains the old terrain texture RIDs.
		LOG( T3DTerrain::INFO, "[data-transition] Quiescing renderer, physics, streaming, and resource callbacks" );
		set_physics_process( false );
		m_bInitialized = false;
		destroyTerrainMesher();
		destroyOceanMesher();
		destroyHlodStreamer();
		destroyLightmapStreamer();
		destroyNodeStreamer( false );
		destroyInstancer();
		destroyCollision();
		destroyLabels();
		destroyMousePicking();
		destroyDisplacementBuffer();
		disconnectResourceSignals( m_pAssets, m_pMaterial );
		if ( m_pAssets.is_valid() ) {
			m_pAssets->uninitialize( this );
		}
		if ( m_pMaterial.is_valid() ) {
			m_pMaterial->uninitialize( this );
		}

		// T3DGeneratedTexture replacement frees old renderer RIDs. Flush teardown
		// commands first so Direct3D/Vulkan backends cannot execute an old draw or
		// compute dispatch after the data object starts replacing those RIDs.
		RenderingServer *const pServer = RenderingServer::get_singleton();
		if ( pServer && !pServer->is_on_render_thread() ) {
			pServer->force_sync();
		}
		LOG( T3DTerrain::INFO, "[data-transition] Runtime consumers quiesced; repository mutation may begin" );
	}

	Error transition_error = OK;
	if ( bClearsRepository ) {
		LOG( T3DTerrain::INFO, "[data-transition] Clearing active terrain repository" );
		m_pData->clear();
	} else {
		transition_error = m_pData->loadDirectory( dir );
	}

	if ( transition_error == OK ) {
		m_dataDirectory = dir;
		if ( bFollowLightmapDefault ) {
			m_regionLightmapOutputDirectory = terrain3dDefaultRegionLightmapOutputDirectory( dir );
		}
		LOG( T3DTerrain::INFO, "[data-transition] Repository mutation committed for ", dir );
	} else {
		LOG( T3DTerrain::ERROR, "[data-transition] Data directory change rejected; restoring previous terrain. Error: ",
			transition_error );
	}

	if ( bWasInitialized ) {
		LOG( T3DTerrain::INFO, "[data-transition] Recreating editor/runtime consumers from the committed repository" );
		initializeInternal();
		setupMousePicking();
		setupDisplacementBuffer();
		updateDisplacementBuffer();
		set_physics_process( bWasPhysicsProcessing );
		LOG( T3DTerrain::INFO, "[data-transition] Consumer restore complete; initialized=", m_bInitialized,
			"; physics_process=", is_physics_processing() );
	} else if ( transition_error == OK ) {
		// Preserve the old setter behavior for pre-tree/property-deserialization
		// calls: build whatever can be initialized at the current lifecycle stage.
		initializeInternal();
	}

	update_configuration_warnings();
}

void T3DTerrain::setAutoShaderProfile( const Ref<Resource> &rProfile ) {
	if ( m_pAutoShaderProfile == rProfile ) {
		connectAutoShaderProfileSignal( m_pAutoShaderProfile );
		applyAutoShaderProfileMode();
		return;
	}

	disconnectAutoShaderProfileSignal( m_pAutoShaderProfile );
	m_pAutoShaderProfile = rProfile;
	connectAutoShaderProfileSignal( m_pAutoShaderProfile );
	applyAutoShaderProfileMode();
}

void T3DTerrain::setAssets( const Ref<T3DAssets> &rAssets ) {
	if ( !differs( m_pAssets, rAssets ) ) {
		return;
	}
	LOG( T3DTerrain::INFO, "Replacing asset registry" );
	const bool bWasInitialized = m_bInitialized;
	const Ref<T3DAssets> pOldAssets = m_pAssets;
	disconnectResourceSignals( pOldAssets, m_pMaterial );
	if ( bWasInitialized && m_pInstancer ) {
		// Clear render records while the old registry still owns their counts.
		m_pInstancer->destroy();
	}
	if ( pOldAssets.is_valid() ) {
		pOldAssets->uninitialize( this );
	}
	m_pAssets = rAssets;
	if ( m_pAssets.is_null() ) {
		m_pAssets.instantiate();
	}
	connectResourceSignals( m_pAssets, m_pMaterial );
	if ( bWasInitialized ) {
		m_pAssets->initialize( this );
		if ( m_pInstancer ) {
			m_pInstancer->initialize( this );
		}
		if ( m_pMaterial.is_valid() ) {
			m_pMaterial->update( T3DMaterial::FULL_REBUILD );
		}
	} else {
		initializeInternal();
	}
	emit_signal( "assets_changed" );
}

void T3DTerrain::setEditor( T3DEditor *const pEditor ) {
	if ( pEditor && pEditor->is_queued_for_deletion() ) {
		LOG( T3DTerrain::ERROR, "Attempted to set a node queued for deletion" );
		return;
	}
	SET_IF_DIFF( m_pEditor, pEditor );
	LOG( T3DTerrain::INFO, "t3dsetting_t Terrain3DEditor: ", m_pEditor );
	if ( m_pMaterial.is_valid() ) {
		m_pMaterial->update( T3DMaterial::FULL_REBUILD );
	}
}

void T3DTerrain::setPlugin( Object *const pPlugin ) {
	if ( pPlugin && pPlugin->is_queued_for_deletion() ) {
		LOG( T3DTerrain::ERROR, "Attempted to set a node queued for deletion" );
		return;
	}
	SET_IF_DIFF( m_pEditorPlugin, pPlugin );
	LOG( T3DTerrain::INFO, "t3dsetting_t Editor Plugin: ", m_pEditorPlugin );
}

void T3DTerrain::setRegionSize( const t3dregionSize_t size ) {
	if ( size != SIZE_128 ) {
		LOG( T3DTerrain::ERROR, "This Terrain3D fork is specialized for 128-sample regions and rejects all other sizes" );
		return;
	}
	if ( !isValidRegionSize( size ) ) {
		LOG( T3DTerrain::ERROR, "Invalid region size: ", size, ". Must be power of 2, 64-2048" );
		return;
	}
	if ( ( int ) ( size ) % m_nInstancerCellSize != 0 ) {
		LOG( T3DTerrain::ERROR, "Region size ", ( int ) ( size ), " is incompatible with instancer cell size ",
			m_nInstancerCellSize, ". Change instancer_cell_size first." );
		return;
	}
	const t3dregionSize_t previous_size = m_nRegionSize;
	const bool bOwnerChanged = m_nRegionSize != size;
	if ( bOwnerChanged ) {
		refreshPerformanceProfileConformity();
	}
	if ( bOwnerChanged && m_pNodeStreamer ) {
		m_pNodeStreamer->suspend();
	}
	m_nRegionSize = size;
	bool bDataChanged = false;
	const bool bDataOwnerReady = m_pData && m_pData->isInitializedFor( this );
	if ( bDataOwnerReady && !m_pData->matchesRegionLayoutInternal( ( int ) ( m_nRegionSize ) ) ) {
		if ( !m_pData->setRegionSizeInternal( ( int ) ( m_nRegionSize ) ) ) {
			m_nRegionSize = previous_size;
			LOG( T3DTerrain::ERROR, "Region size change rejected because active terrain storage could not be reconfigured" );
			return;
		}
		m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
		m_pData->configureCpuRegionCache( effectiveCpuRegionCacheEnabled(), m_nEffectiveCpuRegionCacheBudgetMb,
			m_nCpuRegionLoadBudgetLayers, m_nCpuRegionLoadBudgetMb );
		m_pData->configureInstanceRegionCache( effectiveInstanceRegionCacheEnabled(),
			m_nEffectiveInstanceRegionCacheBudgetMb, m_nInstanceRegionLoadBudgetPages,
			m_nInstanceRegionLoadBudgetMb );
		m_pData->configureResidentPool( 64 );
		bDataChanged = true;
	} else if ( m_pData && !bDataOwnerReady && bOwnerChanged ) {
		LOG( T3DTerrain::DEBUG, "Deferring Terrain3DData region-size reconciliation until data initialization: ",
			( int ) ( m_nRegionSize ) );
	}
	// Equality of the owner property does not prove dependent state is already
	// synchronized. Return only after Terrain3DData has been reconciled.
	if ( !bOwnerChanged && !bDataChanged ) {
		return;
	}
	LOG( T3DTerrain::INFO, "t3dsetting_t region size: ", m_nRegionSize );
	// Runtime migration updates dependent instance layouts only after the data
	// object has been attached to this terrain. Pre-tree property deserialization
	// is intentionally reconciled by Terrain3DData::initialize().
	if ( m_bInitialized && m_pInstancer && !m_pInstancer->refreshResidentRegionLayouts() ) {
		LOG( T3DTerrain::ERROR, "Region size changed, but resident instance pages could not be repartitioned" );
	}
	if ( m_pMaterial.is_valid() && m_pMaterial->isInitialized() ) {
		m_pMaterial->update();
	}
	if ( m_bInitialized ) {
		updateDisplacementBuffer();
	}
}

void T3DTerrain::setSave16Bit( const bool bEnabled ) {
	SET_IF_DIFF( m_bSave16Bit, bEnabled );
	// Region persistence is now unconditionally compact R16. Retain the property
	// for scene compatibility, but do not dirty every region: bounded CPU residency
	// deliberately permits clean region resources to exist without map payloads.
	LOG( T3DTerrain::INFO, "Save heightmaps as 16-bit: ", m_bSave16Bit );
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

Dictionary T3DTerrain::runtimeProfileSampleToDictionary( const t3druntimeProfileSample_t &rSample ) const {
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
	const t3druntimeProfileSample_t &sample = m_runtimeProfileLastSample;
	if ( sample.gpuMissingRequired > 0 ) {
		terrain3dAppendRuntimeProblem( problems, "error", "gpu_required_pages_missing", "gpu_streaming",
			"Required detailed terrain pages are not resident.", sample.gpuMissingRequired );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "gpu_upload_budget_saturated", "gpu_streaming",
			"GPU publication stopped at the current layer budget.", sample.gpuEffectiveUploadLimit );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "gpu_upload_byte_budget", "gpu_streaming",
			"GPU publication stopped at the configured byte budget.", ( int64_t ) ( sample.nGpuUploadBytes ) );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "gpu_update_call_budget", "gpu_streaming",
			"GPU publication stopped at the texture-update call budget.", sample.nGpuUploads );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "info", "gpu_prepare_pending", "gpu_streaming",
			"A required page is still being prepared by the worker pool.", sample.nGpuPreparePendingPages );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "gpu_no_ready_page", "gpu_streaming",
			"A required page had no prepared cooked payload ready for publication.", sample.gpuPreparedQueue );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "gpu_retirement_capacity", "gpu_streaming",
			"No staging layer is free because physical pages are mapped or retiring.", sample.gpuResident );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "error", "gpu_page_capacity_exhausted", "gpu_streaming",
			"Mapped GPU page capacity is exhausted while required pages are missing.", sample.nGpuCapacity );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "info", "cpu_load_budget", "cpu_streaming",
			"CPU region loading stopped at the page budget.", sample.cpuLoads );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "info", "cpu_load_byte_budget", "cpu_streaming",
			"CPU region loading stopped at the byte budget.", ( int64_t ) ( sample.nCpuLoadBytes ) );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "cpu_pinned_capacity", "cpu_streaming",
			"CPU cache replacement could not find an evictable region slot.", sample.cpuPinned );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "info", "instance_load_budget", "instance_streaming",
			"Instance page loading stopped at the page budget.", sample.instanceLoads );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "info", "instance_load_byte_budget", "instance_streaming",
			"Instance page loading stopped at the byte budget.", ( int64_t ) ( sample.nInstanceLoadBytes ) );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_INSTANCE_UPDATE_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "instance_update_budget", "instancer",
			"Instance cell publication remains queued at the per-frame cell budget.", sample.pendingCellUpdates );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_NODE_LOAD_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "info", "node_load_budget", "node_streaming",
			"Dynamic node loading reached the region budget.", sample.nodeLoads );
	}
	if ( ( sample.stallFlags & TERRAIN3D_PROFILE_STALL_NODE_UNLOAD_BUDGET ) != 0u ) {
		terrain3dAppendRuntimeProblem( problems, "info", "node_unload_budget", "node_streaming",
			"Dynamic node unloading reached the region budget.", sample.nodeUnloads );
	}
	if ( sample.nGpuCookedMisses > 0 ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "cooked_page_miss", "gpu_streaming",
			"Cooked runtime pages were unavailable and required fallback preparation.", sample.nGpuCookedMisses );
	}
	if ( sample.pendingCellUpdates > 0 ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "instance_cell_backlog", "instancer",
			"Instance cell updates remain queued after this tick.", sample.pendingCellUpdates );
	}
	if ( sample.nNodeLoadFailures > 0 ) {
		terrain3dAppendRuntimeProblem( problems, "error", "node_load_failure", "node_streaming",
			"One or more streamed node scenes failed to load.", sample.nNodeLoadFailures );
	}
	if ( sample.nGpuFarDirtyRegions > 0 ) {
		terrain3dAppendRuntimeProblem( problems, "info", "far_atlas_backlog", "far_atlas",
			"Far-atlas region patches remain dirty.", sample.nGpuFarDirtyRegions );
	}
	if ( sample.shaderUpdates > 0 ) {
		terrain3dAppendRuntimeProblem( problems, "info", "shader_update", "shaders",
			"Terrain shader source was submitted or rebound during this sampled tick.",
			( int64_t ) ( sample.nShaderUpdateUsec ) );
	}
	if ( sample.shaderVariantCreations > 0 ) {
		terrain3dAppendRuntimeProblem( problems, "info", "shader_variant_created", "shaders",
			"One or more shader variants were created during this sampled tick.",
			sample.shaderVariantCreations );
	}
	if ( sample.nPhysicsTotalUsec >= 2000u ) {
		terrain3dAppendRuntimeProblem( problems, "warning", "terrain_frame_over_2ms", "frame",
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
	const t3druntimeProfileAccumulator_t &stats = m_runtimeProfileAccumulator;
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

void T3DTerrain::markMemoryBudgetDirty( void ) {
	m_bMemoryBudgetPolicyDirty = true;
}

const char *T3DTerrain::memoryLimitReasonName( const t3dmemoryLimitReason_t reason ) {
	switch ( reason ) {
		case TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET:
			return "total_budget";
		case TERRAIN3D_MEMORY_LIMIT_FIXED_CAP:
			return "fixed_cap";
		case TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE:
			return "manual_override";
		case TERRAIN3D_MEMORY_LIMIT_DISABLED:
			return "disabled";
		case TERRAIN3D_MEMORY_LIMIT_NONE:
		default:
			return "none";
	}
}

uint64_t T3DTerrain::calculateFixedMemoryBytes( void ) const {
	uint64_t bytes = sizeof( *this );
	if ( m_pData ) {
		bytes += ( uint64_t ) ( m_pData->getMemoryFixedRepositoryBytes() );
		bytes += getGpuFarAtlasBytes();
	}
	if ( m_pAssets.is_valid() ) {
		bytes += ( uint64_t ) ( m_pAssets->getGeneratedTexturePayloadBytes() );
	}
	if ( m_pTerrainMesher ) {
		bytes += ( uint64_t ) ( m_pTerrainMesher->getVisibleGeometryPayloadBytes() );
		bytes += ( uint64_t ) ( m_pTerrainMesher->getShadowGeometryPayloadBytes() );
	}
	if ( m_pInstancer ) {
		bytes += ( uint64_t ) ( sizeof( T3DInstancer ) );
		bytes += ( uint64_t ) ( m_pInstancer->getActiveMultimeshBytes() );
		bytes += ( uint64_t ) ( m_pInstancer->getRetainedContainerBytes() );
	}
	if ( m_pCollision ) {
		bytes += ( uint64_t ) ( sizeof( T3DCollision ) );
	}
	if ( m_pNodeStreamer ) {
		bytes += ( uint64_t ) ( sizeof( T3DNodeStreamer ) );
		bytes += ( uint64_t ) ( m_pNodeStreamer->getRetainedCapacityBytes() );
	}
	if ( m_pLightmapStreamer ) {
		bytes += ( uint64_t ) ( sizeof( T3DLightmapStreamer ) );
	}
	if ( m_pRuntimeProfileHistory ) {
		bytes += ( uint64_t ) ( sizeof( t3druntimeProfileHistory_t ) );
	}
	return bytes;
}

uint64_t T3DTerrain::calculateTransientMemoryBytes( void ) const {
	uint64_t bytes = 0u;
	if ( m_pData ) {
		bytes += ( uint64_t ) ( m_pData->getMemoryTransientStagingBytes() );
	}
	if ( m_pTerrainMesher ) {
		bytes += ( uint64_t ) ( m_pTerrainMesher->getStreamingBufferBytes() );
	}
	if ( m_pCollision ) {
		bytes += ( uint64_t ) ( m_pCollision->getRetainedBufferBytes() );
	}
	return bytes;
}

t3dmemoryBudgetInput_t T3DTerrain::buildMemoryBudgetInput( void ) const {
	t3dmemoryBudgetInput_t input;
	input.nTotalBudgetBytes = ( uint64_t ) ( m_nTotalMemoryBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	input.nFixedBytes = calculateFixedMemoryBytes();
	input.nTransientReserveBytes = ( uint64_t ) ( m_nMemoryTransientReserveMb ) * TERRAIN3D_MEMORY_MIB;

	t3dmemorySubsystemInput_t &gpu = input.subsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES];
	gpu.bEnabled = effectiveGpuRegionCacheEnabled();
	gpu.bAutomatic = m_bGpuRegionCacheBudgetAuto;
	gpu.nManualBytes = ( uint64_t ) ( m_nGpuRegionCacheBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	gpu.nMinimumBytes = m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheMinimumBytes() ) : TERRAIN3D_MEMORY_MIB;
	gpu.nHardCapBytes = m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheHardCapBytes() ) : 128u * TERRAIN3D_MEMORY_MIB;
	gpu.weight = 5u;

	t3dmemorySubsystemInput_t &cpu = input.subsystems[TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES];
	cpu.bEnabled = effectiveCpuRegionCacheEnabled();
	cpu.bAutomatic = m_bCpuRegionCacheBudgetAuto;
	cpu.nManualBytes = ( uint64_t ) ( m_nCpuRegionCacheBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	cpu.nMinimumBytes = m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheMinimumBytes() ) : 4u * TERRAIN3D_MEMORY_MIB;
	cpu.nHardCapBytes = m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheHardCapBytes() ) : 128u * TERRAIN3D_MEMORY_MIB;
	cpu.weight = 4u;

	t3dmemorySubsystemInput_t &instances = input.subsystems[TERRAIN3D_MEMORY_INSTANCE_PAGES];
	instances.bEnabled = effectiveInstanceRegionCacheEnabled();
	instances.bAutomatic = m_bInstanceRegionCacheBudgetAuto;
	instances.nManualBytes = ( uint64_t ) ( m_nInstanceRegionCacheBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	instances.nMinimumBytes = TERRAIN3D_MEMORY_MIB;
	instances.nHardCapBytes = 128u * TERRAIN3D_MEMORY_MIB;
	instances.weight = 2u;

	t3dmemorySubsystemInput_t &lighting = input.subsystems[TERRAIN3D_MEMORY_BAKED_LIGHTING];
	lighting.bEnabled = m_bLightmapStreamingEnabled;
	lighting.bAutomatic = m_bLightmapResidencyBudgetAuto;
	lighting.nManualBytes = ( uint64_t ) ( m_nLightmapResidencyBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	lighting.nMinimumBytes = TERRAIN3D_MEMORY_MIB;
	lighting.nHardCapBytes = 1024u * TERRAIN3D_MEMORY_MIB;
	lighting.weight = 2u;

	t3dmemorySubsystemInput_t &hlod = input.subsystems[TERRAIN3D_MEMORY_HLOD_PROXIES];
	hlod.bEnabled = m_bHlodEnabled;
	hlod.bAutomatic = m_bHlodMemoryBudgetAuto;
	hlod.nManualBytes = ( uint64_t ) ( m_nHlodMemoryBudgetMb ) * TERRAIN3D_MEMORY_MIB;
	hlod.nMinimumBytes = 4u * TERRAIN3D_MEMORY_MIB;
	hlod.nHardCapBytes = 4096u * TERRAIN3D_MEMORY_MIB;
	hlod.weight = 2u;

	t3dmemorySubsystemInput_t &shader = input.subsystems[TERRAIN3D_MEMORY_SHADER_CACHE];
	shader.bEnabled = m_pMaterial.is_valid();
	shader.bAutomatic = m_bShaderCacheBudgetAuto;
	shader.nManualBytes = ( uint64_t ) ( m_pMaterial.is_valid() ? m_pMaterial->getShaderVariantSourceBudgetMb() : 8 ) * TERRAIN3D_MEMORY_MIB;
	shader.nMinimumBytes = ( uint64_t ) ( TERRAIN3D_MIN_SHADER_VARIANT_SOURCE_BUDGET_MB ) * TERRAIN3D_MEMORY_MIB;
	shader.nHardCapBytes = ( uint64_t ) ( TERRAIN3D_MAX_SHADER_VARIANT_SOURCE_BUDGET_MB ) * TERRAIN3D_MEMORY_MIB;
	shader.weight = 1u;
	return input;
}

void T3DTerrain::applyMemoryBudgetResult( const t3dmemoryBudgetResult_t &rResult, const bool bForce ) {
	const int previous_effective_mb[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = {
		m_nEffectiveGpuRegionCacheBudgetMb,
		m_nEffectiveCpuRegionCacheBudgetMb,
		m_nEffectiveInstanceRegionCacheBudgetMb,
		m_nEffectiveLightmapResidencyBudgetMb,
		m_nEffectiveHlodMemoryBudgetMb,
		m_nEffectiveShaderCacheBudgetMb,
	};
	const uint64_t fixed_far_bytes = getGpuFarAtlasBytes();
	const uint64_t gpu_bytes = rResult.subsystems[TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES].nEffectiveBytes + fixed_far_bytes;
	const int gpu_mb = MAX( 1, ( int ) ( ( gpu_bytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int cpu_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int instance_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_INSTANCE_PAGES].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int lightmap_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_BAKED_LIGHTING].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int hlod_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_HLOD_PROXIES].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );
	const int shader_mb = MAX( 1, ( int ) ( ( rResult.subsystems[TERRAIN3D_MEMORY_SHADER_CACHE].nEffectiveBytes + TERRAIN3D_MEMORY_MIB - 1u ) / TERRAIN3D_MEMORY_MIB ) );

	const int hysteresis_mb = ( int ) ( TERRAIN3D_MEMORY_POLICY_HYSTERESIS_BYTES / TERRAIN3D_MEMORY_MIB );
	const bool bGpuChanged = bForce || std::abs( gpu_mb - m_nEffectiveGpuRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bCpuChanged = bForce || std::abs( cpu_mb - m_nEffectiveCpuRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bInstanceChanged = bForce || std::abs( instance_mb - m_nEffectiveInstanceRegionCacheBudgetMb ) >= hysteresis_mb;
	const bool bLightmapChanged = bForce || std::abs( lightmap_mb - m_nEffectiveLightmapResidencyBudgetMb ) >= hysteresis_mb;
	const bool bHlodChanged = bForce || std::abs( hlod_mb - m_nEffectiveHlodMemoryBudgetMb ) >= hysteresis_mb;
	const bool bShaderChanged = bForce || std::abs( shader_mb - m_nEffectiveShaderCacheBudgetMb ) >= hysteresis_mb;

	if ( bGpuChanged ) {
		m_nEffectiveGpuRegionCacheBudgetMb = gpu_mb;
		if ( m_pData ) {
			m_pData->configureGpuRegionCache( effectiveGpuRegionCacheEnabled(), m_nEffectiveGpuRegionCacheBudgetMb,
				m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb, m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
		}
	}
	if ( bCpuChanged ) {
		m_nEffectiveCpuRegionCacheBudgetMb = cpu_mb;
		if ( m_pData ) {
			m_pData->configureCpuRegionCache( effectiveCpuRegionCacheEnabled(), m_nEffectiveCpuRegionCacheBudgetMb,
				m_nCpuRegionLoadBudgetLayers, m_nCpuRegionLoadBudgetMb );
		}
	}
	if ( bInstanceChanged ) {
		m_nEffectiveInstanceRegionCacheBudgetMb = instance_mb;
		if ( m_pData ) {
			m_pData->configureInstanceRegionCache( effectiveInstanceRegionCacheEnabled(),
				m_nEffectiveInstanceRegionCacheBudgetMb, m_nInstanceRegionLoadBudgetPages,
				m_nInstanceRegionLoadBudgetMb );
		}
	}
	if ( bLightmapChanged ) {
		m_nEffectiveLightmapResidencyBudgetMb = lightmap_mb;
		if ( m_pLightmapStreamer ) {
			m_pLightmapStreamer->setResidencyBudgetMb( m_nEffectiveLightmapResidencyBudgetMb );
		}
	}
	if ( bHlodChanged ) {
		m_nEffectiveHlodMemoryBudgetMb = hlod_mb;
		if ( m_pHlodStreamer ) { m_pHlodStreamer->setMemoryBudgetMb( m_nEffectiveHlodMemoryBudgetMb ); }
	}
	if ( bShaderChanged ) {
		m_nEffectiveShaderCacheBudgetMb = shader_mb;
		if ( m_pMaterial.is_valid() ) {
			m_pMaterial->setEffectiveShaderVariantSourceBudgetMb( m_nEffectiveShaderCacheBudgetMb );
		}
	}

	const int current_effective_mb[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = {
		m_nEffectiveGpuRegionCacheBudgetMb,
		m_nEffectiveCpuRegionCacheBudgetMb,
		m_nEffectiveInstanceRegionCacheBudgetMb,
		m_nEffectiveLightmapResidencyBudgetMb,
		m_nEffectiveHlodMemoryBudgetMb,
		m_nEffectiveShaderCacheBudgetMb,
	};
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		if ( current_effective_mb[index] == previous_effective_mb[index] ) {
			continue;
		}
		emit_signal( "cache_budget_changed", StringName( TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index] ),
			( int64_t ) ( rResult.subsystems[( size_t ) ( index )].nEffectiveBytes ),
			StringName( memoryLimitReasonName( rResult.subsystems[( size_t ) ( index )].limitingReason ) ) );
	}
}

void T3DTerrain::serviceMemoryBudgetPolicy( const bool bForce ) {
	if ( !bForce ) {
		++m_nMemoryBudgetPolicyFrame;
		if ( ( m_nMemoryBudgetPolicyFrame % ( uint64_t ) ( TERRAIN3D_MEMORY_POLICY_INTERVAL_FRAMES ) ) != 0u ) {
			return;
		}
	}
	const t3dmemoryBudgetInput_t input = buildMemoryBudgetInput();
	const t3dmemoryBudgetResult_t result = T3DMemoryBudgetCoordinator::calculate( input );
	applyMemoryBudgetResult( result, bForce || m_nMemoryBudgetPolicyGeneration == 0u );
	const bool bPressure = result.bHardOvercommit || result.nHeadroomBytes < TERRAIN3D_MEMORY_POLICY_HYSTERESIS_BYTES;
	if ( bPressure != m_bMemoryPressureEventState || result.bHardOvercommit != m_bMemoryOvercommitEventState ) {
		m_bMemoryPressureEventState = bPressure;
		m_bMemoryOvercommitEventState = result.bHardOvercommit;
		emit_signal( "memory_pressure_changed", bPressure, result.bHardOvercommit, ( int64_t ) ( result.nHeadroomBytes ) );
	}
	m_nMemoryBudgetResult = result;
	m_bMemoryBudgetPolicyDirty = false;
	++m_nMemoryBudgetPolicyGeneration;
}

void T3DTerrain::forceMemoryBudgetRebalance( void ) {
	serviceMemoryBudgetPolicy( true );
	notify_property_list_changed();
}

void T3DTerrain::setTotalMemoryBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 32, 4096 );
	if ( m_nTotalMemoryBudgetMb == budget_mb ) {
		return;
	}
	m_nTotalMemoryBudgetMb = budget_mb;
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
}

void T3DTerrain::setMemoryTransientReserveMb( const int nReserveMb ) {
	const int reserve_mb = CLAMP( nReserveMb, 4, 1024 );
	if ( m_nMemoryTransientReserveMb == reserve_mb ) {
		return;
	}
	m_nMemoryTransientReserveMb = reserve_mb;
	markMemoryBudgetDirty();
}

void T3DTerrain::setGpuRegionCacheBudgetAuto( const bool bEnabled ) {
	if ( m_bGpuRegionCacheBudgetAuto == bEnabled ) {
		return;
	}
	m_bGpuRegionCacheBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void T3DTerrain::setCpuRegionCacheBudgetAuto( const bool bEnabled ) {
	if ( m_bCpuRegionCacheBudgetAuto == bEnabled ) {
		return;
	}
	m_bCpuRegionCacheBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void T3DTerrain::setInstanceRegionCacheBudgetAuto( const bool bEnabled ) {
	if ( m_bInstanceRegionCacheBudgetAuto == bEnabled ) {
		return;
	}
	m_bInstanceRegionCacheBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void T3DTerrain::setLightmapResidencyBudgetAuto( const bool bEnabled ) {
	if ( m_bLightmapResidencyBudgetAuto == bEnabled ) {
		return;
	}
	m_bLightmapResidencyBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void T3DTerrain::setLightmapResidencyBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, 1, 1024 );
	if ( m_nLightmapResidencyBudgetMb == budget_mb ) {
		return;
	}
	m_nLightmapResidencyBudgetMb = budget_mb;
	markMemoryBudgetDirty();
	if ( !m_bLightmapResidencyBudgetAuto && m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

void T3DTerrain::setHlodMemoryBudgetAuto( const bool bEnabled ) {
	if ( m_bHlodMemoryBudgetAuto == bEnabled ) { return; }
	m_bHlodMemoryBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) { serviceMemoryBudgetPolicy( true ); }
}

void T3DTerrain::setShaderCacheBudgetAuto( const bool bEnabled ) {
	if ( m_bShaderCacheBudgetAuto == bEnabled ) {
		return;
	}
	m_bShaderCacheBudgetAuto = bEnabled;
	notify_property_list_changed();
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
	if ( m_bInitialized ) {
		serviceMemoryBudgetPolicy( true );
	}
}

Dictionary T3DTerrain::getMemoryBudgetStats( void ) const {
	Dictionary stats;
	stats["total_budget_mb"] = m_nTotalMemoryBudgetMb;
	stats["total_budget_bytes"] = ( int64_t ) ( ( uint64_t ) ( m_nTotalMemoryBudgetMb ) * TERRAIN3D_MEMORY_MIB );
	stats["transient_reserve_mb"] = m_nMemoryTransientReserveMb;
	stats["fixed_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nFixedBytes );
	stats["managed_budget_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nManagedBudgetBytes );
	stats["managed_effective_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nManagedEffectiveBytes );
	stats["manual_reserved_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nManualReservedBytes );
	stats["auto_minimum_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nAutoMinimumBytes );
	stats["headroom_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nHeadroomBytes );
	stats["hard_overcommit"] = m_nMemoryBudgetResult.bHardOvercommit;
	stats["policy_generation"] = ( int64_t ) ( m_nMemoryBudgetPolicyGeneration );
	stats["policy_interval_frames"] = TERRAIN3D_MEMORY_POLICY_INTERVAL_FRAMES;

	Array subsystems;
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		Dictionary subsystem;
		const t3dmemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.subsystems[( size_t ) ( index )];
		subsystem["name"] = TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index];
		subsystem["requested_bytes"] = ( int64_t ) ( allocation.nRequestedBytes );
		subsystem["effective_bytes"] = ( int64_t ) ( allocation.nEffectiveBytes );
		subsystem["limiting_constraint"] = memoryLimitReasonName( allocation.limitingReason );
		subsystem["below_minimum"] = allocation.bBelowMinimum;
		subsystems.push_back( subsystem );
	}
	stats["subsystems"] = subsystems;
	stats["gpu_auto"] = m_bGpuRegionCacheBudgetAuto;
	stats["cpu_auto"] = m_bCpuRegionCacheBudgetAuto;
	stats["instance_auto"] = m_bInstanceRegionCacheBudgetAuto;
	stats["lightmap_auto"] = m_bLightmapResidencyBudgetAuto;
	stats["hlod_auto"] = m_bHlodMemoryBudgetAuto;
	stats["shader_auto"] = m_bShaderCacheBudgetAuto;
	stats["gpu_requested_mb"] = m_nGpuRegionCacheBudgetMb;
	stats["gpu_effective_mb"] = m_nEffectiveGpuRegionCacheBudgetMb;
	stats["cpu_requested_mb"] = m_nCpuRegionCacheBudgetMb;
	stats["cpu_effective_mb"] = m_nEffectiveCpuRegionCacheBudgetMb;
	stats["instance_requested_mb"] = m_nInstanceRegionCacheBudgetMb;
	stats["instance_effective_mb"] = m_nEffectiveInstanceRegionCacheBudgetMb;
	stats["lightmap_requested_mb"] = m_nLightmapResidencyBudgetMb;
	stats["lightmap_effective_mb"] = m_nEffectiveLightmapResidencyBudgetMb;
	stats["hlod_requested_mb"] = m_nHlodMemoryBudgetMb;
	stats["hlod_effective_mb"] = m_nEffectiveHlodMemoryBudgetMb;
	stats["shader_requested_mb"] = m_pMaterial.is_valid() ? m_pMaterial->getShaderVariantSourceBudgetMb() : 0;
	stats["shader_effective_mb"] = m_nEffectiveShaderCacheBudgetMb;
	return stats;
}

Dictionary T3DTerrain::getTrackedCurrentResidency( void ) const {
	Dictionary memory;
	const uint64_t gpu_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t middle_height_bytes = m_pData ? ( uint64_t ) ( m_pData->getMiddleHeightCacheReservedBytes() ) : 0u;
	const uint64_t cpu_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t instance_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u;
	const uint64_t lightmap_bytes = m_pLightmapStreamer ? ( uint64_t ) ( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t hlod_bytes = m_pHlodStreamer ? ( uint64_t ) ( m_pHlodStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t shader_source_bytes = m_pMaterial.is_valid() ? ( uint64_t ) ( m_pMaterial->getShaderVariantSourceBytesNative() ) : 0u;
	const uint64_t managed_bytes = gpu_page_bytes + middle_height_bytes + cpu_page_bytes + instance_page_bytes + lightmap_bytes + hlod_bytes + shader_source_bytes;
	const uint64_t fixed_bytes = calculateFixedMemoryBytes();
	const uint64_t transient_bytes = calculateTransientMemoryBytes();
	const uint64_t tracked_bytes = fixed_bytes + managed_bytes + transient_bytes;
	m_nTrackedMemoryPeakBytes = MAX( m_nTrackedMemoryPeakBytes, tracked_bytes );

	Dictionary fixed;
	fixed["bytes"] = ( int64_t ) ( fixed_bytes );
	fixed["accuracy"] = "estimated";
	fixed["material_texture_arrays_bytes"] = ( int64_t ) ( m_pAssets.is_valid() ? m_pAssets->getGeneratedTexturePayloadBytes() : 0u );
	fixed["far_atlas_bytes"] = ( int64_t ) ( getGpuFarAtlasBytes() );
	fixed["clipmap_geometry_bytes"] = ( int64_t ) ( m_pTerrainMesher ? m_pTerrainMesher->getVisibleGeometryPayloadBytes() : 0u );
	fixed["shadow_geometry_bytes"] = ( int64_t ) ( m_pTerrainMesher ? m_pTerrainMesher->getShadowGeometryPayloadBytes() : 0u );
	fixed["repository_and_authoritative_storage_bytes"] = ( int64_t ) ( m_pData ? m_pData->getMemoryFixedRepositoryBytes() : 0u );
	fixed["node_streamer_retained_bytes"] = ( int64_t ) ( m_pNodeStreamer ? m_pNodeStreamer->getRetainedCapacityBytes() : 0u );
	fixed["instancer_retained_bytes"] = ( int64_t ) ( m_pInstancer ? m_pInstancer->getRetainedContainerBytes() : 0u );
	fixed["active_multimesh_bytes"] = ( int64_t ) ( m_pInstancer ? m_pInstancer->getActiveMultimeshBytes() : 0u );
	fixed["profiler_history_bytes"] = ( int64_t ) ( m_pRuntimeProfileHistory ? sizeof( t3druntimeProfileHistory_t ) : 0u );
	memory["fixed_unavoidable"] = fixed;

	Dictionary managed;
	managed["bytes"] = ( int64_t ) ( managed_bytes );
	managed["accuracy"] = "estimated";
	managed["gpu_terrain_page_pool_bytes"] = ( int64_t ) ( gpu_page_bytes );
	managed["middle_height_page_pool_bytes"] = ( int64_t ) ( middle_height_bytes );
	managed["cpu_terrain_page_cache_bytes"] = ( int64_t ) ( cpu_page_bytes );
	managed["instance_region_page_bytes"] = ( int64_t ) ( instance_page_bytes );
	managed["baked_lighting_estimated_bytes"] = ( int64_t ) ( lightmap_bytes );
	managed["hlod_proxy_estimated_bytes"] = ( int64_t ) ( hlod_bytes );
	managed["shader_source_cache_bytes"] = ( int64_t ) ( shader_source_bytes );
	memory["managed_resident"] = managed;

	Dictionary transient;
	transient["bytes"] = ( int64_t ) ( transient_bytes );
	transient["accuracy"] = "estimated";
	transient["data_page_and_far_staging_bytes"] = ( int64_t ) ( m_pData ? m_pData->getMemoryTransientStagingBytes() : 0u );
	transient["clipmap_streaming_buffer_bytes"] = ( int64_t ) ( m_pTerrainMesher ? m_pTerrainMesher->getStreamingBufferBytes() : 0u );
	transient["collision_retained_scratch_bytes"] = ( int64_t ) ( m_pCollision ? m_pCollision->getRetainedBufferBytes() : 0u );
	memory["transient_staging"] = transient;

	const t3deastlAllocationStats_t allocator = T3DEastlAllocator::getAllocationStats();
	Dictionary allocator_stats;
	allocator_stats["live_bytes"] = ( int64_t ) ( allocator.nLiveBytes );
	allocator_stats["peak_bytes"] = ( int64_t ) ( allocator.nPeakBytes );
	allocator_stats["live_allocations"] = ( int64_t ) ( allocator.nLiveAllocations );
	allocator_stats["total_allocations"] = ( int64_t ) ( allocator.nTotalAllocations );
	allocator_stats["accuracy"] = "exact";
	allocator_stats["source"] = "debug_allocator_telemetry";
	allocator_stats["included_in_total"] = false;
	allocator_stats["note"] = "Cross-cutting EASTL telemetry overlaps subsystem container accounting and is not summed twice.";
	memory["eastl_allocator"] = allocator_stats;
	Dictionary zone_stats;
	zone_stats["state"] = ( int ) ( m_runtimeMemoryZone.state() );
	zone_stats["capacity_bytes"] = ( int64_t ) ( m_runtimeMemoryZone.capacity() );
	zone_stats["live_bytes"] = ( int64_t ) ( m_runtimeMemoryZone.liveBytes() );
	zone_stats["live_allocations"] = ( int64_t ) ( m_runtimeMemoryZone.liveAllocations() );
	zone_stats["free_extent_bytes"] = ( int64_t ) ( m_runtimeMemoryZone.freeBytes() );
	zone_stats["arena_committed"] = m_runtimeMemoryArena.committed();
	zone_stats["arena_sealed"] = m_runtimeMemoryArena.sealed();
	memory["memory_zone"] = zone_stats;

	Dictionary external;
	external["accuracy"] = "unavailable";
	external["godot_object_container_overhead_bytes"] = -1;
	external["renderer_driver_padding_bytes"] = -1;
	external["shader_pipeline_driver_bytes"] = -1;
	external["baked_lighting_cpu_package_overhead_bytes"] = -1;
	external["editor_profiler_tool_overhead_bytes"] = -1;
	external["note"] = "Unavailable values are excluded from tracked totals rather than estimated as zero.";
	memory["external_untracked"] = external;

	memory["tracked_current_residency_bytes"] = ( int64_t ) ( tracked_bytes );
	memory["tracked_peak_residency_bytes"] = ( int64_t ) ( m_nTrackedMemoryPeakBytes );
	memory["budget"] = getMemoryBudgetStats();
	memory["performance_target"] = ( int ) ( m_performanceTarget );
	memory["performance_profile_status"] = ( int ) ( m_performanceProfileStatus );
	memory["performance_profile_version"] = ( int ) ( TERRAIN3D_PROFILE_VERSION );
	memory["performance_profile_signature"] = getPerformanceProfileSignature();
	memory["scope"] = "Terrain3D-owned tracked memory; external Godot/renderer/driver allocations are explicitly unavailable.";
	return memory;
}

Dictionary T3DTerrain::getMemoryEstimate( void ) const {
	return getTrackedCurrentResidency();
}

Dictionary T3DTerrain::getRuntimeMemoryStatsFast( void ) const {
	Dictionary memory;
	const uint64_t gpu_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t middle_height_bytes = m_pData ? ( uint64_t ) ( m_pData->getMiddleHeightCacheReservedBytes() ) : 0u;
	const uint64_t cpu_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheReservedBytes() ) : 0u;
	const uint64_t instance_page_bytes = m_pData ? ( uint64_t ) ( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u;
	const uint64_t lightmap_bytes = m_pLightmapStreamer ? ( uint64_t ) ( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t hlod_bytes = m_pHlodStreamer ? ( uint64_t ) ( m_pHlodStreamer->getResidentEstimatedBytes() ) : 0u;
	const uint64_t shader_source_bytes = m_pMaterial.is_valid() ? ( uint64_t ) ( m_pMaterial->getShaderVariantSourceBytesNative() ) : 0u;
	const uint64_t managed_bytes = gpu_page_bytes + middle_height_bytes + cpu_page_bytes + instance_page_bytes + lightmap_bytes + hlod_bytes + shader_source_bytes;
	const uint64_t fixed_bytes = m_nMemoryBudgetResult.nFixedBytes;
	const uint64_t transient_bytes = calculateTransientMemoryBytes();
	const uint64_t tracked_bytes = fixed_bytes + managed_bytes + transient_bytes;
	m_nTrackedMemoryPeakBytes = MAX( m_nTrackedMemoryPeakBytes, tracked_bytes );

	memory["configured_budget_bytes"] = ( int64_t ) ( ( uint64_t ) ( m_nTotalMemoryBudgetMb ) * TERRAIN3D_MEMORY_MIB );
	memory["current_known_bytes"] = ( int64_t ) ( tracked_bytes );
	memory["peak_known_bytes"] = ( int64_t ) ( m_nTrackedMemoryPeakBytes );
	memory["fixed_bytes"] = ( int64_t ) ( fixed_bytes );
	memory["managed_resident_bytes"] = ( int64_t ) ( managed_bytes );
	memory["middle_height_page_pool_bytes"] = ( int64_t ) ( middle_height_bytes );
	memory["transient_bytes"] = ( int64_t ) ( transient_bytes );
	memory["known_headroom_bytes"] = ( int64_t ) ( m_nMemoryBudgetResult.nHeadroomBytes );
	memory["overcommitted"] = m_nMemoryBudgetResult.bHardOvercommit;
	const bool bPressure = m_nMemoryBudgetResult.bHardOvercommit ||
		m_nMemoryBudgetResult.nHeadroomBytes < TERRAIN3D_MEMORY_POLICY_HYSTERESIS_BYTES;
	memory["pressure_state"] = m_nMemoryBudgetResult.bHardOvercommit ? "overcommit" : ( bPressure ? "pressure" : "normal" );
	memory["external_untracked_accuracy"] = "unavailable";
	memory["external_untracked"] = "Godot object/container overhead, renderer/driver padding, and shader pipeline allocations";
	memory["budget"] = getMemoryBudgetStats();

	const bool bAutomatic[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT] = {
		m_bGpuRegionCacheBudgetAuto, m_bCpuRegionCacheBudgetAuto, m_bInstanceRegionCacheBudgetAuto,
		m_bLightmapResidencyBudgetAuto, m_bHlodMemoryBudgetAuto, m_bShaderCacheBudgetAuto
	};
	Array reallocations;
	Array constrained;
	for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
		const t3dmemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.subsystems[( size_t ) ( index )];
		if ( bAutomatic[index] ) {
			Dictionary entry;
			entry["subsystem"] = TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index];
			entry["effective_bytes"] = ( int64_t ) ( allocation.nEffectiveBytes );
			entry["limiting_constraint"] = memoryLimitReasonName( allocation.limitingReason );
			reallocations.push_back( entry );
		}
		if ( allocation.limitingReason != TERRAIN3D_MEMORY_LIMIT_NONE &&
			allocation.limitingReason != TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE ) {
			constrained.push_back( TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index] );
		}
	}
	memory["automatic_allocations"] = reallocations;
	memory["constrained_subsystems"] = constrained;
	return memory;
}

Array T3DTerrain::getShaderTierStatsDictionaries( void ) const {
	Array tiers;
	if ( m_pMaterial.is_null() ) {
		return tiers;
	}
	const Dictionary shader_global = m_pMaterial->getShaderVariantDiagnostics();
	const real_t fNearEnd = getRuntimeNearRangeEndM();
	const real_t fMiddleEnd = getRuntimeMiddleRangeEndM();
	for ( int tier = T3DMaterial::SHADER_TIER_NEAR; tier < T3DMaterial::SHADER_TIER_COUNT; ++tier ) {
		Dictionary row = m_pMaterial->getShaderTierDiagnostics( tier );
		const real_t fRangeStart = tier == T3DMaterial::SHADER_TIER_NEAR ? 0.0f :
			( tier == T3DMaterial::SHADER_TIER_MIDDLE ? fNearEnd : fMiddleEnd );
		const real_t fRangeEnd = tier == T3DMaterial::SHADER_TIER_NEAR ? fNearEnd :
			( tier == T3DMaterial::SHADER_TIER_MIDDLE ? fMiddleEnd : -1.0f );
		row["diagnostics_version"] = TERRAIN3D_DIAGNOSTICS_VERSION;
		row["tier_name"] = tier == T3DMaterial::SHADER_TIER_NEAR ? "Near" :
			( tier == T3DMaterial::SHADER_TIER_MIDDLE ? "Middle" : "Far" );
		row["range_start_m"] = fRangeStart;
		row["range_end_m"] = fRangeEnd;
		row["range_unbounded"] = tier == T3DMaterial::SHADER_TIER_FAR;
		row["shader_rid"] = row.get( "active_shader_rid", ( int64_t ) ( 0 ) );
		row["profile_target"] = ( int ) ( m_performanceTarget );
		row["profile_name"] = m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ? "Xbox 360 Experimental" : "Custom";
		row["profile_status"] = ( int ) ( m_performanceProfileStatus );
		row["profile_status_name"] = getPerformanceProfileStatusName();
		const int filtering = ( int ) ( row.get( "texture_filtering", 0 ) );
		row["texture_filtering_name"] = filtering == ( int ) ( T3DMaterial::LINEAR_ANISOTROPIC ) ? "Linear Anisotropic" :
			( filtering == ( int ) ( T3DMaterial::LINEAR ) ? "Linear" :
				( filtering == ( int ) ( T3DMaterial::NEAREST_ANISOTROPIC ) ? "Nearest Anisotropic" : "Nearest" ) );
		const int lighting = ( int ) ( row.get( "lighting_model", 0 ) );
		row["lighting_model_name"] = lighting == ( int ) ( T3DMaterial::TIER_LIGHTING_UNSHADED ) ? "Unshaded" :
			( lighting == ( int ) ( T3DMaterial::TIER_LIGHTING_LAMBERT ) ? "Lambert" :
				( lighting == ( int ) ( T3DMaterial::TIER_LIGHTING_BLINN_PHONG ) ? "Blinn-Phong" : "PBR/GGX" ) );
		const bool bShadowEnabled = tier == T3DMaterial::SHADER_TIER_NEAR ? m_bNearShadowEnabled :
			( tier == T3DMaterial::SHADER_TIER_MIDDLE ? m_bMiddleShadowEnabled : m_bFarShadowEnabled );
		row["shadow_participates"] = bShadowEnabled && m_nShadowLodCount > 0;
		row["render_batch_count"] = m_pTerrainMesher ? m_pTerrainMesher->getRenderBatchCountForTier( tier ) : 0;
		row["shadow_batch_count"] = m_pTerrainMesher ? m_pTerrainMesher->getShadowBatchCountForTier( tier ) : 0;
		row["shader_source_bytes"] = row.get( "variant_source_length", ( int64_t ) ( 0 ) );
		row["variant_creations_total"] = shader_global.get( "creations_total", ( int64_t ) ( 0 ) );
		row["shader_rebuild_count"] = shader_global.get( "shader_update_count", ( int64_t ) ( 0 ) );
		row["source_submission_usec_last"] = shader_global.get( "source_submission_usec_last", ( int64_t ) ( 0 ) );
		row["source_submission_usec_peak"] = shader_global.get( "source_submission_usec_peak", ( int64_t ) ( 0 ) );
		row["shader_update_usec_last"] = shader_global.get( "shader_update_usec_last", ( int64_t ) ( 0 ) );
		Dictionary features;
		features["compact_tier_active"] = row.get( "compact_tier_active", false );
		features["textured_surface"] = ( int ) ( row.get( "texture_surface_mode", 0 ) ) != ( int ) ( T3DMaterial::TEXTURE_SURFACE_SILHOUETTE );
		features["authored_color"] = ( double ) ( row.get( "authored_color_contribution", 0.0 ) ) > 0.0;
		features["wetness"] = ( double ) ( row.get( "wetness_contribution", 0.0 ) ) > 0.0;
		features["detiling"] = ( int ) ( row.get( "detiling_quality", 0 ) ) != ( int ) ( T3DMaterial::TIER_DETILING_DISABLED );
		features["derived_normals"] = ( int ) ( row.get( "terrain_normal_quality", 0 ) ) != ( int ) ( T3DMaterial::TERRAIN_NORMAL_QUALITY_GEOMETRY );
		row["feature_flags"] = features;
		int modeled_fetches = tier == T3DMaterial::SHADER_TIER_FAR ? 2 : 3;
		if ( ( int ) ( row.get( "material_sampling_quality", 0 ) ) != ( int ) ( T3DMaterial::MATERIAL_SAMPLING_FAST ) ) {
			++modeled_fetches;
		}
		if ( ( bool ) ( features["derived_normals"] ) ) {
			++modeled_fetches;
		}
		if ( ( int ) ( row.get( "surface_response_quality", 0 ) ) != ( int ) ( T3DMaterial::SURFACE_RESPONSE_SIMPLE ) ) {
			++modeled_fetches;
		}
		if ( ( bool ) ( features["authored_color"] ) ) {
			++modeled_fetches;
		}
		if ( !( bool ) ( features["textured_surface"] ) ) {
			modeled_fetches = MAX( 1, modeled_fetches - 1 );
		}
		row["modeled_major_texture_fetches"] = modeled_fetches;
		row["modeled_texture_fetch_basis"] = "Modeled major texture-resource samples; excludes filtering taps, cache behavior, and driver work.";
		tiers.push_back( row );
	}
	return tiers;
}

Array T3DTerrain::getCacheInventory( const bool bDeep ) const {
	Array caches;
	const Dictionary gpu = m_pData ? ( bDeep ? m_pData->getGpuRegionCacheStats() : m_pData->getGpuRegionCacheStatsFast() ) : Dictionary();
	const Dictionary cpu = m_pData ? ( bDeep ? m_pData->getCpuRegionCacheStats() : m_pData->getCpuRegionCacheStatsFast() ) : Dictionary();
	const Dictionary instance = m_pData ? ( bDeep ? m_pData->getInstanceRegionCacheStats() : m_pData->getInstanceRegionCacheStatsFast() ) : Dictionary();
	const Dictionary lightmap = m_pLightmapStreamer ? ( bDeep ? m_pLightmapStreamer->getStats() : m_pLightmapStreamer->getStatsFast() ) : Dictionary();
	const Dictionary shader = m_pMaterial.is_valid() ? m_pMaterial->getShaderVariantDiagnostics() : Dictionary();
	const uint64_t current_bytes[6] = {
		m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheReservedBytes() ) : 0u,
		m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheReservedBytes() ) : 0u,
		m_pData ? ( uint64_t ) ( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u,
		m_pLightmapStreamer ? ( uint64_t ) ( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u,
		m_pMaterial.is_valid() ? ( uint64_t ) ( m_pMaterial->getShaderVariantSourceBytesNative() ) : 0u,
		getGpuFarAtlasBytes(),
	};
	for ( int index = 0; index < 6; ++index ) {
		m_szDiagnosticCachePeakBytes[( size_t ) ( index )] = MAX( m_szDiagnosticCachePeakBytes[( size_t ) ( index )], current_bytes[index] );
	}
	const uint64_t hard_caps[5] = {
		m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheHardCapBytes() ) : 128u * TERRAIN3D_MEMORY_MIB,
		m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheHardCapBytes() ) : 128u * TERRAIN3D_MEMORY_MIB,
		128u * TERRAIN3D_MEMORY_MIB,
		1024u * TERRAIN3D_MEMORY_MIB,
		( uint64_t ) ( TERRAIN3D_MAX_SHADER_VARIANT_SOURCE_BUDGET_MB ) * TERRAIN3D_MEMORY_MIB,
	};
	const Dictionary raw[5] = { gpu, cpu, instance, lightmap, shader };
	const int64_t resident[5] = {
		( int64_t ) ( gpu.get( "resident", 0 ) ), ( int64_t ) ( cpu.get( "resident", 0 ) ), ( int64_t ) ( instance.get( "resident", 0 ) ),
		( int64_t ) ( lightmap.get( "active_packages", 0 ) ), ( int64_t ) ( shader.get( "variants_total", 0 ) )
	};
	const int64_t capacity[5] = {
		( int64_t ) ( gpu.get( "capacity", 0 ) ), ( int64_t ) ( cpu.get( "capacity", 0 ) ), ( int64_t ) ( instance.get( "capacity", 0 ) ),
		( int64_t ) ( lightmap.get( "max_active_packages", 0 ) ), ( int64_t ) ( m_pMaterial.is_valid() ? m_pMaterial->getShaderVariantCacheLimit() : 0 )
	};
	const int64_t evictions[5] = {
		( int64_t ) ( m_runtimeProfileAccumulator.nGpuEvictionsTotal ), ( int64_t ) ( m_runtimeProfileAccumulator.nCpuEvictionsTotal ),
		( int64_t ) ( m_runtimeProfileAccumulator.nInstanceEvictionsTotal ), ( int64_t ) ( lightmap.get( "unloads_total", ( int64_t ) ( 0 ) ) ),
		( int64_t ) ( shader.get( "removals_total", ( int64_t ) ( 0 ) ) )
	};
	const int64_t hits[5] = {
		( int64_t ) ( m_runtimeProfileAccumulator.nGpuCookedHitsTotal ), -1, -1, -1,
		( int64_t ) ( shader.get( "cache_hits_total", ( int64_t ) ( 0 ) ) )
	};
	const int64_t misses[5] = {
		( int64_t ) ( m_runtimeProfileAccumulator.nGpuCookedMissesTotal ), -1, -1, -1,
		( int64_t ) ( shader.get( "cache_misses_total", ( int64_t ) ( 0 ) ) )
	};
	for ( int index = 0; index < 5; ++index ) {
		const t3dmemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.subsystems[( size_t ) ( index )];
		Dictionary row;
		row["diagnostics_version"] = TERRAIN3D_DIAGNOSTICS_VERSION;
		row["cache_name"] = TERRAIN3D_MEMORY_SUBSYSTEM_NAMES[index];
		row["resident"] = resident[index];
		row["effective_capacity"] = capacity[index];
		row["requested_budget_bytes"] = ( int64_t ) ( allocation.nRequestedBytes );
		row["effective_budget_bytes"] = ( int64_t ) ( allocation.nEffectiveBytes );
		row["hard_maximum_bytes"] = ( int64_t ) ( hard_caps[index] );
		row["limiting_constraint"] = memoryLimitReasonName( allocation.limitingReason );
		row["current_bytes"] = ( int64_t ) ( current_bytes[index] );
		row["peak_bytes"] = ( int64_t ) ( m_szDiagnosticCachePeakBytes[( size_t ) ( index )] );
		row["hits"] = hits[index];
		row["misses"] = misses[index];
		row["hit_rate"] = hits[index] >= 0 && misses[index] >= 0 && hits[index] + misses[index] > 0 ?
			( double ) ( hits[index] ) / ( double ) ( hits[index] + misses[index] ) : -1.0;
		row["evictions"] = evictions[index];
		row["queued"] = index == 0 ? ( int64_t ) ( gpu.get( "requested_pages", 0 ) ) :
			( index == 1 ? ( int64_t ) ( cpu.get( "selection_pending", false ) ? 1 : 0 ) :
			( index == 2 ? ( int64_t ) ( instance.get( "selection_pending", false ) ? 1 : 0 ) :
			( index == 3 ? ( int64_t ) ( lightmap.get( "pending_packages", 0 ) ) : 0 ) ) );
		row["preparing"] = index == 0 ? ( int64_t ) ( gpu.get( "prepare_job_pending_pages", 0 ) ) : 0;
		row["uploading"] = index == 0 ? ( int64_t ) ( gpu.get( "uploads_last_tick", 0 ) ) :
			( index == 1 ? ( int64_t ) ( cpu.get( "loads_last_tick", 0 ) ) :
			( index == 2 ? ( int64_t ) ( instance.get( "loads_last_tick", 0 ) ) :
			( index == 3 ? ( int64_t ) ( lightmap.get( "loads_last_tick", 0 ) ) : 0 ) ) );
		row["load_upload_bytes_last"] = index == 0 ? ( int64_t ) ( gpu.get( "upload_bytes_last_tick", ( int64_t ) ( 0 ) ) ) :
			( index == 1 ? ( int64_t ) ( cpu.get( "load_bytes_last_tick", ( int64_t ) ( 0 ) ) ) :
			( index == 2 ? ( int64_t ) ( instance.get( "load_bytes_last_tick", ( int64_t ) ( 0 ) ) ) : 0 ) );
		row["raw"] = raw[index];
		caches.push_back( row );
	}
	Dictionary far;
	far["diagnostics_version"] = TERRAIN3D_DIAGNOSTICS_VERSION;
	far["cache_name"] = "far_atlas";
	far["resident"] = m_pData ? 2 : 0;
	far["effective_capacity"] = 2;
	far["requested_budget_bytes"] = ( int64_t ) ( current_bytes[5] );
	far["effective_budget_bytes"] = ( int64_t ) ( current_bytes[5] );
	far["hard_maximum_bytes"] = ( int64_t ) ( current_bytes[5] );
	far["limiting_constraint"] = "fixed_unavoidable";
	far["current_bytes"] = ( int64_t ) ( current_bytes[5] );
	far["peak_bytes"] = ( int64_t ) ( m_szDiagnosticCachePeakBytes[5] );
	far["hits"] = -1;
	far["misses"] = -1;
	far["hit_rate"] = -1.0;
	far["evictions"] = 0;
	far["queued"] = ( int64_t ) ( gpu.get( "far_height_dirty_regions", 0 ) ) + ( int64_t ) ( gpu.get( "far_color_dirty_regions", 0 ) );
	far["preparing"] = 0;
	far["uploading"] = ( int64_t ) ( gpu.get( "far_height_updates_last_tick", 0 ) ) + ( int64_t ) ( gpu.get( "far_color_updates_last_tick", 0 ) );
	far["load_upload_bytes_last"] = ( int64_t ) ( gpu.get( "far_height_uploaded_bytes_last", ( int64_t ) ( 0 ) ) ) +
		( int64_t ) ( gpu.get( "far_color_uploaded_bytes_last", ( int64_t ) ( 0 ) ) );
	far["raw"] = gpu;
	caches.push_back( far );
	return caches;
}

Dictionary T3DTerrain::getCpuProfileStats( void ) const {
	Dictionary cpu;
	cpu["gpu_page_cache_service_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nGpuCacheServiceUsec );
	cpu["gpu_page_preparation_worker_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nGpuPrepareWorkerUsec );
	cpu["rendering_server_publication_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nGpuPreparePublicationUsec );
	cpu["cpu_cache_service_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nCpuCacheServiceUsec );
	cpu["instance_cache_service_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nInstanceCacheServiceUsec );
	cpu["instance_streaming_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nInstancerUsec );
	cpu["node_streaming_migration_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nNodeStreamingUsec );
	cpu["baked_lighting_streaming_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nLightmapStreamingUsec );
	cpu["collision_rebuild_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nCollisionUsec );
	const Dictionary gpu = m_pData ? m_pData->getGpuRegionCacheStatsFast() : Dictionary();
	cpu["far_height_rebuild_cpu_usec"] = gpu.get( "far_height_processing_usec_last", ( int64_t ) ( 0 ) );
	cpu["far_color_rebuild_cpu_usec"] = gpu.get( "far_color_processing_usec_last", ( int64_t ) ( 0 ) );
	cpu["far_height_publication_cpu_usec"] = gpu.get( "far_height_publication_usec_last", ( int64_t ) ( 0 ) );
	cpu["far_color_publication_cpu_usec"] = gpu.get( "far_color_publication_usec_last", ( int64_t ) ( 0 ) );
	cpu["far_atlas_publication_cpu_usec"] = ( int64_t ) ( gpu.get( "far_height_publication_usec_last", ( int64_t ) ( 0 ) ) ) +
		( int64_t ) ( gpu.get( "far_color_publication_usec_last", ( int64_t ) ( 0 ) ) );
	cpu["far_atlas_rebuild_publication_cpu_usec"] = ( int64_t ) ( gpu.get( "far_height_processing_usec_last", ( int64_t ) ( 0 ) ) ) +
		( int64_t ) ( gpu.get( "far_color_processing_usec_last", ( int64_t ) ( 0 ) ) ) +
		( int64_t ) ( gpu.get( "far_height_publication_usec_last", ( int64_t ) ( 0 ) ) ) +
		( int64_t ) ( gpu.get( "far_color_publication_usec_last", ( int64_t ) ( 0 ) ) );
	cpu["profiler_collection_cpu_usec_last"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecLast );
	cpu["profiler_collection_cpu_usec_average"] = ( int64_t ) ( m_nDiagnosticsCollectionCount > 0u ?
		m_nDiagnosticsCollectionUsecTotal / m_nDiagnosticsCollectionCount : 0u );
	cpu["profiler_collection_cpu_usec_peak"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecPeak );
	cpu["profiler_collection_count"] = ( int64_t ) ( m_nDiagnosticsCollectionCount );
	return cpu;
}

Dictionary T3DTerrain::getGpuInformationStats( void ) const {
	Dictionary info;
	Dictionary measured;
	measured["classification"] = "Measured";
	measured["available"] = false;
	measured["terrain_gpu_time_usec"] = -1;
	measured["note"] = "No isolated Terrain3D GPU timestamp scope is exposed by the current compatibility renderer path.";
	info["measured"] = measured;
	Dictionary modeled;
	modeled["classification"] = "Modeled";
	modeled["render_batches"] = m_pTerrainMesher ? m_pTerrainMesher->getRenderBatchCount() : 0;
	modeled["shadow_batches"] = m_pTerrainMesher ? m_pTerrainMesher->getShadowBatchCount() : 0;
	modeled["tier_shaders"] = getShaderTierStatsDictionaries();
	modeled["upload_bytes_last_tick"] = ( int64_t ) ( m_runtimeProfileLastSample.nGpuUploadBytes );
	modeled["note"] = "Terrain geometry, batches, feature/sample model, and upload traffic; these are not GPU milliseconds.";
	info["modeled"] = modeled;
	Dictionary scene_wide;
	scene_wide["classification"] = "Scene-wide";
	scene_wide["note"] = "Populate from Godot Performance monitors in the editor profiler; values cover the whole scene, not Terrain3D alone.";
	info["scene_wide"] = scene_wide;
	return info;
}

Dictionary T3DTerrain::getRuntimeParitySnapshot( void ) const {
	Dictionary snapshot;
	snapshot["native_api_revision"] = NATIVE_API_REVISION;
	snapshot["native_build_id"] = String( NATIVE_BUILD_ID );
	snapshot["execution_mode"] = ( int ) ( m_editorExecutionMode );
	snapshot["shipping_policy_active"] = !isEditorAuthoringModeInternal();
	snapshot["runtime_shader_tiering_active"] = isRuntimeShaderTieringActive();
	snapshot["gpu_region_cache_enabled"] = effectiveGpuRegionCacheEnabled();
	snapshot["gpu_region_cache_capacity"] = m_pData ? m_pData->getGpuRegionCacheCapacity() : 0;
	const Dictionary middle = m_pData ? m_pData->getMiddleHeightCacheStats() : Dictionary();
	snapshot["middle_cache_enabled"] = ( bool ) ( middle.get( "enabled", false ) );
	snapshot["middle_cache_capacity"] = ( int ) ( middle.get( "physical_layers", 0 ) );
	snapshot["node_streaming_enabled"] = effectiveDynamicNodeStreamingEnabled();
	snapshot["hlod_enabled"] = m_bHlodEnabled;
	snapshot["lightmap_streaming_enabled"] = m_bLightmapStreamingEnabled;
	snapshot["near_tier_lod_count"] = m_nRuntimeNearShaderLods;
	snapshot["middle_tier_lod_count"] = m_nRuntimeMiddleShaderLods;
	snapshot["far_tier_lod_count"] = MAX( 0, m_nMeshLods - m_nRuntimeNearShaderLods - m_nRuntimeMiddleShaderLods );
	snapshot["shadow_lod_count"] = m_nShadowLodCount;
	snapshot["near_shadow_lod_count"] = m_nNearShadowLodCount;
	snapshot["middle_shadow_lod_count"] = m_nMiddleShadowLodCount;
	snapshot["far_shadow_lod_count"] = m_nFarShadowLodCount;
	snapshot["near_shadow_enabled"] = m_bNearShadowEnabled;
	snapshot["middle_shadow_enabled"] = m_bMiddleShadowEnabled;
	snapshot["far_shadow_enabled"] = m_bFarShadowEnabled;

	const Dictionary materials = m_pAssets.is_valid() ? m_pAssets->getRuntimeMaterialCacheStats() : Dictionary();
	snapshot["runtime_material_cache_active"] = ( bool ) ( materials.get( "active", false ) );
	snapshot["runtime_material_physical_layers"] = ( int ) ( materials.get( "physical_layers", 0 ) );
	snapshot["runtime_material_logical_to_physical_hash"] = materials.get( "logical_to_physical_hash", String() );
	snapshot["runtime_material_source_textures_released"] =
		( bool ) ( materials.get( "source_textures_released", false ) );
	snapshot["first_visible_material_required"] = ( int ) ( materials.get( "first_visible_required", 0 ) );
	snapshot["first_visible_material_resident"] = ( int ) ( materials.get( "first_visible_resident", 0 ) );
	snapshot["first_visible_material_pending"] = ( int ) ( materials.get( "first_visible_pending", 0 ) );
	snapshot["first_visible_material_ready"] = ( bool ) ( materials.get( "first_visible_ready", true ) );
	return snapshot;
}

Ref<T3DMemoryStats> T3DTerrain::getMemoryStatsSnapshot( void ) const {
	Ref<T3DMemoryStats> pStats;
	pStats.instantiate();
	pStats->setValues( getRuntimeMemoryStatsFast() );
	return pStats;
}

TypedArray<T3DShaderTierStats> T3DTerrain::getShaderTierStatsSnapshot( void ) const {
	TypedArray<T3DShaderTierStats> output;
	const Array values = getShaderTierStatsDictionaries();
	for ( int index = 0; index < values.size(); ++index ) {
		Ref<T3DShaderTierStats> pStats;
		pStats.instantiate();
		pStats->setValues( values[index] );
		output.push_back( pStats );
	}
	return output;
}

TypedArray<T3DCacheStats> T3DTerrain::getCacheStatsSnapshot( const bool bDeep ) const {
	TypedArray<T3DCacheStats> output;
	const Array values = getCacheInventory( bDeep );
	for ( int index = 0; index < values.size(); ++index ) {
		Ref<T3DCacheStats> pStats;
		pStats.instantiate();
		pStats->setValues( values[index] );
		output.push_back( pStats );
	}
	return output;
}

Ref<T3DFrameStats> T3DTerrain::getFrameStatsSnapshot( void ) const {
	Ref<T3DFrameStats> pStats;
	pStats.instantiate();
	Dictionary values = runtimeProfileSampleToDictionary( m_runtimeProfileLastSample );
	values["diagnostics_version"] = TERRAIN3D_DIAGNOSTICS_VERSION;
	values["profiler_collection_usec"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecLast );
	pStats->setValues( values );
	return pStats;
}

Dictionary T3DTerrain::getRuntimeDiagnostics( const bool bDeep ) const {
	const uint64_t collection_started_usec = Time::get_singleton()->get_ticks_usec();
	Dictionary diagnostics;
	diagnostics["diagnostics_version"] = TERRAIN3D_DIAGNOSTICS_VERSION;
	diagnostics["deep_introspection"] = bDeep;
	diagnostics["profiling_enabled"] = m_bRuntimeProfilingEnabled;
	diagnostics["sample_count"] = ( int64_t ) ( m_nRuntimeProfileSampleCount );
	diagnostics["execution_mode"] = ( int ) ( m_editorExecutionMode );
	diagnostics["performance_target"] = ( int ) ( m_performanceTarget );
	diagnostics["performance_profile_status"] = ( int ) ( m_performanceProfileStatus );
	diagnostics["performance_profile_status_name"] = getPerformanceProfileStatusName();
	diagnostics["performance_profile_version"] = ( int ) ( TERRAIN3D_PROFILE_VERSION );
	diagnostics["performance_profile_signature"] = getPerformanceProfileSignature();
	diagnostics["performance_profile_diff_count"] = getPerformanceProfileDiff().size();
	diagnostics["adaptive_budgets"] = getRuntimeAdaptiveBudgetStats();
	diagnostics["boundary_phasing"] = getRuntimeBoundaryPhasingStats();
	diagnostics["region_streaming"] = getRegionStreamingStats();
	diagnostics["region_streaming_configuration"] = getRegionStreamingConfigurationStatus();
	diagnostics["memory"] = bDeep ? getMemoryEstimate() : getRuntimeMemoryStatsFast();

	Dictionary frame;
	frame["sequence"] = ( int64_t ) ( m_runtimeProfileLastSample.frameSequence );
	frame["physics_total_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nPhysicsTotalUsec );
	frame["data_residency_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nDataResidencyUsec );
	frame["node_streaming_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nNodeStreamingUsec );
	frame["lightmap_streaming_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nLightmapStreamingUsec );
	frame["clipmap_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nClipmapUsec );
	frame["ocean_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nOceanUsec );
	frame["instancer_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nInstancerUsec );
	frame["light_sync_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nLightSyncUsec );
	frame["collision_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nCollisionUsec );
	frame["streaming_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nStreamingUsec );
	frame["gpu_cache_service_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nGpuCacheServiceUsec );
	frame["cpu_cache_service_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nCpuCacheServiceUsec );
	frame["instance_cache_service_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nInstanceCacheServiceUsec );
	frame["target_position"] = Vector2( m_runtimeProfileLastSample.fTargetX, m_runtimeProfileLastSample.fTargetZ );
	frame["movement"] = Vector2( m_runtimeProfileLastSample.fMovementX, m_runtimeProfileLastSample.fMovementZ );
	frame["target_region"] = Vector2i( m_runtimeProfileLastSample.targetRegionX, m_runtimeProfileLastSample.targetRegionZ );
	frame["shader_updates"] = m_runtimeProfileLastSample.shaderUpdates;
	frame["shader_variant_creations"] = m_runtimeProfileLastSample.shaderVariantCreations;
	frame["shader_source_submission_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nShaderSourceSubmissionUsec );
	frame["shader_update_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.nShaderUpdateUsec );
	frame["events"] = getRuntimeProfileEventNames( m_runtimeProfileLastSample.eventFlags );
	frame["stall_reasons"] = getRuntimeProfileStallNames( m_runtimeProfileLastSample.stallFlags );
	diagnostics["frame"] = frame;
	diagnostics["problems"] = getRuntimeProfileProblems();
	diagnostics["history_capacity"] = ( int64_t ) ( TERRAIN3D_RUNTIME_PROFILE_HISTORY_CAPACITY );
	diagnostics["history_count"] = ( int64_t ) ( m_pRuntimeProfileHistory ? m_pRuntimeProfileHistory->nCount : 0u );

	Dictionary clipmap;
	clipmap["near_updates"] = m_runtimeProfileLastSample.clipmapNearUpdates;
	clipmap["middle_updates"] = m_runtimeProfileLastSample.clipmapMiddleUpdates;
	clipmap["far_updates"] = m_runtimeProfileLastSample.clipmapFarUpdates;
	clipmap["teleports"] = m_runtimeProfileLastSample.clipmapTeleports;
	clipmap["buffer_uploads"] = m_runtimeProfileLastSample.nClipmapBufferUploads;
	clipmap["mesh_lods"] = m_nMeshLods;
	clipmap["tessellation_level"] = m_tessellationLevel;
	clipmap["total_levels"] = getTotalClipmapLevels();
	clipmap["mesh_size"] = m_nMeshSize;
	clipmap["vertex_spacing"] = m_fVertexSpacing;
	clipmap["near_lods"] = m_nRuntimeNearShaderLods;
	clipmap["middle_lods"] = m_nRuntimeMiddleShaderLods;
	clipmap["near_range_end_m"] = getRuntimeNearRangeEndM();
	clipmap["middle_range_end_m"] = getRuntimeMiddleRangeEndM();
	clipmap["update_budget"] = m_nRuntimeClipmapUpdateBudget;
	clipmap["render_batches"] = m_pTerrainMesher ? m_pTerrainMesher->getRenderBatchCount() : 0;
	clipmap["shadow_batches"] = m_pTerrainMesher ? m_pTerrainMesher->getShadowBatchCount() : 0;
	clipmap["base_meshes"] = m_pTerrainMesher ? m_pTerrainMesher->getBaseMeshCount() : 0;
	diagnostics["clipmap"] = clipmap;

	Dictionary nodes = m_pNodeStreamer && bDeep ? m_pNodeStreamer->getStats() : Dictionary();
	nodes["active_regions"] = m_runtimeProfileLastSample.nNodeActiveRegions;
	nodes["active_nodes"] = m_runtimeProfileLastSample.nodeActiveNodes;
	nodes["loads_last_tick"] = m_runtimeProfileLastSample.nodeLoads;
	nodes["unloads_last_tick"] = m_runtimeProfileLastSample.nodeUnloads;
	nodes["load_failures_last_tick"] = m_runtimeProfileLastSample.nNodeLoadFailures;
	nodes["enabled"] = m_bDynamicNodeStreamingEnabled;
	nodes["effective"] = effectiveDynamicNodeStreamingEnabled();
	nodes["stream_radius"] = m_nDynamicNodeStreamRadius;
	nodes["load_region_budget"] = m_nDynamicNodeLoadRegionBudget;
	nodes["load_node_budget"] = m_nDynamicNodeLoadNodeBudget;
	nodes["unload_region_budget"] = m_nDynamicNodeUnloadRegionBudget;
	diagnostics["nodes"] = nodes;

	Dictionary baked_lighting = m_pLightmapStreamer ? ( bDeep ? m_pLightmapStreamer->getStats() : m_pLightmapStreamer->getStatsFast() ) : Dictionary();
	baked_lighting["enabled"] = m_bLightmapStreamingEnabled;
	baked_lighting["stream_radius"] = m_nLightmapStreamRadius;
	baked_lighting["load_budget"] = m_nLightmapLoadBudget;
	baked_lighting["unload_budget"] = m_nLightmapUnloadBudget;
	baked_lighting["semantic_shadow_policy_enabled"] = m_bSemanticShadowPolicyEnabled;
	baked_lighting["shadow_quality"] = ( int ) ( m_environmentShadowQuality );
	baked_lighting["shadow_external_mask"] = ( int64_t ) ( m_nEnvironmentShadowExternalMask );
	baked_lighting["semantic_directional_reception_enabled"] = m_bSemanticDirectionalReceptionEnabled;
	baked_lighting["directional_external_mask"] = ( int64_t ) ( m_nEnvironmentDirectionalExternalMask );
	diagnostics["baked_lighting"] = baked_lighting;

	Dictionary instancer = m_pInstancer && bDeep ? m_pInstancer->getStats() : Dictionary();
	instancer["render_cells"] = m_runtimeProfileLastSample.renderCells;
	instancer["pending_cell_updates"] = m_runtimeProfileLastSample.pendingCellUpdates;
	instancer["cell_size"] = m_nInstancerCellSize;
	instancer["streaming_enabled"] = m_bInstancerCellStreamingEnabled;
	instancer["streaming_effective"] = effectiveInstancerCellStreamingEnabled();
	instancer["single_lod_buffers"] = m_bInstancerSingleLodBuffers;
	instancer["streaming_margin"] = m_fInstancerCellStreamingMargin;
	instancer["streaming_update_distance"] = m_fInstancerCellStreamingUpdateDistance;
	instancer["update_cell_budget"] = m_nInstancerUpdateCellBudget;
	diagnostics["instancer"] = instancer;

	if ( m_pCollision ) {
		Dictionary collision = bDeep ? m_pCollision->getStats() : Dictionary();
		collision["enabled"] = m_pCollision->isEnabled();
		collision["update_usec_last"] = ( int64_t ) ( m_runtimeProfileLastSample.nCollisionUsec );
		diagnostics["collision"] = collision;
	}
	if ( m_pMaterial.is_valid() ) {
		diagnostics["shaders"] = m_pMaterial->getShaderVariantDiagnostics();
	}

	Dictionary configuration;
	configuration["region_size"] = ( int ) ( m_nRegionSize );
	configuration["world_region_size_m"] = ( real_t ) ( m_nRegionSize ) * m_fVertexSpacing;
	configuration["gpu_cache_enabled"] = m_bGpuRegionCacheEnabled;
	configuration["gpu_cache_effective"] = effectiveGpuRegionCacheEnabled();
	configuration["gpu_cache_budget_mb"] = m_nGpuRegionCacheBudgetMb;
	configuration["gpu_upload_budget_layers"] = m_nGpuRegionUploadBudgetLayers;
	configuration["gpu_upload_budget_mb"] = m_nGpuRegionUploadBudgetMb;
	configuration["gpu_spare_pages"] = m_nGpuRegionSparePages;
	configuration["gpu_retire_frames"] = m_nGpuRegionRetireFrames;
	configuration["gpu_detail_lods"] = m_nGpuRegionDetailLods;
	configuration["gpu_required_core_pages"] = getGpuRegionRequiredCorePages();
	configuration["gpu_mapped_capacity"] = getGpuRegionMappedCapacity();
	configuration["gpu_detail_range_valid"] = isGpuRegionDetailRangeValid();
	configuration["gpu_guard_regions"] = m_nGpuRegionGuardRegions;
	configuration["gpu_prefetch_rows"] = m_nGpuRegionPrefetchRows;
	configuration["gpu_hysteresis_ticks"] = m_nGpuRegionHysteresisTicks;
	configuration["gpu_color_resolution_divisor"] = m_nGpuRegionColorResolutionDivisor;
	configuration["gpu_derived_resolution_divisor"] = m_nGpuRegionDerivedResolutionDivisor;
	configuration["gpu_cooked_pages_enabled"] = m_bGpuRegionCookedPagesEnabled;
	configuration["gpu_cook_on_save"] = m_bGpuRegionCookOnSave;
	configuration["page_prepare_threads"] = m_nRuntimePagePrepareThreads;
	configuration["sync_detail_lods"] = m_bRuntimeSyncDetailLods;
	configuration["cpu_cache_enabled"] = m_bCpuRegionCacheEnabled;
	configuration["cpu_cache_effective"] = effectiveCpuRegionCacheEnabled();
	configuration["cpu_cache_budget_mb"] = m_nCpuRegionCacheBudgetMb;
	configuration["cpu_load_budget_layers"] = m_nCpuRegionLoadBudgetLayers;
	configuration["cpu_load_budget_mb"] = m_nCpuRegionLoadBudgetMb;
	diagnostics["configuration"] = configuration;

	if ( m_pData ) {
		diagnostics["gpu"] = bDeep ? m_pData->getGpuRegionCacheStats() : m_pData->getGpuRegionCacheStatsFast();
		diagnostics["middle_height_cache"] = m_pData->getMiddleHeightCacheStats();
		diagnostics["cpu"] = bDeep ? m_pData->getCpuRegionCacheStats() : m_pData->getCpuRegionCacheStatsFast();
		diagnostics["instance_cache"] = bDeep ? m_pData->getInstanceRegionCacheStats() : m_pData->getInstanceRegionCacheStatsFast();
		diagnostics["startup_load"] = m_pData->getStartupLoadStats();
	}
	diagnostics["persistent_upload"] = Terrain3DUploadBridge::getStats();
	diagnostics["aggregate"] = getRuntimeProfileAggregate();
	diagnostics["active_render_pipeline"] = getShaderTierStatsDictionaries();
	diagnostics["cache_inventory"] = getCacheInventory( bDeep );
	diagnostics["gpu_information"] = getGpuInformationStats();

	const uint64_t collection_finished_usec = Time::get_singleton()->get_ticks_usec();
	m_nDiagnosticsCollectionUsecLast = collection_finished_usec >= collection_started_usec ?
		collection_finished_usec - collection_started_usec : 0u;
	m_nDiagnosticsCollectionUsecTotal += m_nDiagnosticsCollectionUsecLast;
	m_nDiagnosticsCollectionUsecPeak = MAX( m_nDiagnosticsCollectionUsecPeak, m_nDiagnosticsCollectionUsecLast );
	++m_nDiagnosticsCollectionCount;
	diagnostics["cpu_profile"] = getCpuProfileStats();
	Dictionary overhead;
	overhead["last_usec"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecLast );
	overhead["average_usec"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecTotal / MAX( ( uint64_t ) ( 1u ), m_nDiagnosticsCollectionCount ) );
	overhead["peak_usec"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecPeak );
	overhead["collection_count"] = ( int64_t ) ( m_nDiagnosticsCollectionCount );
	overhead["deep_introspection"] = bDeep;
	overhead["deep_scan_note"] = bDeep ?
		"Deep mode includes O(N) node, instancer, collision, and detailed cache introspection." :
		"Default mode uses bounded/incremental subsystem counters and omits deep collection scans.";
	diagnostics["profiler_overhead"] = overhead;
	return diagnostics;
}

void T3DTerrain::resetRuntimeDiagnostics( void ) {
	m_runtimeProfileLastSample = t3druntimeProfileSample_t{};
	m_nRuntimeProfileSampleCount = 0u;
	m_runtimeProfileAccumulator.reset();
	m_nDiagnosticsCollectionUsecLast = 0u;
	m_nDiagnosticsCollectionUsecTotal = 0u;
	m_nDiagnosticsCollectionUsecPeak = 0u;
	m_nDiagnosticsCollectionCount = 0u;
	m_szDiagnosticCachePeakBytes.fill( 0u );
	if ( m_pRuntimeProfileHistory ) {
		m_pRuntimeProfileHistory->reset();
	}
	if ( m_pMaterial.is_valid() ) {
		m_pMaterial->resetShaderVariantDiagnostics();
	}
	m_nRuntimeProfileShaderUpdatesSeen = 0u;
	m_runtimeProfileShaderCreationsSeen = 0u;
	m_nRuntimeAdaptiveBudgetState.overBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.underBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.nAdjustmentCount = 0u;
	m_nRuntimeBoundaryNodeDeferralsTotal = 0u;
	m_nRuntimeBoundaryInstancerDeferralsTotal = 0u;
	m_runtimeBoundaryNodeDeferralStreak = 0;
	m_runtimeBoundaryInstancerDeferralStreak = 0;
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
	const t3dadaptiveBudgetLimits_t limits = getRuntimeAdaptiveBudgetLimits();
	const t3dadaptiveBudgetPressure_t pressure = getRuntimeAdaptiveBudgetPressure();
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

	t3dadaptiveBudgetState_t displayed_state = m_nRuntimeAdaptiveBudgetState;
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

void T3DTerrain::setPerformanceTarget( const t3dperformanceTarget_t target ) {
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
		const terrain_3d_profile::t3dsetting_t &setting = terrain_3d_profile::XBOX360_EXPERIMENTAL_SETTINGS[i];
		Dictionary row;
		row["owner"] = terrain3dPerformanceProfileOwnerName( setting.owner );
		row["property"] = setting.pszProperty;
		row["key"] = terrain3dPerformanceProfileOwnerName( setting.owner ) + "." + setting.pszProperty;
		row["value"] = terrain3dPerformanceProfileValue( setting );
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
	for ( const terrain_3d_profile::t3dsetting_t &setting : terrain_3d_profile::XBOX360_EXPERIMENTAL_SETTINGS ) {
		const Object *pOwner = this;
		if ( setting.owner == terrain_3d_profile::OWNER_MATERIAL ) {
			if ( m_pMaterial.is_null() ) {
				Dictionary row;
				row["owner"] = "Terrain3DMaterial";
				row["property"] = setting.pszProperty;
				row["key"] = String( "Terrain3DMaterial." ) + setting.pszProperty;
				row["current"] = Variant();
				row["expected"] = terrain3dPerformanceProfileValue( setting );
				differences.push_back( row );
				continue;
			}
			pOwner = m_pMaterial.ptr();
		}
		const Variant current = pOwner->get( StringName( setting.pszProperty ) );
		if ( terrain3dPerformanceProfileValueMatches( current, setting ) ) {
			continue;
		}
		Dictionary row;
		row["owner"] = terrain3dPerformanceProfileOwnerName( setting.owner );
		row["property"] = setting.pszProperty;
		row["key"] = terrain3dPerformanceProfileOwnerName( setting.owner ) + "." + setting.pszProperty;
		row["current"] = current;
		row["expected"] = terrain3dPerformanceProfileValue( setting );
		differences.push_back( row );
	}
	return differences;
}

void T3DTerrain::resetPerformanceProfile( void ) {
	if ( m_performanceTarget != PERFORMANCE_TARGET_CUSTOM ) {
		setPerformanceTarget( m_performanceTarget );
	}
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
void T3DTerrain::setHlodTargetErrorPixels( const real_t fPixels ) { m_fHlodTargetErrorPixels = CLAMP( fPixels, 0.25f, 64.0f ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setTargetErrorPixels( m_fHlodTargetErrorPixels ); } }
void T3DTerrain::setHlodHysteresis( const real_t fRatio ) { m_fHlodHysteresis = CLAMP( fRatio, 0.0f, 0.75f ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setHysteresis( m_fHlodHysteresis ); } }
void T3DTerrain::setHlodMaxLevel( const int nLevel ) { m_nHlodMaxLevel = CLAMP( nLevel, 1, 6 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setMaxLevel( m_nHlodMaxLevel ); } }
void T3DTerrain::setHlodLoadBudget( const int nBudget ) { m_nHlodLoadBudget = CLAMP( nBudget, 0, 64 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setLoadBudget( m_nHlodLoadBudget ); } }
void T3DTerrain::setHlodUnloadBudget( const int nBudget ) { m_nHlodUnloadBudget = CLAMP( nBudget, 0, 64 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setUnloadBudget( m_nHlodUnloadBudget ); } }
void T3DTerrain::setHlodMemoryBudgetMb( const int nBudgetMb ) {
	m_nHlodMemoryBudgetMb = CLAMP( nBudgetMb, 1, 4096 ); markMemoryBudgetDirty();
	if ( !m_bHlodMemoryBudgetAuto && m_bInitialized ) { serviceMemoryBudgetPolicy( true ); }
}
void T3DTerrain::setHlodMaxShadowLevel( const int nLevel ) { m_nHlodMaxShadowLevel = CLAMP( nLevel, 0, 6 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setMaxShadowLevel( m_nHlodMaxShadowLevel ); } }
void T3DTerrain::setHlodForcedDebugLevel( const int nLevel ) { m_nHlodForcedDebugLevel = CLAMP( nLevel, -1, 6 ); if ( m_pHlodStreamer ) { m_pHlodStreamer->setForcedDebugLevel( m_nHlodForcedDebugLevel ); } }
bool T3DTerrain::reloadHlodManifest( void ) { return m_pHlodStreamer && m_pHlodStreamer->reloadManifest(); }
Dictionary T3DTerrain::getHlodStreamingStats( void ) const { return m_pHlodStreamer ? m_pHlodStreamer->getStats() : Dictionary(); }
Array T3DTerrain::getHlodDebugEntries( void ) const { return m_pHlodStreamer ? m_pHlodStreamer->getDebugEntries() : Array(); }
bool T3DTerrain::isHlodRegionSuppressed( const Vector2i &rRegion, const int nMinimumLevel ) const { return m_pHlodStreamer && m_pHlodStreamer->isRegionSuppressed( rRegion, nMinimumLevel ); }
bool T3DTerrain::isHlodRegionDetailRequired( const Vector2i &rRegion ) const { return !m_pHlodStreamer || m_pHlodStreamer->isRegionDetailRequired( rRegion ); }
bool T3DTerrain::isHlodRegionManaged( const Vector2i &rRegion ) const { return m_pHlodStreamer && m_pHlodStreamer->isRegionManaged( rRegion ); }
uint8_t T3DTerrain::getHlodInstancerMinimumLevel( const Vector2i &rRegion, const int nMeshId ) const { return m_pHlodStreamer ? m_pHlodStreamer->getInstancerMinimumLevel( rRegion, nMeshId ) : 0; }
uint8_t T3DTerrain::getHlodNodeMinimumLevel( const Vector2i &rRegion, const int64_t nSpawnId ) const { return m_pHlodStreamer ? m_pHlodStreamer->getNodeMinimumLevel( rRegion, nSpawnId ) : 0; }
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

void T3DTerrain::setLabelDistance( const real_t fDistance ) {
	SET_IF_DIFF( m_fLabelDistance, CLAMP( fDistance, 0.f, 100000.f ) );
	LOG( T3DTerrain::INFO, "t3dsetting_t region label distance: ", m_fLabelDistance );
	updateRegionLabels();
}

void T3DTerrain::setLabelSize( const int nSize ) {
	SET_IF_DIFF( m_nLabelSize, CLAMP( nSize, 24, 128 ) );
	LOG( T3DTerrain::INFO, "t3dsetting_t region label size: ", m_nLabelSize );
	updateRegionLabels();
}

void T3DTerrain::updateRegionLabels( void ) {
	destroyLabels();
	if ( m_pLabelParent && m_fLabelDistance > 0.f && m_pData ) {
		const TypedArray<Vector2i> region_locations = m_pData->getRegionLocations();
		LOG( T3DTerrain::DEBUG, "Creating ", region_locations.size(), " region labels" );
		for ( const Vector2i region_loc : region_locations ) {
			Label3D *const pLabel = memnew( Label3D );
			const String text = region_loc;
			pLabel->set_name( "Label3D" + text.replace( " ", "" ) );
			pLabel->set_pixel_size( .001f );
			pLabel->set_billboard_mode( BaseMaterial3D::BILLBOARD_ENABLED );
			pLabel->set_draw_flag( Label3D::FLAG_DOUBLE_SIDED, true );
			pLabel->set_draw_flag( Label3D::FLAG_DISABLE_DEPTH_TEST, true );
			pLabel->set_draw_flag( Label3D::FLAG_FIXED_SIZE, true );
			pLabel->set_render_priority( 127 );
			pLabel->set_outline_render_priority( 126 );
			pLabel->set_text( text );
			pLabel->set_modulate( Color( 1.f, 1.f, 1.f, .5f ) );
			pLabel->set_outline_modulate( Color( 0.f, 0.f, 0.f, .5f ) );
			pLabel->set_font_size( m_nLabelSize );
			pLabel->set_outline_size( m_nLabelSize / 6 );
			pLabel->set_visibility_range_end( m_fLabelDistance );
			pLabel->set_visibility_range_end_margin( m_fLabelDistance / 10.f );
			pLabel->set_visibility_range_fade_mode( GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF );
			m_pLabelParent->add_child( pLabel, true );
			Vector3 pos =
				Vector3( ( real_t ) ( region_loc.x ) + .5f, 0.f, ( real_t ) ( region_loc.y ) + .5f ) * m_nRegionSize * m_fVertexSpacing;
			const real_t fHeight = m_pData->getHeight( pos );
			pos.y = ( std::isnan( fHeight ) ) ? 0.f : fHeight;
			pLabel->set_position( pos );
		}
	}
}

void T3DTerrain::setCamera( Camera3D *const pCamera ) {
	if ( m_camera.ptr() != pCamera ) {
		LOG( T3DTerrain::EXTREME, "t3dsetting_t camera: ", pCamera );
		m_camera.setTarget( pCamera );
		if ( m_clipmapTarget.isValid() ) {
			set_physics_process( true );
		}
	}
}

void T3DTerrain::setClipmapTarget( Node3D *const pNode ) {
	if ( m_clipmapTarget.ptr() != pNode ) {
		LOG( T3DTerrain::INFO, "t3dsetting_t clipmap target: ", pNode );
		m_clipmapTarget.setTarget( pNode );
		if ( m_clipmapTarget.isValid() ) {
			set_physics_process( true );
		}
	}
}

Vector3 T3DTerrain::getClipmapTargetPosition( void ) const {
	// In Editor, or no clipmap target, use camera
	if ( IS_EDITOR || !m_clipmapTarget.getTarget() ) {
		if ( Node3D *cam = m_camera.getTarget() ) {
			return cam->get_global_position();
		}
	}
	if ( Node3D *target = m_clipmapTarget.getTarget() ) {
		return target->get_global_position();
	}
	return V3_ZERO;
}

void T3DTerrain::setCollisionTarget( Node3D *const pNode ) {
	if ( m_collisionTarget.ptr() != pNode ) {
		LOG( T3DTerrain::INFO, "t3dsetting_t collision target: ", pNode );
		m_collisionTarget.setTarget( pNode );
		if ( m_collisionTarget.isValid() ) {
			set_physics_process( true );
		}
	}
}

Vector3 T3DTerrain::getCollisionTargetPosition( void ) const {
	// In Editor, always prefer camera
	if ( IS_EDITOR ) {
		if ( Node3D *cam = m_camera.getTarget() ) {
			return cam->get_global_position();
		}
	}
	if ( Node3D *target = m_collisionTarget.getTarget() ) {
		return target->get_global_position();
	}
	if ( Node3D *target = m_clipmapTarget.getTarget() ) {
		return target->get_global_position();
	}
	if ( Node3D *cam = m_camera.getTarget() ) {
		return cam->get_global_position();
	}
	return V3_ZERO;
}

void T3DTerrain::updateEnvironmentShadowCasterMask( void ) {
	DirectionalLight3D *const pLight = Object::cast_to<DirectionalLight3D>( m_lightTarget.ptr() );
	if ( !pLight ) {
		return;
	}
	if ( !m_bSemanticShadowPolicyEnabled ) {
		if ( m_bLightShadowMaskCaptured ) {
			pLight->set_shadow_caster_mask( m_nLightShadowCasterMaskBeforeSemantic );
			m_bLightShadowMaskCaptured = false;
		}
		return;
	}
	if ( !m_bLightShadowMaskCaptured ) {
		m_nLightShadowCasterMaskBeforeSemantic = pLight->get_shadow_caster_mask();
		m_bLightShadowMaskCaptured = true;
	}

	uint32_t mask = m_nEnvironmentShadowExternalMask | TERRAIN3D_LIGHTING_LAYER_REGIONAL;
	if ( m_environmentShadowQuality >= ENVIRONMENT_SHADOW_NORMAL ) {
		mask |= TERRAIN3D_LIGHTING_LAYER_LARGE;
	}
	if ( m_environmentShadowQuality >= ENVIRONMENT_SHADOW_ULTRA ) {
		mask |= TERRAIN3D_LIGHTING_LAYER_FOLIAGE;
	}
	pLight->set_shadow_caster_mask( mask );
}

void T3DTerrain::setSemanticShadowPolicyEnabled( const bool bEnabled ) {
	if ( m_bSemanticShadowPolicyEnabled == bEnabled ) {
		return;
	}
	m_bSemanticShadowPolicyEnabled = bEnabled;
	updateEnvironmentShadowCasterMask();
}

void T3DTerrain::setEnvironmentShadowQuality( const t3denvironmentShadowQuality_t eQuality ) {
	const t3denvironmentShadowQuality_t quality = ( t3denvironmentShadowQuality_t ) ( CLAMP( ( int ) ( eQuality ), ( int ) ( ENVIRONMENT_SHADOW_LOW ), ( int ) ( ENVIRONMENT_SHADOW_ULTRA ) ) );
	if ( m_environmentShadowQuality == quality ) {
		return;
	}
	m_environmentShadowQuality = quality;
	updateEnvironmentShadowCasterMask();
}

void T3DTerrain::setEnvironmentShadowExternalMask( const uint32_t nMask ) {
	const uint32_t mask = nMask & 0xFFFFFu;
	if ( m_nEnvironmentShadowExternalMask == mask ) {
		return;
	}
	m_nEnvironmentShadowExternalMask = mask;
	updateEnvironmentShadowCasterMask();
}

void T3DTerrain::updateEnvironmentDirectionalReceiverMask( void ) {
	DirectionalLight3D *const pLight = Object::cast_to<DirectionalLight3D>( m_lightTarget.ptr() );
	if ( !pLight ) {
		return;
	}
	if ( !m_bSemanticDirectionalReceptionEnabled ) {
		if ( m_bLightCullMaskCaptured ) {
			pLight->set_cull_mask( m_nLightCullMaskBeforeSemantic );
			m_bLightCullMaskCaptured = false;
		}
		return;
	}
	if ( !m_bLightCullMaskCaptured ) {
		m_nLightCullMaskBeforeSemantic = pLight->get_cull_mask();
		m_bLightCullMaskCaptured = true;
	}
	const uint32_t mask = m_nEnvironmentDirectionalExternalMask | TERRAIN3D_DIRECTIONAL_RECEIVER_LAYER;
	pLight->set_cull_mask( mask & 0xFFFFFu );
}

void T3DTerrain::setSemanticDirectionalReceptionEnabled( const bool bEnabled ) {
	if ( m_bSemanticDirectionalReceptionEnabled == bEnabled ) {
		return;
	}
	m_bSemanticDirectionalReceptionEnabled = bEnabled;
	updateEnvironmentDirectionalReceiverMask();
}

void T3DTerrain::setEnvironmentDirectionalExternalMask( const uint32_t nMask ) {
	const uint32_t mask = nMask & 0xFFFFFu;
	if ( m_nEnvironmentDirectionalExternalMask == mask ) {
		return;
	}
	m_nEnvironmentDirectionalExternalMask = mask;
	updateEnvironmentDirectionalReceiverMask();
}

void T3DTerrain::setLightTarget( Node3D *const pNode ) {
	if ( m_lightTarget.ptr() != pNode ) {
		DirectionalLight3D *const pOldLight = Object::cast_to<DirectionalLight3D>( m_lightTarget.ptr() );
		if ( m_bSemanticShadowPolicyEnabled && m_bLightShadowMaskCaptured && pOldLight ) {
			pOldLight->set_shadow_caster_mask( m_nLightShadowCasterMaskBeforeSemantic );
		}
		if ( m_bSemanticDirectionalReceptionEnabled && m_bLightCullMaskCaptured && pOldLight ) {
			pOldLight->set_cull_mask( m_nLightCullMaskBeforeSemantic );
		}
		m_bLightShadowMaskCaptured = false;
		m_bLightCullMaskCaptured = false;
		LOG( T3DTerrain::INFO, "t3dsetting_t directional light target: ", pNode );
		m_lightTarget.setTarget( pNode );
		m_bLightStateValid = false;
		updateEnvironmentShadowCasterMask();
		updateEnvironmentDirectionalReceiverMask();
		if ( m_lightTarget.isValid() ) {
			set_physics_process( true );
		}
	}
}

void T3DTerrain::snap( void ) {
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->resetTargetPosition();
	}
	if ( m_bOceanEnabled && m_pOceanMesher ) {
		m_pOceanMesher->resetTargetPosition();
	}
	if ( m_pCollision ) {
		m_pCollision->resetTargetPosition();
	}
	if ( m_tessellationLevel > 0 ) {
		m_lastBufferPosition = V2_MAX;
	}
}

void T3DTerrain::setMaterial( const Ref<T3DMaterial> &rMaterial ) {
	if ( !differs( m_pMaterial, rMaterial ) ) {
		return;
	}
	LOG( T3DTerrain::INFO, "Replacing terrain material" );
	const bool bWasInitialized = m_bInitialized;
	const bool bSuppliedMaterial = rMaterial.is_valid();
	const Ref<T3DMaterial> pOldMaterial = m_pMaterial;
	disconnectResourceSignals( m_pAssets, pOldMaterial );
	if ( pOldMaterial.is_valid() ) {
		pOldMaterial->uninitialize( this );
	}
	m_pMaterial = rMaterial;
	if ( m_pMaterial.is_null() ) {
		m_pMaterial.instantiate();
	}
	if ( m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL && !bSuppliedMaterial ) {
		m_pMaterial->setShaderPerformancePreset( T3DMaterial::SHADER_PRESET_XBOX_360_AUTHENTIC );
	}
	m_bPerformanceProfileMaterialPendingApply = false;
	m_bLightStateValid = false;
	connectResourceSignals( m_pAssets, m_pMaterial );
	applyAutoShaderProfileMode();
	if ( bWasInitialized ) {
		m_pMaterial->initialize( this );
		m_pMaterial->setEffectiveShaderVariantSourceBudgetMb( m_nEffectiveShaderCacheBudgetMb );
		serviceMemoryBudgetPolicy( true );
		setupTerrainMesher();
		updateDisplacementBuffer();
	} else {
		initializeInternal();
	}
	refreshPerformanceProfileConformity();
	emit_signal( "material_changed" );
}

void T3DTerrain::setRuntimeShaderTieringEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bRuntimeShaderTieringEnabled, bEnabled );
	refreshPerformanceProfileConformity();
	LOG( T3DTerrain::INFO, "t3dsetting_t runtime shader tiering: ", bEnabled );
	if ( bEnabled && m_pData && m_bInitialized && !m_pData->ensureFarAtlases() ) {
		LOG( T3DTerrain::WARN, "Runtime far shader tier could not prepare coarse terrain atlases; far-tier rendering may use fallback textures until terrain data is ready." );
	}
	if ( m_pMaterial.is_valid() && m_pMaterial->isInitialized() ) {
		m_pMaterial->update( T3DMaterial::FULL_REBUILD );
	}
	if ( m_bInitialized ) {
		setupTerrainMesher();
	}
}

void T3DTerrain::setRuntimeNearShaderLods( const int nCount ) {
	const int count = CLAMP( nCount, 0, getTotalClipmapLevels() );
	if ( m_nRuntimeNearShaderLods == count ) {
		return;
	}
	m_nRuntimeNearShaderLods = count;
	refreshPerformanceProfileConformity();
	applyRuntimeTierLayout( true );
	notify_property_list_changed();
}

void T3DTerrain::setRuntimeMiddleShaderLods( const int nCount ) {
	const int count = CLAMP( nCount, 0, getTotalClipmapLevels() - m_nRuntimeNearShaderLods );
	if ( m_nRuntimeMiddleShaderLods == count ) {
		return;
	}
	m_nRuntimeMiddleShaderLods = count;
	refreshPerformanceProfileConformity();
	applyRuntimeTierLayout( true );
	notify_property_list_changed();
}

void T3DTerrain::setRuntimeNearRangeEndM( const real_t fRangeM ) {
	setRuntimeNearShaderLods( rangeToLodCount( MAX( fRangeM, ( real_t ) ( 0.0f ) ) ) );
}

real_t T3DTerrain::getRuntimeNearRangeEndM( void ) const {
	return lodCountToRange( m_nRuntimeNearShaderLods );
}

void T3DTerrain::setRuntimeMiddleRangeEndM( const real_t fRangeM ) {
	const int total_non_far = MAX( m_nRuntimeNearShaderLods,
		rangeToLodCount( MAX( fRangeM, ( real_t ) ( 0.0f ) ) ) );
	setRuntimeMiddleShaderLods( total_non_far - m_nRuntimeNearShaderLods );
}

real_t T3DTerrain::getRuntimeMiddleRangeEndM( void ) const {
	return lodCountToRange( m_nRuntimeNearShaderLods + m_nRuntimeMiddleShaderLods );
}

void T3DTerrain::setRuntimeSyncDetailLods( const bool bEnabled ) {
	if ( m_bRuntimeSyncDetailLods == bEnabled ) {
		return;
	}
	m_bRuntimeSyncDetailLods = bEnabled;
	refreshPerformanceProfileConformity();
	applyRuntimeTierLayout( true );
}

void T3DTerrain::setRuntimeClipmapUpdateBudget( const int nCount ) {
	const int count = CLAMP( nCount, 1, TERRAIN3D_MAX_CLIPMAP_LEVELS );
	if ( m_nRuntimeClipmapUpdateBudget == count ) {
		return;
	}
	m_nRuntimeClipmapUpdateBudget = count;
	refreshPerformanceProfileConformity();
	applyRuntimeTierLayout( false );
}

void T3DTerrain::setRuntimePagePrepareThreads( const int nCount ) {
	const int count = CLAMP( nCount, 1, 32 );
	if ( m_nRuntimePagePrepareThreads == count ) {
		return;
	}
	m_nRuntimePagePrepareThreads = count;
	refreshPerformanceProfileConformity();
}

void T3DTerrain::setMeshLods( const int nCount ) {
	SET_IF_DIFF( m_nMeshLods, CLAMP( nCount, 1, 10 ) );
	refreshPerformanceProfileConformity();
	normalizeRuntimeTierLayout( true );
	LOG( T3DTerrain::INFO, "t3dsetting_t mesh levels: ", m_nMeshLods );
	if ( m_pTerrainMesher && m_pMaterial.is_valid() ) {
		m_pMaterial->update();
		setupTerrainMesher();
	}
	if ( m_pData && effectiveGpuRegionCacheEnabled() && !m_bDeferGpuRegionCacheReconfigure ) {
		m_pData->configureGpuRegionCache( true, m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setTessellationLevel( const int nLevel ) {
	const int old_level = m_tessellationLevel;
	SET_IF_DIFF( m_tessellationLevel, CLAMP( nLevel, 0, 6 ) );
	refreshPerformanceProfileConformity();
	normalizeRuntimeTierLayout( true );
	LOG( T3DTerrain::INFO, "t3dsetting_t tessellation level: ", nLevel );
	// The optimized R8 displacement array exists only while tessellation is
	// active. Crossing zero creates or frees it before rebuilding shaders.
	if ( ( old_level == 0 ) != ( m_tessellationLevel == 0 ) && m_pAssets.is_valid() && m_pAssets->isInitialized() &&
		m_pAssets->usesPackedPbr() ) {
		m_pAssets->updateTextureList();
	}
	if ( m_pTerrainMesher && m_pMaterial.is_valid() ) {
		m_pMaterial->update( T3DMaterial::FULL_REBUILD );
		setupTerrainMesher();
		updateDisplacementBuffer();
	}
	if ( m_pData && effectiveGpuRegionCacheEnabled() && !m_bDeferGpuRegionCacheReconfigure ) {
		m_pData->configureGpuRegionCache( true, m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
	notify_property_list_changed();
}

void T3DTerrain::setMeshSize( const int nSize ) {
	SET_IF_DIFF( m_nMeshSize, CLAMP( nSize & ~1, 8, 256 ) ); // Ensure even
	refreshPerformanceProfileConformity();
	normalizeRuntimeTierLayout( true );
	LOG( T3DTerrain::INFO, "t3dsetting_t mesh size: ", m_nMeshSize );
	if ( m_pTerrainMesher && m_pMaterial.is_valid() ) {
		m_pMaterial->update();
		setupTerrainMesher();
		updateDisplacementBuffer();
	}
	if ( m_pData && effectiveGpuRegionCacheEnabled() && !m_bDeferGpuRegionCacheReconfigure ) {
		m_pData->configureGpuRegionCache( true, m_nEffectiveGpuRegionCacheBudgetMb,
			m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
			m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
	}
}

void T3DTerrain::setVertexSpacing( const real_t fSpacing ) {
	if ( !Math::is_equal_approx( fSpacing, ( real_t ) ( 1.0f ) ) ) {
		LOG( T3DTerrain::ERROR, "This Terrain3D fork requires 1 metre vertex spacing so every 128-sample region is exactly 128 metres wide" );
		return;
	}
	if ( Math::is_equal_approx( m_fVertexSpacing, ( real_t ) ( 1.0f ) ) ) {
		return;
	}
	m_fVertexSpacing = 1.0f;
	LOG( T3DTerrain::INFO, "t3dsetting_t fixed vertex spacing: 1 metre" );
	if ( m_pCollision && m_pData && m_pInstancer && m_pMaterial.is_valid() ) {
		m_pInstancer->updateVertexSpacing( m_fVertexSpacing );
		m_pData->setVertexSpacingInternal( m_fVertexSpacing );
		updateRegionLabels();
		m_pMaterial->update();
		setupTerrainMesher();
		m_pCollision->destroy();
		m_pCollision->build();
		updateDisplacementBuffer();
		if ( effectiveGpuRegionCacheEnabled() ) {
			m_pData->configureGpuRegionCache( true, m_nEffectiveGpuRegionCacheBudgetMb,
				m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb,
				m_nGpuRegionSparePages, m_nGpuRegionRetireFrames );
		}
	}
}

void T3DTerrain::setCullMargin( const real_t fMargin ) {
	SET_IF_DIFF( m_fCullMargin, CLAMP( fMargin, 0.f, 100000.f ) );
	LOG( T3DTerrain::INFO, "t3dsetting_t extra cull margin: ", m_fCullMargin );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->updateAabbs();
	}
}

void T3DTerrain::setCastShadows( const RenderingServer::ShadowCastingSetting castShadows ) {
	SET_IF_DIFF( m_castShadows, castShadows );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setCastShadows( m_castShadows );
	}
}

void T3DTerrain::setShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nShadowLodCount, CLAMP( nCount, 0, TERRAIN3D_MAX_CLIPMAP_LEVELS ) );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setShadowLodCount( m_nShadowLodCount );
	}
}

void T3DTerrain::setNearShadowEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bNearShadowEnabled, bEnabled );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowEnabled( { m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled } );
	}
}

void T3DTerrain::setMiddleShadowEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bMiddleShadowEnabled, bEnabled );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowEnabled( { m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled } );
	}
}

void T3DTerrain::setFarShadowEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bFarShadowEnabled, bEnabled );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowEnabled( { m_bNearShadowEnabled, m_bMiddleShadowEnabled, m_bFarShadowEnabled } );
	}
}

void T3DTerrain::setNearShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nNearShadowLodCount, CLAMP( nCount, -1, TERRAIN3D_MAX_CLIPMAP_LEVELS ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts(
			{ m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void T3DTerrain::setMiddleShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nMiddleShadowLodCount, CLAMP( nCount, -1, TERRAIN3D_MAX_CLIPMAP_LEVELS ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts(
			{ m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void T3DTerrain::setFarShadowLodCount( const int nCount ) {
	SET_IF_DIFF( m_nFarShadowLodCount, CLAMP( nCount, -1, TERRAIN3D_MAX_CLIPMAP_LEVELS ) );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setTierShadowLodCounts(
			{ m_nNearShadowLodCount, m_nMiddleShadowLodCount, m_nFarShadowLodCount } );
	}
}

void T3DTerrain::setGiMode( const GeometryInstance3D::GIMode giMode ) {
	SET_IF_DIFF( m_giMode, giMode );
	refreshPerformanceProfileConformity();
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setGiMode( m_giMode );
	}
}

void T3DTerrain::setRenderLayers( const uint32_t nLayers ) {
	SET_IF_DIFF( m_renderLayers, nLayers );
	LOG( T3DTerrain::INFO, "t3dsetting_t terrain render layers to: ", nLayers );
	if ( m_pTerrainMesher ) {
		m_pTerrainMesher->setRenderLayers( m_renderLayers );
	}
}

void T3DTerrain::updateOceanShaderVariant( void ) {
	ShaderMaterial *const pMaterial = Object::cast_to<ShaderMaterial>( m_pOceanMaterial.ptr() );
	if ( !pMaterial || pMaterial->get_shader().is_null() ) {
		return;
	}
	const String current = pMaterial->get_shader()->get_code();
	if ( m_oceanVariantSource.is_empty() && current.contains( "T3D_OCEAN_VARIANT_SOURCE" ) ) {
		m_oceanVariantSource = current;
	}
	if ( m_oceanVariantSource.is_empty() ) {
		return; // Custom ocean material: do not rewrite user shader code.
	}
	String code = m_oceanVariantSource;
	code = code.replace( "#define T3D_OCEAN_VERTEX_ITERATIONS 1",
						"#define T3D_OCEAN_VERTEX_ITERATIONS " + String::num_int64( m_nOceanVertexWaveIterations ) );
	code = code.replace( "#define T3D_OCEAN_FRAGMENT_ITERATIONS 3",
						"#define T3D_OCEAN_FRAGMENT_ITERATIONS " + String::num_int64( m_nOceanFragmentWaveIterations ) );
	code = code.replace( "#define T3D_OCEAN_REFRACTION_MODE 2",
						"#define T3D_OCEAN_REFRACTION_MODE " + String::num_int64( m_oceanRefractionMode ) );
	code = code.replace( "#define T3D_OCEAN_FOAM_MODE 2",
						"#define T3D_OCEAN_FOAM_MODE " + String::num_int64( m_oceanFoamMode ) );
	code = code.replace( "#define T3D_OCEAN_SCATTERING_MODE 2",
						"#define T3D_OCEAN_SCATTERING_MODE " + String::num_int64( m_oceanScatteringMode ) );
	code = code.replace( "#define T3D_OCEAN_SHADING_MODE 2",
						"#define T3D_OCEAN_SHADING_MODE " + String::num_int64( m_oceanShadingMode ) );
	String render_mode = "render_mode cull_back, depth_draw_always, diffuse_burley, specular_schlick_ggx, "
						 "shadows_disabled, skip_vertex_transform;";
	switch ( m_oceanShadingMode ) {
		case OCEAN_SHADING_LAMBERT_DIFFUSE:
			render_mode = "render_mode cull_back, depth_draw_always, diffuse_lambert, specular_disabled, "
						  "shadows_disabled, skip_vertex_transform;";
			break;
		case OCEAN_SHADING_LAMBERT_GGX:
			render_mode = "render_mode cull_back, depth_draw_always, diffuse_lambert, specular_schlick_ggx, "
						  "shadows_disabled, skip_vertex_transform;";
			break;
		case OCEAN_SHADING_UNSHADED:
			render_mode =
				"render_mode cull_back, depth_draw_always, unshaded, shadows_disabled, skip_vertex_transform;";
			break;
		case OCEAN_SHADING_LAMBERT_VERTEX:
			render_mode = "render_mode cull_back, depth_draw_always, diffuse_lambert, specular_disabled, "
						  "vertex_lighting, shadows_disabled, skip_vertex_transform;";
			break;
		case OCEAN_SHADING_BURLEY_GGX:
		default:
			break;
	}
	code = code.replace( "render_mode cull_back, depth_draw_always, diffuse_burley, specular_schlick_ggx, "
						"shadows_disabled, skip_vertex_transform;",
						render_mode );
	if ( m_pOceanVariantShader.is_null() ) {
		m_pOceanVariantShader.instantiate();
	}
	m_pOceanVariantShader->set_code( code );
	pMaterial->set_shader( m_pOceanVariantShader );
}

void T3DTerrain::setOceanPerformancePreset( const t3doceanPerformancePreset_t preset ) {
	m_oceanPerformancePreset = preset;
	switch ( preset ) {
		case OCEAN_PRESET_BORDER_MINIMUM:
			m_nOceanVertexWaveIterations = 0;
			m_nOceanFragmentWaveIterations = 0;
			m_oceanRefractionMode = OCEAN_REFRACTION_OFF;
			m_oceanFoamMode = OCEAN_FOAM_OFF;
			m_oceanScatteringMode = OCEAN_SCATTERING_OFF;
			m_oceanShadingMode = OCEAN_SHADING_LAMBERT_DIFFUSE;
			break;
		case OCEAN_PRESET_BORDER_BALANCED:
			m_nOceanVertexWaveIterations = 1;
			m_nOceanFragmentWaveIterations = 1;
			m_oceanRefractionMode = OCEAN_REFRACTION_DEPTH_TINT;
			m_oceanFoamMode = OCEAN_FOAM_SHORE;
			m_oceanScatteringMode = OCEAN_SCATTERING_APPROXIMATE;
			m_oceanShadingMode = OCEAN_SHADING_LAMBERT_GGX;
			break;
		case OCEAN_PRESET_HIGH:
			m_nOceanVertexWaveIterations = 1;
			m_nOceanFragmentWaveIterations = 2;
			m_oceanRefractionMode = OCEAN_REFRACTION_FULL;
			m_oceanFoamMode = OCEAN_FOAM_SHORE;
			m_oceanScatteringMode = OCEAN_SCATTERING_APPROXIMATE;
			m_oceanShadingMode = OCEAN_SHADING_LAMBERT_GGX;
			break;
		case OCEAN_PRESET_CINEMATIC:
			m_nOceanVertexWaveIterations = 2;
			m_nOceanFragmentWaveIterations = 3;
			m_oceanRefractionMode = OCEAN_REFRACTION_FULL;
			m_oceanFoamMode = OCEAN_FOAM_FULL;
			m_oceanScatteringMode = OCEAN_SCATTERING_FULL;
			m_oceanShadingMode = OCEAN_SHADING_BURLEY_GGX;
			break;
		case OCEAN_PRESET_CUSTOM:
		default:
			break;
	}
	updateOceanShaderVariant();
}

#define T3D_OCEAN_MODE_SETTER( NAME, TYPE, FIELD )         \
	void T3DTerrain::NAME( const TYPE valueArg ) {           \
		if ( FIELD == valueArg ) {                          \
			return;                                      \
		}                                                \
		FIELD = valueArg;                                 \
		m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM; \
		updateOceanShaderVariant();                  \
	}

void T3DTerrain::setOceanVertexWaveIterations( const int nIterations ) {
	const int value = CLAMP( nIterations, 0, 3 );
	if ( m_nOceanVertexWaveIterations == value ) {
		return;
	}
	m_nOceanVertexWaveIterations = value;
	m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM;
	updateOceanShaderVariant();
}
void T3DTerrain::setOceanFragmentWaveIterations( const int nIterations ) {
	const int value = CLAMP( nIterations, 0, 4 );
	if ( m_nOceanFragmentWaveIterations == value ) {
		return;
	}
	m_nOceanFragmentWaveIterations = value;
	m_oceanPerformancePreset = OCEAN_PRESET_CUSTOM;
	updateOceanShaderVariant();
}
T3D_OCEAN_MODE_SETTER( setOceanRefractionMode, t3doceanRefractionMode_t, m_oceanRefractionMode )
T3D_OCEAN_MODE_SETTER( setOceanFoamMode, t3doceanFoamMode_t, m_oceanFoamMode )
T3D_OCEAN_MODE_SETTER( setOceanScatteringMode, t3doceanScatteringMode_t, m_oceanScatteringMode )
T3D_OCEAN_MODE_SETTER( setOceanShadingMode, t3doceanShadingMode_t, m_oceanShadingMode )
#undef T3D_OCEAN_MODE_SETTER

void T3DTerrain::setOceanEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_bOceanEnabled, bEnabled );
	LOG( T3DTerrain::INFO, "t3dsetting_t ocean enabled: ", m_bOceanEnabled );
	if ( m_bOceanEnabled ) {
		if ( m_pOceanMaterial.is_null() ) {
			const String ocean_mat_path = ProjectSettings::get_singleton()->globalize_path( OCEAN_MATERIAL_PATH );
			ResourceLoader *const pRl = ResourceLoader::get_singleton();
			if ( pRl->exists( ocean_mat_path ) ) {
				const Ref<ShaderMaterial> pOceanMat = pRl->load( ocean_mat_path );
				if ( pOceanMat.is_valid() ) {
					const Ref<ShaderMaterial> pLocalOcean = pOceanMat->duplicate( true );
					m_pOceanMaterial = pLocalOcean.is_valid() ? pLocalOcean : pOceanMat;
				}
			}
		}
		setupOceanMesher();
	} else {
		destroyOceanMesher( false );
	}
	notify_property_list_changed();
}

void T3DTerrain::setOceanMeshLods( const int nCount ) {
	SET_IF_DIFF( m_nOceanMeshLods, CLAMP( nCount, 1, 10 ) );
	LOG( T3DTerrain::INFO, "t3dsetting_t ocean mesh levels: ", m_nOceanMeshLods );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void T3DTerrain::setOceanTessellationLevel( const int nLevel ) {
	SET_IF_DIFF( m_oceanTessellationLevel, CLAMP( nLevel, 0, 6 ) );
	LOG( T3DTerrain::INFO, "t3dsetting_t ocean tessellation level: ", nLevel );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void T3DTerrain::setOceanMeshSize( const int nSize ) {
	SET_IF_DIFF( m_nOceanMeshSize, CLAMP( nSize & ~1, 8, 256 ) ); // Ensure even
	LOG( T3DTerrain::INFO, "t3dsetting_t ocean mesh size: ", m_nOceanMeshSize );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void T3DTerrain::setOceanVertexSpacing( const real_t fSpacing ) {
	SET_IF_DIFF( m_fOceanVertexSpacing, CLAMP( fSpacing, 0.25f, 100.0f ) );
	LOG( T3DTerrain::INFO, "t3dsetting_t ocean vertex spacing: ", m_fOceanVertexSpacing );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void T3DTerrain::setOceanCullMargin( const real_t fMargin ) {
	SET_IF_DIFF( m_fOceanCullMargin, CLAMP( fMargin, 0.f, 100000.f ) );
	LOG( T3DTerrain::INFO, "t3dsetting_t extra cull margin: ", m_fOceanCullMargin );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->updateAabbs( m_fOceanCullMargin, V2_ZERO );
	}
}

void T3DTerrain::setOceanCastShadows( const RenderingServer::ShadowCastingSetting castShadows ) {
	SET_IF_DIFF( m_oceanCastShadows, castShadows );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->setCastShadows( m_oceanCastShadows );
	}
}

void T3DTerrain::setOceanGiMode( const GeometryInstance3D::GIMode giMode ) {
	SET_IF_DIFF( m_oceanGiMode, giMode );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->setGiMode( m_oceanGiMode );
	}
}

void T3DTerrain::setOceanRenderLayers( const uint32_t nLayers ) {
	SET_IF_DIFF( m_oceanRenderLayers, nLayers );
	LOG( T3DTerrain::INFO, "t3dsetting_t ocean render layers to: ", nLayers );
	if ( m_pOceanMesher ) {
		m_pOceanMesher->setRenderLayers( m_oceanRenderLayers );
	}
}

void T3DTerrain::setOceanMaterial( const Ref<Material> &rMaterial ) {
	SET_IF_DIFF( m_pOceanMaterial, rMaterial );
	m_oceanVariantSource = String();
	m_pOceanVariantShader.unref();
	m_bLightStateValid = false;
	LOG( T3DTerrain::INFO, "t3dsetting_t ocean material" );
	if ( m_bOceanEnabled ) {
		setupOceanMesher();
	}
}

void T3DTerrain::setMouseLayer( const uint32_t nLayer ) {
	SET_IF_DIFF( m_mouseLayer, CLAMP( nLayer, 21u, 32u ) );
	const uint32_t mouse_mask = 1 << ( m_mouseLayer - 1 );
	LOG( T3DTerrain::INFO, "t3dsetting_t mouse layer: ", m_mouseLayer, " (", mouse_mask,
		") on terrain mesh, material, mouse camera, mouse quad" );

	// Set terrain meshes to mouse layer
	// Mask off editor render layers by ORing user layers 1-20 and current mouse layer
	setRenderLayers( ( m_renderLayers & 0xFFFFF ) | mouse_mask );
	// Set terrain shader to exclude mouse camera from showing holes
	if ( m_pMaterial.is_valid() ) {
		m_pMaterial->setShaderParam( "_mouse_layer", mouse_mask );
	}
	// Set mouse camera to see only mouse layer
	if ( m_pMouseCam ) {
		m_pMouseCam->set_cull_mask( mouse_mask );
	}
	// Set screenquad to mouse layer
	if ( m_pMouseQuad ) {
		m_pMouseQuad->set_layer_mask( mouse_mask );
	}
}

/* Returns the point a ray intersects the ground using either raymarching or the GPU depth texture
 *	p_src_pos (camera position)
 *	p_direction (camera direction looking at the terrain)
 *  p_gpu_mode - false: use raymarching, true: use GPU mode
 * Returns Vec3(NAN) on error or vec3(3.402823466e+38F) on no intersection. Test w/ if (var.x < 3.4e38)
 */
Vector3 T3DTerrain::getIntersection( const Vector3 &rSrcPos, const Vector3 &rDirection, const bool bGpuMode ) {
	if ( rDirection.is_zero_approx() || !rDirection.is_finite() ) {
		LOG( T3DTerrain::ERROR, "Invalid direction vector: ", rDirection );
		return V3_NAN;
	}
	if ( !rSrcPos.is_finite() ) {
		LOG( T3DTerrain::ERROR, "Invalid source vector: ", rSrcPos );
		return V3_NAN;
	}

	Vector3 direction = rDirection.normalized();
	// If looking straight down in a region, use get_height
	if ( direction.y < -.99999f ) {
		const real_t fHeight = m_pData->getHeight( rSrcPos );
		if ( std ::isfinite( fHeight ) ) {
			return Vector3( rSrcPos.x, fHeight, rSrcPos.z );
		}
	}

	// Raymarching mode
	if ( !bGpuMode ) {
		// Must start above terrain if in a region
		real_t fHeight = m_pData->getHeight( rSrcPos );
		if ( fHeight > rSrcPos.y ) { // False if Nan
			return V3_MAX;
		}
		// Raymarch down the ray in small increments until we find the terrain height
		Vector3 point = rSrcPos;
		for ( int i = 0; i < 4000; i++ ) {
			fHeight = m_pData->getHeight( point );
			if ( point.y - fHeight <= 0.f ) { // Nan comparison is false, which continues loop
				return point;
			}
			point += direction;
		}
		return V3_MAX;

	} else {
		// Get depth from perspective camera snapshot
		if ( !m_pMouseCam ) {
			LOG( T3DTerrain::ERROR, "Invalid mouse camera" );
			return V3_NAN;
		}
		// Position mouse cam one unit behind the requested position
		m_pMouseCam->set_global_position( rSrcPos - direction );

		// If looking straight down, then we're not in a region, set rotation directly as look_at() doesn't work
		if ( direction.y < -.99999f ) {
			m_pMouseCam->set_rotation_degrees( Vector3( -90.f, 0.f, 0.f ) );
		} else {
			m_pMouseCam->look_at( m_pMouseCam->get_global_position() + direction, V3_UP );
		}

		m_pMouseVp->set_update_mode( SubViewport::UPDATE_ONCE );
		const Ref<ViewportTexture> pVpTex = m_pMouseVp->get_texture();
		const Ref<Image> pVpImg = pVpTex->get_image();

		// Read the depth pixel from the camera viewport
		const Color screen_depth = pVpImg->get_pixel( 0, 0 );

		// Get position from depth packed in RGB - unpack back to float.
		// Forward+ is 16bit, mobile and compatibility is 10bit.
		// Compatibility also has precision loss for values below 0.5, so
		// we use only the top half of the range, for 21bit depth encoded.
		const real_t fR = floor( ( screen_depth.r * 256.f ) - 128.f );
		const real_t fG = floor( ( screen_depth.g * 256.f ) - 128.f );
		const real_t fB = floor( ( screen_depth.b * 256.f ) - 128.f );

		// Decode the full depth value
		real_t fDecodedDepth = ( fR + fG / 127.f + fB / ( 127.f * 127.f ) ) / 127.f;

		// Near-plane noise filter, or no hit (sky, underside, far clip)
		if ( fDecodedDepth < 0.00001f || fDecodedDepth > 1.f ) {
			// Catch editor ortho camera with src_pos.y at some random value around 500k
			if ( direction.y < -.99999f && rSrcPos.y >= 100000.f ) {
				return Vector3( rSrcPos.x, 0.f, rSrcPos.z );
			}
			return V3_MAX;
		}

		// Necessary for a near-far precision on hits
		if ( fDecodedDepth > 0.99999f ) {
			fDecodedDepth = 1.f;
		}

		// Denormalize distance to get real depth and terrain position.
		fDecodedDepth *= m_pMouseCam->get_far();

		// Project the camera position by the depth value to get the intersection point.
		return m_pMouseCam->get_global_position() + direction * fDecodedDepth;
	}
}

/* Returns the results of a physics raycast, optionally excluding the terrain
 *	p_src_pos (ray start position)
 *	p_direction (ray direction * magnitude), relative to src_pos
 */
Dictionary T3DTerrain::getRaycastResult(
	const Vector3 &rSrcPos, const Vector3 &rDirection,
	const uint32_t nColMask, const bool bExcludeSelf
) const {
	if ( !m_bIsInsideWorld ) {
		return Dictionary();
	}
	PhysicsDirectSpaceState3D *pSpaceState = get_world_3d()->get_direct_space_state();
	if ( !pSpaceState ) {
		LOG( T3DTerrain::ERROR, "Invalid PhysicsDirectSpaceState3D" );
		return Dictionary();
	}
	Ref<PhysicsRayQueryParameters3D> pQuery =
		PhysicsRayQueryParameters3D::create( rSrcPos, rSrcPos + rDirection, nColMask );
	if ( m_pCollision && bExcludeSelf ) {
		pQuery->set_exclude( TypedArray<RID>( m_pCollision->getRid() ) );
	}
	return pSpaceState->intersect_ray( pQuery );
}

/**
 * Generates a static ArrayMesh for the terrain.
 * p_lod (0-8): Determines the granularity of the generated mesh.
 * p_filter: Controls how vertices' Y coordinates are generated from the height map.
 *  HEIGHT_FILTER_NEAREST: Samples the height map in a 'nearest neighbour' fashion.
 *  HEIGHT_FILTER_MINIMUM: Samples a range of heights around each vertex and returns the lowest.
 *   This takes longer than ..._NEAREST, but can be used to create occluders, since it can guarantee the
 *   generated mesh will not extend above or outside the clipmap at any LOD.
 */
Ref<Mesh> T3DTerrain::bakeMesh( const int nLod, const T3DData::t3dheightFilter_t filter ) const {
	LOG( T3DTerrain::INFO, "Baking mesh at lod: ", nLod, " with filter: ", filter );
	Ref<Mesh> pResult;
	ERR_FAIL_COND_V( m_pData == nullptr, pResult );

	Ref<SurfaceTool> pSt;
	pSt.instantiate();
	pSt->begin( Mesh::PRIMITIVE_TRIANGLES );

	PackedVector3Array vertices;
	PackedVector2Array uvs;
	generateTriangles( vertices, &uvs, nLod, filter, false, AABB() );

	ERR_FAIL_COND_V( vertices.size() != uvs.size(), pResult );
	for ( int i = 0; i < vertices.size(); ++i ) {
		pSt->set_uv( uvs[i] );
		pSt->add_vertex( vertices[i] );
	}

	pSt->index();
	pSt->generate_normals();
	pSt->generate_tangents();
	pSt->optimize_indices_for_cache();
	pResult = pSt->commit();
	return pResult;
}

/**
 * Generates an ArrayOccluder3D directly. Unlike bake_mesh(), this path does not calculate normals,
 * tangents, or create an intermediate ArrayMesh that the editor immediately throws away.
 */
Ref<Mesh> T3DTerrain::bakeRegionLightmapMesh(
	const Vector2i &rRegionLocation, const int nLod,
	const int nContextRadiusRegions, const real_t fTexelSize
) const {
	return bakeLightmapClusterMesh( rRegionLocation, rRegionLocation, nLod,
		nContextRadiusRegions, fTexelSize );
}

Ref<Mesh> T3DTerrain::bakeLightmapClusterMesh(
	const Vector2i &rRegionMin, const Vector2i &rRegionMax,
	const int nLod, const int nContextRadiusRegions, const real_t fTexelSize
) const {
	Ref<Mesh> pResult;
	ERR_FAIL_COND_V( m_pData == nullptr, pResult );
	ERR_FAIL_COND_V( fTexelSize <= 0.0f, pResult );

	const int lod = CLAMP( nLod, 0, 8 );
	const int context = CLAMP( nContextRadiusRegions, 0, 4 );
	const int nRegionSize = m_pData->getRegionSizeInternal();
	ERR_FAIL_COND_V( nRegionSize <= 0, pResult );
	const Vector2i ownerMin( MIN( rRegionMin.x, rRegionMax.x ), MIN( rRegionMin.y, rRegionMax.y ) );
	const Vector2i ownerMax( MAX( rRegionMin.x, rRegionMax.x ), MAX( rRegionMin.y, rRegionMax.y ) );
	const Vector2i min_region = ownerMin - Vector2i( context, context );
	const Vector2i max_region = ownerMax + Vector2i( context, context );
	const real_t fMinX = ( real_t ) ( min_region.x * nRegionSize ) * m_fVertexSpacing;
	const real_t fMinZ = ( real_t ) ( min_region.y * nRegionSize ) * m_fVertexSpacing;
	const real_t fSpanX = ( real_t ) ( ( max_region.x - min_region.x + 1 ) * nRegionSize ) * m_fVertexSpacing;
	const real_t fSpanZ = ( real_t ) ( ( max_region.y - min_region.y + 1 ) * nRegionSize ) * m_fVertexSpacing;
	const AABB bounds( Vector3( fMinX, -1000000.0f, fMinZ ), Vector3( fSpanX, 2000000.0f, fSpanZ ) );

	PackedVector3Array vertices;
	PackedVector2Array terrain_uvs;
	generateTriangles( vertices, &terrain_uvs, lod, T3DData::HEIGHT_FILTER_NEAREST, false, bounds );
	ERR_FAIL_COND_V( vertices.is_empty() || vertices.size() != terrain_uvs.size(), pResult );

	Ref<SurfaceTool> pSt;
	pSt.instantiate();
	pSt->begin( Mesh::PRIMITIVE_TRIANGLES );
	for ( int i = 0; i < vertices.size(); ++i ) {
		const Vector3 vertex = vertices[i];
		pSt->set_uv( terrain_uvs[i] );
		pSt->set_uv2( Vector2( ( vertex.x - fMinX ) / fSpanX, ( vertex.z - fMinZ ) / fSpanZ ) );
		pSt->add_vertex( vertex );
	}
	pSt->index();
	pSt->generate_normals();
	pSt->generate_tangents();
	pSt->optimize_indices_for_cache();
	pResult = pSt->commit();
	if ( pResult.is_valid() ) {
		const int lightmap_width = CLAMP( ( int ) ( Math::ceil( fSpanX / fTexelSize ) ), 16, 16384 );
		const int lightmap_height = CLAMP( ( int ) ( Math::ceil( fSpanZ / fTexelSize ) ), 16, 16384 );
		pResult->set_lightmap_size_hint( Vector2i( lightmap_width, lightmap_height ) );
	}
	return pResult;
}

Ref<ArrayOccluder3D> T3DTerrain::bakeOccluder( const int nLod ) const {
	LOG( T3DTerrain::INFO, "Baking occluder at lod: ", nLod );
	Ref<ArrayOccluder3D> pResult;
	ERR_FAIL_COND_V( m_pData == nullptr, pResult );

	PackedVector3Array vertices;
	generateTriangles( vertices, nullptr, nLod, T3DData::HEIGHT_FILTER_MINIMUM, false, AABB() );
	ERR_FAIL_COND_V( vertices.is_empty(), pResult );

	Ref<SurfaceTool> pSt;
	pSt.instantiate();
	pSt->begin( Mesh::PRIMITIVE_TRIANGLES );
	for ( int i = 0; i < vertices.size(); ++i ) {
		pSt->add_vertex( vertices[i] );
	}
	pSt->index();
	pSt->optimize_indices_for_cache();

	const Array arrays = pSt->commit_to_arrays();
	ERR_FAIL_COND_V( arrays.size() <= Mesh::ARRAY_INDEX, pResult );
	const PackedVector3Array indexed_vertices = arrays[Mesh::ARRAY_VERTEX];
	const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
	ERR_FAIL_COND_V( indexed_vertices.is_empty() || indices.is_empty(), pResult );

	pResult.instantiate();
	pResult->set_arrays( indexed_vertices, indices );
	return pResult;
}

/**
 * Generates source geometry faces for input to nav mesh baking. Geometry is only generated where there
 * are no holes and the terrain has been painted as navigable.
 * p_global_aabb: If non-empty, geometry will be generated only within this AABB. If empty, geometry
 *  will be generated for the entire terrain.
 * p_require_nav: If true, this function will only generate geometry for terrain marked navigable.
 *  Otherwise, geometry is generated for the entire terrain within the AABB (which can be useful for
 *  dynamic and/or runtime nav mesh baking).
 */
PackedVector3Array T3DTerrain::generateNavMeshSourceGeometry(
	const AABB &rGlobalAabb,
	const bool bRequireNav
) const {
	LOG( T3DTerrain::INFO, "Generating NavMesh source geometry from terrain" );
	PackedVector3Array faces;
	generateTriangles( faces, nullptr, 0, T3DData::HEIGHT_FILTER_NEAREST, bRequireNav, rGlobalAabb );
	return faces;
}

void T3DTerrain::setWarning( const uint8_t nWarning, const bool bEnabled ) {
	if ( bEnabled ) {
		m_nWarnings |= nWarning;
	} else {
		m_nWarnings &= ~nWarning;
	}
	update_configuration_warnings();
}

PackedStringArray T3DTerrain::_get_configuration_warnings( void ) const {
	PackedStringArray psa;
	if ( m_dataDirectory.is_empty() ) {
		psa.push_back( "No data directory specified. Select a directory then save the scene to write data." );
	}
	if ( m_nWarnings & WARN_MISMATCHED_SIZE ) {
		psa.push_back( "Texture dimensions don't match. Double-click a texture in the FileSystem panel to see its size. "
					  "Read Texture Prep in docs." );
	}
	if ( m_nWarnings & WARN_MISMATCHED_FORMAT ) {
		psa.push_back( "Texture formats don't match. Double-click a texture in the FileSystem panel to see its format. "
					  "Check Import panel. Read Texture Prep in docs." );
	}
	if ( m_nWarnings & WARN_MISMATCHED_MIPMAPS ) {
		psa.push_back( "Texture mipmap settings don't match. Change on the Import panel." );
	}
	return psa;
}

///////////////////////////
// Protected Functions
///////////////////////////

// Notifications are defined in individual classes: Object, Node, Node3D
// Listed below in order of operation
void T3DTerrain::_notification( const int nWhat ) {
	switch ( nWhat ) {
			/// Startup notifications

		case NOTIFICATION_POSTINITIALIZE: {
			// Object initialized, before script is attached
			LOG( T3DTerrain::INFO, "NOTIFICATION_POSTINITIALIZE" );
			buildContainers();
			break;
		}

		case NOTIFICATION_ENTER_WORLD: {
			// Node3D registered to new World3D resource
			// Sent on scene changes
			LOG( T3DTerrain::INFO, "NOTIFICATION_ENTER_WORLD" );
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
			LOG( T3DTerrain::INFO, "NOTIFICATION_ENTER_TREE" );
			set_as_top_level( true ); // Don't inherit transforms from parent. Global only.
			set_notify_transform( true );
			set_meta( "_edit_lock_", true );
			setupMousePicking();
			setupDisplacementBuffer();
			// Reload editor textures - Also see READY
			if ( m_bFreeEditorTextures && !IS_EDITOR && m_pAssets.is_valid() &&
				!m_pAssets->get_path().is_empty() && !m_pAssets->get_path().contains( "Terrain3DAssets" ) ) {
				LOG( T3DTerrain::INFO, "free_editor_textures enabled, reloading Assets path: ", m_pAssets->get_path() );
				m_pAssets =
					ResourceLoader::get_singleton()->load( m_pAssets->get_path(), "", ResourceLoader::CACHE_MODE_IGNORE );
			}
			initializeInternal(); // Rebuild anything freed: meshes, collision, instancer
			set_physics_process( true );
			break;
		}

		case NOTIFICATION_READY: {
			// Node is ready
			LOG( T3DTerrain::INFO, "NOTIFICATION_READY" );
			// Optional: Run the testing suite
			// #include "core/unit_testing.h"
			// test_differs();

			// Clear editor textures - also see ENTER_TREE
			if ( m_bFreeEditorTextures && !IS_EDITOR && m_pAssets.is_valid() ) {
				if ( m_pAssets->get_path().contains( "Terrain3DAssets" ) ) {
					LOG( T3DTerrain::WARN, "free_editor_textures requires `Assets` be saved to a file. Do so, or disable the former "
							  "to turn off this warning" );
				} else {
					LOG( T3DTerrain::INFO, "free_editor_textures enabled, clearing texture assets" );
					m_pAssets->clearTextures();
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
			LOG( T3DTerrain::INFO, "NOTIFICATION_VISIBILITY_CHANGED" );
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
					m_pInstancer->updateMmis( -1, V2I_MAX, true );
				}
			}
			break;
		}

		case NOTIFICATION_EXTENSION_RELOADED: {
			// Object finished hot reloading
			LOG( T3DTerrain::INFO, "NOTIFICATION_EXTENSION_RELOADED" );
			break;
		}

		case NOTIFICATION_EDITOR_PRE_SAVE: {
			// Editor Node is about to save the current scene
			LOG( T3DTerrain::INFO, "NOTIFICATION_EDITOR_PRE_SAVE" );
			if ( m_dataDirectory.is_empty() ) {
				LOG( T3DTerrain::ERROR, "Data directory is empty. Set it to save regions to disk." );
			} else if ( !m_pData ) {
				LOG( T3DTerrain::DEBUG, "Save requested, but no valid data object. Skipping" );
			} else {
				m_pData->saveDirectory( m_dataDirectory );
			}
			if ( !m_pMaterial.is_valid() ) {
				LOG( T3DTerrain::DEBUG, "Save requested, but no valid material. Skipping" );
			} else {
				m_pMaterial->save();
			}
			if ( !m_pAssets.is_valid() ) {
				LOG( T3DTerrain::DEBUG, "Save requested, but no valid texture list. Skipping" );
			} else {
				m_pAssets->save();
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
			LOG( T3DTerrain::INFO, "NOTIFICATION_CRASH" );
			break;
		}

			/// Shut down notifications

		case NOTIFICATION_EXIT_TREE: {
			// Node is about to exit a SceneTree
			// Sent on scene changes
			LOG( T3DTerrain::INFO, "NOTIFICATION_EXIT_TREE" );
			deactivateRuntime();
			destroyMousePicking();
			destroyDisplacementBuffer();
			break;
		}

		case NOTIFICATION_EXIT_WORLD: {
			// Node3D unregistered from current World3D
			// Sent on scene changes
			LOG( T3DTerrain::INFO, "NOTIFICATION_EXIT_WORLD" );
			// Rendering/physics RIDs and process-static terrain storage belong to the
			// active World3D. Preserve the loaded repository, but release every runtime
			// owner so another scene can activate before this node is destroyed.
			deactivateRuntime();
			m_bIsInsideWorld = false;
			break;
		}

		case NOTIFICATION_PREDELETE: {
			// Object is about to be deleted
			LOG( T3DTerrain::INFO, "NOTIFICATION_PREDELETE" );
			disconnectAutoShaderProfileSignal( m_pAutoShaderProfile );
			deactivateRuntime();
			destroyTerrainMesher( true );
			destroyOceanMesher( true );
			destroyHlodStreamer();
		destroyLightmapStreamer();
		destroyNodeStreamer( false );
			destroyInstancer();
			destroyCollision( true );
			m_pAssets.unref();
			m_pMaterial.unref();
			if ( m_pData ) {
				memdelete( m_pData );
				m_pData = nullptr;
			}
			destroyLabels();
			destroyContainers();
			break;
		}

		default:
			break;
	}
}

void T3DTerrain::_validate_property( PropertyInfo &rProperty ) const {
	if ( m_tessellationLevel == 0 ) {
		// Hide all displacement properties
		if ( rProperty.name == StringName( "displacement_scale" ) ||
			rProperty.name == StringName( "displacement_sharpness" ) ||
			rProperty.name == StringName( "buffer_shader_override_enabled" ) ||
			rProperty.name == StringName( "buffer_shader_override" ) ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}
	// Hide all ocean properties if not enabled
	if ( !m_bOceanEnabled && rProperty.name != StringName( "ocean_enabled" ) && rProperty.name.begins_with( "ocean_" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if ( ( m_bGpuRegionCacheBudgetAuto && rProperty.name == StringName( "gpu_region_cache_budget_mb" ) ) ||
		( m_bCpuRegionCacheBudgetAuto && rProperty.name == StringName( "cpu_region_cache_budget_mb" ) ) ||
		( m_bInstanceRegionCacheBudgetAuto && rProperty.name == StringName( "instance_region_cache_budget_mb" ) ) ||
		( m_bLightmapResidencyBudgetAuto && rProperty.name == StringName( "lightmap_residency_budget_mb" ) ) ||
		( m_bHlodMemoryBudgetAuto && rProperty.name == StringName( "hlod_memory_budget_mb" ) ) ) {
		rProperty.usage |= PROPERTY_USAGE_READ_ONLY;
	}
}

void T3DTerrain::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( ENVIRONMENT_SHADOW_LOW );
	BIND_ENUM_CONSTANT( ENVIRONMENT_SHADOW_NORMAL );
	BIND_ENUM_CONSTANT( ENVIRONMENT_SHADOW_HIGH );
	BIND_ENUM_CONSTANT( ENVIRONMENT_SHADOW_ULTRA );
	BIND_ENUM_CONSTANT( ERROR );
	BIND_ENUM_CONSTANT( INFO );
	BIND_ENUM_CONSTANT( DEBUG );
	BIND_ENUM_CONSTANT( EXTREME );

	BIND_ENUM_CONSTANT( SIZE_64 );
	BIND_ENUM_CONSTANT( SIZE_128 );
	BIND_ENUM_CONSTANT( SIZE_256 );
	BIND_ENUM_CONSTANT( SIZE_512 );
	BIND_ENUM_CONSTANT( SIZE_1024 );
	BIND_ENUM_CONSTANT( SIZE_2048 );
	BIND_ENUM_CONSTANT( PERFORMANCE_TARGET_CUSTOM );
	BIND_ENUM_CONSTANT( PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL );
	BIND_ENUM_CONSTANT( PERFORMANCE_PROFILE_CUSTOM );
	BIND_ENUM_CONSTANT( PERFORMANCE_PROFILE_EXACT );
	BIND_ENUM_CONSTANT( PERFORMANCE_PROFILE_MODIFIED );
	BIND_ENUM_CONSTANT( EDITOR_EXECUTION_AUTHORING );
	BIND_ENUM_CONSTANT( EDITOR_EXECUTION_RUNTIME_PROFILE );
	BIND_ENUM_CONSTANT( OCEAN_PRESET_CUSTOM );
	BIND_ENUM_CONSTANT( OCEAN_PRESET_BORDER_MINIMUM );
	BIND_ENUM_CONSTANT( OCEAN_PRESET_BORDER_BALANCED );
	BIND_ENUM_CONSTANT( OCEAN_PRESET_HIGH );
	BIND_ENUM_CONSTANT( OCEAN_PRESET_CINEMATIC );
	BIND_ENUM_CONSTANT( OCEAN_REFRACTION_OFF );
	BIND_ENUM_CONSTANT( OCEAN_REFRACTION_DEPTH_TINT );
	BIND_ENUM_CONSTANT( OCEAN_REFRACTION_FULL );
	BIND_ENUM_CONSTANT( OCEAN_FOAM_OFF );
	BIND_ENUM_CONSTANT( OCEAN_FOAM_SHORE );
	BIND_ENUM_CONSTANT( OCEAN_FOAM_FULL );
	BIND_ENUM_CONSTANT( OCEAN_SCATTERING_OFF );
	BIND_ENUM_CONSTANT( OCEAN_SCATTERING_APPROXIMATE );
	BIND_ENUM_CONSTANT( OCEAN_SCATTERING_FULL );
	BIND_ENUM_CONSTANT( OCEAN_SHADING_LAMBERT_DIFFUSE );
	BIND_ENUM_CONSTANT( OCEAN_SHADING_LAMBERT_GGX );
	BIND_ENUM_CONSTANT( OCEAN_SHADING_BURLEY_GGX );
	BIND_ENUM_CONSTANT( OCEAN_SHADING_UNSHADED );
	BIND_ENUM_CONSTANT( OCEAN_SHADING_LAMBERT_VERTEX );

	ClassDB::bind_method( D_METHOD( "get_version" ), &T3DTerrain::getVersion );
	ClassDB::bind_method( D_METHOD( "get_native_api_revision" ), &T3DTerrain::getNativeApiRevision );
	ClassDB::bind_method( D_METHOD( "get_native_build_id" ), &T3DTerrain::getNativeBuildId );
	ClassDB::bind_method( D_METHOD( "is_initialized" ), &T3DTerrain::isInitialized );
	ClassDB::bind_method( D_METHOD( "set_debug_level", "level" ), &T3DTerrain::setDebugLevel );
	ClassDB::bind_method( D_METHOD( "get_debug_level" ), &T3DTerrain::getDebugLevel );
	ClassDB::bind_method( D_METHOD( "set_data_directory", "directory" ), &T3DTerrain::setDataDirectory );
	ClassDB::bind_method( D_METHOD( "get_data_directory" ), &T3DTerrain::getDataDirectory );
	ClassDB::bind_method( D_METHOD( "set_editor_runtime_streaming_preview", "enabled" ),
		&T3DTerrain::setEditorRuntimeStreamingPreview );
	ClassDB::bind_method( D_METHOD( "is_editor_runtime_streaming_preview" ),
		&T3DTerrain::isEditorRuntimeStreamingPreview );
	ClassDB::bind_method( D_METHOD( "set_editor_node_streaming_preview", "enabled" ),
		&T3DTerrain::setEditorNodeStreamingPreview );
	ClassDB::bind_method( D_METHOD( "is_editor_node_streaming_preview_enabled" ),
		&T3DTerrain::isEditorNodeStreamingPreviewEnabled );
	ClassDB::bind_method( D_METHOD( "is_editor_node_streaming_preview_active" ),
		&T3DTerrain::isEditorNodeStreamingPreviewActive );
	ClassDB::bind_method( D_METHOD( "set_editor_hlod_preview", "enabled" ), &T3DTerrain::setEditorHlodPreview );
	ClassDB::bind_method( D_METHOD( "is_editor_hlod_preview_enabled" ), &T3DTerrain::isEditorHlodPreviewEnabled );
	ClassDB::bind_method( D_METHOD( "is_editor_hlod_preview_active" ), &T3DTerrain::isEditorHlodPreviewActive );
	ClassDB::bind_method( D_METHOD( "set_editor_execution_mode", "mode" ), &T3DTerrain::setEditorExecutionMode );
	ClassDB::bind_method( D_METHOD( "get_editor_execution_mode" ), &T3DTerrain::getEditorExecutionMode );
	ClassDB::bind_method( D_METHOD( "get_memory_estimate" ), &T3DTerrain::getMemoryEstimate );
	ClassDB::bind_method( D_METHOD( "get_tracked_current_residency" ), &T3DTerrain::getTrackedCurrentResidency );
	ClassDB::bind_method( D_METHOD( "get_memory_budget_stats" ), &T3DTerrain::getMemoryBudgetStats );
	ClassDB::bind_method( D_METHOD( "set_total_memory_budget_mb", "budget_mb" ), &T3DTerrain::setTotalMemoryBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_total_memory_budget_mb" ), &T3DTerrain::getTotalMemoryBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_memory_transient_reserve_mb", "reserve_mb" ), &T3DTerrain::setMemoryTransientReserveMb );
	ClassDB::bind_method( D_METHOD( "get_memory_transient_reserve_mb" ), &T3DTerrain::getMemoryTransientReserveMb );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_cache_budget_auto", "enabled" ), &T3DTerrain::setGpuRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cache_budget_auto" ), &T3DTerrain::isGpuRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "set_cpu_region_cache_budget_auto", "enabled" ), &T3DTerrain::setCpuRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_cpu_region_cache_budget_auto" ), &T3DTerrain::isCpuRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "set_instance_region_cache_budget_auto", "enabled" ), &T3DTerrain::setInstanceRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_instance_region_cache_budget_auto" ), &T3DTerrain::isInstanceRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "set_lightmap_residency_budget_auto", "enabled" ), &T3DTerrain::setLightmapResidencyBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_lightmap_residency_budget_auto" ), &T3DTerrain::isLightmapResidencyBudgetAuto );
	ClassDB::bind_method( D_METHOD( "set_hlod_memory_budget_auto", "enabled" ), &T3DTerrain::setHlodMemoryBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_hlod_memory_budget_auto" ), &T3DTerrain::isHlodMemoryBudgetAuto );
	ClassDB::bind_method( D_METHOD( "set_lightmap_residency_budget_mb", "budget_mb" ), &T3DTerrain::setLightmapResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_lightmap_residency_budget_mb" ), &T3DTerrain::getLightmapResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_shader_cache_budget_auto", "enabled" ), &T3DTerrain::setShaderCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_shader_cache_budget_auto" ), &T3DTerrain::isShaderCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "get_effective_gpu_region_cache_budget_mb" ), &T3DTerrain::getEffectiveGpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_cpu_region_cache_budget_mb" ), &T3DTerrain::getEffectiveCpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_instance_region_cache_budget_mb" ), &T3DTerrain::getEffectiveInstanceRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_lightmap_residency_budget_mb" ), &T3DTerrain::getEffectiveLightmapResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_hlod_memory_budget_mb" ), &T3DTerrain::getEffectiveHlodMemoryBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_shader_cache_budget_mb" ), &T3DTerrain::getEffectiveShaderCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_memory_budget_policy_generation" ), &T3DTerrain::getMemoryBudgetPolicyGeneration );
	ClassDB::bind_method( D_METHOD( "is_memory_budget_overcommitted" ), &T3DTerrain::isMemoryBudgetOvercommitted );
	ClassDB::bind_method( D_METHOD( "get_memory_budget_headroom_bytes" ), &T3DTerrain::getMemoryBudgetHeadroomBytes );
	ClassDB::bind_method( D_METHOD( "force_memory_budget_rebalance" ), &T3DTerrain::forceMemoryBudgetRebalance );
	ClassDB::bind_method( D_METHOD( "get_runtime_diagnostics", "deep_introspection" ), &T3DTerrain::getRuntimeDiagnostics, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_runtime_parity_snapshot" ), &T3DTerrain::getRuntimeParitySnapshot );
	ClassDB::bind_method( D_METHOD( "get_memory_stats_snapshot" ), &T3DTerrain::getMemoryStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_shader_tier_stats_snapshot" ), &T3DTerrain::getShaderTierStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_cache_stats_snapshot", "deep_introspection" ), &T3DTerrain::getCacheStatsSnapshot, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_frame_stats_snapshot" ), &T3DTerrain::getFrameStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_runtime_profile_history", "max_samples" ),
		&T3DTerrain::getRuntimeProfileHistory, DEFVAL( 600 ) );
	ClassDB::bind_method( D_METHOD( "get_runtime_profile_history_since", "after_sequence", "max_samples" ),
		&T3DTerrain::getRuntimeProfileHistorySince, DEFVAL( 600 ) );
	ClassDB::bind_method( D_METHOD( "reset_runtime_diagnostics" ), &T3DTerrain::resetRuntimeDiagnostics );
	ClassDB::bind_method( D_METHOD( "set_runtime_profiling_enabled", "enabled" ),
		&T3DTerrain::setRuntimeProfilingEnabled );
	ClassDB::bind_method( D_METHOD( "is_runtime_profiling_enabled" ),
		&T3DTerrain::isRuntimeProfilingEnabled );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_work_budgets_enabled", "enabled" ),
		&T3DTerrain::setRuntimeAdaptiveWorkBudgetsEnabled );
	ClassDB::bind_method( D_METHOD( "is_runtime_adaptive_work_budgets_enabled" ),
		&T3DTerrain::isRuntimeAdaptiveWorkBudgetsEnabled );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_target_ms", "target_ms" ),
		&T3DTerrain::setRuntimeAdaptiveTargetMs );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_target_ms" ),
		&T3DTerrain::getRuntimeAdaptiveTargetMs );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_hard_max_ms", "hard_max_ms" ),
		&T3DTerrain::setRuntimeAdaptiveHardMaxMs );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_hard_max_ms" ),
		&T3DTerrain::getRuntimeAdaptiveHardMaxMs );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_adjustment_window_frames", "frames" ),
		&T3DTerrain::setRuntimeAdaptiveAdjustmentWindowFrames );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_adjustment_window_frames" ),
		&T3DTerrain::getRuntimeAdaptiveAdjustmentWindowFrames );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_gpu_pages", "pages" ),
		&T3DTerrain::setRuntimeAdaptiveMinGpuPages );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_min_gpu_pages" ),
		&T3DTerrain::getRuntimeAdaptiveMinGpuPages );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_cpu_pages", "pages" ),
		&T3DTerrain::setRuntimeAdaptiveMinCpuPages );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_min_cpu_pages" ),
		&T3DTerrain::getRuntimeAdaptiveMinCpuPages );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_instance_pages", "pages" ),
		&T3DTerrain::setRuntimeAdaptiveMinInstancePages );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_min_instance_pages" ),
		&T3DTerrain::getRuntimeAdaptiveMinInstancePages );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_instancer_cells", "cells" ),
		&T3DTerrain::setRuntimeAdaptiveMinInstancerCells );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_min_instancer_cells" ),
		&T3DTerrain::getRuntimeAdaptiveMinInstancerCells );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_node_load_regions", "regions" ),
		&T3DTerrain::setRuntimeAdaptiveMinNodeLoadRegions );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_min_node_load_regions" ),
		&T3DTerrain::getRuntimeAdaptiveMinNodeLoadRegions );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_node_unload_regions", "regions" ),
		&T3DTerrain::setRuntimeAdaptiveMinNodeUnloadRegions );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_min_node_unload_regions" ),
		&T3DTerrain::getRuntimeAdaptiveMinNodeUnloadRegions );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_budget_stats" ),
		&T3DTerrain::getRuntimeAdaptiveBudgetStats );
	ClassDB::bind_method( D_METHOD( "set_runtime_boundary_work_phasing_enabled", "enabled" ),
		&T3DTerrain::setRuntimeBoundaryWorkPhasingEnabled );
	ClassDB::bind_method( D_METHOD( "is_runtime_boundary_work_phasing_enabled" ),
		&T3DTerrain::isRuntimeBoundaryWorkPhasingEnabled );
	ClassDB::bind_method( D_METHOD( "set_runtime_boundary_node_delay_frames", "frames" ),
		&T3DTerrain::setRuntimeBoundaryNodeDelayFrames );
	ClassDB::bind_method( D_METHOD( "get_runtime_boundary_node_delay_frames" ),
		&T3DTerrain::getRuntimeBoundaryNodeDelayFrames );
	ClassDB::bind_method( D_METHOD( "set_runtime_boundary_instancer_delay_frames", "frames" ),
		&T3DTerrain::setRuntimeBoundaryInstancerDelayFrames );
	ClassDB::bind_method( D_METHOD( "get_runtime_boundary_instancer_delay_frames" ),
		&T3DTerrain::getRuntimeBoundaryInstancerDelayFrames );
	ClassDB::bind_method( D_METHOD( "get_runtime_boundary_phasing_stats" ),
		&T3DTerrain::getRuntimeBoundaryPhasingStats );
	ClassDB::bind_method( D_METHOD( "is_editor_authoring_mode" ), &T3DTerrain::isEditorAuthoringMode );
	ClassDB::bind_method( D_METHOD( "is_authoring_residency_readback_safe" ),
		&T3DTerrain::isAuthoringResidencyReadbackSafe );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cache_effective" ), &T3DTerrain::isGpuRegionCacheEffective );
	ClassDB::bind_method( D_METHOD( "is_cpu_region_cache_effective" ), &T3DTerrain::isCpuRegionCacheEffective );
	ClassDB::bind_method( D_METHOD( "is_instance_region_cache_effective" ),
		&T3DTerrain::isInstanceRegionCacheEffective );
	ClassDB::bind_method( D_METHOD( "is_instancer_cell_streaming_effective" ),
		&T3DTerrain::isInstancerCellStreamingEffective );
	ClassDB::bind_method( D_METHOD( "is_dynamic_node_streaming_effective" ),
		&T3DTerrain::isDynamicNodeStreamingEffective );
	ClassDB::bind_method( D_METHOD( "set_performance_target", "target" ), &T3DTerrain::setPerformanceTarget );
	ClassDB::bind_method( D_METHOD( "get_performance_target" ), &T3DTerrain::getPerformanceTarget );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_status" ), &T3DTerrain::getPerformanceProfileStatus );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_status_name" ), &T3DTerrain::getPerformanceProfileStatusName );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_version" ), &T3DTerrain::getPerformanceProfileVersion );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_signature" ), &T3DTerrain::getPerformanceProfileSignature );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_manifest" ), &T3DTerrain::getPerformanceProfileManifest );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_diff" ), &T3DTerrain::getPerformanceProfileDiff );
	ClassDB::bind_method( D_METHOD( "reset_performance_profile" ), &T3DTerrain::resetPerformanceProfile );
	ClassDB::bind_method( D_METHOD( "set_streaming_active_radius", "radius" ), &T3DTerrain::setStreamingActiveRadius );
	ClassDB::bind_method( D_METHOD( "get_streaming_active_radius" ), &T3DTerrain::getStreamingActiveRadius );
	ClassDB::bind_method( D_METHOD( "set_streaming_hot_radius", "radius" ), &T3DTerrain::setStreamingHotRadius );
	ClassDB::bind_method( D_METHOD( "get_streaming_hot_radius" ), &T3DTerrain::getStreamingHotRadius );
	ClassDB::bind_method( D_METHOD( "set_streaming_cold_radius", "radius" ), &T3DTerrain::setStreamingColdRadius );
	ClassDB::bind_method( D_METHOD( "get_streaming_cold_radius" ), &T3DTerrain::getStreamingColdRadius );
	ClassDB::bind_method( D_METHOD( "set_streaming_unload_guard", "regions" ), &T3DTerrain::setStreamingUnloadGuard );
	ClassDB::bind_method( D_METHOD( "get_streaming_unload_guard" ), &T3DTerrain::getStreamingUnloadGuard );
	ClassDB::bind_method( D_METHOD( "set_streaming_prediction_enabled", "enabled" ), &T3DTerrain::setStreamingPredictionEnabled );
	ClassDB::bind_method( D_METHOD( "is_streaming_prediction_enabled" ), &T3DTerrain::isStreamingPredictionEnabled );
	ClassDB::bind_method( D_METHOD( "set_streaming_prediction_lookahead", "seconds" ), &T3DTerrain::setStreamingPredictionLookahead );
	ClassDB::bind_method( D_METHOD( "get_streaming_prediction_lookahead" ), &T3DTerrain::getStreamingPredictionLookahead );
	ClassDB::bind_method( D_METHOD( "set_streaming_prediction_maximum_regions", "regions" ), &T3DTerrain::setStreamingPredictionMaximumRegions );
	ClassDB::bind_method( D_METHOD( "get_streaming_prediction_maximum_regions" ), &T3DTerrain::getStreamingPredictionMaximumRegions );
	ClassDB::bind_method( D_METHOD( "get_region_streaming_state", "region" ), &T3DTerrain::getRegionStreamingState );
	ClassDB::bind_method( D_METHOD( "is_region_streaming_prefetch", "region" ), &T3DTerrain::isRegionStreamingPrefetch );
	ClassDB::bind_method( D_METHOD( "get_region_streaming_stats" ), &T3DTerrain::getRegionStreamingStats );
	ClassDB::bind_method( D_METHOD( "get_region_streaming_configuration_status" ), &T3DTerrain::getRegionStreamingConfigurationStatus );
	ClassDB::bind_method( D_METHOD( "get_height_source_diagnostics", "world_position" ), &T3DTerrain::getHeightSourceDiagnostics );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_cache_enabled", "enabled" ),
		&T3DTerrain::setGpuRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cache_enabled" ), &T3DTerrain::isGpuRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_cache_budget_mb", "budget_mb" ),
		&T3DTerrain::setGpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_budget_mb" ), &T3DTerrain::getGpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_upload_budget_layers", "layers" ),
		&T3DTerrain::setGpuRegionUploadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_upload_budget_layers" ),
		&T3DTerrain::getGpuRegionUploadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_upload_budget_mb", "budget_mb" ),
		&T3DTerrain::setGpuRegionUploadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_upload_budget_mb" ), &T3DTerrain::getGpuRegionUploadBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_spare_pages", "pages" ), &T3DTerrain::setGpuRegionSparePages );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_spare_pages" ), &T3DTerrain::getGpuRegionSparePages );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_retire_frames", "frames" ), &T3DTerrain::setGpuRegionRetireFrames );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_retire_frames" ), &T3DTerrain::getGpuRegionRetireFrames );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_detail_lods", "lods" ), &T3DTerrain::setGpuRegionDetailLods );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_detail_lods" ), &T3DTerrain::getGpuRegionDetailLods );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_detail_range_end_m", "range_m" ),
		&T3DTerrain::setGpuRegionDetailRangeEndM );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_detail_range_end_m" ),
		&T3DTerrain::getGpuRegionDetailRangeEndM );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_max_supported_detail_lods" ),
		&T3DTerrain::getGpuRegionMaxSupportedDetailLods );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_max_supported_detail_range_m" ),
		&T3DTerrain::getGpuRegionMaxSupportedDetailRangeM );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_required_core_pages" ),
		&T3DTerrain::getGpuRegionRequiredCorePages );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_mapped_capacity" ),
		&T3DTerrain::getGpuRegionMappedCapacity );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_detail_range_valid" ),
		&T3DTerrain::isGpuRegionDetailRangeValid );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_guard_regions", "regions" ), &T3DTerrain::setGpuRegionGuardRegions );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_guard_regions" ), &T3DTerrain::getGpuRegionGuardRegions );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_prefetch_rows", "rows" ), &T3DTerrain::setGpuRegionPrefetchRows );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_prefetch_rows" ), &T3DTerrain::getGpuRegionPrefetchRows );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_hysteresis_ticks", "ticks" ), &T3DTerrain::setGpuRegionHysteresisTicks );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_hysteresis_ticks" ), &T3DTerrain::getGpuRegionHysteresisTicks );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_color_resolution_divisor", "divisor" ),
		&T3DTerrain::setGpuRegionColorResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_color_resolution_divisor" ),
		&T3DTerrain::getGpuRegionColorResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_derived_resolution_divisor", "divisor" ),
		&T3DTerrain::setGpuRegionDerivedResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_derived_resolution_divisor" ),
		&T3DTerrain::getGpuRegionDerivedResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "set_gpu_middle_height_resolution_divisor", "divisor" ),
		&T3DTerrain::setGpuMiddleHeightResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "get_gpu_middle_height_resolution_divisor" ),
		&T3DTerrain::getGpuMiddleHeightResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "set_editor_gpu_region_cache_pages", "pages" ),
		&T3DTerrain::setEditorGpuRegionCachePages );
	ClassDB::bind_method( D_METHOD( "get_editor_gpu_region_cache_pages" ),
		&T3DTerrain::getEditorGpuRegionCachePages );
	ClassDB::bind_method( D_METHOD( "set_runtime_material_streaming_enabled", "enabled" ),
		&T3DTerrain::setRuntimeMaterialStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "is_runtime_material_streaming_enabled" ),
		&T3DTerrain::isRuntimeMaterialStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "set_runtime_material_cache_layers", "layers" ),
		&T3DTerrain::setRuntimeMaterialCacheLayers );
	ClassDB::bind_method( D_METHOD( "get_runtime_material_cache_layers" ),
		&T3DTerrain::getRuntimeMaterialCacheLayers );
	ClassDB::bind_method( D_METHOD( "set_runtime_material_upload_budget_layers", "layers" ),
		&T3DTerrain::setRuntimeMaterialUploadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "get_runtime_material_upload_budget_layers" ),
		&T3DTerrain::getRuntimeMaterialUploadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "set_runtime_release_source_material_textures", "enabled" ),
		&T3DTerrain::setRuntimeReleaseSourceMaterialTextures );
	ClassDB::bind_method( D_METHOD( "is_runtime_release_source_material_textures" ),
		&T3DTerrain::isRuntimeReleaseSourceMaterialTextures );
	ClassDB::bind_method( D_METHOD( "set_gpu_far_quality_divisor", "divisor" ),
		&T3DTerrain::setGpuFarQualityDivisor );
	ClassDB::bind_method( D_METHOD( "get_gpu_far_quality_divisor" ),
		&T3DTerrain::getGpuFarQualityDivisor );
	ClassDB::bind_method( D_METHOD( "set_far_material_resolution", "resolution" ),
		&T3DTerrain::setFarMaterialResolution );
	ClassDB::bind_method( D_METHOD( "get_far_material_resolution" ),
		&T3DTerrain::getFarMaterialResolution );
	ClassDB::bind_method( D_METHOD( "set_far_control_resolution", "resolution" ),
		&T3DTerrain::setFarControlResolution );
	ClassDB::bind_method( D_METHOD( "get_far_control_resolution" ),
		&T3DTerrain::getFarControlResolution );
	ClassDB::bind_method( D_METHOD( "get_gpu_far_atlas_side" ), &T3DTerrain::getGpuFarAtlasSide );
	ClassDB::bind_method( D_METHOD( "get_gpu_far_height_atlas_side" ), &T3DTerrain::getGpuFarHeightAtlasSide );
	ClassDB::bind_method( D_METHOD( "get_gpu_far_surface_atlas_side" ), &T3DTerrain::getGpuFarSurfaceAtlasSide );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_cooked_pages_enabled", "enabled" ),
		&T3DTerrain::setGpuRegionCookedPagesEnabled );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cooked_pages_enabled" ),
		&T3DTerrain::isGpuRegionCookedPagesEnabled );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_cook_on_save", "enabled" ),
		&T3DTerrain::setGpuRegionCookOnSave );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cook_on_save" ), &T3DTerrain::isGpuRegionCookOnSave );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_stats" ), &T3DTerrain::getGpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "set_cpu_region_cache_enabled", "enabled" ),
		&T3DTerrain::setCpuRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "is_cpu_region_cache_enabled" ), &T3DTerrain::isCpuRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "set_cpu_region_cache_budget_mb", "budget_mb" ),
		&T3DTerrain::setCpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_budget_mb" ), &T3DTerrain::getCpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_cpu_region_load_budget_layers", "layers" ),
		&T3DTerrain::setCpuRegionLoadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_load_budget_layers" ),
		&T3DTerrain::getCpuRegionLoadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "set_cpu_region_load_budget_mb", "budget_mb" ),
		&T3DTerrain::setCpuRegionLoadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_load_budget_mb" ), &T3DTerrain::getCpuRegionLoadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_stats" ), &T3DTerrain::getCpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "set_instance_region_cache_enabled", "enabled" ), &T3DTerrain::setInstanceRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "is_instance_region_cache_enabled" ), &T3DTerrain::isInstanceRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "set_instance_region_cache_budget_mb", "budget_mb" ), &T3DTerrain::setInstanceRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_instance_region_cache_budget_mb" ), &T3DTerrain::getInstanceRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_instance_region_load_budget_pages", "pages" ), &T3DTerrain::setInstanceRegionLoadBudgetPages );
	ClassDB::bind_method( D_METHOD( "get_instance_region_load_budget_pages" ), &T3DTerrain::getInstanceRegionLoadBudgetPages );
	ClassDB::bind_method( D_METHOD( "set_instance_region_load_budget_mb", "budget_mb" ), &T3DTerrain::setInstanceRegionLoadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_instance_region_load_budget_mb" ), &T3DTerrain::getInstanceRegionLoadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_instance_region_cache_stats" ), &T3DTerrain::getInstanceRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "set_instancer_cell_size", "cell_size" ), &T3DTerrain::setInstancerCellSize );
	ClassDB::bind_method( D_METHOD( "get_instancer_cell_size" ), &T3DTerrain::getInstancerCellSize );
	ClassDB::bind_method( D_METHOD( "set_instancer_cell_streaming_enabled", "enabled" ), &T3DTerrain::setInstancerCellStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "is_instancer_cell_streaming_enabled" ), &T3DTerrain::isInstancerCellStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "set_instancer_single_lod_buffers", "enabled" ), &T3DTerrain::setInstancerSingleLodBuffers );
	ClassDB::bind_method( D_METHOD( "is_instancer_single_lod_buffers" ), &T3DTerrain::isInstancerSingleLodBuffers );
	ClassDB::bind_method( D_METHOD( "set_instancer_cell_streaming_margin", "margin" ), &T3DTerrain::setInstancerCellStreamingMargin );
	ClassDB::bind_method( D_METHOD( "get_instancer_cell_streaming_margin" ), &T3DTerrain::getInstancerCellStreamingMargin );
	ClassDB::bind_method( D_METHOD( "set_instancer_cell_streaming_update_distance", "distance" ), &T3DTerrain::setInstancerCellStreamingUpdateDistance );
	ClassDB::bind_method( D_METHOD( "get_instancer_cell_streaming_update_distance" ), &T3DTerrain::getInstancerCellStreamingUpdateDistance );
	ClassDB::bind_method( D_METHOD( "set_instancer_update_cell_budget", "budget" ), &T3DTerrain::setInstancerUpdateCellBudget );
	ClassDB::bind_method( D_METHOD( "get_instancer_update_cell_budget" ), &T3DTerrain::getInstancerUpdateCellBudget );
	ClassDB::bind_method( D_METHOD( "set_dynamic_node_streaming_enabled", "enabled" ), &T3DTerrain::setDynamicNodeStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "is_dynamic_node_streaming_enabled" ), &T3DTerrain::isDynamicNodeStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "set_lightmap_streaming_enabled", "enabled" ), &T3DTerrain::setLightmapStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "is_lightmap_streaming_enabled" ), &T3DTerrain::isLightmapStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "set_lightmap_stream_radius", "radius" ), &T3DTerrain::setLightmapStreamRadius );
	ClassDB::bind_method( D_METHOD( "get_lightmap_stream_radius" ), &T3DTerrain::getLightmapStreamRadius );
	ClassDB::bind_method( D_METHOD( "set_lightmap_load_budget", "budget" ), &T3DTerrain::setLightmapLoadBudget );
	ClassDB::bind_method( D_METHOD( "get_lightmap_load_budget" ), &T3DTerrain::getLightmapLoadBudget );
	ClassDB::bind_method( D_METHOD( "set_lightmap_unload_budget", "budget" ), &T3DTerrain::setLightmapUnloadBudget );
	ClassDB::bind_method( D_METHOD( "get_lightmap_unload_budget" ), &T3DTerrain::getLightmapUnloadBudget );
	ClassDB::bind_method( D_METHOD( "get_lightmap_streaming_stats" ), &T3DTerrain::getLightmapStreamingStats );
	ClassDB::bind_method( D_METHOD( "set_region_lightmap_output_directory", "directory" ), &T3DTerrain::setRegionLightmapOutputDirectory );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_output_directory" ), &T3DTerrain::getRegionLightmapOutputDirectory );
	ClassDB::bind_method( D_METHOD( "set_region_lightmap_proxy_lod", "lod" ), &T3DTerrain::setRegionLightmapProxyLod );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_proxy_lod" ), &T3DTerrain::getRegionLightmapProxyLod );
	ClassDB::bind_method( D_METHOD( "set_region_lightmap_context_radius", "radius" ), &T3DTerrain::setRegionLightmapContextRadius );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_context_radius" ), &T3DTerrain::getRegionLightmapContextRadius );
	ClassDB::bind_method( D_METHOD( "set_region_lightmap_texel_size", "texel_size" ), &T3DTerrain::setRegionLightmapTexelSize );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_texel_size" ), &T3DTerrain::getRegionLightmapTexelSize );
	ClassDB::bind_method( D_METHOD( "set_region_lightmap_bake_quality", "quality" ), &T3DTerrain::setRegionLightmapBakeQuality );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_bake_quality" ), &T3DTerrain::getRegionLightmapBakeQuality );
	ClassDB::bind_method( D_METHOD( "set_region_lightmap_shadowmask_mode", "mode" ), &T3DTerrain::setRegionLightmapShadowmaskMode );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_shadowmask_mode" ), &T3DTerrain::getRegionLightmapShadowmaskMode );
	ClassDB::bind_method( D_METHOD( "set_hlod_enabled", "enabled" ), &T3DTerrain::setHlodEnabled );
	ClassDB::bind_method( D_METHOD( "is_hlod_enabled" ), &T3DTerrain::isHlodEnabled );
	ClassDB::bind_method( D_METHOD( "set_hlod_manifest_path", "path" ), &T3DTerrain::setHlodManifestPath );
	ClassDB::bind_method( D_METHOD( "get_hlod_manifest_path" ), &T3DTerrain::getHlodManifestPath );
	ClassDB::bind_method( D_METHOD( "set_hlod_target_error_pixels", "pixels" ), &T3DTerrain::setHlodTargetErrorPixels );
	ClassDB::bind_method( D_METHOD( "get_hlod_target_error_pixels" ), &T3DTerrain::getHlodTargetErrorPixels );
	ClassDB::bind_method( D_METHOD( "set_hlod_hysteresis", "ratio" ), &T3DTerrain::setHlodHysteresis );
	ClassDB::bind_method( D_METHOD( "get_hlod_hysteresis" ), &T3DTerrain::getHlodHysteresis );
	ClassDB::bind_method( D_METHOD( "set_hlod_max_level", "level" ), &T3DTerrain::setHlodMaxLevel );
	ClassDB::bind_method( D_METHOD( "get_hlod_max_level" ), &T3DTerrain::getHlodMaxLevel );
	ClassDB::bind_method( D_METHOD( "set_hlod_load_budget", "budget" ), &T3DTerrain::setHlodLoadBudget );
	ClassDB::bind_method( D_METHOD( "get_hlod_load_budget" ), &T3DTerrain::getHlodLoadBudget );
	ClassDB::bind_method( D_METHOD( "set_hlod_unload_budget", "budget" ), &T3DTerrain::setHlodUnloadBudget );
	ClassDB::bind_method( D_METHOD( "get_hlod_unload_budget" ), &T3DTerrain::getHlodUnloadBudget );
	ClassDB::bind_method( D_METHOD( "set_hlod_memory_budget_mb", "budget_mb" ), &T3DTerrain::setHlodMemoryBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_hlod_memory_budget_mb" ), &T3DTerrain::getHlodMemoryBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_hlod_max_shadow_level", "level" ), &T3DTerrain::setHlodMaxShadowLevel );
	ClassDB::bind_method( D_METHOD( "get_hlod_max_shadow_level" ), &T3DTerrain::getHlodMaxShadowLevel );
	ClassDB::bind_method( D_METHOD( "set_hlod_forced_debug_level", "level" ), &T3DTerrain::setHlodForcedDebugLevel );
	ClassDB::bind_method( D_METHOD( "get_hlod_forced_debug_level" ), &T3DTerrain::getHlodForcedDebugLevel );
	ClassDB::bind_method( D_METHOD( "reload_hlod_manifest" ), &T3DTerrain::reloadHlodManifest );
	ClassDB::bind_method( D_METHOD( "get_hlod_streaming_stats" ), &T3DTerrain::getHlodStreamingStats );
	ClassDB::bind_method( D_METHOD( "get_hlod_debug_entries" ), &T3DTerrain::getHlodDebugEntries );
	ClassDB::bind_method( D_METHOD( "set_dynamic_node_stream_radius", "radius" ), &T3DTerrain::setDynamicNodeStreamRadius );
	ClassDB::bind_method( D_METHOD( "get_dynamic_node_stream_radius" ), &T3DTerrain::getDynamicNodeStreamRadius );
	ClassDB::bind_method( D_METHOD( "set_dynamic_node_load_region_budget", "budget" ), &T3DTerrain::setDynamicNodeLoadRegionBudget );
	ClassDB::bind_method( D_METHOD( "get_dynamic_node_load_region_budget" ), &T3DTerrain::getDynamicNodeLoadRegionBudget );
	ClassDB::bind_method( D_METHOD( "set_dynamic_node_load_node_budget", "budget" ), &T3DTerrain::setDynamicNodeLoadNodeBudget );
	ClassDB::bind_method( D_METHOD( "get_dynamic_node_load_node_budget" ), &T3DTerrain::getDynamicNodeLoadNodeBudget );
	ClassDB::bind_method( D_METHOD( "set_dynamic_node_unload_region_budget", "budget" ), &T3DTerrain::setDynamicNodeUnloadRegionBudget );
	ClassDB::bind_method( D_METHOD( "get_dynamic_node_unload_region_budget" ), &T3DTerrain::getDynamicNodeUnloadRegionBudget );
	ClassDB::bind_method( D_METHOD( "add_streamed_scene", "scene", "global_transform", "state", "flags", "hlod_min_level" ), &T3DTerrain::addStreamedScene, DEFVAL( Dictionary() ), DEFVAL( 0 ), DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "add_streamed_scene_path", "scene_path", "global_transform", "state", "flags", "hlod_min_level" ), &T3DTerrain::addStreamedScenePath, DEFVAL( Dictionary() ), DEFVAL( 0 ), DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "remove_streamed_scene", "spawn_id" ), &T3DTerrain::removeStreamedScene );
	ClassDB::bind_method( D_METHOD( "allocate_stream_owner_id" ), &T3DTerrain::allocateStreamOwnerId );
	ClassDB::bind_method( D_METHOD( "set_next_stream_owner_id", "owner_id" ), &T3DTerrain::setNextStreamOwnerId );
	ClassDB::bind_method( D_METHOD( "get_next_stream_owner_id" ), &T3DTerrain::getNextStreamOwnerId );
	ClassDB::bind_method( D_METHOD( "get_node_streaming_stats" ), &T3DTerrain::getNodeStreamingStats );

	// Object references
	ClassDB::bind_method( D_METHOD( "get_data" ), &T3DTerrain::getData );
	ClassDB::bind_method( D_METHOD( "set_material", "material" ), &T3DTerrain::setMaterial );
	ClassDB::bind_method( D_METHOD( "get_material" ), &T3DTerrain::getMaterial );
	ClassDB::bind_method( D_METHOD( "set_assets", "assets" ), &T3DTerrain::setAssets );
	ClassDB::bind_method( D_METHOD( "set_auto_shader_profile", "profile" ), &T3DTerrain::setAutoShaderProfile );
	ClassDB::bind_method( D_METHOD( "get_auto_shader_profile" ), &T3DTerrain::getAutoShaderProfile );
	ClassDB::bind_method( D_METHOD( "get_assets" ), &T3DTerrain::getAssets );
	ClassDB::bind_method( D_METHOD( "get_collision" ), &T3DTerrain::getCollision );
	ClassDB::bind_method( D_METHOD( "get_instancer" ), &T3DTerrain::getInstancer );
	ClassDB::bind_method( D_METHOD( "get_node_streamer" ), &T3DTerrain::getNodeStreamer );
	ClassDB::bind_method( D_METHOD( "set_editor", "editor" ), &T3DTerrain::setEditor );
	ClassDB::bind_method( D_METHOD( "get_editor" ), &T3DTerrain::getEditor );
	ClassDB::bind_method( D_METHOD( "set_plugin", "plugin" ), &T3DTerrain::setPlugin );
	ClassDB::bind_method( D_METHOD( "get_plugin" ), &T3DTerrain::getPlugin );

	// Regions
	ClassDB::bind_method( D_METHOD( "change_region_size", "size" ), &T3DTerrain::changeRegionSize );
	ClassDB::bind_method( D_METHOD( "get_region_size" ), &T3DTerrain::getRegionSize );
	ClassDB::bind_method( D_METHOD( "set_save_16_bit", "enabled" ), &T3DTerrain::setSave16Bit );
	ClassDB::bind_method( D_METHOD( "get_save_16_bit" ), &T3DTerrain::getSave16Bit );
	ClassDB::bind_method( D_METHOD( "set_label_distance", "distance" ), &T3DTerrain::setLabelDistance );
	ClassDB::bind_method( D_METHOD( "get_label_distance" ), &T3DTerrain::getLabelDistance );
	ClassDB::bind_method( D_METHOD( "set_label_size", "size" ), &T3DTerrain::setLabelSize );
	ClassDB::bind_method( D_METHOD( "get_label_size" ), &T3DTerrain::getLabelSize );

	// Target Tracking
	ClassDB::bind_method( D_METHOD( "set_camera", "camera" ), &T3DTerrain::setCamera );
	ClassDB::bind_method( D_METHOD( "get_camera" ), &T3DTerrain::getCamera );
	ClassDB::bind_method( D_METHOD( "set_clipmap_target", "node" ), &T3DTerrain::setClipmapTarget );
	ClassDB::bind_method( D_METHOD( "get_clipmap_target" ), &T3DTerrain::getClipmapTarget );
	ClassDB::bind_method( D_METHOD( "get_clipmap_target_position" ), &T3DTerrain::getClipmapTargetPosition );
	ClassDB::bind_method( D_METHOD( "set_collision_target", "node" ), &T3DTerrain::setCollisionTarget );
	ClassDB::bind_method( D_METHOD( "get_collision_target" ), &T3DTerrain::getCollisionTarget );
	ClassDB::bind_method( D_METHOD( "get_collision_target_position" ), &T3DTerrain::getCollisionTargetPosition );
	ClassDB::bind_method( D_METHOD( "set_light_target", "node" ), &T3DTerrain::setLightTarget );
	ClassDB::bind_method( D_METHOD( "set_semantic_shadow_policy_enabled", "enabled" ), &T3DTerrain::setSemanticShadowPolicyEnabled );
	ClassDB::bind_method( D_METHOD( "is_semantic_shadow_policy_enabled" ), &T3DTerrain::isSemanticShadowPolicyEnabled );
	ClassDB::bind_method( D_METHOD( "set_environment_shadow_quality", "quality" ), &T3DTerrain::setEnvironmentShadowQuality );
	ClassDB::bind_method( D_METHOD( "get_environment_shadow_quality" ), &T3DTerrain::getEnvironmentShadowQuality );
	ClassDB::bind_method( D_METHOD( "set_environment_shadow_external_mask", "mask" ), &T3DTerrain::setEnvironmentShadowExternalMask );
	ClassDB::bind_method( D_METHOD( "get_environment_shadow_external_mask" ), &T3DTerrain::getEnvironmentShadowExternalMask );
	ClassDB::bind_method( D_METHOD( "set_semantic_directional_reception_enabled", "enabled" ), &T3DTerrain::setSemanticDirectionalReceptionEnabled );
	ClassDB::bind_method( D_METHOD( "is_semantic_directional_reception_enabled" ), &T3DTerrain::isSemanticDirectionalReceptionEnabled );
	ClassDB::bind_method( D_METHOD( "set_environment_directional_external_mask", "mask" ), &T3DTerrain::setEnvironmentDirectionalExternalMask );
	ClassDB::bind_method( D_METHOD( "get_environment_directional_external_mask" ), &T3DTerrain::getEnvironmentDirectionalExternalMask );
	ClassDB::bind_method( D_METHOD( "get_light_target" ), &T3DTerrain::getLightTarget );
	ClassDB::bind_method( D_METHOD( "snap" ), &T3DTerrain::snap );

	// Collision
	ClassDB::bind_method( D_METHOD( "set_collision_mode", "mode" ), &T3DTerrain::setCollisionMode );
	ClassDB::bind_method( D_METHOD( "get_collision_mode" ), &T3DTerrain::getCollisionMode );
	ClassDB::bind_method( D_METHOD( "set_collision_shape_size", "size" ), &T3DTerrain::setCollisionShapeSize );
	ClassDB::bind_method( D_METHOD( "get_collision_shape_size" ), &T3DTerrain::getCollisionShapeSize );
	ClassDB::bind_method( D_METHOD( "set_collision_radius", "radius" ), &T3DTerrain::setCollisionRadius );
	ClassDB::bind_method( D_METHOD( "get_collision_radius" ), &T3DTerrain::getCollisionRadius );
	ClassDB::bind_method( D_METHOD( "set_collision_layer", "layers" ), &T3DTerrain::setCollisionLayer );
	ClassDB::bind_method( D_METHOD( "get_collision_layer" ), &T3DTerrain::getCollisionLayer );
	ClassDB::bind_method( D_METHOD( "set_collision_mask", "mask" ), &T3DTerrain::setCollisionMask );
	ClassDB::bind_method( D_METHOD( "get_collision_mask" ), &T3DTerrain::getCollisionMask );
	ClassDB::bind_method( D_METHOD( "set_collision_priority", "priority" ), &T3DTerrain::setCollisionPriority );
	ClassDB::bind_method( D_METHOD( "get_collision_priority" ), &T3DTerrain::getCollisionPriority );
	ClassDB::bind_method( D_METHOD( "set_physics_material", "material" ), &T3DTerrain::setPhysicsMaterial );
	ClassDB::bind_method( D_METHOD( "get_physics_material" ), &T3DTerrain::getPhysicsMaterial );

	// Terrain Mesh
	ClassDB::bind_method( D_METHOD( "set_mesh_lods", "count" ), &T3DTerrain::setMeshLods );
	ClassDB::bind_method( D_METHOD( "get_mesh_lods" ), &T3DTerrain::getMeshLods );
	ClassDB::bind_method( D_METHOD( "set_runtime_shader_tiering_enabled", "enabled" ), &T3DTerrain::setRuntimeShaderTieringEnabled );
	ClassDB::bind_method( D_METHOD( "is_runtime_shader_tiering_enabled" ), &T3DTerrain::isRuntimeShaderTieringEnabled );
	ClassDB::bind_method( D_METHOD( "set_runtime_near_shader_lods", "count" ), &T3DTerrain::setRuntimeNearShaderLods );
	ClassDB::bind_method( D_METHOD( "get_runtime_near_shader_lods" ), &T3DTerrain::getRuntimeNearShaderLods );
	ClassDB::bind_method( D_METHOD( "set_runtime_middle_shader_lods", "count" ), &T3DTerrain::setRuntimeMiddleShaderLods );
	ClassDB::bind_method( D_METHOD( "get_runtime_middle_shader_lods" ), &T3DTerrain::getRuntimeMiddleShaderLods );
	ClassDB::bind_method( D_METHOD( "set_runtime_near_range_end_m", "range_m" ),
		&T3DTerrain::setRuntimeNearRangeEndM );
	ClassDB::bind_method( D_METHOD( "get_runtime_near_range_end_m" ),
		&T3DTerrain::getRuntimeNearRangeEndM );
	ClassDB::bind_method( D_METHOD( "set_runtime_middle_range_end_m", "range_m" ),
		&T3DTerrain::setRuntimeMiddleRangeEndM );
	ClassDB::bind_method( D_METHOD( "get_runtime_middle_range_end_m" ),
		&T3DTerrain::getRuntimeMiddleRangeEndM );
	ClassDB::bind_method( D_METHOD( "set_runtime_sync_detail_lods", "enabled" ),
		&T3DTerrain::setRuntimeSyncDetailLods );
	ClassDB::bind_method( D_METHOD( "is_runtime_sync_detail_lods" ),
		&T3DTerrain::isRuntimeSyncDetailLods );
	ClassDB::bind_method( D_METHOD( "set_runtime_clipmap_update_budget", "count" ), &T3DTerrain::setRuntimeClipmapUpdateBudget );
	ClassDB::bind_method( D_METHOD( "get_runtime_clipmap_update_budget" ), &T3DTerrain::getRuntimeClipmapUpdateBudget );
	ClassDB::bind_method( D_METHOD( "set_runtime_page_prepare_threads", "count" ), &T3DTerrain::setRuntimePagePrepareThreads );
	ClassDB::bind_method( D_METHOD( "get_runtime_page_prepare_threads" ), &T3DTerrain::getRuntimePagePrepareThreads );
	ClassDB::bind_method( D_METHOD( "set_mesh_size", "size" ), &T3DTerrain::setMeshSize );
	ClassDB::bind_method( D_METHOD( "get_mesh_size" ), &T3DTerrain::getMeshSize );
	ClassDB::bind_method( D_METHOD( "set_tessellation_level", "size" ), &T3DTerrain::setTessellationLevel );
	ClassDB::bind_method( D_METHOD( "get_tessellation_level" ), &T3DTerrain::getTessellationLevel );
	ClassDB::bind_method( D_METHOD( "set_vertex_spacing", "scale" ), &T3DTerrain::setVertexSpacing );
	ClassDB::bind_method( D_METHOD( "get_vertex_spacing" ), &T3DTerrain::getVertexSpacing );
	ClassDB::bind_method( D_METHOD( "set_cull_margin", "margin" ), &T3DTerrain::setCullMargin );
	ClassDB::bind_method( D_METHOD( "get_cull_margin" ), &T3DTerrain::getCullMargin );
	ClassDB::bind_method( D_METHOD( "set_cast_shadows", "shadow_casting_setting" ), &T3DTerrain::setCastShadows );
	ClassDB::bind_method( D_METHOD( "get_cast_shadows" ), &T3DTerrain::getCastShadows );
	ClassDB::bind_method( D_METHOD( "set_shadow_lod_count", "lod_count" ), &T3DTerrain::setShadowLodCount );
	ClassDB::bind_method( D_METHOD( "get_shadow_lod_count" ), &T3DTerrain::getShadowLodCount );
	ClassDB::bind_method( D_METHOD( "set_near_shadow_enabled", "enabled" ), &T3DTerrain::setNearShadowEnabled );
	ClassDB::bind_method( D_METHOD( "get_near_shadow_enabled" ), &T3DTerrain::getNearShadowEnabled );
	ClassDB::bind_method( D_METHOD( "set_middle_shadow_enabled", "enabled" ), &T3DTerrain::setMiddleShadowEnabled );
	ClassDB::bind_method( D_METHOD( "get_middle_shadow_enabled" ), &T3DTerrain::getMiddleShadowEnabled );
	ClassDB::bind_method( D_METHOD( "set_far_shadow_enabled", "enabled" ), &T3DTerrain::setFarShadowEnabled );
	ClassDB::bind_method( D_METHOD( "get_far_shadow_enabled" ), &T3DTerrain::getFarShadowEnabled );
	ClassDB::bind_method( D_METHOD( "set_near_shadow_lod_count", "lod_count" ), &T3DTerrain::setNearShadowLodCount );
	ClassDB::bind_method( D_METHOD( "get_near_shadow_lod_count" ), &T3DTerrain::getNearShadowLodCount );
	ClassDB::bind_method( D_METHOD( "set_middle_shadow_lod_count", "lod_count" ), &T3DTerrain::setMiddleShadowLodCount );
	ClassDB::bind_method( D_METHOD( "get_middle_shadow_lod_count" ), &T3DTerrain::getMiddleShadowLodCount );
	ClassDB::bind_method( D_METHOD( "set_far_shadow_lod_count", "lod_count" ), &T3DTerrain::setFarShadowLodCount );
	ClassDB::bind_method( D_METHOD( "get_far_shadow_lod_count" ), &T3DTerrain::getFarShadowLodCount );
	ClassDB::bind_method( D_METHOD( "set_gi_mode", "gi_mode" ), &T3DTerrain::setGiMode );
	ClassDB::bind_method( D_METHOD( "get_gi_mode" ), &T3DTerrain::getGiMode );
	ClassDB::bind_method( D_METHOD( "set_render_layers", "layers" ), &T3DTerrain::setRenderLayers );
	ClassDB::bind_method( D_METHOD( "get_render_layers" ), &T3DTerrain::getRenderLayers );

	// Terrain Displacement
	ClassDB::bind_method( D_METHOD( "set_displacement_scale", "scale" ), &T3DTerrain::setDisplacementScale );
	ClassDB::bind_method( D_METHOD( "get_displacement_scale" ), &T3DTerrain::getDisplacementScale );
	ClassDB::bind_method( D_METHOD( "set_displacement_sharpness", "sharpness" ), &T3DTerrain::setDisplacementSharpness );
	ClassDB::bind_method( D_METHOD( "get_displacement_sharpness" ), &T3DTerrain::getDisplacementSharpness );
	ClassDB::bind_method( D_METHOD( "set_buffer_shader_override_enabled", "enabled" ),
						 &T3DTerrain::setBufferShaderOverrideEnabled );
	ClassDB::bind_method( D_METHOD( "is_buffer_shader_override_enabled" ), &T3DTerrain::isBufferShaderOverrideEnabled );
	ClassDB::bind_method( D_METHOD( "set_buffer_shader_override", "shader" ), &T3DTerrain::setBufferShaderOverride );
	ClassDB::bind_method( D_METHOD( "get_buffer_shader_override" ), &T3DTerrain::getBufferShaderOverride );

	// Ocean Mesh
	ClassDB::bind_method( D_METHOD( "set_ocean_enabled", "enabled" ), &T3DTerrain::setOceanEnabled );
	ClassDB::bind_method( D_METHOD( "is_ocean_enabled" ), &T3DTerrain::isOceanEnabled );
	ClassDB::bind_method( D_METHOD( "set_ocean_performance_preset", "preset" ), &T3DTerrain::setOceanPerformancePreset );
	ClassDB::bind_method( D_METHOD( "get_ocean_performance_preset" ), &T3DTerrain::getOceanPerformancePreset );
	ClassDB::bind_method( D_METHOD( "set_ocean_vertex_wave_iterations", "iterations" ),
						 &T3DTerrain::setOceanVertexWaveIterations );
	ClassDB::bind_method( D_METHOD( "get_ocean_vertex_wave_iterations" ), &T3DTerrain::getOceanVertexWaveIterations );
	ClassDB::bind_method( D_METHOD( "set_ocean_fragment_wave_iterations", "iterations" ),
						 &T3DTerrain::setOceanFragmentWaveIterations );
	ClassDB::bind_method( D_METHOD( "get_ocean_fragment_wave_iterations" ),
						 &T3DTerrain::getOceanFragmentWaveIterations );
	ClassDB::bind_method( D_METHOD( "set_ocean_refraction_mode", "mode" ), &T3DTerrain::setOceanRefractionMode );
	ClassDB::bind_method( D_METHOD( "get_ocean_refraction_mode" ), &T3DTerrain::getOceanRefractionMode );
	ClassDB::bind_method( D_METHOD( "set_ocean_foam_mode", "mode" ), &T3DTerrain::setOceanFoamMode );
	ClassDB::bind_method( D_METHOD( "get_ocean_foam_mode" ), &T3DTerrain::getOceanFoamMode );
	ClassDB::bind_method( D_METHOD( "set_ocean_scattering_mode", "mode" ), &T3DTerrain::setOceanScatteringMode );
	ClassDB::bind_method( D_METHOD( "get_ocean_scattering_mode" ), &T3DTerrain::getOceanScatteringMode );
	ClassDB::bind_method( D_METHOD( "set_ocean_shading_mode", "mode" ), &T3DTerrain::setOceanShadingMode );
	ClassDB::bind_method( D_METHOD( "get_ocean_shading_mode" ), &T3DTerrain::getOceanShadingMode );
	ClassDB::bind_method( D_METHOD( "set_ocean_mesh_lods", "count" ), &T3DTerrain::setOceanMeshLods );
	ClassDB::bind_method( D_METHOD( "get_ocean_mesh_lods" ), &T3DTerrain::getOceanMeshLods );
	ClassDB::bind_method( D_METHOD( "set_ocean_tessellation_level", "size" ), &T3DTerrain::setOceanTessellationLevel );
	ClassDB::bind_method( D_METHOD( "get_ocean_tessellation_level" ), &T3DTerrain::getOceanTessellationLevel );
	ClassDB::bind_method( D_METHOD( "set_ocean_mesh_size", "size" ), &T3DTerrain::setOceanMeshSize );
	ClassDB::bind_method( D_METHOD( "get_ocean_mesh_size" ), &T3DTerrain::getOceanMeshSize );
	ClassDB::bind_method( D_METHOD( "set_ocean_vertex_spacing", "scale" ), &T3DTerrain::setOceanVertexSpacing );
	ClassDB::bind_method( D_METHOD( "get_ocean_vertex_spacing" ), &T3DTerrain::getOceanVertexSpacing );
	ClassDB::bind_method( D_METHOD( "set_ocean_cull_margin", "margin" ), &T3DTerrain::setOceanCullMargin );
	ClassDB::bind_method( D_METHOD( "get_ocean_cull_margin" ), &T3DTerrain::getOceanCullMargin );
	ClassDB::bind_method( D_METHOD( "set_ocean_cast_shadows", "shadow_casting_setting" ),
						 &T3DTerrain::setOceanCastShadows );
	ClassDB::bind_method( D_METHOD( "get_ocean_cast_shadows" ), &T3DTerrain::getOceanCastShadows );
	ClassDB::bind_method( D_METHOD( "set_ocean_gi_mode", "gi_mode" ), &T3DTerrain::setOceanGiMode );
	ClassDB::bind_method( D_METHOD( "get_ocean_gi_mode" ), &T3DTerrain::getOceanGiMode );
	ClassDB::bind_method( D_METHOD( "set_ocean_render_layers", "layers" ), &T3DTerrain::setOceanRenderLayers );
	ClassDB::bind_method( D_METHOD( "get_ocean_render_layers" ), &T3DTerrain::getOceanRenderLayers );
	ClassDB::bind_method( D_METHOD( "set_ocean_material", "material" ), &T3DTerrain::setOceanMaterial );
	ClassDB::bind_method( D_METHOD( "get_ocean_material" ), &T3DTerrain::getOceanMaterial );

	// Rendering
	ClassDB::bind_method( D_METHOD( "set_mouse_layer", "layer" ), &T3DTerrain::setMouseLayer );
	ClassDB::bind_method( D_METHOD( "get_mouse_layer" ), &T3DTerrain::getMouseLayer );
	ClassDB::bind_method( D_METHOD( "set_free_editor_textures" ), &T3DTerrain::setFreeEditorTextures );
	ClassDB::bind_method( D_METHOD( "get_free_editor_textures" ), &T3DTerrain::getFreeEditorTextures );
	ClassDB::bind_method( D_METHOD( "set_instancer_mode", "mode" ), &T3DTerrain::setInstancerMode );
	ClassDB::bind_method( D_METHOD( "get_instancer_mode" ), &T3DTerrain::getInstancerMode );

	// Overlays
	ClassDB::bind_method( D_METHOD( "set_show_region_grid", "enabled" ), &T3DTerrain::setShowRegionGrid );
	ClassDB::bind_method( D_METHOD( "get_show_region_grid" ), &T3DTerrain::getShowRegionGrid );
	ClassDB::bind_method( D_METHOD( "set_show_instancer_grid", "enabled" ), &T3DTerrain::setShowInstancerGrid );
	ClassDB::bind_method( D_METHOD( "get_show_instancer_grid" ), &T3DTerrain::getShowInstancerGrid );
	ClassDB::bind_method( D_METHOD( "set_show_vertex_grid", "enabled" ), &T3DTerrain::setShowVertexGrid );
	ClassDB::bind_method( D_METHOD( "get_show_vertex_grid" ), &T3DTerrain::getShowVertexGrid );
	ClassDB::bind_method( D_METHOD( "set_show_contours", "enabled" ), &T3DTerrain::setShowContours );
	ClassDB::bind_method( D_METHOD( "get_show_contours" ), &T3DTerrain::getShowContours );
	ClassDB::bind_method( D_METHOD( "set_show_navigation", "enabled" ), &T3DTerrain::setShowNavigation );
	ClassDB::bind_method( D_METHOD( "get_show_navigation" ), &T3DTerrain::getShowNavigation );

	// Debug Views
	ClassDB::bind_method( D_METHOD( "set_show_checkered", "enabled" ), &T3DTerrain::setShowCheckered );
	ClassDB::bind_method( D_METHOD( "get_show_checkered" ), &T3DTerrain::getShowCheckered );
	ClassDB::bind_method( D_METHOD( "set_show_grey", "enabled" ), &T3DTerrain::setShowGrey );
	ClassDB::bind_method( D_METHOD( "get_show_grey" ), &T3DTerrain::getShowGrey );
	ClassDB::bind_method( D_METHOD( "set_show_heightmap", "enabled" ), &T3DTerrain::setShowHeightmap );
	ClassDB::bind_method( D_METHOD( "get_show_heightmap" ), &T3DTerrain::getShowHeightmap );
	ClassDB::bind_method( D_METHOD( "set_show_jaggedness", "enabled" ), &T3DTerrain::setShowJaggedness );
	ClassDB::bind_method( D_METHOD( "get_show_jaggedness" ), &T3DTerrain::getShowJaggedness );
	ClassDB::bind_method( D_METHOD( "set_show_autoshader", "enabled" ), &T3DTerrain::setShowAutoshader );
	ClassDB::bind_method( D_METHOD( "get_show_autoshader" ), &T3DTerrain::getShowAutoshader );
	ClassDB::bind_method( D_METHOD( "set_show_control_texture", "enabled" ), &T3DTerrain::setShowControlTexture );
	ClassDB::bind_method( D_METHOD( "get_show_control_texture" ), &T3DTerrain::getShowControlTexture );
	ClassDB::bind_method( D_METHOD( "set_show_control_blend", "enabled" ), &T3DTerrain::setShowControlBlend );
	ClassDB::bind_method( D_METHOD( "get_show_control_blend" ), &T3DTerrain::getShowControlBlend );
	ClassDB::bind_method( D_METHOD( "set_show_control_angle", "enabled" ), &T3DTerrain::setShowControlAngle );
	ClassDB::bind_method( D_METHOD( "get_show_control_angle" ), &T3DTerrain::getShowControlAngle );
	ClassDB::bind_method( D_METHOD( "set_show_control_scale", "enabled" ), &T3DTerrain::setShowControlScale );
	ClassDB::bind_method( D_METHOD( "get_show_control_scale" ), &T3DTerrain::getShowControlScale );
	ClassDB::bind_method( D_METHOD( "set_show_colormap", "enabled" ), &T3DTerrain::setShowColormap );
	ClassDB::bind_method( D_METHOD( "get_show_colormap" ), &T3DTerrain::getShowColormap );
	ClassDB::bind_method( D_METHOD( "set_show_roughmap", "enabled" ), &T3DTerrain::setShowRoughmap );
	ClassDB::bind_method( D_METHOD( "get_show_roughmap" ), &T3DTerrain::getShowRoughmap );
	ClassDB::bind_method( D_METHOD( "set_compute_displacement_enabled", "enabled" ),
						 &T3DTerrain::setComputeDisplacementEnabled );
	ClassDB::bind_method( D_METHOD( "get_compute_displacement_enabled" ), &T3DTerrain::getComputeDisplacementEnabled );
	ClassDB::bind_method( D_METHOD( "set_displacement_update_distance", "distance" ),
						 &T3DTerrain::setDisplacementUpdateDistance );
	ClassDB::bind_method( D_METHOD( "get_displacement_update_distance" ),
						 &T3DTerrain::getDisplacementUpdateDistance );
	ClassDB::bind_method( D_METHOD( "is_compute_displacement_active" ), &T3DTerrain::isComputeDisplacementActive );
	ClassDB::bind_method( D_METHOD( "set_show_displacement_buffer", "enabled" ), &T3DTerrain::setShowDisplacementBuffer );
	ClassDB::bind_method( D_METHOD( "get_show_displacement_buffer" ), &T3DTerrain::getShowDisplacementBuffer );

	// PBR Views
	ClassDB::bind_method( D_METHOD( "set_show_texture_albedo", "enabled" ), &T3DTerrain::setShowTextureAlbedo );
	ClassDB::bind_method( D_METHOD( "get_show_texture_albedo" ), &T3DTerrain::getShowTextureAlbedo );
	ClassDB::bind_method( D_METHOD( "set_show_texture_height", "enabled" ), &T3DTerrain::setShowTextureHeight );
	ClassDB::bind_method( D_METHOD( "get_show_texture_height" ), &T3DTerrain::getShowTextureHeight );
	ClassDB::bind_method( D_METHOD( "set_show_texture_normal", "enabled" ), &T3DTerrain::setShowTextureNormal );
	ClassDB::bind_method( D_METHOD( "get_show_texture_normal" ), &T3DTerrain::getShowTextureNormal );
	ClassDB::bind_method( D_METHOD( "set_show_texture_rough", "enabled" ), &T3DTerrain::setShowTextureRough );
	ClassDB::bind_method( D_METHOD( "get_show_texture_rough" ), &T3DTerrain::getShowTextureRough );
	ClassDB::bind_method( D_METHOD( "set_show_texture_ao", "enabled" ), &T3DTerrain::setShowTextureAo );
	ClassDB::bind_method( D_METHOD( "get_show_texture_ao" ), &T3DTerrain::getShowTextureAo );

	// Utility
	ClassDB::bind_method( D_METHOD( "get_intersection", "src_pos", "direction", "gpu_mode" ), &T3DTerrain::getIntersection,
						 DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_raycast_result", "src_pos", "direction", "collision_mask", "exclude_terrain" ),
						 &T3DTerrain::getRaycastResult, DEFVAL( 0xFFFFFFFF ), DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "bake_mesh", "lod", "filter" ), &T3DTerrain::bakeMesh,
						 DEFVAL( T3DData::HEIGHT_FILTER_NEAREST ) );
	ClassDB::bind_method( D_METHOD( "bake_region_lightmap_mesh", "region_location", "lod", "context_radius_regions", "texel_size" ),
		&T3DTerrain::bakeRegionLightmapMesh, DEFVAL( 2 ), DEFVAL( 1 ), DEFVAL( 1.0f ) );
	ClassDB::bind_method( D_METHOD( "bake_occluder", "lod" ), &T3DTerrain::bakeOccluder );
	ClassDB::bind_method( D_METHOD( "generate_nav_mesh_source_geometry", "global_aabb", "require_nav" ),
						 &T3DTerrain::generateNavMeshSourceGeometry, DEFVAL( true ) );

	ADD_GROUP( "General / Data & Authoring", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "version", PROPERTY_HINT_NONE, "",
							  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				 "", "get_version" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "debug_level", PROPERTY_HINT_ENUM, "Errors,Info,Debug,Extreme" ),
				 "set_debug_level", "get_debug_level" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "data_directory", PROPERTY_HINT_DIR ), "set_data_directory",
				 "get_data_directory" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "editor_execution_mode", PROPERTY_HINT_ENUM,
							  "Authoring,Runtime Profile" ),
		"set_editor_execution_mode", "get_editor_execution_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "editor_runtime_streaming_preview", PROPERTY_HINT_NONE, "",
							  PROPERTY_USAGE_STORAGE ),
		"set_editor_runtime_streaming_preview", "is_editor_runtime_streaming_preview" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_size", PROPERTY_HINT_ENUM,
							  "128:128", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				 "change_region_size", "get_region_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "save_16_bit", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
				 "set_save_16_bit", "get_save_16_bit" );
	// Storage backing is deliberately ordered before Resources so scene loading restores
	// the selected base profile before a serialized material resource and its overrides.
	ADD_PROPERTY( PropertyInfo( Variant::INT, "_performance_target_storage", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
				 "set_performance_target", "get_performance_target" );

	ADD_SUBGROUP( "Regional Lightmap Baking", "region_lightmap_" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "region_lightmap_output_directory", PROPERTY_HINT_DIR ),
		"set_region_lightmap_output_directory", "get_region_lightmap_output_directory" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_lightmap_proxy_lod", PROPERTY_HINT_RANGE,
		"0,8,1" ), "set_region_lightmap_proxy_lod", "get_region_lightmap_proxy_lod" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_lightmap_context_radius", PROPERTY_HINT_RANGE,
		"0,4,1,suffix: regions" ), "set_region_lightmap_context_radius", "get_region_lightmap_context_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "region_lightmap_texel_size", PROPERTY_HINT_RANGE,
		"0.05,16,0.05,suffix: m/texel" ), "set_region_lightmap_texel_size", "get_region_lightmap_texel_size" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_lightmap_bake_quality", PROPERTY_HINT_ENUM,
		"Low,Medium,High,Ultra" ), "set_region_lightmap_bake_quality", "get_region_lightmap_bake_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_lightmap_shadowmask_mode", PROPERTY_HINT_ENUM,
		"None,Replace,Overlay" ), "set_region_lightmap_shadowmask_mode", "get_region_lightmap_shadowmask_mode" );

	ADD_GROUP( "Resources", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DMaterial" ),
				 "set_material", "get_material" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "assets", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DAssets" ), "set_assets",
				 "get_assets" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "auto_shader_profile", PROPERTY_HINT_RESOURCE_TYPE,
								  "Terrain3DAutoShaderProfile" ),
				 "set_auto_shader_profile", "get_auto_shader_profile" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "light_target", PROPERTY_HINT_NODE_TYPE, "DirectionalLight3D",
							  PROPERTY_USAGE_DEFAULT, "Node3D" ),
				 "set_light_target", "get_light_target" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "Terrain3DData" ),
				 "", "get_data" );
	ADD_PROPERTY(
		PropertyInfo( Variant::OBJECT, "collision", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "Terrain3DCollision" ),
		"", "get_collision" );
	ADD_PROPERTY(
		PropertyInfo( Variant::OBJECT, "instancer", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "Terrain3DInstancer" ),
		"", "get_instancer" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "node_streamer", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "Terrain3DNodeStreamer" ), "", "get_node_streamer" );

	ADD_GROUP( "Performance Profile", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "performance_target", PROPERTY_HINT_ENUM,
							  "Custom,Xbox 360 Experimental", PROPERTY_USAGE_EDITOR ),
				 "set_performance_target", "get_performance_target" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "performance_profile_status", PROPERTY_HINT_ENUM,
		"Custom,Exact,Modified", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
		"", "get_performance_profile_status" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "performance_profile_version", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_performance_profile_version" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "performance_profile_signature", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_performance_profile_signature" );

	ADD_GROUP( "Terrain Rendering", "" );

	ADD_SUBGROUP( "Geometry & Clipmap", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "clipmap_target", PROPERTY_HINT_NODE_TYPE, "Node3D",
							  PROPERTY_USAGE_DEFAULT, "Node3D" ),
				 "set_clipmap_target", "get_clipmap_target" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mesh_lods", PROPERTY_HINT_RANGE, "1,10,1" ), "set_mesh_lods",
				 "get_mesh_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mesh_size", PROPERTY_HINT_RANGE, "8,256,2" ), "set_mesh_size",
				 "get_mesh_size" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "vertex_spacing", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "set_vertex_spacing", "get_vertex_spacing" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cull_margin", PROPERTY_HINT_RANGE, "0.0,10000.0,.5,or_greater" ),
				 "set_cull_margin", "get_cull_margin" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER ), "set_render_layers",
				 "get_render_layers" );

	ADD_SUBGROUP( "Shader Tier Ranges", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_shader_tiering_enabled" ),
		"set_runtime_shader_tiering_enabled", "is_runtime_shader_tiering_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_near_shader_lods", PROPERTY_HINT_RANGE, "0,16,1" ),
		"set_runtime_near_shader_lods", "get_runtime_near_shader_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_middle_shader_lods", PROPERTY_HINT_RANGE, "0,16,1" ),
		"set_runtime_middle_shader_lods", "get_runtime_middle_shader_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "runtime_near_range_end_m", PROPERTY_HINT_RANGE,
		"0,4096,1,or_greater,suffix:m" ), "set_runtime_near_range_end_m", "get_runtime_near_range_end_m" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "runtime_middle_range_end_m", PROPERTY_HINT_RANGE,
		"0,8192,1,or_greater,suffix:m" ), "set_runtime_middle_range_end_m", "get_runtime_middle_range_end_m" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_sync_detail_lods" ),
		"set_runtime_sync_detail_lods", "is_runtime_sync_detail_lods" );

	ADD_SUBGROUP( "Shadows & GI", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only" ),
				 "set_cast_shadows", "get_cast_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shadow_lod_count", PROPERTY_HINT_RANGE, "0,16,1" ),
				 "set_shadow_lod_count", "get_shadow_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "near_shadow_enabled" ), "set_near_shadow_enabled", "get_near_shadow_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "middle_shadow_enabled" ), "set_middle_shadow_enabled", "get_middle_shadow_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "far_shadow_enabled" ), "set_far_shadow_enabled", "get_far_shadow_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
		"set_near_shadow_lod_count", "get_near_shadow_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
		"set_middle_shadow_lod_count", "get_middle_shadow_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
		"set_far_shadow_lod_count", "get_far_shadow_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic" ), "set_gi_mode",
				 "get_gi_mode" );

	ADD_SUBGROUP( "Displacement / Tessellation", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "tessellation_level", PROPERTY_HINT_RANGE, "0,6,1" ),
				 "set_tessellation_level", "get_tessellation_level" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "compute_displacement_enabled" ), "set_compute_displacement_enabled",
				 "get_compute_displacement_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "displacement_update_distance", PROPERTY_HINT_RANGE,
							  "0.0,16.0,0.01,suffix:m" ),
				 "set_displacement_update_distance", "get_displacement_update_distance" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "displacement_scale", PROPERTY_HINT_RANGE, "0.0, 2.0, 0.01" ),
				 "set_displacement_scale", "get_displacement_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "displacement_sharpness", PROPERTY_HINT_RANGE, "0.0, 1.0, 0.01" ),
				 "set_displacement_sharpness", "get_displacement_sharpness" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "buffer_shader_override_enabled" ), "set_buffer_shader_override_enabled",
				 "is_buffer_shader_override_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "buffer_shader_override", PROPERTY_HINT_RESOURCE_TYPE, "Shader" ),
				 "set_buffer_shader_override", "get_buffer_shader_override" );

	ADD_GROUP( "Streaming & Residency", "" );

	ADD_SUBGROUP( "Region Tiers", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "streaming_active_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
		"set_streaming_active_radius", "get_streaming_active_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "streaming_hot_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
		"set_streaming_hot_radius", "get_streaming_hot_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "streaming_cold_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
		"set_streaming_cold_radius", "get_streaming_cold_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "streaming_unload_guard", PROPERTY_HINT_RANGE, "0,4,1,suffix: regions" ),
		"set_streaming_unload_guard", "get_streaming_unload_guard" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "streaming_prediction_enabled" ),
		"set_streaming_prediction_enabled", "is_streaming_prediction_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "streaming_prediction_lookahead", PROPERTY_HINT_RANGE, "0,4,0.05,suffix:s" ),
		"set_streaming_prediction_lookahead", "get_streaming_prediction_lookahead" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "streaming_prediction_maximum_regions", PROPERTY_HINT_RANGE, "0,16,1,suffix: regions" ),
		"set_streaming_prediction_maximum_regions", "get_streaming_prediction_maximum_regions" );

	ADD_SUBGROUP( "GPU Terrain Pages", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_cache_enabled" ), "set_gpu_region_cache_enabled",
				 "is_gpu_region_cache_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_cache_budget_auto" ),
		"set_gpu_region_cache_budget_auto", "is_gpu_region_cache_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,128,1" ),
				 "set_gpu_region_cache_budget_mb", "get_gpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_cache_effective_mb", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_effective_gpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_upload_budget_layers", PROPERTY_HINT_RANGE, "1,64,1" ),
				 "set_gpu_region_upload_budget_layers", "get_gpu_region_upload_budget_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_upload_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ),
				 "set_gpu_region_upload_budget_mb", "get_gpu_region_upload_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_spare_pages", PROPERTY_HINT_RANGE, "2,63,1" ),
				 "set_gpu_region_spare_pages", "get_gpu_region_spare_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_retire_frames", PROPERTY_HINT_RANGE, "1,16,1" ),
				 "set_gpu_region_retire_frames", "get_gpu_region_retire_frames" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_detail_lods", PROPERTY_HINT_RANGE,
		"0,16,1,suffix: LODs (0 = all)" ), "set_gpu_region_detail_lods", "get_gpu_region_detail_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "gpu_region_detail_range_end_m", PROPERTY_HINT_RANGE,
		"0,8192,1,or_greater,suffix:m" ), "set_gpu_region_detail_range_end_m",
		"get_gpu_region_detail_range_end_m" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_max_supported_detail_lods", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_gpu_region_max_supported_detail_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "gpu_region_max_supported_detail_range_m", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_gpu_region_max_supported_detail_range_m" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_required_core_pages", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_gpu_region_required_core_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_mapped_capacity", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_gpu_region_mapped_capacity" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_detail_range_valid", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "is_gpu_region_detail_range_valid" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_guard_regions", PROPERTY_HINT_RANGE, "0,4,1",
		PROPERTY_USAGE_STORAGE ), "set_gpu_region_guard_regions", "get_gpu_region_guard_regions" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_prefetch_rows", PROPERTY_HINT_RANGE, "0,4,1",
		PROPERTY_USAGE_STORAGE ), "set_gpu_region_prefetch_rows", "get_gpu_region_prefetch_rows" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_hysteresis_ticks", PROPERTY_HINT_RANGE, "0,240,1" ),
				 "set_gpu_region_hysteresis_ticks", "get_gpu_region_hysteresis_ticks" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_color_resolution_divisor", PROPERTY_HINT_ENUM, "Full:1,Half:2,Quarter:4" ),
				 "set_gpu_region_color_resolution_divisor", "get_gpu_region_color_resolution_divisor" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_derived_resolution_divisor", PROPERTY_HINT_ENUM, "Full:1,Half:2,Quarter:4" ),
				 "set_gpu_region_derived_resolution_divisor", "get_gpu_region_derived_resolution_divisor" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_middle_height_resolution_divisor", PROPERTY_HINT_ENUM, "Full:1,Half:2,Quarter:4" ),
				 "set_gpu_middle_height_resolution_divisor", "get_gpu_middle_height_resolution_divisor" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "editor_gpu_region_cache_pages", PROPERTY_HINT_RANGE, "16,256,1,suffix: pages" ),
				 "set_editor_gpu_region_cache_pages", "get_editor_gpu_region_cache_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_far_quality_divisor", PROPERTY_HINT_ENUM, "Full:1,Half:2,Quarter:4,Eighth:8" ),
				 "set_gpu_far_quality_divisor", "get_gpu_far_quality_divisor" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_material_resolution", PROPERTY_HINT_ENUM, "64:64,128:128,256:256" ),
		"set_far_material_resolution", "get_far_material_resolution" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_control_resolution", PROPERTY_HINT_ENUM, "64:64,128:128,256:256" ),
		"set_far_control_resolution", "get_far_control_resolution" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_far_atlas_side", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_gpu_far_atlas_side" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_far_height_atlas_side", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_gpu_far_height_atlas_side" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_far_surface_atlas_side", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_gpu_far_surface_atlas_side" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_cooked_pages_enabled" ),
				 "set_gpu_region_cooked_pages_enabled", "is_gpu_region_cooked_pages_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_cook_on_save" ),
				 "set_gpu_region_cook_on_save", "is_gpu_region_cook_on_save" );

	ADD_SUBGROUP( "Runtime Material Streaming", "runtime_material_" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_material_streaming_enabled" ),
				 "set_runtime_material_streaming_enabled", "is_runtime_material_streaming_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_material_cache_layers", PROPERTY_HINT_RANGE, "4,32,1,suffix: layers" ),
				 "set_runtime_material_cache_layers", "get_runtime_material_cache_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_material_upload_budget_layers", PROPERTY_HINT_RANGE, "1,32,1,suffix: layers/frame" ),
				 "set_runtime_material_upload_budget_layers", "get_runtime_material_upload_budget_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_material_release_source_textures" ),
				 "set_runtime_release_source_material_textures", "is_runtime_release_source_material_textures" );

	ADD_SUBGROUP( "CPU Terrain Pages", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "cpu_region_cache_enabled" ), "set_cpu_region_cache_enabled",
				 "is_cpu_region_cache_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "cpu_region_cache_budget_auto" ),
		"set_cpu_region_cache_budget_auto", "is_cpu_region_cache_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cpu_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,128,1" ),
				 "set_cpu_region_cache_budget_mb", "get_cpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cpu_region_cache_effective_mb", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_effective_cpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cpu_region_load_budget_layers", PROPERTY_HINT_RANGE, "1,64,1" ),
				 "set_cpu_region_load_budget_layers", "get_cpu_region_load_budget_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cpu_region_load_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ),
				 "set_cpu_region_load_budget_mb", "get_cpu_region_load_budget_mb" );

	ADD_SUBGROUP( "Static Instance Pages", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "instance_region_cache_enabled" ), "set_instance_region_cache_enabled", "is_instance_region_cache_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "instance_region_cache_budget_auto" ),
		"set_instance_region_cache_budget_auto", "is_instance_region_cache_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,128,1" ), "set_instance_region_cache_budget_mb", "get_instance_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_region_cache_effective_mb", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_effective_instance_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_region_load_budget_pages", PROPERTY_HINT_RANGE, "1,64,1" ), "set_instance_region_load_budget_pages", "get_instance_region_load_budget_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_region_load_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ), "set_instance_region_load_budget_mb", "get_instance_region_load_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instancer_cell_size", PROPERTY_HINT_ENUM,
		"8,16,32,64,128,suffix: samples" ), "set_instancer_cell_size", "get_instancer_cell_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "instancer_cell_streaming_enabled" ),
		"set_instancer_cell_streaming_enabled", "is_instancer_cell_streaming_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "instancer_single_lod_buffers" ),
		"set_instancer_single_lod_buffers", "is_instancer_single_lod_buffers" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "instancer_cell_streaming_margin", PROPERTY_HINT_RANGE,
		"0,1024,1,suffix:m" ), "set_instancer_cell_streaming_margin", "get_instancer_cell_streaming_margin" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "instancer_cell_streaming_update_distance", PROPERTY_HINT_RANGE,
		"1,256,1,suffix:m" ), "set_instancer_cell_streaming_update_distance",
		"get_instancer_cell_streaming_update_distance" );

	ADD_SUBGROUP( "Dynamic Nodes", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "dynamic_node_streaming_enabled" ), "set_dynamic_node_streaming_enabled", "is_dynamic_node_streaming_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "dynamic_node_stream_radius", PROPERTY_HINT_RANGE, "0,16,1" ), "set_dynamic_node_stream_radius", "get_dynamic_node_stream_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "dynamic_node_load_region_budget", PROPERTY_HINT_RANGE, "0,64,1" ), "set_dynamic_node_load_region_budget", "get_dynamic_node_load_region_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "dynamic_node_load_node_budget", PROPERTY_HINT_RANGE, "0,4096,1" ), "set_dynamic_node_load_node_budget", "get_dynamic_node_load_node_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "dynamic_node_unload_region_budget", PROPERTY_HINT_RANGE, "0,64,1" ), "set_dynamic_node_unload_region_budget", "get_dynamic_node_unload_region_budget" );

	ADD_SUBGROUP( "Baked Lighting", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "lightmap_streaming_enabled" ), "set_lightmap_streaming_enabled", "is_lightmap_streaming_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "lightmap_residency_budget_auto" ),
		"set_lightmap_residency_budget_auto", "is_lightmap_residency_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lightmap_residency_budget_mb", PROPERTY_HINT_RANGE, "1,1024,1,suffix: MiB" ),
		"set_lightmap_residency_budget_mb", "get_lightmap_residency_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lightmap_residency_effective_mb", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_effective_lightmap_residency_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lightmap_stream_radius", PROPERTY_HINT_RANGE, "0,16,1,suffix: regions" ), "set_lightmap_stream_radius", "get_lightmap_stream_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lightmap_load_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ), "set_lightmap_load_budget", "get_lightmap_load_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lightmap_unload_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ), "set_lightmap_unload_budget", "get_lightmap_unload_budget" );

	ADD_PROPERTY( PropertyInfo( Variant::INT, "stream_owner_counter", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ), "set_next_stream_owner_id", "get_next_stream_owner_id" );

	ADD_SUBGROUP( "HLOD", "hlod_" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "hlod_enabled" ), "set_hlod_enabled", "is_hlod_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "hlod_manifest_path", PROPERTY_HINT_FILE, "*.tres,*.res" ), "set_hlod_manifest_path", "get_hlod_manifest_path" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "hlod_target_error_pixels", PROPERTY_HINT_RANGE, "0.25,64,0.25,suffix:px" ), "set_hlod_target_error_pixels", "get_hlod_target_error_pixels" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "hlod_hysteresis", PROPERTY_HINT_RANGE, "0,0.75,0.01" ), "set_hlod_hysteresis", "get_hlod_hysteresis" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_max_level", PROPERTY_HINT_RANGE, "1,6,1" ), "set_hlod_max_level", "get_hlod_max_level" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_load_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ), "set_hlod_load_budget", "get_hlod_load_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_unload_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ), "set_hlod_unload_budget", "get_hlod_unload_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "hlod_memory_budget_auto" ), "set_hlod_memory_budget_auto", "is_hlod_memory_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_memory_budget_mb", PROPERTY_HINT_RANGE, "1,4096,1,suffix: MiB" ), "set_hlod_memory_budget_mb", "get_hlod_memory_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_memory_effective_mb", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_effective_hlod_memory_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_max_shadow_level", PROPERTY_HINT_RANGE, "0,6,1" ), "set_hlod_max_shadow_level", "get_hlod_max_shadow_level" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_forced_debug_level", PROPERTY_HINT_RANGE, "-1,6,1" ), "set_hlod_forced_debug_level", "get_hlod_forced_debug_level" );

	ADD_GROUP( "Runtime Scheduling - Advanced", "" );

	ADD_SUBGROUP( "Work Budgets", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_clipmap_update_budget", PROPERTY_HINT_RANGE, "1,16,1" ),
		"set_runtime_clipmap_update_budget", "get_runtime_clipmap_update_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_page_prepare_threads", PROPERTY_HINT_RANGE, "1,32,1" ),
		"set_runtime_page_prepare_threads", "get_runtime_page_prepare_threads" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instancer_update_cell_budget", PROPERTY_HINT_RANGE,
		"0,65536,1,or_greater,suffix: cells/frame" ), "set_instancer_update_cell_budget",
		"get_instancer_update_cell_budget" );

	ADD_SUBGROUP( "Adaptive Policy", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_adaptive_work_budgets_enabled" ),
		"set_runtime_adaptive_work_budgets_enabled", "is_runtime_adaptive_work_budgets_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "runtime_adaptive_target_ms", PROPERTY_HINT_RANGE,
		"0.05,50.0,0.05,suffix:ms" ), "set_runtime_adaptive_target_ms", "get_runtime_adaptive_target_ms" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "runtime_adaptive_hard_max_ms", PROPERTY_HINT_RANGE,
		"0.05,100.0,0.05,suffix:ms" ), "set_runtime_adaptive_hard_max_ms",
		"get_runtime_adaptive_hard_max_ms" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_adjustment_window_frames", PROPERTY_HINT_RANGE,
		"1,120,1" ), "set_runtime_adaptive_adjustment_window_frames",
		"get_runtime_adaptive_adjustment_window_frames" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_gpu_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
		"set_runtime_adaptive_min_gpu_pages", "get_runtime_adaptive_min_gpu_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_cpu_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
		"set_runtime_adaptive_min_cpu_pages", "get_runtime_adaptive_min_cpu_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_instance_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
		"set_runtime_adaptive_min_instance_pages", "get_runtime_adaptive_min_instance_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_instancer_cells", PROPERTY_HINT_RANGE,
		"0,65536,1" ), "set_runtime_adaptive_min_instancer_cells",
		"get_runtime_adaptive_min_instancer_cells" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_node_load_regions", PROPERTY_HINT_RANGE,
		"0,64,1" ), "set_runtime_adaptive_min_node_load_regions",
		"get_runtime_adaptive_min_node_load_regions" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_node_unload_regions", PROPERTY_HINT_RANGE,
		"0,64,1" ), "set_runtime_adaptive_min_node_unload_regions",
		"get_runtime_adaptive_min_node_unload_regions" );

	ADD_SUBGROUP( "Boundary Phasing", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_boundary_work_phasing_enabled" ),
		"set_runtime_boundary_work_phasing_enabled", "is_runtime_boundary_work_phasing_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_boundary_node_delay_frames", PROPERTY_HINT_RANGE,
		"0,8,1,suffix:frames" ), "set_runtime_boundary_node_delay_frames",
		"get_runtime_boundary_node_delay_frames" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_boundary_instancer_delay_frames", PROPERTY_HINT_RANGE,
		"0,8,1,suffix:frames" ), "set_runtime_boundary_instancer_delay_frames",
		"get_runtime_boundary_instancer_delay_frames" );

	ADD_GROUP( "Memory", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "total_memory_budget_mb", PROPERTY_HINT_RANGE,
		"32,4096,1,suffix: MiB" ), "set_total_memory_budget_mb", "get_total_memory_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "memory_transient_reserve_mb", PROPERTY_HINT_RANGE,
		"4,1024,1,suffix: MiB" ), "set_memory_transient_reserve_mb", "get_memory_transient_reserve_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "shader_cache_budget_auto" ),
		"set_shader_cache_budget_auto", "is_shader_cache_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shader_cache_effective_mb", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_effective_shader_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "memory_budget_overcommitted", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "is_memory_budget_overcommitted" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "memory_budget_headroom_bytes", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_memory_budget_headroom_bytes" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "memory_budget_policy_generation", PROPERTY_HINT_NONE, "",
		PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ), "", "get_memory_budget_policy_generation" );

	ADD_GROUP( "Collision", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_mode", PROPERTY_HINT_ENUM,
							  "Disabled,Dynamic / Game,Dynamic / Editor,Full / Game,Full / Editor" ),
				 "set_collision_mode", "get_collision_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_shape_size", PROPERTY_HINT_RANGE, "8,64,8" ),
				 "set_collision_shape_size", "get_collision_shape_size" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_radius", PROPERTY_HINT_RANGE, "16,256,16" ),
				 "set_collision_radius", "get_collision_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "collision_target", PROPERTY_HINT_NODE_TYPE, "Node3D",
							  PROPERTY_USAGE_DEFAULT, "Node3D" ),
				 "set_collision_target", "get_collision_target" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS ), "set_collision_layer",
				 "get_collision_layer" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS ), "set_collision_mask",
				 "get_collision_mask" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "collision_priority", PROPERTY_HINT_RANGE, "0.1,256,.1" ),
				 "set_collision_priority", "get_collision_priority" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial" ),
				 "set_physics_material", "get_physics_material" );

	ADD_GROUP( "Ocean", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "ocean_enabled" ), "set_ocean_enabled", "is_ocean_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_performance_preset", PROPERTY_HINT_ENUM,
							  "Custom,Border Minimum,Border Balanced,High,Cinematic" ),
				 "set_ocean_performance_preset", "get_ocean_performance_preset" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_vertex_wave_iterations", PROPERTY_HINT_RANGE, "0,3,1" ),
				 "set_ocean_vertex_wave_iterations", "get_ocean_vertex_wave_iterations" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_fragment_wave_iterations", PROPERTY_HINT_RANGE, "0,4,1" ),
				 "set_ocean_fragment_wave_iterations", "get_ocean_fragment_wave_iterations" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_refraction_mode", PROPERTY_HINT_ENUM,
							  "Off,Depth Tint,Full Screen Refraction" ),
				 "set_ocean_refraction_mode", "get_ocean_refraction_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_foam_mode", PROPERTY_HINT_ENUM, "Off,Shore Only,Shore + Crest" ),
				 "set_ocean_foam_mode", "get_ocean_foam_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_scattering_mode", PROPERTY_HINT_ENUM, "Off,Approximate,Full" ),
				 "set_ocean_scattering_mode", "get_ocean_scattering_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_shading_mode", PROPERTY_HINT_ENUM,
							  "Lambert Diffuse,Lambert + GGX,Burley + GGX,Unshaded,Lambert Vertex Lit" ),
				 "set_ocean_shading_mode", "get_ocean_shading_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_mesh_lods", PROPERTY_HINT_RANGE, "1,10,1" ), "set_ocean_mesh_lods",
				 "get_ocean_mesh_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_tessellation_level", PROPERTY_HINT_RANGE, "0,6,1" ),
				 "set_ocean_tessellation_level", "get_ocean_tessellation_level" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_mesh_size", PROPERTY_HINT_RANGE, "8,256,2" ), "set_ocean_mesh_size",
				 "get_ocean_mesh_size" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "ocean_vertex_spacing", PROPERTY_HINT_RANGE, "0.25,10.0,0.05,or_greater" ),
				 "set_ocean_vertex_spacing", "get_ocean_vertex_spacing" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "ocean_cull_margin", PROPERTY_HINT_RANGE, "0.0,10000.0,.5,or_greater" ),
				 "set_ocean_cull_margin", "get_ocean_cull_margin" );
	ADD_PROPERTY(
		PropertyInfo( Variant::INT, "ocean_cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only" ),
		"set_ocean_cast_shadows", "get_ocean_cast_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic" ),
				 "set_ocean_gi_mode", "get_ocean_gi_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_render_layers", PROPERTY_HINT_LAYERS_3D_RENDER ),
				 "set_ocean_render_layers", "get_ocean_render_layers" );
	ADD_PROPERTY(
		PropertyInfo( Variant::OBJECT, "ocean_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial,BaseMaterial3D" ),
		"set_ocean_material", "get_ocean_material" );

	ADD_GROUP( "Semantic Lighting", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "semantic_shadow_policy_enabled" ), "set_semantic_shadow_policy_enabled", "is_semantic_shadow_policy_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "environment_shadow_quality", PROPERTY_HINT_ENUM, "Low,Normal,High,Ultra" ), "set_environment_shadow_quality", "get_environment_shadow_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "environment_shadow_external_mask", PROPERTY_HINT_LAYERS_3D_RENDER ), "set_environment_shadow_external_mask", "get_environment_shadow_external_mask" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "semantic_directional_reception_enabled" ), "set_semantic_directional_reception_enabled", "is_semantic_directional_reception_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "environment_directional_external_mask", PROPERTY_HINT_LAYERS_3D_RENDER ), "set_environment_directional_external_mask", "get_environment_directional_external_mask" );

	ADD_GROUP( "Diagnostics & Profiling", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_profiling_enabled" ),
		"set_runtime_profiling_enabled", "is_runtime_profiling_enabled" );

	ADD_GROUP( "Editor Visualization", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "label_distance", PROPERTY_HINT_RANGE, "0.0,10000.0,0.5,or_greater" ),
				 "set_label_distance", "get_label_distance" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "label_size", PROPERTY_HINT_RANGE, "24,128,1" ), "set_label_size",
				 "get_label_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ), "set_show_region_grid", "get_show_region_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mouse_layer", PROPERTY_HINT_RANGE, "21, 32" ), "set_mouse_layer",
				 "get_mouse_layer" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "free_editor_textures" ), "set_free_editor_textures",
				 "get_free_editor_textures" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instancer_mode", PROPERTY_HINT_ENUM, "Disabled,Normal" ),
				 "set_instancer_mode", "get_instancer_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_region_grid" ), "set_show_region_grid", "get_show_region_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_instancer_grid" ), "set_show_instancer_grid",
				 "get_show_instancer_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_vertex_grid" ), "set_show_vertex_grid", "get_show_vertex_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_contours" ), "set_show_contours", "get_show_contours" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_navigation" ), "set_show_navigation", "get_show_navigation" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_checkered" ), "set_show_checkered", "get_show_checkered" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_grey" ), "set_show_grey", "get_show_grey" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_heightmap" ), "set_show_heightmap", "get_show_heightmap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_jaggedness" ), "set_show_jaggedness", "get_show_jaggedness" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_autoshader" ), "set_show_autoshader", "get_show_autoshader" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_texture" ), "set_show_control_texture",
				 "get_show_control_texture" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_blend" ), "set_show_control_blend", "get_show_control_blend" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_angle" ), "set_show_control_angle", "get_show_control_angle" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_scale" ), "set_show_control_scale", "get_show_control_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_colormap" ), "set_show_colormap", "get_show_colormap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_roughmap" ), "set_show_roughmap", "get_show_roughmap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_displacement_buffer" ), "set_show_displacement_buffer",
				 "get_show_displacement_buffer" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_albedo" ), "set_show_texture_albedo",
				 "get_show_texture_albedo" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_height" ), "set_show_texture_height",
				 "get_show_texture_height" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_normal" ), "set_show_texture_normal",
				 "get_show_texture_normal" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_rough" ), "set_show_texture_rough", "get_show_texture_rough" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_ao" ), "set_show_texture_ao", "get_show_texture_ao" );

	ADD_SIGNAL( MethodInfo( "material_changed" ) );
	ADD_SIGNAL( MethodInfo( "performance_profile_state_changed", PropertyInfo( Variant::INT, "status" ) ) );
	ADD_SIGNAL( MethodInfo( "memory_pressure_changed", PropertyInfo( Variant::BOOL, "pressure" ),
		PropertyInfo( Variant::BOOL, "overcommitted" ), PropertyInfo( Variant::INT, "headroom_bytes" ) ) );
	ADD_SIGNAL( MethodInfo( "cache_budget_changed", PropertyInfo( Variant::STRING_NAME, "subsystem" ),
		PropertyInfo( Variant::INT, "effective_bytes" ), PropertyInfo( Variant::STRING_NAME, "limiting_constraint" ) ) );
	ADD_SIGNAL( MethodInfo( "cache_thrash_state_changed", PropertyInfo( Variant::STRING_NAME, "cache" ),
		PropertyInfo( Variant::BOOL, "thrashing" ), PropertyInfo( Variant::INT, "activity" ) ) );
	ADD_SIGNAL( MethodInfo( "assets_changed" ) );
	ADD_SIGNAL( MethodInfo( "editor_execution_mode_changed", PropertyInfo( Variant::INT, "mode" ) ) );
}
