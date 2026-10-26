// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "integration/godot/terrain_3d_binding.h"
#include "Terrain3DConstants.h"
#include "core/utility/logger.h"
#include "core/utility/terrain_3d_integration_helpers.h"
#include "core/utility/terrain_3d_util.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "lighting/policy/terrain_3d_lighting_policy.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "memory/arena/terrain_3d_zone_allocator.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "simd/core/terrain_3d_parallel.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "streaming/policy/terrain_3d_performance_profile.h"
#include "streaming/policy/terrain_3d_streaming_policy.h"
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
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>

void Terrain3D::setWarning(
	const uint8_t nWarning,
	const bool	  bEnabled ) {
	if ( bEnabled ) {
		m_nWarnings |= nWarning;
	} else {
		m_nWarnings &= ~nWarning;
	}
	update_configuration_warnings();
}

PackedStringArray Terrain3D::_get_configuration_warnings(
	void ) const {
	PackedStringArray psa;
	if ( m_dataDirectory.is_empty() ) {
		psa.push_back( "No data directory specified. Select a directory then save the scene to write data." );
	}
	if ( m_nWarnings & WARN_MISMATCHED_SIZE ) {
		psa.push_back(
			"Texture dimensions don't match. Double-click a texture in the FileSystem panel to see its size. "
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

bool Terrain3D::_set(
	const StringName &rName,
	const Variant	 &rProperty ) {
	return Node3D::_set( rName, rProperty );
}

bool Terrain3D::_get(
	const StringName &rName,
	Variant			 &rProperty ) const {
	return Node3D::_get( rName, rProperty );
}

void Terrain3D::_validate_property(
	PropertyInfo &rProperty ) const {
	t3dApplyPropertyPrefixUsage( rProperty, "ocean_", "ocean_enabled", !m_bOceanEnabled );
	t3dApplyPropertyUsageRules(
		rProperty,
		{
			{ "displacement_scale", m_nTessellationLevel == 0, PROPERTY_USAGE_NO_EDITOR },
			{ "displacement_sharpness", m_nTessellationLevel == 0, PROPERTY_USAGE_NO_EDITOR },
			{ "gpu_region_cache_budget_mb", m_bGpuRegionCacheBudgetAuto, PROPERTY_USAGE_READ_ONLY, true },
			{ "cpu_region_cache_budget_mb", m_bCpuRegionCacheBudgetAuto, PROPERTY_USAGE_READ_ONLY, true },
			{ "instance_region_cache_budget_mb", m_bInstanceRegionCacheBudgetAuto, PROPERTY_USAGE_READ_ONLY, true },
			{ "lightmap_residency_budget_mb", m_bLightmapResidencyBudgetAuto, PROPERTY_USAGE_READ_ONLY, true },
			{ "hlod_memory_budget_mb", m_bHlodMemoryBudgetAuto, PROPERTY_USAGE_READ_ONLY, true },
		} );
}

void Terrain3D::_bind_methods(
	void ) {
	BIND_ENUM_CONSTANT( ENVIRONMENT_SHADOW_LOW );
	BIND_ENUM_CONSTANT( ENVIRONMENT_SHADOW_NORMAL );
	BIND_ENUM_CONSTANT( ENVIRONMENT_SHADOW_HIGH );
	BIND_ENUM_CONSTANT( ENVIRONMENT_SHADOW_ULTRA );
	BIND_ENUM_CONSTANT( ERROR );
	BIND_ENUM_CONSTANT( INFO );
	BIND_ENUM_CONSTANT( DEBUG );
	BIND_ENUM_CONSTANT( EXTREME );

	//	BIND_ENUM_CONSTANT( SIZE_64 );
	BIND_ENUM_CONSTANT( SIZE_128 );
	//	BIND_ENUM_CONSTANT( SIZE_256 );
	//	BIND_ENUM_CONSTANT( SIZE_512 );
	//	BIND_ENUM_CONSTANT( SIZE_1024 );
	//	BIND_ENUM_CONSTANT( SIZE_2048 );
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

	ClassDB::bind_method( D_METHOD( "get_version" ), &Terrain3D::getVersion );
	ClassDB::bind_method( D_METHOD( "get_native_api_revision" ), &Terrain3D::getNativeApiRevision );
	ClassDB::bind_method( D_METHOD( "get_native_build_id" ), &Terrain3D::getNativeBuildId );
	ClassDB::bind_method( D_METHOD( "get_native_build_flavor" ), &Terrain3D::getNativeBuildFlavor );
	ClassDB::bind_method( D_METHOD( "is_initialized" ), &Terrain3D::isInitialized );
    ClassDB::bind_method( D_METHOD( "set_environment_forcing", "rain", "snow", "dry", "wind", "gust", "delta_time", "enabled" ),
                          &Terrain3D::setEnvironmentForcing );
    ClassDB::bind_method( D_METHOD( "submit_environment_impulses", "packed_impulses" ),
                          &Terrain3D::submitEnvironmentImpulses );
    ClassDB::bind_method( D_METHOD( "get_environment_dropped_impulses" ),
                          &Terrain3D::getEnvironmentDroppedImpulses );
    ClassDB::bind_method( D_METHOD( "set_environment_region_climate", "region", "climate" ),
                          &Terrain3D::setEnvironmentRegionClimate );
    ClassDB::bind_method( D_METHOD( "get_environment_region_climate", "region" ),
                          &Terrain3D::getEnvironmentRegionClimate );
    ClassDB::bind_method( D_METHOD( "export_environment_region_climate" ),
                          &Terrain3D::exportEnvironmentRegionClimate );
    ClassDB::bind_method( D_METHOD( "import_environment_region_climate", "data" ),
                          &Terrain3D::importEnvironmentRegionClimate );


	ClassDB::bind_method( D_METHOD( "set_editor_node_streaming_preview", "enabled" ),
						  &Terrain3D::setEditorNodeStreamingPreview );
	ClassDB::bind_method( D_METHOD( "is_editor_node_streaming_preview_enabled" ),
						  &Terrain3D::isEditorNodeStreamingPreviewEnabled );
	ClassDB::bind_method( D_METHOD( "is_editor_node_streaming_preview_active" ),
						  &Terrain3D::isEditorNodeStreamingPreviewActive );
	ClassDB::bind_method( D_METHOD( "set_editor_hlod_preview", "enabled" ), &Terrain3D::setEditorHlodPreview );
	ClassDB::bind_method( D_METHOD( "is_editor_hlod_preview_enabled" ), &Terrain3D::isEditorHlodPreviewEnabled );
	ClassDB::bind_method( D_METHOD( "is_editor_hlod_preview_active" ), &Terrain3D::isEditorHlodPreviewActive );
	ClassDB::bind_method( D_METHOD( "get_memory_estimate" ), &Terrain3D::getMemoryEstimate );
	ClassDB::bind_method( D_METHOD( "get_tracked_current_residency" ), &Terrain3D::getTrackedCurrentResidency );
	ClassDB::bind_method( D_METHOD( "get_memory_budget_stats" ), &Terrain3D::getMemoryBudgetStats );
	ClassDB::bind_method( D_METHOD( "get_memory_tracker_stats" ), &Terrain3D::getMemoryTrackerStats );
	ClassDB::bind_method(
		D_METHOD( "set_editor_authoring_memory_profile_mb", "total_mb", "transient_mb", "cooking_mb" ),
		&Terrain3D::setEditorAuthoringMemoryProfileMb );

	ClassDB::bind_method( D_METHOD( "get_effective_gpu_region_cache_budget_mb" ),
						  &Terrain3D::getEffectiveGpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_cpu_region_cache_budget_mb" ),
						  &Terrain3D::getEffectiveCpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_instance_region_cache_budget_mb" ),
						  &Terrain3D::getEffectiveInstanceRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_lightmap_residency_budget_mb" ),
						  &Terrain3D::getEffectiveLightmapResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_hlod_memory_budget_mb" ),
						  &Terrain3D::getEffectiveHlodMemoryBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_shader_cache_budget_mb" ),
						  &Terrain3D::getEffectiveShaderCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_memory_budget_policy_generation" ),
						  &Terrain3D::getMemoryBudgetPolicyGeneration );
	ClassDB::bind_method( D_METHOD( "is_memory_budget_overcommitted" ), &Terrain3D::isMemoryBudgetOvercommitted );
	ClassDB::bind_method( D_METHOD( "get_memory_budget_headroom_bytes" ), &Terrain3D::getMemoryBudgetHeadroomBytes );
	ClassDB::bind_method( D_METHOD( "force_memory_budget_rebalance" ), &Terrain3D::forceMemoryBudgetRebalance );
	ClassDB::bind_method( D_METHOD( "get_runtime_diagnostics", "deep_introspection" ),
						  &Terrain3D::getRuntimeDiagnostics,
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "set_streaming_profiler_enabled", "enabled", "sample_every" ),
						  &Terrain3D::setStreamingProfilerEnabled,
						  DEFVAL( 8 ) );
	ClassDB::bind_method( D_METHOD( "get_streaming_profiler_stats" ), &Terrain3D::getStreamingProfilerStats );
	ClassDB::bind_method( D_METHOD( "reset_streaming_profiler" ), &Terrain3D::resetStreamingProfiler );
	ClassDB::bind_method( D_METHOD( "get_runtime_parity_snapshot" ), &Terrain3D::getRuntimeParitySnapshot );
	ClassDB::bind_method( D_METHOD( "get_memory_stats_snapshot" ), &Terrain3D::getMemoryStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_shader_tier_stats_snapshot" ), &Terrain3D::getShaderTierStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_cache_stats_snapshot", "deep_introspection" ),
						  &Terrain3D::getCacheStatsSnapshot,
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_frame_stats_snapshot" ), &Terrain3D::getFrameStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_runtime_profile_history", "max_samples" ),
						  &Terrain3D::getRuntimeProfileHistory,
						  DEFVAL( 600 ) );
	ClassDB::bind_method( D_METHOD( "get_runtime_profile_history_since", "after_sequence", "max_samples" ),
						  &Terrain3D::getRuntimeProfileHistorySince,
						  DEFVAL( 600 ) );
	ClassDB::bind_method( D_METHOD( "reset_runtime_diagnostics" ), &Terrain3D::resetRuntimeDiagnostics );

	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_budget_stats" ), &Terrain3D::getRuntimeAdaptiveBudgetStats );

	ClassDB::bind_method( D_METHOD( "get_runtime_boundary_phasing_stats" ),
						  &Terrain3D::getRuntimeBoundaryPhasingStats );

	ClassDB::bind_method( D_METHOD( "is_editor_authoring_mode" ), &Terrain3D::isEditorAuthoringMode );
	ClassDB::bind_method( D_METHOD( "is_authoring_residency_readback_safe" ),
						  &Terrain3D::isAuthoringResidencyReadbackSafe );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cache_effective" ), &Terrain3D::isGpuRegionCacheEffective );
	ClassDB::bind_method( D_METHOD( "is_cpu_region_cache_effective" ), &Terrain3D::isCpuRegionCacheEffective );
	ClassDB::bind_method( D_METHOD( "is_instance_region_cache_effective" ),
						  &Terrain3D::isInstanceRegionCacheEffective );
	ClassDB::bind_method( D_METHOD( "is_instancer_cell_streaming_effective" ),
						  &Terrain3D::isInstancerCellStreamingEffective );
	ClassDB::bind_method( D_METHOD( "is_dynamic_node_streaming_effective" ),
						  &Terrain3D::isDynamicNodeStreamingEffective );
	ClassDB::bind_method( D_METHOD( "set_performance_target", "target" ), &Terrain3D::setPerformanceTarget );

	ClassDB::bind_method( D_METHOD( "get_performance_profile_status" ), &Terrain3D::getPerformanceProfileStatus );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_status_name" ),
						  &Terrain3D::getPerformanceProfileStatusName );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_version" ), &Terrain3D::getPerformanceProfileVersion );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_signature" ), &Terrain3D::getPerformanceProfileSignature );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_manifest" ), &Terrain3D::getPerformanceProfileManifest );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_diff" ), &Terrain3D::getPerformanceProfileDiff );
	ClassDB::bind_method( D_METHOD( "reset_performance_profile" ), &Terrain3D::resetPerformanceProfile );

	ClassDB::bind_method( D_METHOD( "get_region_streaming_state", "region" ), &Terrain3D::getRegionStreamingState );
	ClassDB::bind_method( D_METHOD( "is_region_streaming_prefetch", "region" ), &Terrain3D::isRegionStreamingPrefetch );
	ClassDB::bind_method( D_METHOD( "get_region_streaming_stats" ), &Terrain3D::getRegionStreamingStats );
	ClassDB::bind_method( D_METHOD( "get_region_streaming_configuration_status" ),
						  &Terrain3D::getRegionStreamingConfigurationStatus );
	ClassDB::bind_method( D_METHOD( "get_height_source_diagnostics", "world_position" ),
						  &Terrain3D::getHeightSourceDiagnostics );

	ClassDB::bind_method( D_METHOD( "get_gpu_region_max_supported_detail_lods" ),
						  &Terrain3D::getGpuRegionMaxSupportedDetailLods );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_max_supported_detail_range_m" ),
						  &Terrain3D::getGpuRegionMaxSupportedDetailRangeM );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_required_core_pages" ), &Terrain3D::getGpuRegionRequiredCorePages );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_mapped_capacity" ), &Terrain3D::getGpuRegionMappedCapacity );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_detail_range_valid" ), &Terrain3D::isGpuRegionDetailRangeValid );

	ClassDB::bind_method( D_METHOD( "get_far_vt_height_source_side" ), &Terrain3D::getFarVtHeightSourceSide );
	ClassDB::bind_method( D_METHOD( "get_far_vt_surface_source_side" ), &Terrain3D::getFarVtSurfaceSourceSide );

	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_stats" ), &Terrain3D::getGpuRegionCacheStats );

	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_stats" ), &Terrain3D::getCpuRegionCacheStats );

	ClassDB::bind_method( D_METHOD( "get_instance_region_cache_stats" ), &Terrain3D::getInstanceRegionCacheStats );

	ClassDB::bind_method( D_METHOD( "get_lightmap_streaming_stats" ), &Terrain3D::getLightmapStreamingStats );

	ClassDB::bind_method( D_METHOD( "reload_hlod_manifest" ), &Terrain3D::reloadHlodManifest );
	ClassDB::bind_method( D_METHOD( "get_hlod_streaming_stats" ), &Terrain3D::getHlodStreamingStats );
	ClassDB::bind_method( D_METHOD( "get_instance_occlusion_stats" ), &Terrain3D::getInstanceOcclusionStats );
	ClassDB::bind_method( D_METHOD( "get_instance_physics_stats" ), &Terrain3D::getInstancePhysicsStats );
	ClassDB::bind_method( D_METHOD( "get_hlod_debug_entries" ), &Terrain3D::getHlodDebugEntries );

	ClassDB::bind_method(
		D_METHOD( "add_streamed_scene", "scene", "global_transform", "state", "flags", "hlod_min_level" ),
		&Terrain3D::addStreamedScene,
		DEFVAL( Dictionary() ),
		DEFVAL( 0 ),
		DEFVAL( 1 ) );
	ClassDB::bind_method(
		D_METHOD( "add_streamed_scene_path", "scene_path", "global_transform", "state", "flags", "hlod_min_level" ),
		&Terrain3D::addStreamedScenePath,
		DEFVAL( Dictionary() ),
		DEFVAL( 0 ),
		DEFVAL( 1 ) );
	ClassDB::bind_method( D_METHOD( "remove_streamed_scene", "spawn_id" ), &Terrain3D::removeStreamedScene );
	ClassDB::bind_method( D_METHOD( "allocate_stream_owner_id" ), &Terrain3D::allocateStreamOwnerId );
	ClassDB::bind_method( D_METHOD( "get_node_streaming_stats" ), &Terrain3D::getNodeStreamingStats );

	// Object references
	ClassDB::bind_method( D_METHOD( "get_data" ), &Terrain3D::getData );
	ClassDB::bind_method( D_METHOD( "set_assets", "assets" ), &Terrain3D::setAssets );
	ClassDB::bind_method( D_METHOD( "get_assets" ), &Terrain3D::getAssets );
	ClassDB::bind_method( D_METHOD( "get_collision" ), &Terrain3D::getCollision );
	ClassDB::bind_method( D_METHOD( "get_instancer" ), &Terrain3D::getInstancer );
	ClassDB::bind_method( D_METHOD( "get_node_streamer" ), &Terrain3D::getNodeStreamer );
	ClassDB::bind_method( D_METHOD( "set_editor", "editor" ), &Terrain3D::setEditor );
	ClassDB::bind_method( D_METHOD( "get_editor" ), &Terrain3D::getEditor );
	ClassDB::bind_method( D_METHOD( "set_plugin", "plugin" ), &Terrain3D::setPlugin );
	ClassDB::bind_method( D_METHOD( "get_plugin" ), &Terrain3D::getPlugin );

	// Regions
	ClassDB::bind_method( D_METHOD( "get_region_size" ), &Terrain3D::getRegionSize );

	// Target Tracking
	ClassDB::bind_method( D_METHOD( "set_camera", "camera" ), &Terrain3D::setCamera );
	ClassDB::bind_method( D_METHOD( "get_camera" ), &Terrain3D::getCamera );
	ClassDB::bind_method( D_METHOD( "get_clipmap_target_position" ), &Terrain3D::getClipmapTargetPosition );
	ClassDB::bind_method( D_METHOD( "get_collision_target_position" ), &Terrain3D::getCollisionTargetPosition );
	ClassDB::bind_method( D_METHOD( "set_light_target", "node" ), &Terrain3D::setLightTarget );

	ClassDB::bind_method( D_METHOD( "get_light_target" ), &Terrain3D::getLightTarget );
	ClassDB::bind_method( D_METHOD( "snap" ), &Terrain3D::snap );

	// Collision

	ClassDB::bind_method( D_METHOD( "get_rd_renderer_diagnostics", "deep_introspection" ),
						  &Terrain3D::getRdRendererDiagnostics,
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_gpu_upload_diagnostics" ), &Terrain3D::getGpuUploadDiagnostics );
	ClassDB::bind_method( D_METHOD( "bake_virtual_textures" ), &Terrain3D::bakeVirtualTextures );
	ClassDB::bind_method( D_METHOD( "force_rebake_virtual_textures" ), &Terrain3D::forceRebakeVirtualTextures );
	ClassDB::bind_method( D_METHOD( "bake_changed_virtual_textures" ), &Terrain3D::bakeChangedVirtualTextures );
	ClassDB::bind_method( D_METHOD( "get_force_rebake_virtual_textures_status" ),
						  &Terrain3D::getForceRebakeVirtualTexturesStatus );

	// Terrain Mesh

	ClassDB::bind_method( D_METHOD( "get_vertex_spacing" ), &Terrain3D::getVertexSpacing );

	// Terrain Displacement

	// Ocean Mesh

	// Rendering
	ClassDB::bind_method( D_METHOD( "set_free_editor_textures" ), &Terrain3D::setFreeEditorTextures );
	ClassDB::bind_method( D_METHOD( "get_free_editor_textures" ), &Terrain3D::getFreeEditorTextures );

	// Overlays

	// Debug Views

	ClassDB::bind_method( D_METHOD( "is_compute_displacement_active" ), &Terrain3D::isComputeDisplacementActive );

	// PBR Views

	// Utility
	ClassDB::bind_method( D_METHOD( "get_intersection", "src_pos", "direction", "gpu_mode" ),
						  &Terrain3D::getIntersection,
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_raycast_result", "src_pos", "direction", "collision_mask", "exclude_terrain" ),
						  &Terrain3D::getRaycastResult,
						  DEFVAL( 0xFFFFFFFF ),
						  DEFVAL( false ) );
#if defined( T3D_EDITOR_NATIVE )
	ClassDB::bind_method( D_METHOD( "bake_mesh", "lod", "filter" ),
						  &Terrain3D::bakeMesh,
						  DEFVAL( HEIGHT_FILTER_NEAREST ) );
	ClassDB::bind_method(
		D_METHOD( "bake_region_lightmap_mesh", "region_location", "lod", "context_radius_regions", "texel_size" ),
		&Terrain3D::bakeRegionLightmapMesh,
		DEFVAL( 2 ),
		DEFVAL( 1 ),
		DEFVAL( 1.0f ) );
	ClassDB::bind_method( D_METHOD( "bake_occluder", "lod" ), &Terrain3D::bakeOccluder );

#endif // T3D_EDITOR_NATIVE
	ClassDB::bind_method( D_METHOD( "generate_nav_mesh_source_geometry", "global_aabb", "require_nav" ),
						  &Terrain3D::generateNavMeshSourceGeometry,
						  DEFVAL( true ) );
	ADD_GROUP( "Dataset & Editor", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"version",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_version" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDebugLevel,
					   getDebugLevel,
					   "level",
					   PropertyInfo( Variant::INT, "debug_level", PROPERTY_HINT_ENUM, "Errors,Info,Debug,Extreme" ),
					   "set_debug_level",
					   "get_debug_level" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDataDirectory,
					   getDataDirectory,
					   "directory",
					   PropertyInfo( Variant::STRING, "data_directory", PROPERTY_HINT_DIR ),
					   "set_data_directory",
					   "get_data_directory" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setEditorExecutionMode,
					   getEditorExecutionMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "editor_execution_mode",
									 PROPERTY_HINT_ENUM,
									 "Authoring,Runtime Profile",
									 PROPERTY_USAGE_EDITOR ),
					   "set_editor_execution_mode",
					   "get_editor_execution_mode" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setEditorRuntimeStreamingPreview,
		isEditorRuntimeStreamingPreview,
		"enabled",
		PropertyInfo( Variant::BOOL, "editor_runtime_streaming_preview", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE ),
		"set_editor_runtime_streaming_preview",
		"is_editor_runtime_streaming_preview" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"region_size",
								PROPERTY_HINT_ENUM,
								"128:128",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_region_size" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setSave16Bit,
					   getSave16Bit,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "save_16_bit", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
					   "set_save_16_bit",
					   "get_save_16_bit" );
	// Storage backing restores only profile identity. Serialized terrain/resource fields
	// retain their exact values; the public setter remains the explicit apply/reset path.
	T3D_BIND_PROPERTY(
		Terrain3D,
		setPerformanceTargetStorage,
		getPerformanceTarget,
		"target",
		PropertyInfo( Variant::INT, "_performance_target_storage", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"_set_performance_target_storage",
		"get_performance_target" );

	ADD_SUBGROUP( "Region Lightmap Bake", "region_lightmap_" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRegionLightmapOutputDirectory,
					   getRegionLightmapOutputDirectory,
					   "directory",
					   PropertyInfo( Variant::STRING, "region_lightmap_output_directory", PROPERTY_HINT_DIR ),
					   "set_region_lightmap_output_directory",
					   "get_region_lightmap_output_directory" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRegionLightmapProxyLod,
					   getRegionLightmapProxyLod,
					   "lod",
					   PropertyInfo( Variant::INT, "region_lightmap_proxy_lod", PROPERTY_HINT_RANGE, "0,8,1" ),
					   "set_region_lightmap_proxy_lod",
					   "get_region_lightmap_proxy_lod" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRegionLightmapContextRadius,
		getRegionLightmapContextRadius,
		"radius",
		PropertyInfo( Variant::INT, "region_lightmap_context_radius", PROPERTY_HINT_RANGE, "0,4,1,suffix: regions" ),
		"set_region_lightmap_context_radius",
		"get_region_lightmap_context_radius" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRegionLightmapTexelSize,
					   getRegionLightmapTexelSize,
					   "texel_size",
					   PropertyInfo( Variant::FLOAT,
									 "region_lightmap_texel_size",
									 PROPERTY_HINT_RANGE,
									 "0.05,16,0.05,suffix: m/texel" ),
					   "set_region_lightmap_texel_size",
					   "get_region_lightmap_texel_size" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRegionLightmapBakeQuality,
		getRegionLightmapBakeQuality,
		"quality",
		PropertyInfo( Variant::INT, "region_lightmap_bake_quality", PROPERTY_HINT_ENUM, "Low,Medium,High,Ultra" ),
		"set_region_lightmap_bake_quality",
		"get_region_lightmap_bake_quality" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRegionLightmapShadowmaskMode,
		getRegionLightmapShadowmaskMode,
		"mode",
		PropertyInfo( Variant::INT, "region_lightmap_shadowmask_mode", PROPERTY_HINT_ENUM, "None,Replace,Overlay" ),
		"set_region_lightmap_shadowmask_mode",
		"get_region_lightmap_shadowmask_mode" );

	ADD_GROUP( "Terrain Resources", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setMaterial,
					   getMaterial,
					   "material",
					   PropertyInfo( Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DMaterial" ),
					   "set_material",
					   "get_material" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "assets", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DAssets" ),
				  "set_assets",
				  "get_assets" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setFoliageProfile,
		getFoliageProfile,
		"profile",
		PropertyInfo( Variant::OBJECT, "foliage_profile", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DFoliagePreset" ),
		"set_foliage_profile",
		"get_foliage_profile" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setAutoShaderProfile,
					   getAutoShaderProfile,
					   "profile",
					   PropertyInfo( Variant::OBJECT,
									 "auto_shader_profile",
									 PROPERTY_HINT_RESOURCE_TYPE,
									 "Terrain3DAutoShaderProfile" ),
					   "set_auto_shader_profile",
					   "get_auto_shader_profile" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
								"light_target",
								PROPERTY_HINT_NODE_TYPE,
								"DirectionalLight3D",
								PROPERTY_USAGE_DEFAULT,
								"Node3D" ),
				  "set_light_target",
				  "get_light_target" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "Terrain3DData" ),
				  "",
				  "get_data" );
	ADD_PROPERTY(
		PropertyInfo( Variant::OBJECT, "collision", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "Terrain3DCollision" ),
		"",
		"get_collision" );
	ADD_PROPERTY(
		PropertyInfo( Variant::OBJECT, "instancer", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "Terrain3DInstancer" ),
		"",
		"get_instancer" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
								"node_streamer",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_NONE,
								"Terrain3DNodeStreamer" ),
				  "",
				  "get_node_streamer" );

	ADD_GROUP( "Quality & Effective Profile", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"performance_target",
								PROPERTY_HINT_ENUM,
								"Custom,Xbox 360 Experimental",
								PROPERTY_USAGE_EDITOR ),
				  "set_performance_target",
				  "get_performance_target" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setVirtualTextureQualityProfile,
					   getVirtualTextureQualityProfile,
					   "profile",
					   PropertyInfo( Variant::INT,
									 "virtual_texture_quality_profile",
									 PROPERTY_HINT_ENUM,
									 "Just Make It Run,Low,Normal,High,Very High,Make My GPU SCREAM",
									 PROPERTY_USAGE_DEFAULT ),
					   "set_virtual_texture_quality_profile",
					   "get_virtual_texture_quality_profile" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"performance_profile_status",
								PROPERTY_HINT_ENUM,
								"Custom,Exact,Modified",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_performance_profile_status" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"performance_profile_version",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_performance_profile_version" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
								"performance_profile_signature",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_performance_profile_signature" );

	ADD_GROUP( "Instance Texture Sampling", "" );
	// Negative bias requests finer mips; positive bias reduces texture detail and residency demand.
	T3D_BIND_PROPERTY( Terrain3D,
					   setClutterTextureMipBias,
					   getClutterTextureMipBias,
					   "bias",
					   PropertyInfo( Variant::FLOAT, "clutter_texture_mip_bias", PROPERTY_HINT_RANGE, "-4,4,0.1" ),
					   "set_clutter_texture_mip_bias",
					   "get_clutter_texture_mip_bias" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setClutterTextureFiltering,
					   getClutterTextureFiltering,
					   "filter",
					   PropertyInfo( Variant::INT,
									 "clutter_texture_filtering",
									 PROPERTY_HINT_ENUM,
									 "Use Material:-1,Nearest:0,Linear:1,Nearest Mipmap:2,Linear Mipmap:3,Nearest "
									 "Mipmap Anisotropic:4,Linear Mipmap Anisotropic:5" ),
					   "set_clutter_texture_filtering",
					   "get_clutter_texture_filtering" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGroundCoverTextureMipBias,
					   getGroundCoverTextureMipBias,
					   "bias",
					   PropertyInfo( Variant::FLOAT, "ground_cover_texture_mip_bias", PROPERTY_HINT_RANGE, "-4,4,0.1" ),
					   "set_ground_cover_texture_mip_bias",
					   "get_ground_cover_texture_mip_bias" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGroundCoverTextureFiltering,
					   getGroundCoverTextureFiltering,
					   "filter",
					   PropertyInfo( Variant::INT,
									 "ground_cover_texture_filtering",
									 PROPERTY_HINT_ENUM,
									 "Use Material:-1,Nearest:0,Linear:1,Nearest Mipmap:2,Linear Mipmap:3,Nearest "
									 "Mipmap Anisotropic:4,Linear Mipmap Anisotropic:5" ),
					   "set_ground_cover_texture_filtering",
					   "get_ground_cover_texture_filtering" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOccluderTextureMipBias,
					   getOccluderTextureMipBias,
					   "bias",
					   PropertyInfo( Variant::FLOAT, "occluder_texture_mip_bias", PROPERTY_HINT_RANGE, "-4,4,0.1" ),
					   "set_occluder_texture_mip_bias",
					   "get_occluder_texture_mip_bias" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOccluderTextureFiltering,
					   getOccluderTextureFiltering,
					   "filter",
					   PropertyInfo( Variant::INT,
									 "occluder_texture_filtering",
									 PROPERTY_HINT_ENUM,
									 "Use Material:-1,Nearest:0,Linear:1,Nearest Mipmap:2,Linear Mipmap:3,Nearest "
									 "Mipmap Anisotropic:4,Linear Mipmap Anisotropic:5" ),
					   "set_occluder_texture_filtering",
					   "get_occluder_texture_filtering" );

	ADD_GROUP( "Terrain Rendering", "" );

	ADD_SUBGROUP( "Geometry & Clipmap", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setClipmapTarget,
					   getClipmapTarget,
					   "node",
					   PropertyInfo( Variant::OBJECT,
									 "clipmap_target",
									 PROPERTY_HINT_NODE_TYPE,
									 "Node3D",
									 PROPERTY_USAGE_DEFAULT,
									 "Node3D" ),
					   "set_clipmap_target",
					   "get_clipmap_target" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setMeshLods,
					   getMeshLods,
					   "count",
					   PropertyInfo( Variant::INT, "mesh_lods", PROPERTY_HINT_RANGE, "1,10,1" ),
					   "set_mesh_lods",
					   "get_mesh_lods" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setMeshSize,
					   getMeshSize,
					   "size",
					   PropertyInfo( Variant::INT, "mesh_size", PROPERTY_HINT_RANGE, "8,256,2" ),
					   "set_mesh_size",
					   "get_mesh_size" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT,
								"vertex_spacing",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_vertex_spacing" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCullMargin,
					   getCullMargin,
					   "margin",
					   PropertyInfo( Variant::FLOAT, "cull_margin", PROPERTY_HINT_RANGE, "0.0,10000.0,.5,or_greater" ),
					   "set_cull_margin",
					   "get_cull_margin" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRenderLayers,
					   getRenderLayers,
					   "layers",
					   PropertyInfo( Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER ),
					   "set_render_layers",
					   "get_render_layers" );

	ADD_SUBGROUP( "Near / Middle / Far Detail", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeShaderTieringEnabled,
					   isRuntimeShaderTieringEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "runtime_shader_tiering_enabled" ),
					   "set_runtime_shader_tiering_enabled",
					   "is_runtime_shader_tiering_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeNearShaderLods,
					   getRuntimeNearShaderLods,
					   "count",
					   PropertyInfo( Variant::INT, "runtime_near_shader_lods", PROPERTY_HINT_RANGE, "0,16,1" ),
					   "set_runtime_near_shader_lods",
					   "get_runtime_near_shader_lods" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeMiddleShaderLods,
					   getRuntimeMiddleShaderLods,
					   "count",
					   PropertyInfo( Variant::INT, "runtime_middle_shader_lods", PROPERTY_HINT_RANGE, "0,16,1" ),
					   "set_runtime_middle_shader_lods",
					   "get_runtime_middle_shader_lods" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeNearRangeEndM,
		getRuntimeNearRangeEndM,
		"range_m",
		PropertyInfo( Variant::FLOAT, "runtime_near_range_end_m", PROPERTY_HINT_RANGE, "0,4096,1,or_greater,suffix:m" ),
		"set_runtime_near_range_end_m",
		"get_runtime_near_range_end_m" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeMiddleRangeEndM,
					   getRuntimeMiddleRangeEndM,
					   "range_m",
					   PropertyInfo( Variant::FLOAT,
									 "runtime_middle_range_end_m",
									 PROPERTY_HINT_RANGE,
									 "0,8192,1,or_greater,suffix:m" ),
					   "set_runtime_middle_range_end_m",
					   "get_runtime_middle_range_end_m" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeSyncDetailLods,
					   isRuntimeSyncDetailLods,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "runtime_sync_detail_lods" ),
					   "set_runtime_sync_detail_lods",
					   "is_runtime_sync_detail_lods" );

	ADD_SUBGROUP( "Shadows & Sunlight", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDominantSunOnly,
					   isDominantSunOnly,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "dominant_sun_only" ),
					   "set_dominant_sun_only",
					   "is_dominant_sun_only" );

	T3D_BIND_PROPERTY(
		Terrain3D,
		setCastShadows,
		getCastShadows,
		"shadow_casting_setting",
		PropertyInfo( Variant::INT, "cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only" ),
		"set_cast_shadows",
		"get_cast_shadows" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShadowLodCount,
					   getShadowLodCount,
					   "lod_count",
					   PropertyInfo( Variant::INT, "shadow_lod_count", PROPERTY_HINT_RANGE, "0,16,1" ),
					   "set_shadow_lod_count",
					   "get_shadow_lod_count" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setNearShadowEnabled,
					   getNearShadowEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "near_shadow_enabled" ),
					   "set_near_shadow_enabled",
					   "get_near_shadow_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setMiddleShadowEnabled,
					   getMiddleShadowEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "middle_shadow_enabled" ),
					   "set_middle_shadow_enabled",
					   "get_middle_shadow_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setFarShadowEnabled,
					   getFarShadowEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "far_shadow_enabled" ),
					   "set_far_shadow_enabled",
					   "get_far_shadow_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setNearShadowLodCount,
					   getNearShadowLodCount,
					   "lod_count",
					   PropertyInfo( Variant::INT, "near_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
					   "set_near_shadow_lod_count",
					   "get_near_shadow_lod_count" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setMiddleShadowLodCount,
					   getMiddleShadowLodCount,
					   "lod_count",
					   PropertyInfo( Variant::INT, "middle_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
					   "set_middle_shadow_lod_count",
					   "get_middle_shadow_lod_count" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setFarShadowLodCount,
					   getFarShadowLodCount,
					   "lod_count",
					   PropertyInfo( Variant::INT, "far_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
					   "set_far_shadow_lod_count",
					   "get_far_shadow_lod_count" );

	ADD_SUBGROUP( "Tessellation & Displacement", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setTessellationLevel,
					   getTessellationLevel,
					   "size",
					   PropertyInfo( Variant::INT, "tessellation_level", PROPERTY_HINT_RANGE, "0,6,1" ),
					   "set_tessellation_level",
					   "get_tessellation_level" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setComputeDisplacementEnabled,
					   getComputeDisplacementEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "compute_displacement_enabled" ),
					   "set_compute_displacement_enabled",
					   "get_compute_displacement_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setDisplacementUpdateDistance,
		getDisplacementUpdateDistance,
		"distance",
		PropertyInfo( Variant::FLOAT, "displacement_update_distance", PROPERTY_HINT_RANGE, "0.0,16.0,0.01,suffix:m" ),
		"set_displacement_update_distance",
		"get_displacement_update_distance" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDisplacementScale,
					   getDisplacementScale,
					   "scale",
					   PropertyInfo( Variant::FLOAT, "displacement_scale", PROPERTY_HINT_RANGE, "0.0, 2.0, 0.01" ),
					   "set_displacement_scale",
					   "get_displacement_scale" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDisplacementSharpness,
					   getDisplacementSharpness,
					   "sharpness",
					   PropertyInfo( Variant::FLOAT, "displacement_sharpness", PROPERTY_HINT_RANGE, "0.0, 1.0, 0.01" ),
					   "set_displacement_sharpness",
					   "get_displacement_sharpness" );

	ADD_GROUP( "Streaming & Residency", "" );

	ADD_SUBGROUP( "Region Streaming Radii", "" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setStreamingActiveRadius,
		getStreamingActiveRadius,
		"radius",
		PropertyInfo( Variant::INT, "streaming_active_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
		"set_streaming_active_radius",
		"get_streaming_active_radius" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setStreamingHotRadius,
		getStreamingHotRadius,
		"radius",
		PropertyInfo( Variant::INT, "streaming_hot_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
		"set_streaming_hot_radius",
		"get_streaming_hot_radius" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setStreamingColdRadius,
		getStreamingColdRadius,
		"radius",
		PropertyInfo( Variant::INT, "streaming_cold_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
		"set_streaming_cold_radius",
		"get_streaming_cold_radius" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setStreamingUnloadGuard,
		getStreamingUnloadGuard,
		"regions",
		PropertyInfo( Variant::INT, "streaming_unload_guard", PROPERTY_HINT_RANGE, "0,4,1,suffix: regions" ),
		"set_streaming_unload_guard",
		"get_streaming_unload_guard" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setStreamingPredictionEnabled,
					   isStreamingPredictionEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "streaming_prediction_enabled" ),
					   "set_streaming_prediction_enabled",
					   "is_streaming_prediction_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setStreamingPredictionLookahead,
		getStreamingPredictionLookahead,
		"seconds",
		PropertyInfo( Variant::FLOAT, "streaming_prediction_lookahead", PROPERTY_HINT_RANGE, "0,4,0.05,suffix:s" ),
		"set_streaming_prediction_lookahead",
		"get_streaming_prediction_lookahead" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setStreamingPredictionMaximumRegions,
					   getStreamingPredictionMaximumRegions,
					   "regions",
					   PropertyInfo( Variant::INT,
									 "streaming_prediction_maximum_regions",
									 PROPERTY_HINT_RANGE,
									 "0,16,1,suffix: regions" ),
					   "set_streaming_prediction_maximum_regions",
					   "get_streaming_prediction_maximum_regions" );

	ADD_SUBGROUP( "GPU Terrain Page Cache", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionCacheEnabled,
					   isGpuRegionCacheEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "gpu_region_cache_enabled" ),
					   "set_gpu_region_cache_enabled",
					   "is_gpu_region_cache_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionCacheBudgetAuto,
					   isGpuRegionCacheBudgetAuto,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "gpu_region_cache_budget_auto" ),
					   "set_gpu_region_cache_budget_auto",
					   "is_gpu_region_cache_budget_auto" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setGpuRegionCacheBudgetMb,
		getGpuRegionCacheBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "gpu_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,512,1,suffix: MiB" ),
		"set_gpu_region_cache_budget_mb",
		"get_gpu_region_cache_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setGpuRegionCachePageLimit,
		getGpuRegionCachePageLimit,
		"pages",
		PropertyInfo( Variant::INT, "gpu_region_cache_page_limit", PROPERTY_HINT_RANGE, "16,256,1,suffix: pages" ),
		"set_gpu_region_cache_page_limit",
		"get_gpu_region_cache_page_limit" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"gpu_region_cache_effective_mb",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_effective_gpu_region_cache_budget_mb" );
	ADD_SUBGROUP( "GPU Upload & Retirement", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionUploadBudgetLayers,
					   getGpuRegionUploadBudgetLayers,
					   "layers",
					   PropertyInfo( Variant::INT, "gpu_region_upload_budget_layers", PROPERTY_HINT_RANGE, "1,64,1" ),
					   "set_gpu_region_upload_budget_layers",
					   "get_gpu_region_upload_budget_layers" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionUploadBudgetMb,
					   getGpuRegionUploadBudgetMb,
					   "budget_mb",
					   PropertyInfo( Variant::INT, "gpu_region_upload_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ),
					   "set_gpu_region_upload_budget_mb",
					   "get_gpu_region_upload_budget_mb" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionSparePages,
					   getGpuRegionSparePages,
					   "pages",
					   PropertyInfo( Variant::INT, "gpu_region_spare_pages", PROPERTY_HINT_RANGE, "2,255,1" ),
					   "set_gpu_region_spare_pages",
					   "get_gpu_region_spare_pages" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionRetireFrames,
					   getGpuRegionRetireFrames,
					   "frames",
					   PropertyInfo( Variant::INT, "gpu_region_retire_frames", PROPERTY_HINT_RANGE, "1,16,1" ),
					   "set_gpu_region_retire_frames",
					   "get_gpu_region_retire_frames" );
	ADD_SUBGROUP( "GPU Detail Range", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionDetailLods,
					   getGpuRegionDetailLods,
					   "lods",
					   PropertyInfo( Variant::INT,
									 "gpu_region_detail_lods",
									 PROPERTY_HINT_RANGE,
									 "0,16,1,suffix: LODs (0 = Near tier)" ),
					   "set_gpu_region_detail_lods",
					   "get_gpu_region_detail_lods" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionDetailRangeEndM,
					   getGpuRegionDetailRangeEndM,
					   "range_m",
					   PropertyInfo( Variant::FLOAT,
									 "gpu_region_detail_range_end_m",
									 PROPERTY_HINT_RANGE,
									 "0,8192,1,or_greater,suffix:m" ),
					   "set_gpu_region_detail_range_end_m",
					   "get_gpu_region_detail_range_end_m" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"gpu_region_max_supported_detail_lods",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_gpu_region_max_supported_detail_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT,
								"gpu_region_max_supported_detail_range_m",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_gpu_region_max_supported_detail_range_m" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"gpu_region_required_core_pages",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_gpu_region_required_core_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"gpu_region_mapped_capacity",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_gpu_region_mapped_capacity" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL,
								"gpu_region_detail_range_valid",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "is_gpu_region_detail_range_valid" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setGpuRegionGuardRegions,
		getGpuRegionGuardRegions,
		"regions",
		PropertyInfo( Variant::INT, "gpu_region_guard_regions", PROPERTY_HINT_RANGE, "0,4,1", PROPERTY_USAGE_STORAGE ),
		"set_gpu_region_guard_regions",
		"get_gpu_region_guard_regions" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setGpuRegionPrefetchRows,
		getGpuRegionPrefetchRows,
		"rows",
		PropertyInfo( Variant::INT, "gpu_region_prefetch_rows", PROPERTY_HINT_RANGE, "0,4,1", PROPERTY_USAGE_STORAGE ),
		"set_gpu_region_prefetch_rows",
		"get_gpu_region_prefetch_rows" );
	ADD_SUBGROUP( "GPU Page Fidelity", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionHysteresisTicks,
					   getGpuRegionHysteresisTicks,
					   "ticks",
					   PropertyInfo( Variant::INT, "gpu_region_hysteresis_ticks", PROPERTY_HINT_RANGE, "0,240,1" ),
					   "set_gpu_region_hysteresis_ticks",
					   "get_gpu_region_hysteresis_ticks" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionColorResolutionDivisor,
					   getGpuRegionColorResolutionDivisor,
					   "divisor",
					   PropertyInfo( Variant::INT,
									 "gpu_region_color_resolution_divisor",
									 PROPERTY_HINT_ENUM,
									 "Full:1,Half:2,Quarter:4" ),
					   "set_gpu_region_color_resolution_divisor",
					   "get_gpu_region_color_resolution_divisor" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionDerivedResolutionDivisor,
					   getGpuRegionDerivedResolutionDivisor,
					   "divisor",
					   PropertyInfo( Variant::INT,
									 "gpu_region_derived_resolution_divisor",
									 PROPERTY_HINT_ENUM,
									 "Full:1,Half:2,Quarter:4" ),
					   "set_gpu_region_derived_resolution_divisor",
					   "get_gpu_region_derived_resolution_divisor" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuMiddleHeightResolutionDivisor,
					   getGpuMiddleHeightResolutionDivisor,
					   "divisor",
					   PropertyInfo( Variant::INT,
									 "gpu_middle_height_resolution_divisor",
									 PROPERTY_HINT_ENUM,
									 "Full:1,Half:2,Quarter:4" ),
					   "set_gpu_middle_height_resolution_divisor",
					   "get_gpu_middle_height_resolution_divisor" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setEditorGpuRegionCachePages,
		getEditorGpuRegionCachePages,
		"pages",
		PropertyInfo( Variant::INT, "editor_gpu_region_cache_pages", PROPERTY_HINT_RANGE, "16,256,1,suffix: pages" ),
		"set_editor_gpu_region_cache_pages",
		"get_editor_gpu_region_cache_pages" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setGpuFarQualityDivisor,
		getGpuFarQualityDivisor,
		"divisor",
		PropertyInfo( Variant::INT, "gpu_far_quality_divisor", PROPERTY_HINT_ENUM, "Full:1,Half:2,Quarter:4,Eighth:8" ),
		"set_gpu_far_quality_divisor",
		"get_gpu_far_quality_divisor" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setFarMaterialResolution,
		getFarMaterialResolution,
		"resolution",
		PropertyInfo( Variant::INT, "far_material_resolution", PROPERTY_HINT_ENUM, "64:64,128:128,256:256" ),
		"set_far_material_resolution",
		"get_far_material_resolution" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setFarControlResolution,
		getFarControlResolution,
		"resolution",
		PropertyInfo( Variant::INT, "far_control_resolution", PROPERTY_HINT_ENUM, "64:64,128:128,256:256" ),
		"set_far_control_resolution",
		"get_far_control_resolution" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"far_vt_height_source_side",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_far_vt_height_source_side" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"far_vt_surface_source_side",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_far_vt_surface_source_side" );
	ADD_SUBGROUP( "Cooked Terrain Pages", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionCookedPagesEnabled,
					   isGpuRegionCookedPagesEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "gpu_region_cooked_pages_enabled" ),
					   "set_gpu_region_cooked_pages_enabled",
					   "is_gpu_region_cooked_pages_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setGpuRegionCookOnSave,
					   isGpuRegionCookOnSave,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "gpu_region_cook_on_save" ),
					   "set_gpu_region_cook_on_save",
					   "is_gpu_region_cook_on_save" );

	ADD_SUBGROUP( "CPU Region Cache", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCpuRegionCacheEnabled,
					   isCpuRegionCacheEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "cpu_region_cache_enabled" ),
					   "set_cpu_region_cache_enabled",
					   "is_cpu_region_cache_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCpuRegionCacheBudgetAuto,
					   isCpuRegionCacheBudgetAuto,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "cpu_region_cache_budget_auto" ),
					   "set_cpu_region_cache_budget_auto",
					   "is_cpu_region_cache_budget_auto" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCpuRegionCacheBudgetMb,
					   getCpuRegionCacheBudgetMb,
					   "budget_mb",
					   PropertyInfo( Variant::INT, "cpu_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,128,1" ),
					   "set_cpu_region_cache_budget_mb",
					   "get_cpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"cpu_region_cache_effective_mb",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_effective_cpu_region_cache_budget_mb" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCpuRegionLoadBudgetLayers,
					   getCpuRegionLoadBudgetLayers,
					   "layers",
					   PropertyInfo( Variant::INT, "cpu_region_load_budget_layers", PROPERTY_HINT_RANGE, "1,64,1" ),
					   "set_cpu_region_load_budget_layers",
					   "get_cpu_region_load_budget_layers" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCpuRegionLoadBudgetMb,
					   getCpuRegionLoadBudgetMb,
					   "budget_mb",
					   PropertyInfo( Variant::INT, "cpu_region_load_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ),
					   "set_cpu_region_load_budget_mb",
					   "get_cpu_region_load_budget_mb" );

	ADD_SUBGROUP( "Instance Residency", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstanceRegionCacheEnabled,
					   isInstanceRegionCacheEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "instance_region_cache_enabled" ),
					   "set_instance_region_cache_enabled",
					   "is_instance_region_cache_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstanceRegionCacheBudgetAuto,
					   isInstanceRegionCacheBudgetAuto,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "instance_region_cache_budget_auto" ),
					   "set_instance_region_cache_budget_auto",
					   "is_instance_region_cache_budget_auto" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstanceRegionCacheBudgetMb,
					   getInstanceRegionCacheBudgetMb,
					   "budget_mb",
					   PropertyInfo( Variant::INT, "instance_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,128,1" ),
					   "set_instance_region_cache_budget_mb",
					   "get_instance_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"instance_region_cache_effective_mb",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_effective_instance_region_cache_budget_mb" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstanceRegionLoadBudgetPages,
					   getInstanceRegionLoadBudgetPages,
					   "pages",
					   PropertyInfo( Variant::INT, "instance_region_load_budget_pages", PROPERTY_HINT_RANGE, "1,64,1" ),
					   "set_instance_region_load_budget_pages",
					   "get_instance_region_load_budget_pages" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstanceRegionLoadBudgetMb,
					   getInstanceRegionLoadBudgetMb,
					   "budget_mb",
					   PropertyInfo( Variant::INT, "instance_region_load_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ),
					   "set_instance_region_load_budget_mb",
					   "get_instance_region_load_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setInstancerCellSize,
		getInstancerCellSize,
		"cell_size",
		PropertyInfo( Variant::INT, "instancer_cell_size", PROPERTY_HINT_ENUM, "8,16,32,64,128,suffix: samples" ),
		"set_instancer_cell_size",
		"get_instancer_cell_size" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstancerCellStreamingEnabled,
					   isInstancerCellStreamingEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "instancer_cell_streaming_enabled" ),
					   "set_instancer_cell_streaming_enabled",
					   "is_instancer_cell_streaming_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstancerSingleLodBuffers,
					   isInstancerSingleLodBuffers,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "instancer_single_lod_buffers" ),
					   "set_instancer_single_lod_buffers",
					   "is_instancer_single_lod_buffers" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setInstancerCellStreamingMargin,
		getInstancerCellStreamingMargin,
		"margin",
		PropertyInfo( Variant::FLOAT, "instancer_cell_streaming_margin", PROPERTY_HINT_RANGE, "0,1024,1,suffix:m" ),
		"set_instancer_cell_streaming_margin",
		"get_instancer_cell_streaming_margin" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstancerCellStreamingUpdateDistance,
					   getInstancerCellStreamingUpdateDistance,
					   "distance",
					   PropertyInfo( Variant::FLOAT,
									 "instancer_cell_streaming_update_distance",
									 PROPERTY_HINT_RANGE,
									 "1,256,1,suffix:m" ),
					   "set_instancer_cell_streaming_update_distance",
					   "get_instancer_cell_streaming_update_distance" );

	ADD_SUBGROUP( "Dynamic Node Streaming", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDynamicNodeStreamingEnabled,
					   isDynamicNodeStreamingEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "dynamic_node_streaming_enabled" ),
					   "set_dynamic_node_streaming_enabled",
					   "is_dynamic_node_streaming_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDynamicNodeStreamRadius,
					   getDynamicNodeStreamRadius,
					   "radius",
					   PropertyInfo( Variant::INT, "dynamic_node_stream_radius", PROPERTY_HINT_RANGE, "0,16,1" ),
					   "set_dynamic_node_stream_radius",
					   "get_dynamic_node_stream_radius" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDynamicNodeLoadRegionBudget,
					   getDynamicNodeLoadRegionBudget,
					   "budget",
					   PropertyInfo( Variant::INT, "dynamic_node_load_region_budget", PROPERTY_HINT_RANGE, "0,64,1" ),
					   "set_dynamic_node_load_region_budget",
					   "get_dynamic_node_load_region_budget" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDynamicNodeLoadNodeBudget,
					   getDynamicNodeLoadNodeBudget,
					   "budget",
					   PropertyInfo( Variant::INT, "dynamic_node_load_node_budget", PROPERTY_HINT_RANGE, "0,4096,1" ),
					   "set_dynamic_node_load_node_budget",
					   "get_dynamic_node_load_node_budget" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setDynamicNodeUnloadRegionBudget,
					   getDynamicNodeUnloadRegionBudget,
					   "budget",
					   PropertyInfo( Variant::INT, "dynamic_node_unload_region_budget", PROPERTY_HINT_RANGE, "0,64,1" ),
					   "set_dynamic_node_unload_region_budget",
					   "get_dynamic_node_unload_region_budget" );

	ADD_SUBGROUP( "Terrain Lightmap Residency", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setLightmapStreamingEnabled,
					   isLightmapStreamingEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "lightmap_streaming_enabled" ),
					   "set_lightmap_streaming_enabled",
					   "is_lightmap_streaming_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setLightmapResidencyBudgetAuto,
					   isLightmapResidencyBudgetAuto,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "lightmap_residency_budget_auto" ),
					   "set_lightmap_residency_budget_auto",
					   "is_lightmap_residency_budget_auto" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setLightmapResidencyBudgetMb,
		getLightmapResidencyBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "lightmap_residency_budget_mb", PROPERTY_HINT_RANGE, "1,1024,1,suffix: MiB" ),
		"set_lightmap_residency_budget_mb",
		"get_lightmap_residency_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"lightmap_residency_effective_mb",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_effective_lightmap_residency_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setLightmapStreamRadius,
		getLightmapStreamRadius,
		"radius",
		PropertyInfo( Variant::INT, "lightmap_stream_radius", PROPERTY_HINT_RANGE, "0,16,1,suffix: regions" ),
		"set_lightmap_stream_radius",
		"get_lightmap_stream_radius" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setLightmapLoadBudget,
		getLightmapLoadBudget,
		"budget",
		PropertyInfo( Variant::INT, "lightmap_load_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ),
		"set_lightmap_load_budget",
		"get_lightmap_load_budget" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setLightmapUnloadBudget,
		getLightmapUnloadBudget,
		"budget",
		PropertyInfo( Variant::INT, "lightmap_unload_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ),
		"set_lightmap_unload_budget",
		"get_lightmap_unload_budget" );

	T3D_BIND_PROPERTY(
		Terrain3D,
		setNextStreamOwnerId,
		getNextStreamOwnerId,
		"owner_id",
		PropertyInfo( Variant::INT, "stream_owner_counter", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
		"set_next_stream_owner_id",
		"get_next_stream_owner_id" );

	ADD_SUBGROUP( "HLOD Residency", "hlod_" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setHlodEnabled,
					   isHlodEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "hlod_enabled" ),
					   "set_hlod_enabled",
					   "is_hlod_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setHlodManifestPath,
					   getHlodManifestPath,
					   "path",
					   PropertyInfo( Variant::STRING, "hlod_manifest_path", PROPERTY_HINT_FILE, "*.tres,*.res" ),
					   "set_hlod_manifest_path",
					   "get_hlod_manifest_path" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setHlodTargetErrorPixels,
		getHlodTargetErrorPixels,
		"pixels",
		PropertyInfo( Variant::FLOAT, "hlod_target_error_pixels", PROPERTY_HINT_RANGE, "0.25,64,0.25,suffix:px" ),
		"set_hlod_target_error_pixels",
		"get_hlod_target_error_pixels" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setHlodHysteresis,
					   getHlodHysteresis,
					   "ratio",
					   PropertyInfo( Variant::FLOAT, "hlod_hysteresis", PROPERTY_HINT_RANGE, "0,0.75,0.01" ),
					   "set_hlod_hysteresis",
					   "get_hlod_hysteresis" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setHlodMaxLevel,
					   getHlodMaxLevel,
					   "level",
					   PropertyInfo( Variant::INT, "hlod_max_level", PROPERTY_HINT_RANGE, "1,6,1" ),
					   "set_hlod_max_level",
					   "get_hlod_max_level" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setHlodLoadBudget,
		getHlodLoadBudget,
		"budget",
		PropertyInfo( Variant::INT, "hlod_load_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ),
		"set_hlod_load_budget",
		"get_hlod_load_budget" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setHlodUnloadBudget,
		getHlodUnloadBudget,
		"budget",
		PropertyInfo( Variant::INT, "hlod_unload_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ),
		"set_hlod_unload_budget",
		"get_hlod_unload_budget" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setHlodMemoryBudgetAuto,
					   isHlodMemoryBudgetAuto,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "hlod_memory_budget_auto" ),
					   "set_hlod_memory_budget_auto",
					   "is_hlod_memory_budget_auto" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setHlodMemoryBudgetMb,
		getHlodMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "hlod_memory_budget_mb", PROPERTY_HINT_RANGE, "1,4096,1,suffix: MiB" ),
		"set_hlod_memory_budget_mb",
		"get_hlod_memory_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"hlod_memory_effective_mb",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_effective_hlod_memory_budget_mb" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setHlodMaxShadowLevel,
					   getHlodMaxShadowLevel,
					   "level",
					   PropertyInfo( Variant::INT, "hlod_max_shadow_level", PROPERTY_HINT_RANGE, "0,6,1" ),
					   "set_hlod_max_shadow_level",
					   "get_hlod_max_shadow_level" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setHlodForcedDebugLevel,
					   getHlodForcedDebugLevel,
					   "level",
					   PropertyInfo( Variant::INT, "hlod_forced_debug_level", PROPERTY_HINT_RANGE, "-1,6,1" ),
					   "set_hlod_forced_debug_level",
					   "get_hlod_forced_debug_level" );

	ADD_GROUP( "Frame Scheduling (Advanced)", "" );

	ADD_SUBGROUP( "Work Limits", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeClipmapUpdateBudget,
					   getRuntimeClipmapUpdateBudget,
					   "count",
					   PropertyInfo( Variant::INT, "runtime_clipmap_update_budget", PROPERTY_HINT_RANGE, "1,16,1" ),
					   "set_runtime_clipmap_update_budget",
					   "get_runtime_clipmap_update_budget" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimePagePrepareThreads,
					   getRuntimePagePrepareThreads,
					   "count",
					   PropertyInfo( Variant::INT, "runtime_page_prepare_threads", PROPERTY_HINT_RANGE, "1,32,1" ),
					   "set_runtime_page_prepare_threads",
					   "get_runtime_page_prepare_threads" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstancerUpdateCellBudget,
					   getInstancerUpdateCellBudget,
					   "budget",
					   PropertyInfo( Variant::INT,
									 "instancer_update_cell_budget",
									 PROPERTY_HINT_RANGE,
									 "0,65536,1,or_greater,suffix: cells/frame" ),
					   "set_instancer_update_cell_budget",
					   "get_instancer_update_cell_budget" );

	ADD_SUBGROUP( "Adaptive Scheduling", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeAdaptiveWorkBudgetsEnabled,
					   isRuntimeAdaptiveWorkBudgetsEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "runtime_adaptive_work_budgets_enabled" ),
					   "set_runtime_adaptive_work_budgets_enabled",
					   "is_runtime_adaptive_work_budgets_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveTargetMs,
		getRuntimeAdaptiveTargetMs,
		"target_ms",
		PropertyInfo( Variant::FLOAT, "runtime_adaptive_target_ms", PROPERTY_HINT_RANGE, "0.05,50.0,0.05,suffix:ms" ),
		"set_runtime_adaptive_target_ms",
		"get_runtime_adaptive_target_ms" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeAdaptiveHardMaxMs,
					   getRuntimeAdaptiveHardMaxMs,
					   "hard_max_ms",
					   PropertyInfo( Variant::FLOAT,
									 "runtime_adaptive_hard_max_ms",
									 PROPERTY_HINT_RANGE,
									 "0.05,100.0,0.05,suffix:ms" ),
					   "set_runtime_adaptive_hard_max_ms",
					   "get_runtime_adaptive_hard_max_ms" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveAdjustmentWindowFrames,
		getRuntimeAdaptiveAdjustmentWindowFrames,
		"frames",
		PropertyInfo( Variant::INT, "runtime_adaptive_adjustment_window_frames", PROPERTY_HINT_RANGE, "1,120,1" ),
		"set_runtime_adaptive_adjustment_window_frames",
		"get_runtime_adaptive_adjustment_window_frames" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeAdaptiveMinGpuPages,
					   getRuntimeAdaptiveMinGpuPages,
					   "pages",
					   PropertyInfo( Variant::INT, "runtime_adaptive_min_gpu_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
					   "set_runtime_adaptive_min_gpu_pages",
					   "get_runtime_adaptive_min_gpu_pages" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeAdaptiveMinCpuPages,
					   getRuntimeAdaptiveMinCpuPages,
					   "pages",
					   PropertyInfo( Variant::INT, "runtime_adaptive_min_cpu_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
					   "set_runtime_adaptive_min_cpu_pages",
					   "get_runtime_adaptive_min_cpu_pages" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveMinInstancePages,
		getRuntimeAdaptiveMinInstancePages,
		"pages",
		PropertyInfo( Variant::INT, "runtime_adaptive_min_instance_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
		"set_runtime_adaptive_min_instance_pages",
		"get_runtime_adaptive_min_instance_pages" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveMinInstancerCells,
		getRuntimeAdaptiveMinInstancerCells,
		"cells",
		PropertyInfo( Variant::INT, "runtime_adaptive_min_instancer_cells", PROPERTY_HINT_RANGE, "0,65536,1" ),
		"set_runtime_adaptive_min_instancer_cells",
		"get_runtime_adaptive_min_instancer_cells" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveMinNodeLoadRegions,
		getRuntimeAdaptiveMinNodeLoadRegions,
		"regions",
		PropertyInfo( Variant::INT, "runtime_adaptive_min_node_load_regions", PROPERTY_HINT_RANGE, "0,64,1" ),
		"set_runtime_adaptive_min_node_load_regions",
		"get_runtime_adaptive_min_node_load_regions" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveMinNodeUnloadRegions,
		getRuntimeAdaptiveMinNodeUnloadRegions,
		"regions",
		PropertyInfo( Variant::INT, "runtime_adaptive_min_node_unload_regions", PROPERTY_HINT_RANGE, "0,64,1" ),
		"set_runtime_adaptive_min_node_unload_regions",
		"get_runtime_adaptive_min_node_unload_regions" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeAdaptiveMinWorldUploadKb,
					   getRuntimeAdaptiveMinWorldUploadKb,
					   "kb",
					   PropertyInfo( Variant::INT,
									 "runtime_adaptive_min_world_upload_kb",
									 PROPERTY_HINT_RANGE,
									 "1,65536,1,suffix:KiB" ),
					   "set_runtime_adaptive_min_world_upload_kb",
					   "get_runtime_adaptive_min_world_upload_kb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveMinWorldClusters,
		getRuntimeAdaptiveMinWorldClusters,
		"clusters",
		PropertyInfo( Variant::INT, "runtime_adaptive_min_world_clusters", PROPERTY_HINT_RANGE, "1,1048576,1" ),
		"set_runtime_adaptive_min_world_clusters",
		"get_runtime_adaptive_min_world_clusters" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveMinVtRequests,
		getRuntimeAdaptiveMinVtRequests,
		"requests",
		PropertyInfo( Variant::INT, "runtime_adaptive_min_vt_requests", PROPERTY_HINT_RANGE, "1,65536,1" ),
		"set_runtime_adaptive_min_vt_requests",
		"get_runtime_adaptive_min_vt_requests" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeAdaptiveMinVtAdmissions,
		getRuntimeAdaptiveMinVtAdmissions,
		"admissions",
		PropertyInfo( Variant::INT, "runtime_adaptive_min_vt_admissions", PROPERTY_HINT_RANGE, "1,4096,1" ),
		"set_runtime_adaptive_min_vt_admissions",
		"get_runtime_adaptive_min_vt_admissions" );

	ADD_SUBGROUP( "Boundary Phasing", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeBoundaryWorkPhasingEnabled,
					   isRuntimeBoundaryWorkPhasingEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "runtime_boundary_work_phasing_enabled" ),
					   "set_runtime_boundary_work_phasing_enabled",
					   "is_runtime_boundary_work_phasing_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeBoundaryNodeDelayFrames,
		getRuntimeBoundaryNodeDelayFrames,
		"frames",
		PropertyInfo( Variant::INT, "runtime_boundary_node_delay_frames", PROPERTY_HINT_RANGE, "0,8,1,suffix:frames" ),
		"set_runtime_boundary_node_delay_frames",
		"get_runtime_boundary_node_delay_frames" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeBoundaryInstancerDelayFrames,
					   getRuntimeBoundaryInstancerDelayFrames,
					   "frames",
					   PropertyInfo( Variant::INT,
									 "runtime_boundary_instancer_delay_frames",
									 PROPERTY_HINT_RANGE,
									 "0,8,1,suffix:frames" ),
					   "set_runtime_boundary_instancer_delay_frames",
					   "get_runtime_boundary_instancer_delay_frames" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setRuntimeBoundaryWorldDelayFrames,
		getRuntimeBoundaryWorldDelayFrames,
		"frames",
		PropertyInfo( Variant::INT, "runtime_boundary_world_delay_frames", PROPERTY_HINT_RANGE, "0,8,1,suffix:frames" ),
		"set_runtime_boundary_world_delay_frames",
		"get_runtime_boundary_world_delay_frames" );

	ADD_GROUP( "World Rendering", "world_" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setWorldMaxSourceVertices,
		getWorldMaxSourceVertices,
		"value",
		PropertyInfo( Variant::INT, "world_max_source_vertices", PROPERTY_HINT_RANGE, "1024,16777216,1024" ),
		"set_world_max_source_vertices",
		"get_world_max_source_vertices" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setWorldMaxClusters,
					   getWorldMaxClusters,
					   "value",
					   PropertyInfo( Variant::INT, "world_max_clusters", PROPERTY_HINT_RANGE, "256,1048576,256" ),
					   "set_world_max_clusters",
					   "get_world_max_clusters" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setWorldMaxTerrainClusters,
					   getWorldMaxTerrainClusters,
					   "value",
					   PropertyInfo( Variant::INT, "world_max_terrain_clusters", PROPERTY_HINT_RANGE, "64,1048576,64" ),
					   "set_world_max_terrain_clusters",
					   "get_world_max_terrain_clusters" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setWorldMaxTransforms,
					   getWorldMaxTransforms,
					   "value",
					   PropertyInfo( Variant::INT, "world_max_transforms", PROPERTY_HINT_RANGE, "256,1048576,256" ),
					   "set_world_max_transforms",
					   "get_world_max_transforms" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setWorldMaxVisibleItems,
					   getWorldMaxVisibleItems,
					   "value",
					   PropertyInfo( Variant::INT, "world_max_visible_items", PROPERTY_HINT_RANGE, "256,1048576,256" ),
					   "set_world_max_visible_items",
					   "get_world_max_visible_items" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setWorldMaxMaterials,
					   getWorldMaxMaterials,
					   "value",
					   PropertyInfo( Variant::INT, "world_max_materials", PROPERTY_HINT_RANGE, "16,65536,16" ),
					   "set_world_max_materials",
					   "get_world_max_materials" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setWorldUploadBudgetKb,
		getWorldUploadBudgetKb,
		"value",
		PropertyInfo( Variant::INT, "world_upload_budget_kb", PROPERTY_HINT_RANGE, "64,65536,64,suffix:KiB" ),
		"set_world_upload_budget_kb",
		"get_world_upload_budget_kb" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setWorldClusterUploadBudget,
					   getWorldClusterUploadBudget,
					   "value",
					   PropertyInfo( Variant::INT,
									 "world_cluster_upload_budget",
									 PROPERTY_HINT_RANGE,
									 "64,1048576,64,suffix:clusters/frame" ),
					   "set_world_cluster_upload_budget",
					   "get_world_cluster_upload_budget" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setWorldHiZMaxDimension,
					   getWorldHiZMaxDimension,
					   "value",
					   PropertyInfo( Variant::INT, "world_hiz_max_dimension", PROPERTY_HINT_RANGE, "64,8192,64" ),
					   "set_world_hiz_max_dimension",
					   "get_world_hiz_max_dimension" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setWorldHiZMaxMipCount,
					   getWorldHiZMaxMipCount,
					   "value",
					   PropertyInfo( Variant::INT, "world_hiz_max_mip_count", PROPERTY_HINT_RANGE, "1,16,1" ),
					   "set_world_hiz_max_mip_count",
					   "get_world_hiz_max_mip_count" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setVtRequestBudget,
		getVtRequestBudget,
		"value",
		PropertyInfo( Variant::INT, "vt_request_budget", PROPERTY_HINT_RANGE, "1,65536,1,suffix:requests/frame" ),
		"set_vt_request_budget",
		"get_vt_request_budget" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setVtAdmissionBudget,
		getVtAdmissionBudget,
		"value",
		PropertyInfo( Variant::INT, "vt_admission_budget", PROPERTY_HINT_RANGE, "1,4096,1,suffix:requests/frame" ),
		"set_vt_admission_budget",
		"get_vt_admission_budget" );

	ADD_GROUP( "Memory Budgets", "" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setTotalMemoryBudgetMb,
		getTotalMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "total_memory_budget_mb", PROPERTY_HINT_RANGE, "32,4096,1,suffix: MiB" ),
		"set_total_memory_budget_mb",
		"get_total_memory_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setMemoryTransientReserveMb,
		getMemoryTransientReserveMb,
		"reserve_mb",
		PropertyInfo( Variant::INT, "memory_transient_reserve_mb", PROPERTY_HINT_RANGE, "4,1024,1,suffix: MiB" ),
		"set_memory_transient_reserve_mb",
		"get_memory_transient_reserve_mb" );
	ADD_SUBGROUP( "Resource Caps (0 = Shared Total)", "" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setVirtualTextureMemoryBudgetMb,
		getVirtualTextureMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "virtual_texture_memory_budget_mb", PROPERTY_HINT_RANGE, "0,4096,1,suffix: MiB" ),
		"set_virtual_texture_memory_budget_mb",
		"get_virtual_texture_memory_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setInstanceRenderMemoryBudgetMb,
		getInstanceRenderMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "instance_render_memory_budget_mb", PROPERTY_HINT_RANGE, "0,4096,1,suffix: MiB" ),
		"set_instance_render_memory_budget_mb",
		"get_instance_render_memory_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setWorldRenderMemoryBudgetMb,
		getWorldRenderMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "world_render_memory_budget_mb", PROPERTY_HINT_RANGE, "0,4096,1,suffix: MiB" ),
		"set_world_render_memory_budget_mb",
		"get_world_render_memory_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setTerrainRenderMemoryBudgetMb,
		getTerrainRenderMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "terrain_render_memory_budget_mb", PROPERTY_HINT_RANGE, "0,4096,1,suffix: MiB" ),
		"set_terrain_render_memory_budget_mb",
		"get_terrain_render_memory_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setHlodRenderMemoryBudgetMb,
		getHlodRenderMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "hlod_render_memory_budget_mb", PROPERTY_HINT_RANGE, "0,4096,1,suffix: MiB" ),
		"set_hlod_render_memory_budget_mb",
		"get_hlod_render_memory_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setOceanRenderMemoryBudgetMb,
		getOceanRenderMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "ocean_render_memory_budget_mb", PROPERTY_HINT_RANGE, "0,4096,1,suffix: MiB" ),
		"set_ocean_render_memory_budget_mb",
		"get_ocean_render_memory_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setForwardLightingMemoryBudgetMb,
		getForwardLightingMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "forward_lighting_memory_budget_mb", PROPERTY_HINT_RANGE, "0,4096,1,suffix: MiB" ),
		"set_forward_lighting_memory_budget_mb",
		"get_forward_lighting_memory_budget_mb" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setCookingMemoryBudgetMb,
		getCookingMemoryBudgetMb,
		"budget_mb",
		PropertyInfo( Variant::INT, "cooking_memory_budget_mb", PROPERTY_HINT_RANGE, "0,4096,1,suffix: MiB" ),
		"set_cooking_memory_budget_mb",
		"get_cooking_memory_budget_mb" );
	ADD_SUBGROUP( "Policy Diagnostics", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShaderCacheBudgetAuto,
					   isShaderCacheBudgetAuto,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "shader_cache_budget_auto" ),
					   "set_shader_cache_budget_auto",
					   "is_shader_cache_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"shader_cache_effective_mb",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_effective_shader_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL,
								"memory_budget_overcommitted",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "is_memory_budget_overcommitted" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"memory_budget_headroom_bytes",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_memory_budget_headroom_bytes" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
								"memory_budget_policy_generation",
								PROPERTY_HINT_NONE,
								"",
								PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
				  "",
				  "get_memory_budget_policy_generation" );

	ADD_GROUP( "Collision", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCollisionMode,
					   getCollisionMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "collision_mode",
									 PROPERTY_HINT_ENUM,
									 "Disabled,Dynamic / Game,Dynamic / Editor,Full / Game,Full / Editor" ),
					   "set_collision_mode",
					   "get_collision_mode" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCollisionShapeSize,
					   getCollisionShapeSize,
					   "size",
					   PropertyInfo( Variant::INT, "collision_shape_size", PROPERTY_HINT_RANGE, "8,64,8" ),
					   "set_collision_shape_size",
					   "get_collision_shape_size" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCollisionRadius,
					   getCollisionRadius,
					   "radius",
					   PropertyInfo( Variant::INT, "collision_radius", PROPERTY_HINT_RANGE, "16,256,16" ),
					   "set_collision_radius",
					   "get_collision_radius" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCollisionTarget,
					   getCollisionTarget,
					   "node",
					   PropertyInfo( Variant::OBJECT,
									 "collision_target",
									 PROPERTY_HINT_NODE_TYPE,
									 "Node3D",
									 PROPERTY_USAGE_DEFAULT,
									 "Node3D" ),
					   "set_collision_target",
					   "get_collision_target" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCollisionLayer,
					   getCollisionLayer,
					   "layers",
					   PropertyInfo( Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS ),
					   "set_collision_layer",
					   "get_collision_layer" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCollisionMask,
					   getCollisionMask,
					   "mask",
					   PropertyInfo( Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS ),
					   "set_collision_mask",
					   "get_collision_mask" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setCollisionPriority,
					   getCollisionPriority,
					   "priority",
					   PropertyInfo( Variant::FLOAT, "collision_priority", PROPERTY_HINT_RANGE, "0.1,256,.1" ),
					   "set_collision_priority",
					   "get_collision_priority" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setPhysicsMaterial,
		getPhysicsMaterial,
		"material",
		PropertyInfo( Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial" ),
		"set_physics_material",
		"get_physics_material" );

	ADD_GROUP( "Ocean", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanEnabled,
					   isOceanEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "ocean_enabled" ),
					   "set_ocean_enabled",
					   "is_ocean_enabled" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanPerformancePreset,
					   getOceanPerformancePreset,
					   "preset",
					   PropertyInfo( Variant::INT,
									 "ocean_performance_preset",
									 PROPERTY_HINT_ENUM,
									 "Custom,Border Minimum,Border Balanced,High,Cinematic" ),
					   "set_ocean_performance_preset",
					   "get_ocean_performance_preset" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanVertexWaveIterations,
					   getOceanVertexWaveIterations,
					   "iterations",
					   PropertyInfo( Variant::INT, "ocean_vertex_wave_iterations", PROPERTY_HINT_RANGE, "0,3,1" ),
					   "set_ocean_vertex_wave_iterations",
					   "get_ocean_vertex_wave_iterations" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanFragmentWaveIterations,
					   getOceanFragmentWaveIterations,
					   "iterations",
					   PropertyInfo( Variant::INT, "ocean_fragment_wave_iterations", PROPERTY_HINT_RANGE, "0,4,1" ),
					   "set_ocean_fragment_wave_iterations",
					   "get_ocean_fragment_wave_iterations" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanRefractionMode,
					   getOceanRefractionMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "ocean_refraction_mode",
									 PROPERTY_HINT_ENUM,
									 "Off,Depth Tint,Full Screen Refraction" ),
					   "set_ocean_refraction_mode",
					   "get_ocean_refraction_mode" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setOceanFoamMode,
		getOceanFoamMode,
		"mode",
		PropertyInfo( Variant::INT, "ocean_foam_mode", PROPERTY_HINT_ENUM, "Off,Shore Only,Shore + Crest" ),
		"set_ocean_foam_mode",
		"get_ocean_foam_mode" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setOceanScatteringMode,
		getOceanScatteringMode,
		"mode",
		PropertyInfo( Variant::INT, "ocean_scattering_mode", PROPERTY_HINT_ENUM, "Off,Approximate,Full" ),
		"set_ocean_scattering_mode",
		"get_ocean_scattering_mode" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanShadingMode,
					   getOceanShadingMode,
					   "mode",
					   PropertyInfo( Variant::INT,
									 "ocean_shading_mode",
									 PROPERTY_HINT_ENUM,
									 "Lambert Diffuse,Lambert + GGX,Burley + GGX,Unshaded,Lambert Vertex Lit" ),
					   "set_ocean_shading_mode",
					   "get_ocean_shading_mode" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanMeshLods,
					   getOceanMeshLods,
					   "count",
					   PropertyInfo( Variant::INT, "ocean_mesh_lods", PROPERTY_HINT_RANGE, "1,10,1" ),
					   "set_ocean_mesh_lods",
					   "get_ocean_mesh_lods" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanTessellationLevel,
					   getOceanTessellationLevel,
					   "size",
					   PropertyInfo( Variant::INT, "ocean_tessellation_level", PROPERTY_HINT_RANGE, "0,6,1" ),
					   "set_ocean_tessellation_level",
					   "get_ocean_tessellation_level" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanMeshSize,
					   getOceanMeshSize,
					   "size",
					   PropertyInfo( Variant::INT, "ocean_mesh_size", PROPERTY_HINT_RANGE, "8,256,2" ),
					   "set_ocean_mesh_size",
					   "get_ocean_mesh_size" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setOceanVertexSpacing,
		getOceanVertexSpacing,
		"scale",
		PropertyInfo( Variant::FLOAT, "ocean_vertex_spacing", PROPERTY_HINT_RANGE, "0.25,10.0,0.05,or_greater" ),
		"set_ocean_vertex_spacing",
		"get_ocean_vertex_spacing" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setOceanCullMargin,
		getOceanCullMargin,
		"margin",
		PropertyInfo( Variant::FLOAT, "ocean_cull_margin", PROPERTY_HINT_RANGE, "0.0,10000.0,.5,or_greater" ),
		"set_ocean_cull_margin",
		"get_ocean_cull_margin" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setOceanCastShadows,
		getOceanCastShadows,
		"shadow_casting_setting",
		PropertyInfo( Variant::INT, "ocean_cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only" ),
		"set_ocean_cast_shadows",
		"get_ocean_cast_shadows" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setOceanRenderLayers,
					   getOceanRenderLayers,
					   "layers",
					   PropertyInfo( Variant::INT, "ocean_render_layers", PROPERTY_HINT_LAYERS_3D_RENDER ),
					   "set_ocean_render_layers",
					   "get_ocean_render_layers" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setOceanSettings,
		getOceanSettings,
		"settings",
		PropertyInfo( Variant::OBJECT, "ocean_settings", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DOceanSettings" ),
		"set_ocean_settings",
		"get_ocean_settings" );

	ADD_GROUP( "Semantic Lighting", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setSemanticShadowPolicyEnabled,
					   isSemanticShadowPolicyEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "semantic_shadow_policy_enabled" ),
					   "set_semantic_shadow_policy_enabled",
					   "is_semantic_shadow_policy_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setEnvironmentShadowQuality,
		getEnvironmentShadowQuality,
		"quality",
		PropertyInfo( Variant::INT, "environment_shadow_quality", PROPERTY_HINT_ENUM, "Low,Normal,High,Ultra" ),
		"set_environment_shadow_quality",
		"get_environment_shadow_quality" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setEnvironmentShadowExternalMask,
					   getEnvironmentShadowExternalMask,
					   "mask",
					   PropertyInfo( Variant::INT, "environment_shadow_external_mask", PROPERTY_HINT_LAYERS_3D_RENDER ),
					   "set_environment_shadow_external_mask",
					   "get_environment_shadow_external_mask" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setSemanticDirectionalReceptionEnabled,
					   isSemanticDirectionalReceptionEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "semantic_directional_reception_enabled" ),
					   "set_semantic_directional_reception_enabled",
					   "is_semantic_directional_reception_enabled" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setEnvironmentDirectionalExternalMask,
		getEnvironmentDirectionalExternalMask,
		"mask",
		PropertyInfo( Variant::INT, "environment_directional_external_mask", PROPERTY_HINT_LAYERS_3D_RENDER ),
		"set_environment_directional_external_mask",
		"get_environment_directional_external_mask" );

	ADD_GROUP( "Editor Diagnostics & Display", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setRuntimeProfilingEnabled,
					   isRuntimeProfilingEnabled,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "runtime_profiling_enabled" ),
					   "set_runtime_profiling_enabled",
					   "is_runtime_profiling_enabled" );

	ADD_SUBGROUP( "Viewport Controls", "" );
	T3D_BIND_PROPERTY(
		Terrain3D,
		setLabelDistance,
		getLabelDistance,
		"distance",
		PropertyInfo( Variant::FLOAT, "label_distance", PROPERTY_HINT_RANGE, "0.0,10000.0,0.5,or_greater" ),
		"set_label_distance",
		"get_label_distance" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setLabelSize,
					   getLabelSize,
					   "size",
					   PropertyInfo( Variant::INT, "label_size", PROPERTY_HINT_RANGE, "24,128,1" ),
					   "set_label_size",
					   "get_label_size" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowRegionGrid,
					   getShowRegionGrid,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
					   "set_show_region_grid",
					   "get_show_region_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "free_editor_textures" ),
				  "set_free_editor_textures",
				  "get_free_editor_textures" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setInstancerMode,
					   getInstancerMode,
					   "mode",
					   PropertyInfo( Variant::INT, "instancer_mode", PROPERTY_HINT_ENUM, "Disabled,Normal" ),
					   "set_instancer_mode",
					   "get_instancer_mode" );
	ADD_SUBGROUP( "Viewport Overlays", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_region_grid" ), "set_show_region_grid", "get_show_region_grid" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowInstancerGrid,
					   getShowInstancerGrid,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_instancer_grid" ),
					   "set_show_instancer_grid",
					   "get_show_instancer_grid" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowVertexGrid,
					   getShowVertexGrid,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_vertex_grid" ),
					   "set_show_vertex_grid",
					   "get_show_vertex_grid" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowContours,
					   getShowContours,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_contours" ),
					   "set_show_contours",
					   "get_show_contours" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowNavigation,
					   getShowNavigation,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_navigation" ),
					   "set_show_navigation",
					   "get_show_navigation" );
	ADD_SUBGROUP( "Terrain Debug Shading", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowCheckered,
					   getShowCheckered,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_checkered" ),
					   "set_show_checkered",
					   "get_show_checkered" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowGrey,
					   getShowGrey,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_grey" ),
					   "set_show_grey",
					   "get_show_grey" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowHeightmap,
					   getShowHeightmap,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_heightmap" ),
					   "set_show_heightmap",
					   "get_show_heightmap" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowJaggedness,
					   getShowJaggedness,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_jaggedness" ),
					   "set_show_jaggedness",
					   "get_show_jaggedness" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowAutoshader,
					   getShowAutoshader,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_autoshader" ),
					   "set_show_autoshader",
					   "get_show_autoshader" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowControlTexture,
					   getShowControlTexture,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_control_texture" ),
					   "set_show_control_texture",
					   "get_show_control_texture" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowControlBlend,
					   getShowControlBlend,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_control_blend" ),
					   "set_show_control_blend",
					   "get_show_control_blend" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowControlAngle,
					   getShowControlAngle,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_control_angle" ),
					   "set_show_control_angle",
					   "get_show_control_angle" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowControlScale,
					   getShowControlScale,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_control_scale" ),
					   "set_show_control_scale",
					   "get_show_control_scale" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowColormap,
					   getShowColormap,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_colormap" ),
					   "set_show_colormap",
					   "get_show_colormap" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowRoughmap,
					   getShowRoughmap,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_roughmap" ),
					   "set_show_roughmap",
					   "get_show_roughmap" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowDisplacementBuffer,
					   getShowDisplacementBuffer,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_displacement_buffer" ),
					   "set_show_displacement_buffer",
					   "get_show_displacement_buffer" );
	ADD_SUBGROUP( "Material Channel Views", "" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowTextureAlbedo,
					   getShowTextureAlbedo,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_texture_albedo" ),
					   "set_show_texture_albedo",
					   "get_show_texture_albedo" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowTextureHeight,
					   getShowTextureHeight,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_texture_height" ),
					   "set_show_texture_height",
					   "get_show_texture_height" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowTextureNormal,
					   getShowTextureNormal,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_texture_normal" ),
					   "set_show_texture_normal",
					   "get_show_texture_normal" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowTextureRough,
					   getShowTextureRough,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_texture_rough" ),
					   "set_show_texture_rough",
					   "get_show_texture_rough" );
	T3D_BIND_PROPERTY( Terrain3D,
					   setShowTextureAo,
					   getShowTextureAo,
					   "enabled",
					   PropertyInfo( Variant::BOOL, "show_texture_ao" ),
					   "set_show_texture_ao",
					   "get_show_texture_ao" );

	ADD_SIGNAL( MethodInfo( "material_changed" ) );
	ADD_SIGNAL( MethodInfo( "performance_profile_state_changed", PropertyInfo( Variant::INT, "status" ) ) );
	ADD_SIGNAL( MethodInfo( "memory_pressure_changed",
							PropertyInfo( Variant::BOOL, "pressure" ),
							PropertyInfo( Variant::BOOL, "overcommitted" ),
							PropertyInfo( Variant::INT, "headroom_bytes" ) ) );
	ADD_SIGNAL( MethodInfo( "cache_budget_changed",
							PropertyInfo( Variant::STRING_NAME, "subsystem" ),
							PropertyInfo( Variant::INT, "effective_bytes" ),
							PropertyInfo( Variant::STRING_NAME, "limiting_constraint" ) ) );
	ADD_SIGNAL( MethodInfo( "cache_thrash_state_changed",
							PropertyInfo( Variant::STRING_NAME, "cache" ),
							PropertyInfo( Variant::BOOL, "thrashing" ),
							PropertyInfo( Variant::INT, "activity" ) ) );
	ADD_SIGNAL( MethodInfo( "assets_changed" ) );
	ADD_SIGNAL( MethodInfo( "editor_execution_mode_changed", PropertyInfo( Variant::INT, "mode" ) ) );
}
