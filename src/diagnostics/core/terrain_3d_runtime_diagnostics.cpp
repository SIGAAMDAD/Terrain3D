#include "diagnostics/core/terrain_3d_streaming_profiler.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

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
#include "memory/arena/terrain_3d_zone_allocator.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "simd/core/terrain_3d_parallel.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "streaming/policy/terrain_3d_performance_profile.h"
#include "streaming/policy/terrain_3d_streaming_policy.h"
#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"
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
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>

Dictionary Terrain3D::getHeightSourceDiagnostics(
	const Vector3 &rWorldPosition ) const {
	Dictionary						diagnostics;
	const Vector2i					region = m_pData ? m_pData->getRegionLocation( rWorldPosition ) : s_v2iMax;
	const t3dRegionStreamingState_t state = getRegionStreamingStateInternal( region );
	const Vector3					target = getClipmapTargetPosition();
	const real_t distance = MAX( Math::abs( rWorldPosition.x - target.x ), Math::abs( rWorldPosition.z - target.z ) );
	const int	 lod_count = rangeToLodCount( distance );
	const int	 mesh_lod = MAX( 0, lod_count - 1 );
	const int	 first_far_lod = CLAMP( m_nRuntimeNearShaderLods + m_nRuntimeMiddleShaderLods, 0, 30 );
	const real_t tessellation_scale =
		( real_t ) ( ( uint64_t ) ( 1 ) << ( uint64_t ) ( CLAMP( m_nTessellationLevel, 0, 6 ) ) );
	const real_t mesh_spacing = s_fRegionVertexSpacing *
								( real_t ) ( ( uint64_t ) ( 1 ) << ( uint64_t ) ( CLAMP( mesh_lod, 0, 30 ) ) ) /
								tessellation_scale;
	const real_t far_source_spacing = s_fRegionVertexSpacing * ( real_t ) ( T3D_ACTIVE_WORLD_TEXELS ) /
									  ( real_t ) ( MAX( 1, getFarVtHeightSourceSide() ) );

	String height_source = "none";
	real_t source_spacing = 0.0f;
	if ( isEditorAuthoringModeInternal() || state == TERRAIN3D_REGION_STREAMING_ACTIVE ) {
		height_source = "near_region_height";
		source_spacing = s_fRegionVertexSpacing;
	} else if ( state == TERRAIN3D_REGION_STREAMING_HOT ) {
		height_source = "middle_height_cache";
		source_spacing = s_fRegionVertexSpacing *
						 ( real_t ) ( isEditorAuthoringModeInternal() ? 1 : getGpuMiddleHeightResolutionDivisor() );
	} else if ( state == TERRAIN3D_REGION_STREAMING_COLD ) {
		height_source = "far_height_lattice";
		source_spacing = far_source_spacing;
	}

	diagnostics["world_position"] = rWorldPosition;
	diagnostics["region"] = region;
	diagnostics["streaming_state"] = ( int ) ( state );
	diagnostics["streaming_state_name"] = String( t3dStreamingStateName( state ) );
	diagnostics["render_mesh_lod"] = mesh_lod;
	diagnostics["mesh_vertex_spacing"] = mesh_spacing;
	diagnostics["first_far_mesh_lod"] = first_far_lod;
	diagnostics["first_far_mesh_spacing"] = s_fRegionVertexSpacing *
											( real_t ) ( ( uint64_t ) ( 1 ) << ( uint64_t ) ( first_far_lod ) ) /
											tessellation_scale;
	diagnostics["height_source"] = height_source;
	diagnostics["height_source_spacing"] = source_spacing;
	diagnostics["far_vt_height_source_side"] = getFarVtHeightSourceSide();
	diagnostics["far_vt_regional_sample_side"] = getFarVtSurfaceSourceSide();
	diagnostics["surface_gpu_resident"] = m_pData && m_pData->isGpuSurfaceRegionResident( region );
	diagnostics["middle_height_resident"] = m_pData && m_pData->isMiddleHeightRegionResident( region );
	diagnostics["geometry_source_invariant_valid"] =
		state == TERRAIN3D_REGION_STREAMING_UNLOADED ||
		source_spacing <= MAX( mesh_spacing, s_fRegionVertexSpacing ) + 0.0001f;
	return diagnostics;
}

