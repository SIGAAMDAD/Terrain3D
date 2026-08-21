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


Dictionary T3DTerrain::getHeightSourceDiagnostics( const Vector3 &rWorldPosition ) const {
	Dictionary diagnostics;
	const Vector2i region = m_pData ? m_pData->getRegionLocation( rWorldPosition ) : V2I_MAX;
	const t3dRegionStreamingState_t state = getRegionStreamingStateInternal( region );
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

Array T3DTerrain::getShaderTierStatsDictionaries( void ) const {
	Array tiers;
	if ( m_pMaterial.is_null() ) {
		return tiers;
	}
	const Dictionary shader_global = m_pMaterial->getShaderVariantDiagnostics();
	const real_t fNearEnd = getRuntimeNearRangeEndM();
	const real_t fMiddleEnd = getRuntimeMiddleRangeEndM();
	for ( int tier = SHADER_TIER_NEAR; tier < SHADER_TIER_COUNT; ++tier ) {
		Dictionary row = m_pMaterial->getShaderTierDiagnostics( tier );
		const real_t fRangeStart = tier == SHADER_TIER_NEAR ? 0.0f :
			( tier == SHADER_TIER_MIDDLE ? fNearEnd : fMiddleEnd );
		const real_t fRangeEnd = tier == SHADER_TIER_NEAR ? fNearEnd :
			( tier == SHADER_TIER_MIDDLE ? fMiddleEnd : -1.0f );
		row["diagnostics_version"] = TERRAIN3D_DIAGNOSTICS_VERSION;
		row["tier_name"] = tier == SHADER_TIER_NEAR ? "Near" :
			( tier == SHADER_TIER_MIDDLE ? "Middle" : "Far" );
		row["range_start_m"] = fRangeStart;
		row["range_end_m"] = fRangeEnd;
		row["range_unbounded"] = tier == SHADER_TIER_FAR;
		row["shader_rid"] = row.get( "active_shader_rid", ( int64_t ) ( 0 ) );
		row["profile_target"] = ( int ) ( m_performanceTarget );
		row["profile_name"] = m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ? "Xbox 360 Experimental" : "Custom";
		row["profile_status"] = ( int ) ( m_performanceProfileStatus );
		row["profile_status_name"] = getPerformanceProfileStatusName();
		const int filtering = ( int ) ( row.get( "texture_filtering", 0 ) );
		row["texture_filtering_name"] = filtering == ( int ) ( LINEAR_ANISOTROPIC ) ? "Linear Anisotropic" :
			( filtering == ( int ) ( LINEAR ) ? "Linear" :
				( filtering == ( int ) ( NEAREST_ANISOTROPIC ) ? "Nearest Anisotropic" : "Nearest" ) );
		const int lighting = ( int ) ( row.get( "lighting_model", 0 ) );
		row["lighting_model_name"] = lighting == ( int ) ( TIER_LIGHTING_UNSHADED ) ? "Unshaded" :
			( lighting == ( int ) ( TIER_LIGHTING_LAMBERT ) ? "Lambert" :
				( lighting == ( int ) ( TIER_LIGHTING_BLINN_PHONG ) ? "Blinn-Phong" : "PBR/GGX" ) );
		const bool bShadowEnabled = tier == SHADER_TIER_NEAR ? m_bNearShadowEnabled :
			( tier == SHADER_TIER_MIDDLE ? m_bMiddleShadowEnabled : m_bFarShadowEnabled );
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
		features["textured_surface"] = ( int ) ( row.get( "texture_surface_mode", 0 ) ) != ( int ) ( TEXTURE_SURFACE_SILHOUETTE );
		features["authored_color"] = ( double ) ( row.get( "authored_color_contribution", 0.0 ) ) > 0.0;
		features["wetness"] = ( double ) ( row.get( "wetness_contribution", 0.0 ) ) > 0.0;
		features["detiling"] = ( int ) ( row.get( "detiling_quality", 0 ) ) != ( int ) ( TIER_DETILING_DISABLED );
		features["derived_normals"] = ( int ) ( row.get( "terrain_normal_quality", 0 ) ) != ( int ) ( TERRAIN_NORMAL_QUALITY_GEOMETRY );
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
		const t3dMemorySubsystemAllocation_t &allocation = m_nMemoryBudgetResult.subsystems[( size_t ) ( index )];
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
	diagnostics["persistent_upload"] = T3DUploadBridge::getStats();
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
	m_runtimeProfileLastSample = t3dRuntimeProfileSample_t{};
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

