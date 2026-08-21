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
#include "streaming/terrain_3d_upload_bridge.h"
#include "terrain_3d.h"

#include <algorithm>
#include <cstring>
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
#include <limits>
#include <numeric>
#include <utility>

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

void T3DTerrain::_validate_property( PropertyInfo &rProperty ) const {
	if ( m_nTessellationLevel == 0 ) {
		// Hide all displacement properties
		if ( rProperty.name == StringName( "displacement_scale" ) ||
				rProperty.name == StringName( "displacement_sharpness" ) ||
				rProperty.name == StringName( "buffer_shader_override_enabled" ) ||
				rProperty.name == StringName( "buffer_shader_override" ) ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}
	// Hide all ocean properties if not enabled
	if ( !m_bOceanEnabled && rProperty.name != StringName( "ocean_enabled" ) &&
			rProperty.name.begins_with( "ocean_" ) ) {
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
	ClassDB::bind_method(
			D_METHOD( "is_editor_runtime_streaming_preview" ), &T3DTerrain::isEditorRuntimeStreamingPreview );
	ClassDB::bind_method(
			D_METHOD( "set_editor_node_streaming_preview", "enabled" ), &T3DTerrain::setEditorNodeStreamingPreview );
	ClassDB::bind_method(
			D_METHOD( "is_editor_node_streaming_preview_enabled" ), &T3DTerrain::isEditorNodeStreamingPreviewEnabled );
	ClassDB::bind_method(
			D_METHOD( "is_editor_node_streaming_preview_active" ), &T3DTerrain::isEditorNodeStreamingPreviewActive );
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
	ClassDB::bind_method(
			D_METHOD( "set_memory_transient_reserve_mb", "reserve_mb" ), &T3DTerrain::setMemoryTransientReserveMb );
	ClassDB::bind_method( D_METHOD( "get_memory_transient_reserve_mb" ), &T3DTerrain::getMemoryTransientReserveMb );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_cache_budget_auto", "enabled" ), &T3DTerrain::setGpuRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cache_budget_auto" ), &T3DTerrain::isGpuRegionCacheBudgetAuto );
	ClassDB::bind_method(
			D_METHOD( "set_cpu_region_cache_budget_auto", "enabled" ), &T3DTerrain::setCpuRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_cpu_region_cache_budget_auto" ), &T3DTerrain::isCpuRegionCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "set_instance_region_cache_budget_auto", "enabled" ),
			&T3DTerrain::setInstanceRegionCacheBudgetAuto );
	ClassDB::bind_method(
			D_METHOD( "is_instance_region_cache_budget_auto" ), &T3DTerrain::isInstanceRegionCacheBudgetAuto );
	ClassDB::bind_method(
			D_METHOD( "set_lightmap_residency_budget_auto", "enabled" ), &T3DTerrain::setLightmapResidencyBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_lightmap_residency_budget_auto" ), &T3DTerrain::isLightmapResidencyBudgetAuto );
	ClassDB::bind_method( D_METHOD( "set_hlod_memory_budget_auto", "enabled" ), &T3DTerrain::setHlodMemoryBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_hlod_memory_budget_auto" ), &T3DTerrain::isHlodMemoryBudgetAuto );
	ClassDB::bind_method(
			D_METHOD( "set_lightmap_residency_budget_mb", "budget_mb" ), &T3DTerrain::setLightmapResidencyBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_lightmap_residency_budget_mb" ), &T3DTerrain::getLightmapResidencyBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "set_shader_cache_budget_auto", "enabled" ), &T3DTerrain::setShaderCacheBudgetAuto );
	ClassDB::bind_method( D_METHOD( "is_shader_cache_budget_auto" ), &T3DTerrain::isShaderCacheBudgetAuto );
	ClassDB::bind_method(
			D_METHOD( "get_effective_gpu_region_cache_budget_mb" ), &T3DTerrain::getEffectiveGpuRegionCacheBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "get_effective_cpu_region_cache_budget_mb" ), &T3DTerrain::getEffectiveCpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_instance_region_cache_budget_mb" ),
			&T3DTerrain::getEffectiveInstanceRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_effective_lightmap_residency_budget_mb" ),
			&T3DTerrain::getEffectiveLightmapResidencyBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "get_effective_hlod_memory_budget_mb" ), &T3DTerrain::getEffectiveHlodMemoryBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "get_effective_shader_cache_budget_mb" ), &T3DTerrain::getEffectiveShaderCacheBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "get_memory_budget_policy_generation" ), &T3DTerrain::getMemoryBudgetPolicyGeneration );
	ClassDB::bind_method( D_METHOD( "is_memory_budget_overcommitted" ), &T3DTerrain::isMemoryBudgetOvercommitted );
	ClassDB::bind_method( D_METHOD( "get_memory_budget_headroom_bytes" ), &T3DTerrain::getMemoryBudgetHeadroomBytes );
	ClassDB::bind_method( D_METHOD( "force_memory_budget_rebalance" ), &T3DTerrain::forceMemoryBudgetRebalance );
	ClassDB::bind_method( D_METHOD( "get_runtime_diagnostics", "deep_introspection" ),
			&T3DTerrain::getRuntimeDiagnostics,
			DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_runtime_parity_snapshot" ), &T3DTerrain::getRuntimeParitySnapshot );
	ClassDB::bind_method( D_METHOD( "get_memory_stats_snapshot" ), &T3DTerrain::getMemoryStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_shader_tier_stats_snapshot" ), &T3DTerrain::getShaderTierStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_cache_stats_snapshot", "deep_introspection" ),
			&T3DTerrain::getCacheStatsSnapshot,
			DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_frame_stats_snapshot" ), &T3DTerrain::getFrameStatsSnapshot );
	ClassDB::bind_method( D_METHOD( "get_runtime_profile_history", "max_samples" ),
			&T3DTerrain::getRuntimeProfileHistory,
			DEFVAL( 600 ) );
	ClassDB::bind_method( D_METHOD( "get_runtime_profile_history_since", "after_sequence", "max_samples" ),
			&T3DTerrain::getRuntimeProfileHistorySince,
			DEFVAL( 600 ) );
	ClassDB::bind_method( D_METHOD( "reset_runtime_diagnostics" ), &T3DTerrain::resetRuntimeDiagnostics );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_profiling_enabled", "enabled" ), &T3DTerrain::setRuntimeProfilingEnabled );
	ClassDB::bind_method( D_METHOD( "is_runtime_profiling_enabled" ), &T3DTerrain::isRuntimeProfilingEnabled );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_work_budgets_enabled", "enabled" ),
			&T3DTerrain::setRuntimeAdaptiveWorkBudgetsEnabled );
	ClassDB::bind_method(
			D_METHOD( "is_runtime_adaptive_work_budgets_enabled" ), &T3DTerrain::isRuntimeAdaptiveWorkBudgetsEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_adaptive_target_ms", "target_ms" ), &T3DTerrain::setRuntimeAdaptiveTargetMs );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_target_ms" ), &T3DTerrain::getRuntimeAdaptiveTargetMs );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_adaptive_hard_max_ms", "hard_max_ms" ), &T3DTerrain::setRuntimeAdaptiveHardMaxMs );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_hard_max_ms" ), &T3DTerrain::getRuntimeAdaptiveHardMaxMs );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_adjustment_window_frames", "frames" ),
			&T3DTerrain::setRuntimeAdaptiveAdjustmentWindowFrames );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_adjustment_window_frames" ),
			&T3DTerrain::getRuntimeAdaptiveAdjustmentWindowFrames );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_adaptive_min_gpu_pages", "pages" ), &T3DTerrain::setRuntimeAdaptiveMinGpuPages );
	ClassDB::bind_method(
			D_METHOD( "get_runtime_adaptive_min_gpu_pages" ), &T3DTerrain::getRuntimeAdaptiveMinGpuPages );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_adaptive_min_cpu_pages", "pages" ), &T3DTerrain::setRuntimeAdaptiveMinCpuPages );
	ClassDB::bind_method(
			D_METHOD( "get_runtime_adaptive_min_cpu_pages" ), &T3DTerrain::getRuntimeAdaptiveMinCpuPages );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_instance_pages", "pages" ),
			&T3DTerrain::setRuntimeAdaptiveMinInstancePages );
	ClassDB::bind_method(
			D_METHOD( "get_runtime_adaptive_min_instance_pages" ), &T3DTerrain::getRuntimeAdaptiveMinInstancePages );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_instancer_cells", "cells" ),
			&T3DTerrain::setRuntimeAdaptiveMinInstancerCells );
	ClassDB::bind_method(
			D_METHOD( "get_runtime_adaptive_min_instancer_cells" ), &T3DTerrain::getRuntimeAdaptiveMinInstancerCells );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_node_load_regions", "regions" ),
			&T3DTerrain::setRuntimeAdaptiveMinNodeLoadRegions );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_min_node_load_regions" ),
			&T3DTerrain::getRuntimeAdaptiveMinNodeLoadRegions );
	ClassDB::bind_method( D_METHOD( "set_runtime_adaptive_min_node_unload_regions", "regions" ),
			&T3DTerrain::setRuntimeAdaptiveMinNodeUnloadRegions );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_min_node_unload_regions" ),
			&T3DTerrain::getRuntimeAdaptiveMinNodeUnloadRegions );
	ClassDB::bind_method( D_METHOD( "get_runtime_adaptive_budget_stats" ), &T3DTerrain::getRuntimeAdaptiveBudgetStats );
	ClassDB::bind_method( D_METHOD( "set_runtime_boundary_work_phasing_enabled", "enabled" ),
			&T3DTerrain::setRuntimeBoundaryWorkPhasingEnabled );
	ClassDB::bind_method(
			D_METHOD( "is_runtime_boundary_work_phasing_enabled" ), &T3DTerrain::isRuntimeBoundaryWorkPhasingEnabled );
	ClassDB::bind_method( D_METHOD( "set_runtime_boundary_node_delay_frames", "frames" ),
			&T3DTerrain::setRuntimeBoundaryNodeDelayFrames );
	ClassDB::bind_method(
			D_METHOD( "get_runtime_boundary_node_delay_frames" ), &T3DTerrain::getRuntimeBoundaryNodeDelayFrames );
	ClassDB::bind_method( D_METHOD( "set_runtime_boundary_instancer_delay_frames", "frames" ),
			&T3DTerrain::setRuntimeBoundaryInstancerDelayFrames );
	ClassDB::bind_method( D_METHOD( "get_runtime_boundary_instancer_delay_frames" ),
			&T3DTerrain::getRuntimeBoundaryInstancerDelayFrames );
	ClassDB::bind_method(
			D_METHOD( "get_runtime_boundary_phasing_stats" ), &T3DTerrain::getRuntimeBoundaryPhasingStats );
	ClassDB::bind_method( D_METHOD( "is_editor_authoring_mode" ), &T3DTerrain::isEditorAuthoringMode );
	ClassDB::bind_method(
			D_METHOD( "is_authoring_residency_readback_safe" ), &T3DTerrain::isAuthoringResidencyReadbackSafe );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cache_effective" ), &T3DTerrain::isGpuRegionCacheEffective );
	ClassDB::bind_method( D_METHOD( "is_cpu_region_cache_effective" ), &T3DTerrain::isCpuRegionCacheEffective );
	ClassDB::bind_method(
			D_METHOD( "is_instance_region_cache_effective" ), &T3DTerrain::isInstanceRegionCacheEffective );
	ClassDB::bind_method(
			D_METHOD( "is_instancer_cell_streaming_effective" ), &T3DTerrain::isInstancerCellStreamingEffective );
	ClassDB::bind_method(
			D_METHOD( "is_dynamic_node_streaming_effective" ), &T3DTerrain::isDynamicNodeStreamingEffective );
	ClassDB::bind_method( D_METHOD( "set_performance_target", "target" ), &T3DTerrain::setPerformanceTarget );
	ClassDB::bind_method( D_METHOD( "get_performance_target" ), &T3DTerrain::getPerformanceTarget );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_status" ), &T3DTerrain::getPerformanceProfileStatus );
	ClassDB::bind_method(
			D_METHOD( "get_performance_profile_status_name" ), &T3DTerrain::getPerformanceProfileStatusName );
	ClassDB::bind_method( D_METHOD( "get_performance_profile_version" ), &T3DTerrain::getPerformanceProfileVersion );
	ClassDB::bind_method(
			D_METHOD( "get_performance_profile_signature" ), &T3DTerrain::getPerformanceProfileSignature );
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
	ClassDB::bind_method(
			D_METHOD( "set_streaming_prediction_enabled", "enabled" ), &T3DTerrain::setStreamingPredictionEnabled );
	ClassDB::bind_method( D_METHOD( "is_streaming_prediction_enabled" ), &T3DTerrain::isStreamingPredictionEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_streaming_prediction_lookahead", "seconds" ), &T3DTerrain::setStreamingPredictionLookahead );
	ClassDB::bind_method(
			D_METHOD( "get_streaming_prediction_lookahead" ), &T3DTerrain::getStreamingPredictionLookahead );
	ClassDB::bind_method( D_METHOD( "set_streaming_prediction_maximum_regions", "regions" ),
			&T3DTerrain::setStreamingPredictionMaximumRegions );
	ClassDB::bind_method(
			D_METHOD( "get_streaming_prediction_maximum_regions" ), &T3DTerrain::getStreamingPredictionMaximumRegions );
	ClassDB::bind_method( D_METHOD( "get_region_streaming_state", "region" ), &T3DTerrain::getRegionStreamingState );
	ClassDB::bind_method(
			D_METHOD( "is_region_streaming_prefetch", "region" ), &T3DTerrain::isRegionStreamingPrefetch );
	ClassDB::bind_method( D_METHOD( "get_region_streaming_stats" ), &T3DTerrain::getRegionStreamingStats );
	ClassDB::bind_method( D_METHOD( "get_region_streaming_configuration_status" ),
			&T3DTerrain::getRegionStreamingConfigurationStatus );
	ClassDB::bind_method(
			D_METHOD( "get_height_source_diagnostics", "world_position" ), &T3DTerrain::getHeightSourceDiagnostics );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_cache_enabled", "enabled" ), &T3DTerrain::setGpuRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cache_enabled" ), &T3DTerrain::isGpuRegionCacheEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_cache_budget_mb", "budget_mb" ), &T3DTerrain::setGpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_budget_mb" ), &T3DTerrain::getGpuRegionCacheBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_upload_budget_layers", "layers" ), &T3DTerrain::setGpuRegionUploadBudgetLayers );
	ClassDB::bind_method(
			D_METHOD( "get_gpu_region_upload_budget_layers" ), &T3DTerrain::getGpuRegionUploadBudgetLayers );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_upload_budget_mb", "budget_mb" ), &T3DTerrain::setGpuRegionUploadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_upload_budget_mb" ), &T3DTerrain::getGpuRegionUploadBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_spare_pages", "pages" ), &T3DTerrain::setGpuRegionSparePages );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_spare_pages" ), &T3DTerrain::getGpuRegionSparePages );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_retire_frames", "frames" ), &T3DTerrain::setGpuRegionRetireFrames );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_retire_frames" ), &T3DTerrain::getGpuRegionRetireFrames );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_detail_lods", "lods" ), &T3DTerrain::setGpuRegionDetailLods );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_detail_lods" ), &T3DTerrain::getGpuRegionDetailLods );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_detail_range_end_m", "range_m" ), &T3DTerrain::setGpuRegionDetailRangeEndM );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_detail_range_end_m" ), &T3DTerrain::getGpuRegionDetailRangeEndM );
	ClassDB::bind_method(
			D_METHOD( "get_gpu_region_max_supported_detail_lods" ), &T3DTerrain::getGpuRegionMaxSupportedDetailLods );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_max_supported_detail_range_m" ),
			&T3DTerrain::getGpuRegionMaxSupportedDetailRangeM );
	ClassDB::bind_method(
			D_METHOD( "get_gpu_region_required_core_pages" ), &T3DTerrain::getGpuRegionRequiredCorePages );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_mapped_capacity" ), &T3DTerrain::getGpuRegionMappedCapacity );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_detail_range_valid" ), &T3DTerrain::isGpuRegionDetailRangeValid );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_guard_regions", "regions" ), &T3DTerrain::setGpuRegionGuardRegions );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_guard_regions" ), &T3DTerrain::getGpuRegionGuardRegions );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_prefetch_rows", "rows" ), &T3DTerrain::setGpuRegionPrefetchRows );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_prefetch_rows" ), &T3DTerrain::getGpuRegionPrefetchRows );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_hysteresis_ticks", "ticks" ), &T3DTerrain::setGpuRegionHysteresisTicks );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_hysteresis_ticks" ), &T3DTerrain::getGpuRegionHysteresisTicks );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_color_resolution_divisor", "divisor" ),
			&T3DTerrain::setGpuRegionColorResolutionDivisor );
	ClassDB::bind_method(
			D_METHOD( "get_gpu_region_color_resolution_divisor" ), &T3DTerrain::getGpuRegionColorResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_derived_resolution_divisor", "divisor" ),
			&T3DTerrain::setGpuRegionDerivedResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_derived_resolution_divisor" ),
			&T3DTerrain::getGpuRegionDerivedResolutionDivisor );
	ClassDB::bind_method( D_METHOD( "set_gpu_middle_height_resolution_divisor", "divisor" ),
			&T3DTerrain::setGpuMiddleHeightResolutionDivisor );
	ClassDB::bind_method(
			D_METHOD( "get_gpu_middle_height_resolution_divisor" ), &T3DTerrain::getGpuMiddleHeightResolutionDivisor );
	ClassDB::bind_method(
			D_METHOD( "set_editor_gpu_region_cache_pages", "pages" ), &T3DTerrain::setEditorGpuRegionCachePages );
	ClassDB::bind_method( D_METHOD( "get_editor_gpu_region_cache_pages" ), &T3DTerrain::getEditorGpuRegionCachePages );
	ClassDB::bind_method( D_METHOD( "set_runtime_material_streaming_enabled", "enabled" ),
			&T3DTerrain::setRuntimeMaterialStreamingEnabled );
	ClassDB::bind_method(
			D_METHOD( "is_runtime_material_streaming_enabled" ), &T3DTerrain::isRuntimeMaterialStreamingEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_material_cache_layers", "layers" ), &T3DTerrain::setRuntimeMaterialCacheLayers );
	ClassDB::bind_method( D_METHOD( "get_runtime_material_cache_layers" ), &T3DTerrain::getRuntimeMaterialCacheLayers );
	ClassDB::bind_method( D_METHOD( "set_runtime_material_upload_budget_layers", "layers" ),
			&T3DTerrain::setRuntimeMaterialUploadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "get_runtime_material_upload_budget_layers" ),
			&T3DTerrain::getRuntimeMaterialUploadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "set_runtime_release_source_material_textures", "enabled" ),
			&T3DTerrain::setRuntimeReleaseSourceMaterialTextures );
	ClassDB::bind_method( D_METHOD( "is_runtime_release_source_material_textures" ),
			&T3DTerrain::isRuntimeReleaseSourceMaterialTextures );
	ClassDB::bind_method( D_METHOD( "set_gpu_far_quality_divisor", "divisor" ), &T3DTerrain::setGpuFarQualityDivisor );
	ClassDB::bind_method( D_METHOD( "get_gpu_far_quality_divisor" ), &T3DTerrain::getGpuFarQualityDivisor );
	ClassDB::bind_method(
			D_METHOD( "set_far_material_resolution", "resolution" ), &T3DTerrain::setFarMaterialResolution );
	ClassDB::bind_method( D_METHOD( "get_far_material_resolution" ), &T3DTerrain::getFarMaterialResolution );
	ClassDB::bind_method(
			D_METHOD( "set_far_control_resolution", "resolution" ), &T3DTerrain::setFarControlResolution );
	ClassDB::bind_method( D_METHOD( "get_far_control_resolution" ), &T3DTerrain::getFarControlResolution );
	ClassDB::bind_method( D_METHOD( "get_gpu_far_atlas_side" ), &T3DTerrain::getGpuFarAtlasSide );
	ClassDB::bind_method( D_METHOD( "get_gpu_far_height_atlas_side" ), &T3DTerrain::getGpuFarHeightAtlasSide );
	ClassDB::bind_method( D_METHOD( "get_gpu_far_surface_atlas_side" ), &T3DTerrain::getGpuFarSurfaceAtlasSide );
	ClassDB::bind_method(
			D_METHOD( "set_gpu_region_cooked_pages_enabled", "enabled" ), &T3DTerrain::setGpuRegionCookedPagesEnabled );
	ClassDB::bind_method(
			D_METHOD( "is_gpu_region_cooked_pages_enabled" ), &T3DTerrain::isGpuRegionCookedPagesEnabled );
	ClassDB::bind_method( D_METHOD( "set_gpu_region_cook_on_save", "enabled" ), &T3DTerrain::setGpuRegionCookOnSave );
	ClassDB::bind_method( D_METHOD( "is_gpu_region_cook_on_save" ), &T3DTerrain::isGpuRegionCookOnSave );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_stats" ), &T3DTerrain::getGpuRegionCacheStats );
	ClassDB::bind_method(
			D_METHOD( "set_cpu_region_cache_enabled", "enabled" ), &T3DTerrain::setCpuRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "is_cpu_region_cache_enabled" ), &T3DTerrain::isCpuRegionCacheEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_cpu_region_cache_budget_mb", "budget_mb" ), &T3DTerrain::setCpuRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_budget_mb" ), &T3DTerrain::getCpuRegionCacheBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "set_cpu_region_load_budget_layers", "layers" ), &T3DTerrain::setCpuRegionLoadBudgetLayers );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_load_budget_layers" ), &T3DTerrain::getCpuRegionLoadBudgetLayers );
	ClassDB::bind_method(
			D_METHOD( "set_cpu_region_load_budget_mb", "budget_mb" ), &T3DTerrain::setCpuRegionLoadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_load_budget_mb" ), &T3DTerrain::getCpuRegionLoadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_stats" ), &T3DTerrain::getCpuRegionCacheStats );
	ClassDB::bind_method(
			D_METHOD( "set_instance_region_cache_enabled", "enabled" ), &T3DTerrain::setInstanceRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "is_instance_region_cache_enabled" ), &T3DTerrain::isInstanceRegionCacheEnabled );
	ClassDB::bind_method( D_METHOD( "set_instance_region_cache_budget_mb", "budget_mb" ),
			&T3DTerrain::setInstanceRegionCacheBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "get_instance_region_cache_budget_mb" ), &T3DTerrain::getInstanceRegionCacheBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_instance_region_load_budget_pages", "pages" ),
			&T3DTerrain::setInstanceRegionLoadBudgetPages );
	ClassDB::bind_method(
			D_METHOD( "get_instance_region_load_budget_pages" ), &T3DTerrain::getInstanceRegionLoadBudgetPages );
	ClassDB::bind_method(
			D_METHOD( "set_instance_region_load_budget_mb", "budget_mb" ), &T3DTerrain::setInstanceRegionLoadBudgetMb );
	ClassDB::bind_method(
			D_METHOD( "get_instance_region_load_budget_mb" ), &T3DTerrain::getInstanceRegionLoadBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_instance_region_cache_stats" ), &T3DTerrain::getInstanceRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "set_instancer_cell_size", "cell_size" ), &T3DTerrain::setInstancerCellSize );
	ClassDB::bind_method( D_METHOD( "get_instancer_cell_size" ), &T3DTerrain::getInstancerCellSize );
	ClassDB::bind_method( D_METHOD( "set_instancer_cell_streaming_enabled", "enabled" ),
			&T3DTerrain::setInstancerCellStreamingEnabled );
	ClassDB::bind_method(
			D_METHOD( "is_instancer_cell_streaming_enabled" ), &T3DTerrain::isInstancerCellStreamingEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_instancer_single_lod_buffers", "enabled" ), &T3DTerrain::setInstancerSingleLodBuffers );
	ClassDB::bind_method( D_METHOD( "is_instancer_single_lod_buffers" ), &T3DTerrain::isInstancerSingleLodBuffers );
	ClassDB::bind_method(
			D_METHOD( "set_instancer_cell_streaming_margin", "margin" ), &T3DTerrain::setInstancerCellStreamingMargin );
	ClassDB::bind_method(
			D_METHOD( "get_instancer_cell_streaming_margin" ), &T3DTerrain::getInstancerCellStreamingMargin );
	ClassDB::bind_method( D_METHOD( "set_instancer_cell_streaming_update_distance", "distance" ),
			&T3DTerrain::setInstancerCellStreamingUpdateDistance );
	ClassDB::bind_method( D_METHOD( "get_instancer_cell_streaming_update_distance" ),
			&T3DTerrain::getInstancerCellStreamingUpdateDistance );
	ClassDB::bind_method(
			D_METHOD( "set_instancer_update_cell_budget", "budget" ), &T3DTerrain::setInstancerUpdateCellBudget );
	ClassDB::bind_method( D_METHOD( "get_instancer_update_cell_budget" ), &T3DTerrain::getInstancerUpdateCellBudget );
	ClassDB::bind_method(
			D_METHOD( "set_dynamic_node_streaming_enabled", "enabled" ), &T3DTerrain::setDynamicNodeStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "is_dynamic_node_streaming_enabled" ), &T3DTerrain::isDynamicNodeStreamingEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_lightmap_streaming_enabled", "enabled" ), &T3DTerrain::setLightmapStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "is_lightmap_streaming_enabled" ), &T3DTerrain::isLightmapStreamingEnabled );
	ClassDB::bind_method( D_METHOD( "set_lightmap_stream_radius", "radius" ), &T3DTerrain::setLightmapStreamRadius );
	ClassDB::bind_method( D_METHOD( "get_lightmap_stream_radius" ), &T3DTerrain::getLightmapStreamRadius );
	ClassDB::bind_method( D_METHOD( "set_lightmap_load_budget", "budget" ), &T3DTerrain::setLightmapLoadBudget );
	ClassDB::bind_method( D_METHOD( "get_lightmap_load_budget" ), &T3DTerrain::getLightmapLoadBudget );
	ClassDB::bind_method( D_METHOD( "set_lightmap_unload_budget", "budget" ), &T3DTerrain::setLightmapUnloadBudget );
	ClassDB::bind_method( D_METHOD( "get_lightmap_unload_budget" ), &T3DTerrain::getLightmapUnloadBudget );
	ClassDB::bind_method( D_METHOD( "get_lightmap_streaming_stats" ), &T3DTerrain::getLightmapStreamingStats );
	ClassDB::bind_method( D_METHOD( "set_region_lightmap_output_directory", "directory" ),
			&T3DTerrain::setRegionLightmapOutputDirectory );
	ClassDB::bind_method(
			D_METHOD( "get_region_lightmap_output_directory" ), &T3DTerrain::getRegionLightmapOutputDirectory );
	ClassDB::bind_method( D_METHOD( "set_region_lightmap_proxy_lod", "lod" ), &T3DTerrain::setRegionLightmapProxyLod );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_proxy_lod" ), &T3DTerrain::getRegionLightmapProxyLod );
	ClassDB::bind_method(
			D_METHOD( "set_region_lightmap_context_radius", "radius" ), &T3DTerrain::setRegionLightmapContextRadius );
	ClassDB::bind_method(
			D_METHOD( "get_region_lightmap_context_radius" ), &T3DTerrain::getRegionLightmapContextRadius );
	ClassDB::bind_method(
			D_METHOD( "set_region_lightmap_texel_size", "texel_size" ), &T3DTerrain::setRegionLightmapTexelSize );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_texel_size" ), &T3DTerrain::getRegionLightmapTexelSize );
	ClassDB::bind_method(
			D_METHOD( "set_region_lightmap_bake_quality", "quality" ), &T3DTerrain::setRegionLightmapBakeQuality );
	ClassDB::bind_method( D_METHOD( "get_region_lightmap_bake_quality" ), &T3DTerrain::getRegionLightmapBakeQuality );
	ClassDB::bind_method(
			D_METHOD( "set_region_lightmap_shadowmask_mode", "mode" ), &T3DTerrain::setRegionLightmapShadowmaskMode );
	ClassDB::bind_method(
			D_METHOD( "get_region_lightmap_shadowmask_mode" ), &T3DTerrain::getRegionLightmapShadowmaskMode );
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
	ClassDB::bind_method(
			D_METHOD( "set_dynamic_node_stream_radius", "radius" ), &T3DTerrain::setDynamicNodeStreamRadius );
	ClassDB::bind_method( D_METHOD( "get_dynamic_node_stream_radius" ), &T3DTerrain::getDynamicNodeStreamRadius );
	ClassDB::bind_method(
			D_METHOD( "set_dynamic_node_load_region_budget", "budget" ), &T3DTerrain::setDynamicNodeLoadRegionBudget );
	ClassDB::bind_method(
			D_METHOD( "get_dynamic_node_load_region_budget" ), &T3DTerrain::getDynamicNodeLoadRegionBudget );
	ClassDB::bind_method(
			D_METHOD( "set_dynamic_node_load_node_budget", "budget" ), &T3DTerrain::setDynamicNodeLoadNodeBudget );
	ClassDB::bind_method( D_METHOD( "get_dynamic_node_load_node_budget" ), &T3DTerrain::getDynamicNodeLoadNodeBudget );
	ClassDB::bind_method( D_METHOD( "set_dynamic_node_unload_region_budget", "budget" ),
			&T3DTerrain::setDynamicNodeUnloadRegionBudget );
	ClassDB::bind_method(
			D_METHOD( "get_dynamic_node_unload_region_budget" ), &T3DTerrain::getDynamicNodeUnloadRegionBudget );
	ClassDB::bind_method(
			D_METHOD( "add_streamed_scene", "scene", "global_transform", "state", "flags", "hlod_min_level" ),
			&T3DTerrain::addStreamedScene,
			DEFVAL( Dictionary() ),
			DEFVAL( 0 ),
			DEFVAL( 1 ) );
	ClassDB::bind_method(
			D_METHOD( "add_streamed_scene_path", "scene_path", "global_transform", "state", "flags", "hlod_min_level" ),
			&T3DTerrain::addStreamedScenePath,
			DEFVAL( Dictionary() ),
			DEFVAL( 0 ),
			DEFVAL( 1 ) );
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
	ClassDB::bind_method(
			D_METHOD( "set_semantic_shadow_policy_enabled", "enabled" ), &T3DTerrain::setSemanticShadowPolicyEnabled );
	ClassDB::bind_method( D_METHOD( "is_semantic_shadow_policy_enabled" ), &T3DTerrain::isSemanticShadowPolicyEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_environment_shadow_quality", "quality" ), &T3DTerrain::setEnvironmentShadowQuality );
	ClassDB::bind_method( D_METHOD( "get_environment_shadow_quality" ), &T3DTerrain::getEnvironmentShadowQuality );
	ClassDB::bind_method(
			D_METHOD( "set_environment_shadow_external_mask", "mask" ), &T3DTerrain::setEnvironmentShadowExternalMask );
	ClassDB::bind_method(
			D_METHOD( "get_environment_shadow_external_mask" ), &T3DTerrain::getEnvironmentShadowExternalMask );
	ClassDB::bind_method( D_METHOD( "set_semantic_directional_reception_enabled", "enabled" ),
			&T3DTerrain::setSemanticDirectionalReceptionEnabled );
	ClassDB::bind_method( D_METHOD( "is_semantic_directional_reception_enabled" ),
			&T3DTerrain::isSemanticDirectionalReceptionEnabled );
	ClassDB::bind_method( D_METHOD( "set_environment_directional_external_mask", "mask" ),
			&T3DTerrain::setEnvironmentDirectionalExternalMask );
	ClassDB::bind_method( D_METHOD( "get_environment_directional_external_mask" ),
			&T3DTerrain::getEnvironmentDirectionalExternalMask );
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
	ClassDB::bind_method(
			D_METHOD( "set_runtime_shader_tiering_enabled", "enabled" ), &T3DTerrain::setRuntimeShaderTieringEnabled );
	ClassDB::bind_method( D_METHOD( "is_runtime_shader_tiering_enabled" ), &T3DTerrain::isRuntimeShaderTieringEnabled );
	ClassDB::bind_method( D_METHOD( "set_runtime_near_shader_lods", "count" ), &T3DTerrain::setRuntimeNearShaderLods );
	ClassDB::bind_method( D_METHOD( "get_runtime_near_shader_lods" ), &T3DTerrain::getRuntimeNearShaderLods );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_middle_shader_lods", "count" ), &T3DTerrain::setRuntimeMiddleShaderLods );
	ClassDB::bind_method( D_METHOD( "get_runtime_middle_shader_lods" ), &T3DTerrain::getRuntimeMiddleShaderLods );
	ClassDB::bind_method( D_METHOD( "set_runtime_near_range_end_m", "range_m" ), &T3DTerrain::setRuntimeNearRangeEndM );
	ClassDB::bind_method( D_METHOD( "get_runtime_near_range_end_m" ), &T3DTerrain::getRuntimeNearRangeEndM );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_middle_range_end_m", "range_m" ), &T3DTerrain::setRuntimeMiddleRangeEndM );
	ClassDB::bind_method( D_METHOD( "get_runtime_middle_range_end_m" ), &T3DTerrain::getRuntimeMiddleRangeEndM );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_sync_detail_lods", "enabled" ), &T3DTerrain::setRuntimeSyncDetailLods );
	ClassDB::bind_method( D_METHOD( "is_runtime_sync_detail_lods" ), &T3DTerrain::isRuntimeSyncDetailLods );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_clipmap_update_budget", "count" ), &T3DTerrain::setRuntimeClipmapUpdateBudget );
	ClassDB::bind_method( D_METHOD( "get_runtime_clipmap_update_budget" ), &T3DTerrain::getRuntimeClipmapUpdateBudget );
	ClassDB::bind_method(
			D_METHOD( "set_runtime_page_prepare_threads", "count" ), &T3DTerrain::setRuntimePagePrepareThreads );
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
	ClassDB::bind_method(
			D_METHOD( "set_middle_shadow_lod_count", "lod_count" ), &T3DTerrain::setMiddleShadowLodCount );
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
	ClassDB::bind_method(
			D_METHOD( "set_displacement_sharpness", "sharpness" ), &T3DTerrain::setDisplacementSharpness );
	ClassDB::bind_method( D_METHOD( "get_displacement_sharpness" ), &T3DTerrain::getDisplacementSharpness );
	ClassDB::bind_method(
			D_METHOD( "set_buffer_shader_override_enabled", "enabled" ), &T3DTerrain::setBufferShaderOverrideEnabled );
	ClassDB::bind_method( D_METHOD( "is_buffer_shader_override_enabled" ), &T3DTerrain::isBufferShaderOverrideEnabled );
	ClassDB::bind_method( D_METHOD( "set_buffer_shader_override", "shader" ), &T3DTerrain::setBufferShaderOverride );
	ClassDB::bind_method( D_METHOD( "get_buffer_shader_override" ), &T3DTerrain::getBufferShaderOverride );

	// Ocean Mesh
	ClassDB::bind_method( D_METHOD( "set_ocean_enabled", "enabled" ), &T3DTerrain::setOceanEnabled );
	ClassDB::bind_method( D_METHOD( "is_ocean_enabled" ), &T3DTerrain::isOceanEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_ocean_performance_preset", "preset" ), &T3DTerrain::setOceanPerformancePreset );
	ClassDB::bind_method( D_METHOD( "get_ocean_performance_preset" ), &T3DTerrain::getOceanPerformancePreset );
	ClassDB::bind_method(
			D_METHOD( "set_ocean_vertex_wave_iterations", "iterations" ), &T3DTerrain::setOceanVertexWaveIterations );
	ClassDB::bind_method( D_METHOD( "get_ocean_vertex_wave_iterations" ), &T3DTerrain::getOceanVertexWaveIterations );
	ClassDB::bind_method( D_METHOD( "set_ocean_fragment_wave_iterations", "iterations" ),
			&T3DTerrain::setOceanFragmentWaveIterations );
	ClassDB::bind_method(
			D_METHOD( "get_ocean_fragment_wave_iterations" ), &T3DTerrain::getOceanFragmentWaveIterations );
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
	ClassDB::bind_method(
			D_METHOD( "set_ocean_cast_shadows", "shadow_casting_setting" ), &T3DTerrain::setOceanCastShadows );
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
	ClassDB::bind_method(
			D_METHOD( "set_compute_displacement_enabled", "enabled" ), &T3DTerrain::setComputeDisplacementEnabled );
	ClassDB::bind_method( D_METHOD( "get_compute_displacement_enabled" ), &T3DTerrain::getComputeDisplacementEnabled );
	ClassDB::bind_method(
			D_METHOD( "set_displacement_update_distance", "distance" ), &T3DTerrain::setDisplacementUpdateDistance );
	ClassDB::bind_method( D_METHOD( "get_displacement_update_distance" ), &T3DTerrain::getDisplacementUpdateDistance );
	ClassDB::bind_method( D_METHOD( "is_compute_displacement_active" ), &T3DTerrain::isComputeDisplacementActive );
	ClassDB::bind_method(
			D_METHOD( "set_show_displacement_buffer", "enabled" ), &T3DTerrain::setShowDisplacementBuffer );
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
	ClassDB::bind_method( D_METHOD( "get_intersection", "src_pos", "direction", "gpu_mode" ),
			&T3DTerrain::getIntersection,
			DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_raycast_result", "src_pos", "direction", "collision_mask", "exclude_terrain" ),
			&T3DTerrain::getRaycastResult,
			DEFVAL( 0xFFFFFFFF ),
			DEFVAL( false ) );
	ClassDB::bind_method(
			D_METHOD( "bake_mesh", "lod", "filter" ), &T3DTerrain::bakeMesh, DEFVAL( HEIGHT_FILTER_NEAREST ) );
	ClassDB::bind_method(
			D_METHOD( "bake_region_lightmap_mesh", "region_location", "lod", "context_radius_regions", "texel_size" ),
			&T3DTerrain::bakeRegionLightmapMesh,
			DEFVAL( 2 ),
			DEFVAL( 1 ),
			DEFVAL( 1.0f ) );
	ClassDB::bind_method( D_METHOD( "bake_occluder", "lod" ), &T3DTerrain::bakeOccluder );
	ClassDB::bind_method( D_METHOD( "generate_nav_mesh_source_geometry", "global_aabb", "require_nav" ),
			&T3DTerrain::generateNavMeshSourceGeometry,
			DEFVAL( true ) );

	ADD_GROUP( "General / Data & Authoring", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING,
						  "version",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"",
			"get_version" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "debug_level", PROPERTY_HINT_ENUM, "Errors,Info,Debug,Extreme" ),
			"set_debug_level",
			"get_debug_level" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "data_directory", PROPERTY_HINT_DIR ),
			"set_data_directory",
			"get_data_directory" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "editor_execution_mode", PROPERTY_HINT_ENUM, "Authoring,Runtime Profile" ),
			"set_editor_execution_mode",
			"get_editor_execution_mode" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::BOOL, "editor_runtime_streaming_preview", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
			"set_editor_runtime_streaming_preview",
			"is_editor_runtime_streaming_preview" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "region_size",
						  PROPERTY_HINT_ENUM,
						  "128:128",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"change_region_size",
			"get_region_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "save_16_bit", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
			"set_save_16_bit",
			"get_save_16_bit" );
	// Storage backing is deliberately ordered before Resources so scene loading restores
	// the selected base profile before a serialized material resource and its overrides.
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "_performance_target_storage", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
			"set_performance_target",
			"get_performance_target" );

	ADD_SUBGROUP( "Regional Lightmap Baking", "region_lightmap_" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "region_lightmap_output_directory", PROPERTY_HINT_DIR ),
			"set_region_lightmap_output_directory",
			"get_region_lightmap_output_directory" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "region_lightmap_proxy_lod", PROPERTY_HINT_RANGE, "0,8,1" ),
			"set_region_lightmap_proxy_lod",
			"get_region_lightmap_proxy_lod" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::INT, "region_lightmap_context_radius", PROPERTY_HINT_RANGE, "0,4,1,suffix: regions" ),
			"set_region_lightmap_context_radius",
			"get_region_lightmap_context_radius" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::FLOAT, "region_lightmap_texel_size", PROPERTY_HINT_RANGE, "0.05,16,0.05,suffix: m/texel" ),
			"set_region_lightmap_texel_size",
			"get_region_lightmap_texel_size" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "region_lightmap_bake_quality", PROPERTY_HINT_ENUM, "Low,Medium,High,Ultra" ),
			"set_region_lightmap_bake_quality",
			"get_region_lightmap_bake_quality" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "region_lightmap_shadowmask_mode", PROPERTY_HINT_ENUM, "None,Replace,Overlay" ),
			"set_region_lightmap_shadowmask_mode",
			"get_region_lightmap_shadowmask_mode" );

	ADD_GROUP( "Resources", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "T3DMaterial" ),
			"set_material",
			"get_material" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "assets", PROPERTY_HINT_RESOURCE_TYPE, "T3DAssets" ),
			"set_assets",
			"get_assets" );
	ADD_PROPERTY(
			PropertyInfo( Variant::OBJECT, "auto_shader_profile", PROPERTY_HINT_RESOURCE_TYPE, "T3DAutoShaderProfile" ),
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
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "T3DData" ),
			"",
			"get_data" );
	ADD_PROPERTY(
			PropertyInfo( Variant::OBJECT, "collision", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "T3DCollision" ),
			"",
			"get_collision" );
	ADD_PROPERTY(
			PropertyInfo( Variant::OBJECT, "instancer", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "T3DInstancer" ),
			"",
			"get_instancer" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::OBJECT, "node_streamer", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE, "T3DNodeStreamer" ),
			"",
			"get_node_streamer" );

	ADD_GROUP( "Performance Profile", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "performance_target",
						  PROPERTY_HINT_ENUM,
						  "Custom,Xbox 360 Experimental",
						  PROPERTY_USAGE_EDITOR ),
			"set_performance_target",
			"get_performance_target" );
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

	ADD_GROUP( "Terrain Rendering", "" );

	ADD_SUBGROUP( "Geometry & Clipmap", "" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
						  "clipmap_target",
						  PROPERTY_HINT_NODE_TYPE,
						  "Node3D",
						  PROPERTY_USAGE_DEFAULT,
						  "Node3D" ),
			"set_clipmap_target",
			"get_clipmap_target" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mesh_lods", PROPERTY_HINT_RANGE, "1,10,1" ),
			"set_mesh_lods",
			"get_mesh_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mesh_size", PROPERTY_HINT_RANGE, "8,256,2" ),
			"set_mesh_size",
			"get_mesh_size" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT,
						  "vertex_spacing",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"set_vertex_spacing",
			"get_vertex_spacing" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cull_margin", PROPERTY_HINT_RANGE, "0.0,10000.0,.5,or_greater" ),
			"set_cull_margin",
			"get_cull_margin" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER ),
			"set_render_layers",
			"get_render_layers" );

	ADD_SUBGROUP( "Shader Tier Ranges", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_shader_tiering_enabled" ),
			"set_runtime_shader_tiering_enabled",
			"is_runtime_shader_tiering_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_near_shader_lods", PROPERTY_HINT_RANGE, "0,16,1" ),
			"set_runtime_near_shader_lods",
			"get_runtime_near_shader_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_middle_shader_lods", PROPERTY_HINT_RANGE, "0,16,1" ),
			"set_runtime_middle_shader_lods",
			"get_runtime_middle_shader_lods" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::FLOAT, "runtime_near_range_end_m", PROPERTY_HINT_RANGE, "0,4096,1,or_greater,suffix:m" ),
			"set_runtime_near_range_end_m",
			"get_runtime_near_range_end_m" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::FLOAT, "runtime_middle_range_end_m", PROPERTY_HINT_RANGE, "0,8192,1,or_greater,suffix:m" ),
			"set_runtime_middle_range_end_m",
			"get_runtime_middle_range_end_m" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_sync_detail_lods" ),
			"set_runtime_sync_detail_lods",
			"is_runtime_sync_detail_lods" );

	ADD_SUBGROUP( "Shadows & GI", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only" ),
			"set_cast_shadows",
			"get_cast_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shadow_lod_count", PROPERTY_HINT_RANGE, "0,16,1" ),
			"set_shadow_lod_count",
			"get_shadow_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "near_shadow_enabled" ),
			"set_near_shadow_enabled",
			"get_near_shadow_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "middle_shadow_enabled" ),
			"set_middle_shadow_enabled",
			"get_middle_shadow_enabled" );
	ADD_PROPERTY(
			PropertyInfo( Variant::BOOL, "far_shadow_enabled" ), "set_far_shadow_enabled", "get_far_shadow_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
			"set_near_shadow_lod_count",
			"get_near_shadow_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
			"set_middle_shadow_lod_count",
			"get_middle_shadow_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_shadow_lod_count", PROPERTY_HINT_RANGE, "-1,16,1" ),
			"set_far_shadow_lod_count",
			"get_far_shadow_lod_count" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic" ),
			"set_gi_mode",
			"get_gi_mode" );

	ADD_SUBGROUP( "Displacement / Tessellation", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "tessellation_level", PROPERTY_HINT_RANGE, "0,6,1" ),
			"set_tessellation_level",
			"get_tessellation_level" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "compute_displacement_enabled" ),
			"set_compute_displacement_enabled",
			"get_compute_displacement_enabled" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::FLOAT, "displacement_update_distance", PROPERTY_HINT_RANGE, "0.0,16.0,0.01,suffix:m" ),
			"set_displacement_update_distance",
			"get_displacement_update_distance" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "displacement_scale", PROPERTY_HINT_RANGE, "0.0, 2.0, 0.01" ),
			"set_displacement_scale",
			"get_displacement_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "displacement_sharpness", PROPERTY_HINT_RANGE, "0.0, 1.0, 0.01" ),
			"set_displacement_sharpness",
			"get_displacement_sharpness" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "buffer_shader_override_enabled" ),
			"set_buffer_shader_override_enabled",
			"is_buffer_shader_override_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "buffer_shader_override", PROPERTY_HINT_RESOURCE_TYPE, "Shader" ),
			"set_buffer_shader_override",
			"get_buffer_shader_override" );

	ADD_GROUP( "Streaming & Residency", "" );

	ADD_SUBGROUP( "Region Tiers", "" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "streaming_active_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
			"set_streaming_active_radius",
			"get_streaming_active_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "streaming_hot_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
			"set_streaming_hot_radius",
			"get_streaming_hot_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "streaming_cold_radius", PROPERTY_HINT_RANGE, "0,31,1,suffix: regions" ),
			"set_streaming_cold_radius",
			"get_streaming_cold_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "streaming_unload_guard", PROPERTY_HINT_RANGE, "0,4,1,suffix: regions" ),
			"set_streaming_unload_guard",
			"get_streaming_unload_guard" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "streaming_prediction_enabled" ),
			"set_streaming_prediction_enabled",
			"is_streaming_prediction_enabled" );
	ADD_PROPERTY(
			PropertyInfo( Variant::FLOAT, "streaming_prediction_lookahead", PROPERTY_HINT_RANGE, "0,4,0.05,suffix:s" ),
			"set_streaming_prediction_lookahead",
			"get_streaming_prediction_lookahead" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "streaming_prediction_maximum_regions",
						  PROPERTY_HINT_RANGE,
						  "0,16,1,suffix: regions" ),
			"set_streaming_prediction_maximum_regions",
			"get_streaming_prediction_maximum_regions" );

	ADD_SUBGROUP( "GPU Terrain Pages", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_cache_enabled" ),
			"set_gpu_region_cache_enabled",
			"is_gpu_region_cache_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_cache_budget_auto" ),
			"set_gpu_region_cache_budget_auto",
			"is_gpu_region_cache_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,128,1" ),
			"set_gpu_region_cache_budget_mb",
			"get_gpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "gpu_region_cache_effective_mb",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"",
			"get_effective_gpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_upload_budget_layers", PROPERTY_HINT_RANGE, "1,64,1" ),
			"set_gpu_region_upload_budget_layers",
			"get_gpu_region_upload_budget_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_upload_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ),
			"set_gpu_region_upload_budget_mb",
			"get_gpu_region_upload_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_spare_pages", PROPERTY_HINT_RANGE, "2,63,1" ),
			"set_gpu_region_spare_pages",
			"get_gpu_region_spare_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_retire_frames", PROPERTY_HINT_RANGE, "1,16,1" ),
			"set_gpu_region_retire_frames",
			"get_gpu_region_retire_frames" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::INT, "gpu_region_detail_lods", PROPERTY_HINT_RANGE, "0,16,1,suffix: LODs (0 = all)" ),
			"set_gpu_region_detail_lods",
			"get_gpu_region_detail_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT,
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
	ADD_PROPERTY(
			PropertyInfo(
					Variant::INT, "gpu_region_guard_regions", PROPERTY_HINT_RANGE, "0,4,1", PROPERTY_USAGE_STORAGE ),
			"set_gpu_region_guard_regions",
			"get_gpu_region_guard_regions" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::INT, "gpu_region_prefetch_rows", PROPERTY_HINT_RANGE, "0,4,1", PROPERTY_USAGE_STORAGE ),
			"set_gpu_region_prefetch_rows",
			"get_gpu_region_prefetch_rows" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "gpu_region_hysteresis_ticks", PROPERTY_HINT_RANGE, "0,240,1" ),
			"set_gpu_region_hysteresis_ticks",
			"get_gpu_region_hysteresis_ticks" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "gpu_region_color_resolution_divisor",
						  PROPERTY_HINT_ENUM,
						  "Full:1,Half:2,Quarter:4" ),
			"set_gpu_region_color_resolution_divisor",
			"get_gpu_region_color_resolution_divisor" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "gpu_region_derived_resolution_divisor",
						  PROPERTY_HINT_ENUM,
						  "Full:1,Half:2,Quarter:4" ),
			"set_gpu_region_derived_resolution_divisor",
			"get_gpu_region_derived_resolution_divisor" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "gpu_middle_height_resolution_divisor",
						  PROPERTY_HINT_ENUM,
						  "Full:1,Half:2,Quarter:4" ),
			"set_gpu_middle_height_resolution_divisor",
			"get_gpu_middle_height_resolution_divisor" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::INT, "editor_gpu_region_cache_pages", PROPERTY_HINT_RANGE, "16,256,1,suffix: pages" ),
			"set_editor_gpu_region_cache_pages",
			"get_editor_gpu_region_cache_pages" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::INT, "gpu_far_quality_divisor", PROPERTY_HINT_ENUM, "Full:1,Half:2,Quarter:4,Eighth:8" ),
			"set_gpu_far_quality_divisor",
			"get_gpu_far_quality_divisor" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_material_resolution", PROPERTY_HINT_ENUM, "64:64,128:128,256:256" ),
			"set_far_material_resolution",
			"get_far_material_resolution" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_control_resolution", PROPERTY_HINT_ENUM, "64:64,128:128,256:256" ),
			"set_far_control_resolution",
			"get_far_control_resolution" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "gpu_far_atlas_side",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"",
			"get_gpu_far_atlas_side" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "gpu_far_height_atlas_side",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"",
			"get_gpu_far_height_atlas_side" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "gpu_far_surface_atlas_side",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"",
			"get_gpu_far_surface_atlas_side" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_cooked_pages_enabled" ),
			"set_gpu_region_cooked_pages_enabled",
			"is_gpu_region_cooked_pages_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "gpu_region_cook_on_save" ),
			"set_gpu_region_cook_on_save",
			"is_gpu_region_cook_on_save" );

	ADD_SUBGROUP( "Runtime Material Streaming", "runtime_material_" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_material_streaming_enabled" ),
			"set_runtime_material_streaming_enabled",
			"is_runtime_material_streaming_enabled" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "runtime_material_cache_layers", PROPERTY_HINT_RANGE, "4,32,1,suffix: layers" ),
			"set_runtime_material_cache_layers",
			"get_runtime_material_cache_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "runtime_material_upload_budget_layers",
						  PROPERTY_HINT_RANGE,
						  "1,32,1,suffix: layers/frame" ),
			"set_runtime_material_upload_budget_layers",
			"get_runtime_material_upload_budget_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_material_release_source_textures" ),
			"set_runtime_release_source_material_textures",
			"is_runtime_release_source_material_textures" );

	ADD_SUBGROUP( "CPU Terrain Pages", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "cpu_region_cache_enabled" ),
			"set_cpu_region_cache_enabled",
			"is_cpu_region_cache_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "cpu_region_cache_budget_auto" ),
			"set_cpu_region_cache_budget_auto",
			"is_cpu_region_cache_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cpu_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,128,1" ),
			"set_cpu_region_cache_budget_mb",
			"get_cpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "cpu_region_cache_effective_mb",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"",
			"get_effective_cpu_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cpu_region_load_budget_layers", PROPERTY_HINT_RANGE, "1,64,1" ),
			"set_cpu_region_load_budget_layers",
			"get_cpu_region_load_budget_layers" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "cpu_region_load_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ),
			"set_cpu_region_load_budget_mb",
			"get_cpu_region_load_budget_mb" );

	ADD_SUBGROUP( "Static Instance Pages", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "instance_region_cache_enabled" ),
			"set_instance_region_cache_enabled",
			"is_instance_region_cache_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "instance_region_cache_budget_auto" ),
			"set_instance_region_cache_budget_auto",
			"is_instance_region_cache_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_region_cache_budget_mb", PROPERTY_HINT_RANGE, "1,128,1" ),
			"set_instance_region_cache_budget_mb",
			"get_instance_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "instance_region_cache_effective_mb",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"",
			"get_effective_instance_region_cache_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_region_load_budget_pages", PROPERTY_HINT_RANGE, "1,64,1" ),
			"set_instance_region_load_budget_pages",
			"get_instance_region_load_budget_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instance_region_load_budget_mb", PROPERTY_HINT_RANGE, "1,64,1" ),
			"set_instance_region_load_budget_mb",
			"get_instance_region_load_budget_mb" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "instancer_cell_size", PROPERTY_HINT_ENUM, "8,16,32,64,128,suffix: samples" ),
			"set_instancer_cell_size",
			"get_instancer_cell_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "instancer_cell_streaming_enabled" ),
			"set_instancer_cell_streaming_enabled",
			"is_instancer_cell_streaming_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "instancer_single_lod_buffers" ),
			"set_instancer_single_lod_buffers",
			"is_instancer_single_lod_buffers" );
	ADD_PROPERTY(
			PropertyInfo( Variant::FLOAT, "instancer_cell_streaming_margin", PROPERTY_HINT_RANGE, "0,1024,1,suffix:m" ),
			"set_instancer_cell_streaming_margin",
			"get_instancer_cell_streaming_margin" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT,
						  "instancer_cell_streaming_update_distance",
						  PROPERTY_HINT_RANGE,
						  "1,256,1,suffix:m" ),
			"set_instancer_cell_streaming_update_distance",
			"get_instancer_cell_streaming_update_distance" );

	ADD_SUBGROUP( "Dynamic Nodes", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "dynamic_node_streaming_enabled" ),
			"set_dynamic_node_streaming_enabled",
			"is_dynamic_node_streaming_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "dynamic_node_stream_radius", PROPERTY_HINT_RANGE, "0,16,1" ),
			"set_dynamic_node_stream_radius",
			"get_dynamic_node_stream_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "dynamic_node_load_region_budget", PROPERTY_HINT_RANGE, "0,64,1" ),
			"set_dynamic_node_load_region_budget",
			"get_dynamic_node_load_region_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "dynamic_node_load_node_budget", PROPERTY_HINT_RANGE, "0,4096,1" ),
			"set_dynamic_node_load_node_budget",
			"get_dynamic_node_load_node_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "dynamic_node_unload_region_budget", PROPERTY_HINT_RANGE, "0,64,1" ),
			"set_dynamic_node_unload_region_budget",
			"get_dynamic_node_unload_region_budget" );

	ADD_SUBGROUP( "Baked Lighting", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "lightmap_streaming_enabled" ),
			"set_lightmap_streaming_enabled",
			"is_lightmap_streaming_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "lightmap_residency_budget_auto" ),
			"set_lightmap_residency_budget_auto",
			"is_lightmap_residency_budget_auto" );
	ADD_PROPERTY(
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
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lightmap_stream_radius", PROPERTY_HINT_RANGE, "0,16,1,suffix: regions" ),
			"set_lightmap_stream_radius",
			"get_lightmap_stream_radius" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "lightmap_load_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ),
			"set_lightmap_load_budget",
			"get_lightmap_load_budget" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::INT, "lightmap_unload_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ),
			"set_lightmap_unload_budget",
			"get_lightmap_unload_budget" );

	ADD_PROPERTY( PropertyInfo( Variant::INT, "stream_owner_counter", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
			"set_next_stream_owner_id",
			"get_next_stream_owner_id" );

	ADD_SUBGROUP( "HLOD", "hlod_" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "hlod_enabled" ), "set_hlod_enabled", "is_hlod_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "hlod_manifest_path", PROPERTY_HINT_FILE, "*.tres,*.res" ),
			"set_hlod_manifest_path",
			"get_hlod_manifest_path" );
	ADD_PROPERTY(
			PropertyInfo( Variant::FLOAT, "hlod_target_error_pixels", PROPERTY_HINT_RANGE, "0.25,64,0.25,suffix:px" ),
			"set_hlod_target_error_pixels",
			"get_hlod_target_error_pixels" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "hlod_hysteresis", PROPERTY_HINT_RANGE, "0,0.75,0.01" ),
			"set_hlod_hysteresis",
			"get_hlod_hysteresis" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_max_level", PROPERTY_HINT_RANGE, "1,6,1" ),
			"set_hlod_max_level",
			"get_hlod_max_level" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "hlod_load_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ),
			"set_hlod_load_budget",
			"get_hlod_load_budget" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "hlod_unload_budget", PROPERTY_HINT_RANGE, "0,64,1,suffix: packages/update" ),
			"set_hlod_unload_budget",
			"get_hlod_unload_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "hlod_memory_budget_auto" ),
			"set_hlod_memory_budget_auto",
			"is_hlod_memory_budget_auto" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_memory_budget_mb", PROPERTY_HINT_RANGE, "1,4096,1,suffix: MiB" ),
			"set_hlod_memory_budget_mb",
			"get_hlod_memory_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "hlod_memory_effective_mb",
						  PROPERTY_HINT_NONE,
						  "",
						  PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
			"",
			"get_effective_hlod_memory_budget_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_max_shadow_level", PROPERTY_HINT_RANGE, "0,6,1" ),
			"set_hlod_max_shadow_level",
			"get_hlod_max_shadow_level" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hlod_forced_debug_level", PROPERTY_HINT_RANGE, "-1,6,1" ),
			"set_hlod_forced_debug_level",
			"get_hlod_forced_debug_level" );

	ADD_GROUP( "Runtime Scheduling - Advanced", "" );

	ADD_SUBGROUP( "Work Budgets", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_clipmap_update_budget", PROPERTY_HINT_RANGE, "1,16,1" ),
			"set_runtime_clipmap_update_budget",
			"get_runtime_clipmap_update_budget" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_page_prepare_threads", PROPERTY_HINT_RANGE, "1,32,1" ),
			"set_runtime_page_prepare_threads",
			"get_runtime_page_prepare_threads" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "instancer_update_cell_budget",
						  PROPERTY_HINT_RANGE,
						  "0,65536,1,or_greater,suffix: cells/frame" ),
			"set_instancer_update_cell_budget",
			"get_instancer_update_cell_budget" );

	ADD_SUBGROUP( "Adaptive Policy", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_adaptive_work_budgets_enabled" ),
			"set_runtime_adaptive_work_budgets_enabled",
			"is_runtime_adaptive_work_budgets_enabled" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::FLOAT, "runtime_adaptive_target_ms", PROPERTY_HINT_RANGE, "0.05,50.0,0.05,suffix:ms" ),
			"set_runtime_adaptive_target_ms",
			"get_runtime_adaptive_target_ms" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::FLOAT, "runtime_adaptive_hard_max_ms", PROPERTY_HINT_RANGE, "0.05,100.0,0.05,suffix:ms" ),
			"set_runtime_adaptive_hard_max_ms",
			"get_runtime_adaptive_hard_max_ms" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "runtime_adaptive_adjustment_window_frames", PROPERTY_HINT_RANGE, "1,120,1" ),
			"set_runtime_adaptive_adjustment_window_frames",
			"get_runtime_adaptive_adjustment_window_frames" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_gpu_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
			"set_runtime_adaptive_min_gpu_pages",
			"get_runtime_adaptive_min_gpu_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_cpu_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
			"set_runtime_adaptive_min_cpu_pages",
			"get_runtime_adaptive_min_cpu_pages" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_instance_pages", PROPERTY_HINT_RANGE, "0,64,1" ),
			"set_runtime_adaptive_min_instance_pages",
			"get_runtime_adaptive_min_instance_pages" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "runtime_adaptive_min_instancer_cells", PROPERTY_HINT_RANGE, "0,65536,1" ),
			"set_runtime_adaptive_min_instancer_cells",
			"get_runtime_adaptive_min_instancer_cells" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "runtime_adaptive_min_node_load_regions", PROPERTY_HINT_RANGE, "0,64,1" ),
			"set_runtime_adaptive_min_node_load_regions",
			"get_runtime_adaptive_min_node_load_regions" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "runtime_adaptive_min_node_unload_regions", PROPERTY_HINT_RANGE, "0,64,1" ),
			"set_runtime_adaptive_min_node_unload_regions",
			"get_runtime_adaptive_min_node_unload_regions" );

	ADD_SUBGROUP( "Boundary Phasing", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_boundary_work_phasing_enabled" ),
			"set_runtime_boundary_work_phasing_enabled",
			"is_runtime_boundary_work_phasing_enabled" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::INT, "runtime_boundary_node_delay_frames", PROPERTY_HINT_RANGE, "0,8,1,suffix:frames" ),
			"set_runtime_boundary_node_delay_frames",
			"get_runtime_boundary_node_delay_frames" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "runtime_boundary_instancer_delay_frames",
						  PROPERTY_HINT_RANGE,
						  "0,8,1,suffix:frames" ),
			"set_runtime_boundary_instancer_delay_frames",
			"get_runtime_boundary_instancer_delay_frames" );

	ADD_GROUP( "Memory", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "total_memory_budget_mb", PROPERTY_HINT_RANGE, "32,4096,1,suffix: MiB" ),
			"set_total_memory_budget_mb",
			"get_total_memory_budget_mb" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "memory_transient_reserve_mb", PROPERTY_HINT_RANGE, "4,1024,1,suffix: MiB" ),
			"set_memory_transient_reserve_mb",
			"get_memory_transient_reserve_mb" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "shader_cache_budget_auto" ),
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
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "collision_mode",
						  PROPERTY_HINT_ENUM,
						  "Disabled,Dynamic / Game,Dynamic / Editor,Full / Game,Full / Editor" ),
			"set_collision_mode",
			"get_collision_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_shape_size", PROPERTY_HINT_RANGE, "8,64,8" ),
			"set_collision_shape_size",
			"get_collision_shape_size" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_radius", PROPERTY_HINT_RANGE, "16,256,16" ),
			"set_collision_radius",
			"get_collision_radius" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT,
						  "collision_target",
						  PROPERTY_HINT_NODE_TYPE,
						  "Node3D",
						  PROPERTY_USAGE_DEFAULT,
						  "Node3D" ),
			"set_collision_target",
			"get_collision_target" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS ),
			"set_collision_layer",
			"get_collision_layer" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS ),
			"set_collision_mask",
			"get_collision_mask" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "collision_priority", PROPERTY_HINT_RANGE, "0.1,256,.1" ),
			"set_collision_priority",
			"get_collision_priority" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial" ),
			"set_physics_material",
			"get_physics_material" );

	ADD_GROUP( "Ocean", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "ocean_enabled" ), "set_ocean_enabled", "is_ocean_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "ocean_performance_preset",
						  PROPERTY_HINT_ENUM,
						  "Custom,Border Minimum,Border Balanced,High,Cinematic" ),
			"set_ocean_performance_preset",
			"get_ocean_performance_preset" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_vertex_wave_iterations", PROPERTY_HINT_RANGE, "0,3,1" ),
			"set_ocean_vertex_wave_iterations",
			"get_ocean_vertex_wave_iterations" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_fragment_wave_iterations", PROPERTY_HINT_RANGE, "0,4,1" ),
			"set_ocean_fragment_wave_iterations",
			"get_ocean_fragment_wave_iterations" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "ocean_refraction_mode",
						  PROPERTY_HINT_ENUM,
						  "Off,Depth Tint,Full Screen Refraction" ),
			"set_ocean_refraction_mode",
			"get_ocean_refraction_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_foam_mode", PROPERTY_HINT_ENUM, "Off,Shore Only,Shore + Crest" ),
			"set_ocean_foam_mode",
			"get_ocean_foam_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_scattering_mode", PROPERTY_HINT_ENUM, "Off,Approximate,Full" ),
			"set_ocean_scattering_mode",
			"get_ocean_scattering_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
						  "ocean_shading_mode",
						  PROPERTY_HINT_ENUM,
						  "Lambert Diffuse,Lambert + GGX,Burley + GGX,Unshaded,Lambert Vertex Lit" ),
			"set_ocean_shading_mode",
			"get_ocean_shading_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_mesh_lods", PROPERTY_HINT_RANGE, "1,10,1" ),
			"set_ocean_mesh_lods",
			"get_ocean_mesh_lods" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_tessellation_level", PROPERTY_HINT_RANGE, "0,6,1" ),
			"set_ocean_tessellation_level",
			"get_ocean_tessellation_level" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_mesh_size", PROPERTY_HINT_RANGE, "8,256,2" ),
			"set_ocean_mesh_size",
			"get_ocean_mesh_size" );
	ADD_PROPERTY(
			PropertyInfo( Variant::FLOAT, "ocean_vertex_spacing", PROPERTY_HINT_RANGE, "0.25,10.0,0.05,or_greater" ),
			"set_ocean_vertex_spacing",
			"get_ocean_vertex_spacing" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "ocean_cull_margin", PROPERTY_HINT_RANGE, "0.0,10000.0,.5,or_greater" ),
			"set_ocean_cull_margin",
			"get_ocean_cull_margin" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "ocean_cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only" ),
			"set_ocean_cast_shadows",
			"get_ocean_cast_shadows" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic" ),
			"set_ocean_gi_mode",
			"get_ocean_gi_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "ocean_render_layers", PROPERTY_HINT_LAYERS_3D_RENDER ),
			"set_ocean_render_layers",
			"get_ocean_render_layers" );
	ADD_PROPERTY(
			PropertyInfo(
					Variant::OBJECT, "ocean_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial,BaseMaterial3D" ),
			"set_ocean_material",
			"get_ocean_material" );

	ADD_GROUP( "Semantic Lighting", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "semantic_shadow_policy_enabled" ),
			"set_semantic_shadow_policy_enabled",
			"is_semantic_shadow_policy_enabled" );
	ADD_PROPERTY(
			PropertyInfo( Variant::INT, "environment_shadow_quality", PROPERTY_HINT_ENUM, "Low,Normal,High,Ultra" ),
			"set_environment_shadow_quality",
			"get_environment_shadow_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "environment_shadow_external_mask", PROPERTY_HINT_LAYERS_3D_RENDER ),
			"set_environment_shadow_external_mask",
			"get_environment_shadow_external_mask" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "semantic_directional_reception_enabled" ),
			"set_semantic_directional_reception_enabled",
			"is_semantic_directional_reception_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "environment_directional_external_mask", PROPERTY_HINT_LAYERS_3D_RENDER ),
			"set_environment_directional_external_mask",
			"get_environment_directional_external_mask" );

	ADD_GROUP( "Diagnostics & Profiling", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "runtime_profiling_enabled" ),
			"set_runtime_profiling_enabled",
			"is_runtime_profiling_enabled" );

	ADD_GROUP( "Editor Visualization", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "label_distance", PROPERTY_HINT_RANGE, "0.0,10000.0,0.5,or_greater" ),
			"set_label_distance",
			"get_label_distance" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "label_size", PROPERTY_HINT_RANGE, "24,128,1" ),
			"set_label_size",
			"get_label_size" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
			"set_show_region_grid",
			"get_show_region_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mouse_layer", PROPERTY_HINT_RANGE, "21, 32" ),
			"set_mouse_layer",
			"get_mouse_layer" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "free_editor_textures" ),
			"set_free_editor_textures",
			"get_free_editor_textures" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "instancer_mode", PROPERTY_HINT_ENUM, "Disabled,Normal" ),
			"set_instancer_mode",
			"get_instancer_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_region_grid" ), "set_show_region_grid", "get_show_region_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_instancer_grid" ),
			"set_show_instancer_grid",
			"get_show_instancer_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_vertex_grid" ), "set_show_vertex_grid", "get_show_vertex_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_contours" ), "set_show_contours", "get_show_contours" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_navigation" ), "set_show_navigation", "get_show_navigation" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_checkered" ), "set_show_checkered", "get_show_checkered" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_grey" ), "set_show_grey", "get_show_grey" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_heightmap" ), "set_show_heightmap", "get_show_heightmap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_jaggedness" ), "set_show_jaggedness", "get_show_jaggedness" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_autoshader" ), "set_show_autoshader", "get_show_autoshader" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_texture" ),
			"set_show_control_texture",
			"get_show_control_texture" );
	ADD_PROPERTY(
			PropertyInfo( Variant::BOOL, "show_control_blend" ), "set_show_control_blend", "get_show_control_blend" );
	ADD_PROPERTY(
			PropertyInfo( Variant::BOOL, "show_control_angle" ), "set_show_control_angle", "get_show_control_angle" );
	ADD_PROPERTY(
			PropertyInfo( Variant::BOOL, "show_control_scale" ), "set_show_control_scale", "get_show_control_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_colormap" ), "set_show_colormap", "get_show_colormap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_roughmap" ), "set_show_roughmap", "get_show_roughmap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_displacement_buffer" ),
			"set_show_displacement_buffer",
			"get_show_displacement_buffer" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_albedo" ),
			"set_show_texture_albedo",
			"get_show_texture_albedo" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_height" ),
			"set_show_texture_height",
			"get_show_texture_height" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_normal" ),
			"set_show_texture_normal",
			"get_show_texture_normal" );
	ADD_PROPERTY(
			PropertyInfo( Variant::BOOL, "show_texture_rough" ), "set_show_texture_rough", "get_show_texture_rough" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_ao" ), "set_show_texture_ao", "get_show_texture_ao" );

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