Array Terrain3D::getShaderTierStatsDictionaries(
	void ) const {
	Array tiers;
	if ( m_material.is_null() ) {
		return tiers;
	}
	Dictionary shader_global;
	if ( m_pRenderCoordinator ) {
		const Dictionary render_diagnostics = m_pRenderCoordinator->getDiagnostics();
		shader_global = render_diagnostics.get( "shader", Dictionary() );
	}
	const real_t fNearEnd = getRuntimeNearRangeEndM();
	const real_t fMiddleEnd = getRuntimeMiddleRangeEndM();
	for ( int tier = SHADER_TIER_NEAR; tier < SHADER_TIER_COUNT; ++tier ) {
		Dictionary	 row = m_material->getShaderTierDiagnostics( tier );
		const real_t fRangeStart =
			tier == SHADER_TIER_NEAR ? 0.0f : ( tier == SHADER_TIER_MIDDLE ? fNearEnd : fMiddleEnd );
		const real_t fRangeEnd =
			tier == SHADER_TIER_NEAR ? fNearEnd : ( tier == SHADER_TIER_MIDDLE ? fMiddleEnd : -1.0f );
		row["diagnostics_version"] = T3D_DIAGNOSTICS_VERSION;
		row["tier_name"] = tier == SHADER_TIER_NEAR ? "Near" : ( tier == SHADER_TIER_MIDDLE ? "Middle" : "Far" );
		row["range_start_m"] = fRangeStart;
		row["range_end_m"] = fRangeEnd;
		row["range_unbounded"] = tier == SHADER_TIER_FAR;
		row["shader_rid"] = row.get( "active_shader_rid", ( int64_t ) ( 0 ) );
		row["profile_target"] = ( int ) ( m_performanceTarget );
		row["profile_name"] =
			m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ? "Xbox 360 Experimental" : "Custom";
		row["profile_status"] = ( int ) ( m_performanceProfileStatus );
		row["profile_status_name"] = getPerformanceProfileStatusName();
		const int filtering = ( int ) ( row.get( "texture_filtering", 0 ) );
		row["texture_filtering_name"] =
			filtering == ( int ) ( LINEAR_ANISOTROPIC )
				? "Linear Anisotropic"
				: ( filtering == ( int ) ( LINEAR )
						? "Linear"
						: ( filtering == ( int ) ( NEAREST_ANISOTROPIC ) ? "Nearest Anisotropic" : "Nearest" ) );
		const int lighting = ( int ) ( row.get( "lighting_model", 0 ) );
		row["lighting_model_name"] =
			lighting == ( int ) ( TIER_LIGHTING_UNSHADED )
				? "Unshaded"
				: ( lighting == ( int ) ( TIER_LIGHTING_LAMBERT )
						? "Lambert"
						: ( lighting == ( int ) ( TIER_LIGHTING_BLINN_PHONG ) ? "Blinn-Phong" : "PBR/GGX" ) );
		const bool bShadowEnabled = tier == SHADER_TIER_NEAR
										? m_bNearShadowEnabled
										: ( tier == SHADER_TIER_MIDDLE ? m_bMiddleShadowEnabled : m_bFarShadowEnabled );
		row["shadow_participates"] = bShadowEnabled && m_nShadowLodCount > 0;
		row["render_batch_count"] = m_pTerrainMesher ? m_pTerrainMesher->getRenderBatchCountForTier( tier ) : 0;
		row["shadow_batch_count"] = m_pTerrainMesher ? m_pTerrainMesher->getShadowBatchCountForTier( tier ) : 0;
		row["physical_shader_ready"] = shader_global.get( "physical_shader_ready", ( int64_t ) ( 0 ) );
		row["physical_shader_expected"] = shader_global.get( "physical_shader_expected", ( int64_t ) ( 0 ) );
		row["shader_compile_attempted"] = shader_global.get( "shader_compile_attempted", ( int64_t ) ( 0 ) );
		row["shader_compile_failed"] = shader_global.get( "shader_compile_failed", ( int64_t ) ( 0 ) );
		row["spirv_prepare_usec_last"] = shader_global.get( "resource_spirv_prepare_usec_last", ( int64_t ) ( 0 ) );
		Dictionary features;
		features["compact_tier_active"] = row.get( "compact_tier_active", false );
		features["textured_surface"] =
			( int ) ( row.get( "texture_surface_mode", 0 ) ) != ( int ) ( TEXTURE_SURFACE_SILHOUETTE );
		features["authored_color"] = ( double ) ( row.get( "authored_color_contribution", 0.0 ) ) > 0.0;
		features["wetness"] = ( double ) ( row.get( "wetness_contribution", 0.0 ) ) > 0.0;
		features["detiling"] = ( int ) ( row.get( "detiling_quality", 0 ) ) != ( int ) ( TIER_DETILING_DISABLED );
		features["derived_normals"] =
			( int ) ( row.get( "terrain_normal_quality", 0 ) ) != ( int ) ( TERRAIN_NORMAL_QUALITY_GEOMETRY );
		row["feature_flags"] = features;
		int modeled_fetches = tier == SHADER_TIER_FAR ? 2 : 3;
		if ( ( int ) ( row.get( "material_sampling_quality", 0 ) ) != ( int ) ( MATERIAL_SAMPLING_FAST ) ) {
			++modeled_fetches;
		}
		if ( ( bool ) ( features["derived_normals"] ) ) {
			++modeled_fetches;
		}
		if ( ( int ) ( row.get( "surface_response_quality", 0 ) ) != ( int ) ( SURFACE_RESPONSE_SIMPLE ) ) {
			++modeled_fetches;
		}
		if ( ( bool ) ( features["authored_color"] ) ) {
			++modeled_fetches;
		}
		if ( !( bool ) ( features["textured_surface"] ) ) {
			modeled_fetches = MAX( 1, modeled_fetches - 1 );
		}
		row["modeled_major_texture_fetches"] = modeled_fetches;
		row["modeled_texture_fetch_basis"] =
			"Modeled major texture-resource samples; excludes filtering taps, cache behavior, and driver work.";
		tiers.push_back( row );
	}
	return tiers;
}

Array Terrain3D::getCacheInventory(
	const bool bDeep ) const {
	Array			 caches;
	const Dictionary gpu =
		m_pData ? ( bDeep ? m_pData->getGpuRegionCacheStats() : m_pData->getGpuRegionCacheStatsFast() ) : Dictionary();
	const Dictionary cpu =
		m_pData ? ( bDeep ? m_pData->getCpuRegionCacheStats() : m_pData->getCpuRegionCacheStatsFast() ) : Dictionary();
	const Dictionary instance =
		m_pData ? ( bDeep ? m_pData->getInstanceRegionCacheStats() : m_pData->getInstanceRegionCacheStatsFast() )
				: Dictionary();
	const Dictionary lightmap = m_pLightmapStreamer
									? ( bDeep ? m_pLightmapStreamer->getStats() : m_pLightmapStreamer->getStatsFast() )
									: Dictionary();
	Dictionary		 shader;
	if ( m_pRenderCoordinator ) {
		const Dictionary render_diagnostics = m_pRenderCoordinator->getDiagnostics();
		shader = render_diagnostics.get( "shader", Dictionary() );
	}
	const uint64_t current_bytes[5] = {
		m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheReservedBytes() ) : 0u,
		m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheReservedBytes() ) : 0u,
		m_pData ? ( uint64_t ) ( m_pData->getInstanceRegionCacheReservedBytes() ) : 0u,
		m_pLightmapStreamer ? ( uint64_t ) ( m_pLightmapStreamer->getResidentEstimatedBytes() ) : 0u,
		0u,
	};
	for ( int index = 0; index < 5; ++index ) {
		m_szDiagnosticCachePeakBytes[( size_t ) ( index )] =
			MAX( m_szDiagnosticCachePeakBytes[( size_t ) ( index )], current_bytes[index] );
	}
	const uint64_t hard_caps[5] = {
		m_pData ? ( uint64_t ) ( m_pData->getGpuRegionCacheHardCapBytes() ) : 128u * T3D_MEMORY_MIB,
		m_pData ? ( uint64_t ) ( m_pData->getCpuRegionCacheHardCapBytes() ) : 128u * T3D_MEMORY_MIB,
		128u * T3D_MEMORY_MIB,
		1024u * T3D_MEMORY_MIB,
		0u,
	};
	const Dictionary raw[5] = { gpu, cpu, instance, lightmap, shader };
	const int64_t	 resident[5] = { ( int64_t ) ( gpu.get( "resident", 0 ) ),
									 ( int64_t ) ( cpu.get( "resident", 0 ) ),
									 ( int64_t ) ( instance.get( "resident", 0 ) ),
									 ( int64_t ) ( lightmap.get( "active_packages", 0 ) ),
									 ( int64_t ) ( shader.get( "physical_shader_ready", 0 ) ) };
	const int64_t	 capacity[5] = { ( int64_t ) ( gpu.get( "capacity", 0 ) ),
									 ( int64_t ) ( cpu.get( "capacity", 0 ) ),
									 ( int64_t ) ( instance.get( "capacity", 0 ) ),
									 ( int64_t ) ( lightmap.get( "max_active_packages", 0 ) ),
									 ( int64_t ) ( shader.get( "physical_shader_expected", 0 ) ) };
	const int64_t	 evictions[5] = { ( int64_t ) ( m_runtimeProfileAccumulator.m_nGpuEvictionsTotal ),
									  ( int64_t ) ( m_runtimeProfileAccumulator.m_nCpuEvictionsTotal ),
									  ( int64_t ) ( m_runtimeProfileAccumulator.m_nInstanceEvictionsTotal ),
									  ( int64_t ) ( lightmap.get( "unloads_total", ( int64_t ) ( 0 ) ) ),
									  0 };
	const int64_t	 hits[5] = { ( int64_t ) ( m_runtimeProfileAccumulator.m_nGpuCookedHitsTotal ), -1, -1, -1, -1 };
	const int64_t	 misses[5] = { ( int64_t ) ( m_runtimeProfileAccumulator.m_nGpuCookedMissesTotal ),
								   -1,
								   -1,
								   -1,
								   ( int64_t ) ( shader.get( "physical_shader_misses_after_ready", ( int64_t ) ( 0 ) ) ) };
	for ( int index = 0; index < 5; ++index ) {
		const t3dMemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.m_szSubsystems[( size_t ) ( index )];
		Dictionary							  row;
		row["diagnostics_version"] = T3D_DIAGNOSTICS_VERSION;
		row["cache_name"] = s_pszMemorySubsystemNames[index];
		row["resident"] = resident[index];
		row["effective_capacity"] = capacity[index];
		row["requested_budget_bytes"] = ( int64_t ) ( allocation.m_nRequestedBytes );
		row["effective_budget_bytes"] = ( int64_t ) ( allocation.m_nEffectiveBytes );
		row["hard_maximum_bytes"] = ( int64_t ) ( hard_caps[index] );
		row["limiting_constraint"] = memoryLimitReasonName( allocation.m_limitingReason );
		row["current_bytes"] = ( int64_t ) ( current_bytes[index] );
		row["peak_bytes"] = ( int64_t ) ( m_szDiagnosticCachePeakBytes[( size_t ) ( index )] );
		row["hits"] = hits[index];
		row["misses"] = misses[index];
		row["hit_rate"] = hits[index] >= 0 && misses[index] >= 0 && hits[index] + misses[index] > 0
							  ? ( double ) ( hits[index] ) / ( double ) ( hits[index] + misses[index] )
							  : -1.0;
		row["evictions"] = evictions[index];
		row["queued"] =
			index == 0
				? ( int64_t ) ( gpu.get( "requested_pages", 0 ) )
				: ( index == 1
						? ( int64_t ) ( cpu.get( "selection_pending", false ) ? 1 : 0 )
						: ( index == 2 ? ( int64_t ) ( instance.get( "selection_pending", false ) ? 1 : 0 )
									   : ( index == 3 ? ( int64_t ) ( lightmap.get( "pending_packages", 0 ) ) : 0 ) ) );
		row["preparing"] = index == 0 ? ( int64_t ) ( gpu.get( "prepare_job_pending_pages", 0 ) ) : 0;
		row["uploading"] =
			index == 0
				? ( int64_t ) ( gpu.get( "uploads_last_tick", 0 ) )
				: ( index == 1
						? ( int64_t ) ( cpu.get( "loads_last_tick", 0 ) )
						: ( index == 2 ? ( int64_t ) ( instance.get( "loads_last_tick", 0 ) )
									   : ( index == 3 ? ( int64_t ) ( lightmap.get( "loads_last_tick", 0 ) ) : 0 ) ) );
		row["load_upload_bytes_last"] =
			index == 0
				? ( int64_t ) ( gpu.get( "upload_bytes_last_tick", ( int64_t ) ( 0 ) ) )
				: ( index == 1
						? ( int64_t ) ( cpu.get( "load_bytes_last_tick", ( int64_t ) ( 0 ) ) )
						: ( index == 2 ? ( int64_t ) ( instance.get( "load_bytes_last_tick", ( int64_t ) ( 0 ) ) )
									   : 0 ) );
		row["raw"] = raw[index];
		caches.push_back( row );
	}
	return caches;
}

Dictionary Terrain3D::getCpuProfileStats(
	void ) const {
	Dictionary cpu;
	cpu["gpu_page_cache_service_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nGpuCacheServiceUsec );
	cpu["gpu_page_preparation_worker_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nGpuPrepareWorkerUsec );
	cpu["rendering_server_publication_cpu_usec"] =
		( int64_t ) ( m_runtimeProfileLastSample.m_nGpuPreparePublicationUsec );
	cpu["cpu_cache_service_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nCpuCacheServiceUsec );
	cpu["instance_cache_service_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nInstanceCacheServiceUsec );
	cpu["instance_streaming_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nInstancerUsec );
	cpu["node_streaming_migration_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nNodeStreamingUsec );
	cpu["baked_lighting_streaming_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nLightmapStreamingUsec );
	cpu["collision_rebuild_cpu_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nCollisionUsec );
	const Dictionary gpu = m_pData ? m_pData->getGpuRegionCacheStatsFast() : Dictionary();
	cpu["far_height_rebuild_cpu_usec"] = gpu.get( "far_height_processing_usec_last", ( int64_t ) ( 0 ) );
	cpu["far_color_rebuild_cpu_usec"] = gpu.get( "far_color_processing_usec_last", ( int64_t ) ( 0 ) );
	cpu["far_height_publication_cpu_usec"] = gpu.get( "far_height_publication_usec_last", ( int64_t ) ( 0 ) );
	cpu["far_color_publication_cpu_usec"] = gpu.get( "far_color_publication_usec_last", ( int64_t ) ( 0 ) );
	cpu["profiler_collection_cpu_usec_last"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecLast );
	cpu["profiler_collection_cpu_usec_average"] =
		( int64_t ) ( m_nDiagnosticsCollectionCount > 0u
						  ? m_nDiagnosticsCollectionUsecTotal / m_nDiagnosticsCollectionCount
						  : 0u );
	cpu["profiler_collection_cpu_usec_peak"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecPeak );
	cpu["profiler_collection_count"] = ( int64_t ) ( m_nDiagnosticsCollectionCount );
	return cpu;
}

Dictionary Terrain3D::getGpuInformationStats(
	void ) const {
	Dictionary info;
	Dictionary measured;
	measured["classification"] = "Measured";
	measured["available"] = false;
	measured["terrain_gpu_time_usec"] = -1;
	measured["note"] =
		"No isolated Terrain3D GPU timestamp scope is exposed by the current compatibility renderer path.";
	info["measured"] = measured;
	Dictionary modeled;
	modeled["classification"] = "Modeled";
	modeled["render_batches"] = m_pTerrainMesher ? m_pTerrainMesher->getRenderBatchCount() : 0;
	modeled["shadow_batches"] = m_pTerrainMesher ? m_pTerrainMesher->getShadowBatchCount() : 0;
	modeled["tier_shaders"] = getShaderTierStatsDictionaries();
	modeled["upload_bytes_last_tick"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nGpuUploadBytes );
	modeled["note"] =
		"Terrain geometry, batches, feature/sample model, and upload traffic; these are not GPU milliseconds.";
	info["modeled"] = modeled;
	Dictionary scene_wide;
	scene_wide["classification"] = "Scene-wide";
	scene_wide["note"] = "Populate from Godot Performance monitors in the editor profiler; values cover the whole "
						 "scene, not Terrain3D alone.";
	info["scene_wide"] = scene_wide;
	return info;
}

Dictionary Terrain3D::getRuntimeParitySnapshot(
	void ) const {
	Dictionary snapshot;
	snapshot["native_api_revision"] = ( int64_t ) ( T3D_NATIVE_API_REVISION );
	snapshot["native_build_id"] = String( s_pszNativeBuildId );
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

	const bool legacy_material_arrays_active = m_assets.is_valid() && m_assets->getAlbedoArrayRid().is_valid();
	snapshot["legacy_material_arrays_active"] = legacy_material_arrays_active;
	snapshot["vt_v1_material_residency_authoritative"] = m_assets.is_valid() && m_assets->usesPackedPbr() &&
														 m_bGpuRegionCookedPagesEnabled &&
														 !isEditorAuthoringModeInternal();
	return snapshot;
}

Ref< Terrain3DMemoryStats > Terrain3D::getMemoryStatsSnapshot(
	void ) const {
	Ref< Terrain3DMemoryStats > pStats;
	pStats.instantiate();
	pStats->setValues( getRuntimeMemoryStatsFast() );
	return pStats;
}

TypedArray< Terrain3DShaderTierStats > Terrain3D::getShaderTierStatsSnapshot(
	void ) const {
	TypedArray< Terrain3DShaderTierStats > output;
	const Array							   values = getShaderTierStatsDictionaries();
	for ( int index = 0; index < values.size(); ++index ) {
		Ref< Terrain3DShaderTierStats > pStats;
		pStats.instantiate();
		pStats->setValues( values[index] );
		output.push_back( pStats );
	}
	return output;
}

TypedArray< Terrain3DCacheStats > Terrain3D::getCacheStatsSnapshot(
	const bool bDeep ) const {
	TypedArray< Terrain3DCacheStats > output;
	const Array						  values = getCacheInventory( bDeep );
	for ( int index = 0; index < values.size(); ++index ) {
		Ref< Terrain3DCacheStats > pStats;
		pStats.instantiate();
		pStats->setValues( values[index] );
		output.push_back( pStats );
	}
	return output;
}

Ref< Terrain3DFrameStats > Terrain3D::getFrameStatsSnapshot(
	void ) const {
	Ref< Terrain3DFrameStats > pStats;
	pStats.instantiate();
	Dictionary values = runtimeProfileSampleToDictionary( m_runtimeProfileLastSample );
	values["diagnostics_version"] = T3D_DIAGNOSTICS_VERSION;
	values["profiler_collection_usec"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecLast );
	pStats->setValues( values );
	return pStats;
}

void Terrain3D::setStreamingProfilerEnabled(
	const bool bEnabled,
	const int  nSampleEvery ) {
	Terrain3DStreamingProfiler::get().setEnabled( bEnabled, ( uint32_t ) ( nSampleEvery > 0 ? nSampleEvery : 1 ) );
}

void Terrain3D::resetStreamingProfiler(
	void ) {
	Terrain3DStreamingProfiler::get().reset();
}

Dictionary Terrain3D::getStreamingProfilerStats(
	void ) const {
	const Terrain3DStreamingProfiler &profiler = Terrain3DStreamingProfiler::get();
	Dictionary						  output;
	output["enabled"] = profiler.enabled();
	output["sample_every"] = ( int64_t ) profiler.sampleEvery();
	output["timing_clock"] = "steady_clock";
	output["timing_unit"] = "microseconds";
	output["gpu_execution_timing"] = false;
	output["scope"] = "CPU work and GPU submission only; no device synchronization";
	Dictionary stages;
	for ( uint32_t i = 0u; i < T3D_SP_COUNT; ++i ) {
		const auto								   stage = static_cast< t3dStreamProfileStage_t >( i );
		const Terrain3DStreamingProfiler::Snapshot sample = profiler.snapshot( stage );
		Dictionary								   values;
		values["calls"] = ( int64_t ) sample.calls;
		values["timed_samples"] = ( int64_t ) sample.samples;
		values["total_usec_sampled"] = ( double ) sample.totalNanoseconds / 1000.0;
		values["avg_usec_sampled"] =
			sample.samples ? ( double ) sample.totalNanoseconds / ( 1000.0 * ( double ) sample.samples ) : 0.0;
		values["peak_usec_sampled"] = ( double ) sample.maxNanoseconds / 1000.0;
		values["bytes"] = ( int64_t ) sample.bytes;
		values["work_units"] = ( int64_t ) sample.workUnits;
		values["work_unit_name"] = Terrain3DStreamingProfiler::workUnitName( stage );
		values["events"] = ( int64_t ) sample.events;
		values["event_name"] = Terrain3DStreamingProfiler::eventName( stage );
		// Percentiles are conservative upper bounds from exponentially sized microsecond bins.
		uint64_t	   cumulative = 0u;
		uint64_t	   p50 = 0u, p95 = 0u, p99 = 0u;
		const uint64_t c50 = ( sample.samples * 50u + 99u ) / 100u;
		const uint64_t c95 = ( sample.samples * 95u + 99u ) / 100u;
		const uint64_t c99 = ( sample.samples * 99u + 99u ) / 100u;
		for ( uint32_t j = 0u; j < Terrain3DStreamingProfiler::HISTOGRAM_BUCKETS; ++j ) {
			cumulative += sample.histogram[j];
			const uint64_t bound = 1ull << j;
			if ( !p50 && cumulative >= c50 && c50 ) {
				p50 = bound;
			}
			if ( !p95 && cumulative >= c95 && c95 ) {
				p95 = bound;
			}
			if ( !p99 && cumulative >= c99 && c99 ) {
				p99 = bound;
			}
		}
		values["p50_usec_upper"] = ( int64_t ) p50;
		values["p95_usec_upper"] = ( int64_t ) p95;
		values["p99_usec_upper"] = ( int64_t ) p99;
		Array histogram;
		for ( uint32_t j = 0u; j < Terrain3DStreamingProfiler::HISTOGRAM_BUCKETS; ++j ) {
			histogram.push_back( ( int64_t ) sample.histogram[j] );
		}
		values["log2_usec_histogram"] = histogram;
		stages[Terrain3DStreamingProfiler::name( stage )] = values;
	}
	output["stages"] = stages;
	return output;
}

Dictionary Terrain3D::getRuntimeDiagnostics(
	const bool bDeep ) const {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_H08_RUNTIME_DIAGNOSTICS );
	const uint64_t collection_started_usec = Time::get_singleton()->get_ticks_usec();
	Dictionary	   diagnostics;
	diagnostics["diagnostics_version"] = T3D_DIAGNOSTICS_VERSION;
	diagnostics["deep_introspection"] = bDeep;
	diagnostics["profiling_enabled"] = m_bRuntimeProfilingEnabled;
	if ( Terrain3DStreamingProfiler::get().enabled() ) {
		diagnostics["streaming_profile"] = getStreamingProfilerStats();
	}
	diagnostics["sample_count"] = ( int64_t ) ( m_nRuntimeProfileSampleCount );
	diagnostics["execution_mode"] = ( int ) ( m_editorExecutionMode );
	diagnostics["performance_target"] = ( int ) ( m_performanceTarget );
	diagnostics["performance_profile_status"] = ( int ) ( m_performanceProfileStatus );
	diagnostics["performance_profile_status_name"] = getPerformanceProfileStatusName();
	diagnostics["performance_profile_version"] = ( int ) ( T3D_PROFILE_VERSION );
	diagnostics["performance_profile_signature"] = getPerformanceProfileSignature();
	diagnostics["performance_profile_diff_count"] = getPerformanceProfileDiff().size();
	diagnostics["adaptive_budgets"] = getRuntimeAdaptiveBudgetStats();
	diagnostics["boundary_phasing"] = getRuntimeBoundaryPhasingStats();
	diagnostics["region_streaming"] = getRegionStreamingStats();
	diagnostics["region_streaming_configuration"] = getRegionStreamingConfigurationStatus();
	diagnostics["memory"] = bDeep ? getMemoryEstimate() : getRuntimeMemoryStatsFast();
	diagnostics["memory_tracker"] = getMemoryTrackerStats();
	Dictionary authoring_texture_residency;
	authoring_texture_residency["release_requested"] = m_bFreeEditorTextures;
	authoring_texture_residency["runtime_metadata_only"] = m_assets.is_valid() && m_assets->hasRuntimeMaterialView();
	authoring_texture_residency["logical_material_count"] = m_assets.is_valid() ? m_assets->getTextureCount() : 0;
	authoring_texture_residency["released_authoring_references"] =
		m_assets.is_valid() ? ( int64_t ) m_assets->getRuntimeReleasedAuthoringReferenceCount() : 0;
	diagnostics["authoring_texture_residency"] = authoring_texture_residency;

	Dictionary startup;
	startup["generation"] = ( int64_t ) ( m_nStartupGeneration );
	startup["attempt_count"] = ( int64_t ) ( m_nStartupAttemptCount );
	startup["cancellation_count"] = ( int64_t ) ( m_nStartupCancellationCount );
	startup["generation_invalidation_count"] = ( int64_t ) ( m_nStartupGenerationInvalidationCount );
	startup["lifecycle_state"] = ( int64_t ) ( m_runtimeLifecycleState );
	startup["start_usec"] = ( int64_t ) ( m_nStartupStartUsec );
	startup["end_usec"] = ( int64_t ) ( m_nStartupEndUsec );
	startup["total_usec_last"] = ( int64_t ) ( m_nStartupTotalUsecLast );
	startup["data_initialize_usec_last"] = ( int64_t ) ( m_nStartupDataInitializeUsecLast );
	startup["owner_prepare_usec_last"] = ( int64_t ) ( m_nStartupOwnerPrepareUsecLast );
	startup["coordinator_initialize_usec_last"] = ( int64_t ) ( m_nStartupCoordinatorInitializeUsecLast );
	startup["owner_finalize_usec_last"] = ( int64_t ) ( m_nStartupOwnerFinalizeUsecLast );
	startup["succeeded_last"] = m_bStartupSucceededLast;
	Dictionary						 finalization;
	const t3dStartupDomainSnapshot_t finalizationSnapshot = t3dStartupDomainSnapshot( m_startupFinalizationState );
	const t3dStartupStage_t			 eFinalizationStage = t3dStartupDomainGetStage( finalizationSnapshot );
	finalization["stage"] = ( int64_t ) ( eFinalizationStage );
	finalization["stage_name"] = String( t3dStartupStageName( eFinalizationStage ) );
	finalization["generation"] = ( int64_t ) ( finalizationSnapshot.m_nGeneration );
	finalization["completed_units"] = ( int64_t ) ( finalizationSnapshot.m_nCompletedUnits );
	finalization["total_units"] = ( int64_t ) ( finalizationSnapshot.m_nTotalUnits );
	finalization["start_usec"] = ( int64_t ) ( finalizationSnapshot.m_nStartUsec );
	finalization["end_usec"] = ( int64_t ) ( finalizationSnapshot.m_nEndUsec );
	startup["finalization"] = finalization;
	const uint64_t nAccountedStartupUsec = m_nStartupDataInitializeUsecLast + m_nStartupOwnerPrepareUsecLast +
										   m_nStartupCoordinatorInitializeUsecLast + m_nStartupOwnerFinalizeUsecLast;
	startup["accounted_usec_last"] = ( int64_t ) ( nAccountedStartupUsec );
	startup["unaccounted_usec_last"] =
		( int64_t ) ( m_nStartupTotalUsecLast > nAccountedStartupUsec ? m_nStartupTotalUsecLast - nAccountedStartupUsec
																	  : 0u );
	const Dictionary dataStartup = m_pData ? m_pData->getStartupLoadStats() : Dictionary();
	const Dictionary coordinatorStartup = m_pRenderCoordinator
											  ? ( bDeep ? m_pRenderCoordinator->getStartupDiagnostics()
														: m_pRenderCoordinator->getPublishedStartupDiagnostics() )
											  : Dictionary();
	const Dictionary shaderStartup = coordinatorStartup.get( "shader", Dictionary() );
	const Dictionary vtStartup = coordinatorStartup.get( "virtual_texture", Dictionary() );
	startup["startup_generation"] = ( int64_t ) ( m_nStartupGeneration );
	startup["startup_state"] = String( t3dStartupStageName( eFinalizationStage ) );
	startup["shader_stage"] = shaderStartup.get( "startup_stage_name", "IDLE" );
	startup["shader_completed"] = shaderStartup.get( "shader_completed", 0 );
	startup["shader_total"] = shaderStartup.get( "shader_total", 0 );
	startup["shader_cpu_ready"] = shaderStartup.get( "shader_cpu_ready", false );
	startup["shader_gpu_ready"] = shaderStartup.get( "shader_gpu_ready", false );
	startup["shader_elapsed_ms"] = shaderStartup.get( "shader_elapsed_ms", 0.0 );
	startup["vt_stage"] = vtStartup.get( "startup_stage_name", "IDLE" );
	startup["vt_cpu_ready"] = vtStartup.get( "vt_cpu_ready", false );
	startup["vt_storage_ready"] = vtStartup.get( "vt_storage_ready", false );
	startup["vt_gpu_ready"] = vtStartup.get( "vt_gpu_ready", false );
	startup["vt_cpu_bytes"] = vtStartup.get( "vt_cpu_bytes", 0 );
	startup["vt_gpu_bytes"] = vtStartup.get( "vt_gpu_bytes", 0 );
	startup["vt_elapsed_ms"] = vtStartup.get( "vt_elapsed_ms", 0.0 );
	startup["terrain_data_stage"] = dataStartup.get( "startup_stage_name", "IDLE" );
	startup["terrain_data_region_count"] = dataStartup.get( "startup_region_count", 0 );
	startup["terrain_data_bytes_read"] = dataStartup.get( "startup_bytes_read", 0 );
	startup["terrain_data_ready"] = dataStartup.get( "startup_ready", false );
	startup["terrain_data_elapsed_ms"] = dataStartup.get( "startup_elapsed_ms", 0.0 );
	startup["final_sync_attempts"] = coordinatorStartup.get( "final_sync_attempts", 0 );
	startup["final_sync_successes"] = coordinatorStartup.get( "final_sync_successes", 0 );
	startup["final_sync_failures"] = coordinatorStartup.get( "final_sync_failures", 0 );
	startup["final_sync_elapsed_ms"] = coordinatorStartup.get( "final_sync_elapsed_ms", 0.0 );
	startup["render_lifecycle_stage"] = coordinatorStartup.get( "render_lifecycle_stage", 0 );
	startup["render_lifecycle_failure_code"] = coordinatorStartup.get( "render_lifecycle_failure_code", 0 );
	startup["render_lifecycle_recovery_revision"] = coordinatorStartup.get( "render_lifecycle_recovery_revision", 0 );
	startup["data"] = dataStartup;
	startup["render_coordinator"] = coordinatorStartup;
	diagnostics["startup"] = startup;

	Dictionary frame;
	frame["sequence"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nFrameSequence );
	frame["physics_total_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nPhysicsTotalUsec );
	frame["data_residency_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nDataResidencyUsec );
	frame["node_streaming_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nNodeStreamingUsec );
	frame["lightmap_streaming_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nLightmapStreamingUsec );
	frame["clipmap_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nClipmapUsec );
	frame["ocean_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nOceanUsec );
	frame["instancer_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nInstancerUsec );
	frame["light_sync_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nLightSyncUsec );
	frame["collision_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nCollisionUsec );
	frame["streaming_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nStreamingUsec );
	frame["gpu_cache_service_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nGpuCacheServiceUsec );
	frame["cpu_cache_service_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nCpuCacheServiceUsec );
	frame["instance_cache_service_usec"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nInstanceCacheServiceUsec );
	frame["target_position"] = Vector2( m_runtimeProfileLastSample.m_fTargetX, m_runtimeProfileLastSample.m_fTargetZ );
	frame["movement"] = Vector2( m_runtimeProfileLastSample.m_fMovementX, m_runtimeProfileLastSample.m_fMovementZ );
	frame["target_region"] =
		Vector2i( m_runtimeProfileLastSample.m_nTargetRegionX, m_runtimeProfileLastSample.m_nTargetRegionZ );
	frame["events"] = getRuntimeProfileEventNames( m_runtimeProfileLastSample.m_nEventFlags );
	frame["stall_reasons"] = getRuntimeProfileStallNames( m_runtimeProfileLastSample.m_nStallFlags );
	diagnostics["frame"] = frame;
	diagnostics["problems"] = getRuntimeProfileProblems();
	diagnostics["history_capacity"] = ( int64_t ) ( T3D_RUNTIME_PROFILE_HISTORY_CAPACITY );
	diagnostics["history_count"] = ( int64_t ) ( m_pRuntimeProfileHistory ? m_pRuntimeProfileHistory->m_nCount : 0u );

	Dictionary clipmap;
	clipmap["near_updates"] = m_runtimeProfileLastSample.m_nClipmapNearUpdates;
	clipmap["middle_updates"] = m_runtimeProfileLastSample.m_nClipmapMiddleUpdates;
	clipmap["far_updates"] = m_runtimeProfileLastSample.m_nClipmapFarUpdates;
	clipmap["teleports"] = m_runtimeProfileLastSample.m_nClipmapTeleports;
	clipmap["buffer_uploads"] = m_runtimeProfileLastSample.m_nClipmapBufferUploads;
	clipmap["mesh_lods"] = m_nMeshLods;
	clipmap["tessellation_level"] = m_nTessellationLevel;
	clipmap["total_levels"] = getTotalClipmapLevels();
	clipmap["mesh_size"] = m_nMeshSize;
	clipmap["vertex_spacing"] = s_fRegionVertexSpacing;
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
	nodes["active_regions"] = m_runtimeProfileLastSample.m_nNodeActiveRegions;
	nodes["active_nodes"] = m_runtimeProfileLastSample.m_nNodeActiveNodes;
	nodes["loads_last_tick"] = m_runtimeProfileLastSample.m_nNodeLoads;
	nodes["unloads_last_tick"] = m_runtimeProfileLastSample.m_nNodeUnloads;
	nodes["load_failures_last_tick"] = m_runtimeProfileLastSample.m_nNodeLoadFailures;
	nodes["enabled"] = m_bDynamicNodeStreamingEnabled;
	nodes["effective"] = effectiveDynamicNodeStreamingEnabled();
	nodes["stream_radius"] = m_nDynamicNodeStreamRadius;
	nodes["load_region_budget"] = m_nDynamicNodeLoadRegionBudget;
	nodes["load_node_budget"] = m_nDynamicNodeLoadNodeBudget;
	nodes["unload_region_budget"] = m_nDynamicNodeUnloadRegionBudget;
	diagnostics["nodes"] = nodes;

	Dictionary baked_lighting = m_pLightmapStreamer
									? ( bDeep ? m_pLightmapStreamer->getStats() : m_pLightmapStreamer->getStatsFast() )
									: Dictionary();
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
	instancer["render_cells"] = m_runtimeProfileLastSample.m_nRenderCells;
	instancer["pending_cell_updates"] = m_runtimeProfileLastSample.m_nPendingCellUpdates;
	instancer["cell_size"] = m_nInstancerCellSize;
	instancer["streaming_enabled"] = m_bInstancerCellStreamingEnabled;
	instancer["streaming_effective"] = effectiveInstancerCellStreamingEnabled();
	instancer["single_lod_buffers"] = m_bInstancerSingleLodBuffers;
	instancer["streaming_margin"] = m_fInstancerCellStreamingMargin;
	instancer["streaming_update_distance"] = m_fInstancerCellStreamingUpdateDistance;
	instancer["update_cell_budget"] = m_nInstancerUpdateCellBudget;
	diagnostics["instancer"] = instancer;
	if ( m_pInstanceOcclusion ) {
		diagnostics["instance_occlusion"] = m_pInstanceOcclusion->getStats();
	}
	if ( m_pInstancePhysics ) {
		diagnostics["instance_physics"] = m_pInstancePhysics->getStats();
	}

	if ( m_pCollision ) {
		Dictionary collision = bDeep ? m_pCollision->getStats() : Dictionary();
		collision["enabled"] = m_pCollision->isEnabled();
		collision["update_usec_last"] = ( int64_t ) ( m_runtimeProfileLastSample.m_nCollisionUsec );
		diagnostics["collision"] = collision;
	}
	if ( m_pRenderCoordinator ) {
		const Dictionary render_diagnostics = m_pRenderCoordinator->getDiagnostics();
		diagnostics["shaders"] = render_diagnostics.get( "shader", Dictionary() );
		// Published, synchronization-free renderer telemetry for streaming acceptance.
		diagnostics["streaming_renderer"] = render_diagnostics;
	}

	Dictionary configuration;
	configuration["region_size"] = ( int ) ( T3D_REGION_SIZE );
	configuration["world_region_size_m"] = ( real_t ) ( T3D_REGION_SIZE ) *s_fRegionVertexSpacing;
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
		diagnostics["instance_cache"] =
			bDeep ? m_pData->getInstanceRegionCacheStats() : m_pData->getInstanceRegionCacheStatsFast();
		diagnostics["startup_load"] = m_pData->getStartupLoadStats();
	}
	Terrain3DRdTextureManager *const pTextureManager = Terrain3DRdTextureManager::getSingleton();
	diagnostics["persistent_upload"] = pTextureManager ? pTextureManager->getStats() : Dictionary();
	diagnostics["aggregate"] = getRuntimeProfileAggregate();
	diagnostics["active_render_pipeline"] = getShaderTierStatsDictionaries();
	diagnostics["cache_inventory"] = getCacheInventory( bDeep );
	diagnostics["gpu_information"] = getGpuInformationStats();

	const uint64_t collection_finished_usec = Time::get_singleton()->get_ticks_usec();
	m_nDiagnosticsCollectionUsecLast =
		collection_finished_usec >= collection_started_usec ? collection_finished_usec - collection_started_usec : 0u;
	m_nDiagnosticsCollectionUsecTotal += m_nDiagnosticsCollectionUsecLast;
	m_nDiagnosticsCollectionUsecPeak = MAX( m_nDiagnosticsCollectionUsecPeak, m_nDiagnosticsCollectionUsecLast );
	++m_nDiagnosticsCollectionCount;
	diagnostics["cpu_profile"] = getCpuProfileStats();
	Dictionary overhead;
	overhead["last_usec"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecLast );
	overhead["average_usec"] =
		( int64_t ) ( m_nDiagnosticsCollectionUsecTotal / MAX( ( uint64_t ) ( 1u ), m_nDiagnosticsCollectionCount ) );
	overhead["peak_usec"] = ( int64_t ) ( m_nDiagnosticsCollectionUsecPeak );
	overhead["collection_count"] = ( int64_t ) ( m_nDiagnosticsCollectionCount );
	overhead["deep_introspection"] = bDeep;
	overhead["deep_scan_note"] =
		bDeep ? "Deep mode includes O(N) node, instancer, collision, and detailed cache introspection."
			  : "Default mode uses bounded/incremental subsystem counters and omits deep collection scans.";
	diagnostics["profiler_overhead"] = overhead;
	T3D_STREAM_PROFILE_WORK( T3D_SP_H08_RUNTIME_DIAGNOSTICS, ( uint64_t ) diagnostics.size() );
	return diagnostics;
}

void Terrain3D::resetRuntimeDiagnostics(
	void ) {
	m_runtimeProfileLastSample = t3dRuntimeProfileSample_t{};
	m_nRuntimeProfileSampleCount = 0u;
	m_runtimeProfileAccumulator.reset();
	m_nDiagnosticsCollectionUsecLast = 0u;
	m_nDiagnosticsCollectionUsecTotal = 0u;
	m_nDiagnosticsCollectionUsecPeak = 0u;
	m_nDiagnosticsCollectionCount = 0u;
	memset( m_szDiagnosticCachePeakBytes.data(),
			0,
			sizeof( m_szDiagnosticCachePeakBytes[0] ) * m_szDiagnosticCachePeakBytes.size() );
	if ( m_pRuntimeProfileHistory ) {
		m_pRuntimeProfileHistory->reset();
	}
	m_nRuntimeAdaptiveBudgetState.m_nOverBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.m_nUnderBudgetStreak = 0;
	m_nRuntimeAdaptiveBudgetState.m_nAdjustmentCount = 0u;
	m_nRuntimeBoundaryNodeDeferralsTotal = 0u;
	m_nRuntimeBoundaryInstancerDeferralsTotal = 0u;
	m_nRuntimeBoundaryNodeDeferralStreak = 0;
	m_nRuntimeBoundaryInstancerDeferralStreak = 0;
}
