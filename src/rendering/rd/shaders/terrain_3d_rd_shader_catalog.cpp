#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

static const char *const s_terrainPaths[3] = {
	"res://addons/terrain_3d/rd/terrain_clipmap_near.glsl",
	"res://addons/terrain_3d/rd/terrain_clipmap_middle.glsl",
	"res://addons/terrain_3d/rd/terrain_clipmap_far.glsl",
};

static const char *const s_otherGlobalPaths[] = {
	"res://addons/terrain_3d/rd/terrain_instances.glsl",
	"res://addons/terrain_3d/rd/terrain_instance_occluder_depth.glsl",
	"res://addons/terrain_3d/rd/terrain_hlod_cull.glsl",
	"res://addons/terrain_3d/rd/terrain_instance_cull.glsl",
	"res://addons/terrain_3d/rd/terrain_world_hiz_reduce.glsl",
	"res://addons/terrain_3d/rd/terrain_world_visibility.glsl",
	"res://addons/terrain_3d/rd/terrain_cluster_compact.glsl",
	"res://addons/terrain_3d/rd/terrain_vt_feedback_multi_writer.glsl",
	"res://addons/terrain_3d/rd/terrain_world_raster.glsl",
	"res://addons/terrain_3d/rd/terrain_ocean.glsl",
	"res://addons/terrain_3d/rd/terrain_clipmap_screen_space.glsl",
	"res://addons/terrain_3d/compute/terrain_displacement.glsl",
	"res://addons/terrain_3d/compute/terrain_page_storage.glsl",
	"res://addons/terrain_3d/compute/terrain_page_derived.glsl",
};

// The bake source is deliberately not in s_localPaths: that table is
// eagerly loaded at shipping renderer startup by Terrain3DRdShaderLibrary.
static const char *const s_visibilityBakePath =
    "res://addons/terrain_3d/rd/terrain_vt_visibility_probe.glsl";

static const char *const s_localPaths[] = {
	"res://addons/terrain_3d/compute/terrain_brush.glsl",
	"res://addons/terrain_3d/compute/terrain_derived.glsl",
};

const char *t3dRdVisibilityBakeSourcePath() {
    return s_visibilityBakePath;
}

uint32_t t3dRdShaderCatalogGlobalSourceCount() {
	return 3u + ( uint32_t ) ( sizeof( s_otherGlobalPaths ) / sizeof( s_otherGlobalPaths[0] ) );
}

const char *t3dRdShaderCatalogGlobalSourcePath(
	const uint32_t index ) {
	if ( index < 3u ) {
		return s_terrainPaths[index];
	}
	const uint32_t other = index - 3u;
	return other < sizeof( s_otherGlobalPaths ) / sizeof( s_otherGlobalPaths[0] ) ? s_otherGlobalPaths[other] : NULL;
}

uint32_t t3dRdShaderCatalogSourceCount() {
	return t3dRdShaderCatalogGlobalSourceCount() + ( uint32_t ) ( sizeof( s_localPaths ) / sizeof( s_localPaths[0] ) );
}

const char *t3dRdShaderCatalogSourcePath(
	const uint32_t index ) {
	const uint32_t global_count = t3dRdShaderCatalogGlobalSourceCount();
	if ( index < global_count ) {
		return t3dRdShaderCatalogGlobalSourcePath( index );
	}
	const uint32_t local = index - global_count;
	return local < sizeof( s_localPaths ) / sizeof( s_localPaths[0] ) ? s_localPaths[local] : NULL;
}

static const char *const s_forwardVersions[T3D_TERRAIN_SHADER_PATH_COUNT][T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
	{ "color_native",
	  "color_specular",
	  "color_motion",
	  "color_specular_motion",
	  "color_native_radiance_array",
	  "color_specular_radiance_array",
	  "color_motion_radiance_array",
	  "color_specular_motion_radiance_array",
	  "depth",
	  "depth_normal_roughness",
	  "depth_normal_roughness_voxel_gi" },
	{ "compat_color_native",
	  "compat_color_specular",
	  "compat_color_motion",
	  "compat_color_specular_motion",
	  "compat_color_native_radiance_array",
	  "compat_color_specular_radiance_array",
	  "compat_color_motion_radiance_array",
	  "compat_color_specular_motion_radiance_array",
	  "compat_depth",
	  "compat_depth_normal_roughness",
	  "compat_depth_normal_roughness_voxel_gi" },
	{ "editor_color_native",
	  "editor_color_specular",
	  "editor_color_motion",
	  "editor_color_specular_motion",
	  "editor_color_native_radiance_array",
	  "editor_color_specular_radiance_array",
	  "editor_color_motion_radiance_array",
	  "editor_color_specular_motion_radiance_array",
	  "editor_depth",
	  "editor_depth_normal_roughness",
	  "editor_depth_normal_roughness_voxel_gi" },
};

static const char *const s_oneSunVersions[T3D_TERRAIN_SHADER_PATH_COUNT][T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
	{ "color_native_one_sun",
	  "color_specular_one_sun",
	  "color_motion_one_sun",
	  "color_specular_motion_one_sun",
	  "color_native_one_sun_radiance_array",
	  "color_specular_one_sun_radiance_array",
	  "color_motion_one_sun_radiance_array",
	  "color_specular_motion_one_sun_radiance_array" },
	{ "compat_color_native_one_sun",
	  "compat_color_specular_one_sun",
	  "compat_color_motion_one_sun",
	  "compat_color_specular_motion_one_sun",
	  "compat_color_native_one_sun_radiance_array",
	  "compat_color_specular_one_sun_radiance_array",
	  "compat_color_motion_one_sun_radiance_array",
	  "compat_color_specular_motion_one_sun_radiance_array" },
	{ "editor_color_native_one_sun",
	  "editor_color_specular_one_sun",
	  "editor_color_motion_one_sun",
	  "editor_color_specular_motion_one_sun",
	  "editor_color_native_one_sun_radiance_array",
	  "editor_color_specular_one_sun_radiance_array",
	  "editor_color_motion_one_sun_radiance_array",
	  "editor_color_specular_motion_one_sun_radiance_array" },
};

static const char *const s_worldStripVersions[T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
	"terrain_strip_color_native",
	"terrain_strip_color_specular",
	"terrain_strip_color_motion",
	"terrain_strip_color_specular_motion",
	"terrain_strip_color_native_radiance_array",
	"terrain_strip_color_specular_radiance_array",
	"terrain_strip_color_motion_radiance_array",
	"terrain_strip_color_specular_motion_radiance_array",
	"terrain_strip_depth",
	"terrain_strip_depth_normal_roughness",
	"terrain_strip_depth_normal_roughness_voxel_gi",
};

static const char *const s_worldMeshVersions[T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
	"mesh_color_native",
	"mesh_color_specular",
	"mesh_color_motion",
	"mesh_color_specular_motion",
	"mesh_color_native_radiance_array",
	"mesh_color_specular_radiance_array",
	"mesh_color_motion_radiance_array",
	"mesh_color_specular_motion_radiance_array",
	"mesh_depth",
	"mesh_depth_normal_roughness",
	"mesh_depth_normal_roughness_voxel_gi",
};

static const char *const s_worldMeshFeedbackVersions[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
	"feedback_mesh_color_native",
	"feedback_mesh_color_specular",
	"feedback_mesh_color_motion",
	"feedback_mesh_color_specular_motion",
	"feedback_mesh_color_native_radiance_array",
	"feedback_mesh_color_specular_radiance_array",
	"feedback_mesh_color_motion_radiance_array",
	"feedback_mesh_color_specular_motion_radiance_array" };

static const char *const s_instanceFeedbackVersions[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
	"feedback_color_native",
	"feedback_color_specular",
	"feedback_color_motion",
	"feedback_color_specular_motion",
	"feedback_color_native_radiance_array",
	"feedback_color_specular_radiance_array",
	"feedback_color_motion_radiance_array",
	"feedback_color_specular_motion_radiance_array" };

// Trial names must match the additive RDShaderFile versions.
static const char *const s_materialTrialVersions[T3D_RD_MATERIAL_SHADER_TRIAL_COUNT] = {
	"silhouette_native", "silhouette_only_native",
	"control_nearest_native", "control_bilinear_native", "control_adaptive_native",
	"authored_exact_native", "authored_hardware_native", "planar_native",
	"two_key_native", "active_key_native", "fixed_taps_native",
	"transform_reuse_native", "authored_corner_native", "megavt_early_native"
};
static const char *const s_alphaSamplingTrialVersions[T3D_RD_ALPHA_SAMPLING_TRIAL_COUNT] = {
	"alpha_gather_depth", "alpha_edge_depth", "alpha_gather_dnr", "alpha_edge_dnr"
};

static const char *const s_vtSamplingTrialVersions[T3D_RD_VT_SAMPLING_TRIAL_COUNT] = {
	"prepared_mip_bias_native", "vt_ancestor_hints_native", "vt_ancestor_hints_prepared_bias_native"
};
const char *t3dRdVtSamplingTrialName( const uint32_t trial ) {
	return trial >= 1u && trial <= T3D_RD_VT_SAMPLING_TRIAL_COUNT ? s_vtSamplingTrialVersions[trial - 1u] : NULL;
}

const char *t3dRdMaterialTrialName( const uint32_t trial ) {
	return trial >= 1u && trial <= T3D_RD_MATERIAL_SHADER_TRIAL_COUNT ? s_materialTrialVersions[trial - 1u] : NULL;
}
const char *t3dRdAlphaSamplingTrialName( const uint32_t trial ) {
	return trial >= 1u && trial <= T3D_RD_ALPHA_SAMPLING_TRIAL_COUNT ? s_alphaSamplingTrialVersions[trial - 1u] : NULL;
}

t3dRdShaderCatalogEntry_t t3dRdShaderCatalogGet(
	const t3dRdShaderId_t id ) {
	if ( id >= T3D_RD_SHADER_TERRAIN_VT_FEEDBACK_HINT_BASE && id < T3D_RD_SHADER_COUNT ) {
		const uint32_t offset = ( uint32_t ) id - T3D_RD_SHADER_TERRAIN_VT_FEEDBACK_HINT_BASE;
		static const char *const versions[T3D_RD_VT_FEEDBACK_HINT_VARIANT_COUNT] = {
			"feedback_color_native_vt_hints", "feedback_color_native_vt_hints_prepared_bias"
		};
		return { id, s_terrainPaths[offset / T3D_RD_VT_FEEDBACK_HINT_VARIANT_COUNT],
			versions[offset % T3D_RD_VT_FEEDBACK_HINT_VARIANT_COUNT],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	}
	// Experimental versions are loaded only by an explicit cold library policy.
	if ( id >= T3D_RD_SHADER_MATERIAL_TRIAL_BASE && id < T3D_RD_SHADER_INSTANCE_ALPHA_TRIAL_BASE ) {
		const uint32_t offset = ( uint32_t ) id - T3D_RD_SHADER_MATERIAL_TRIAL_BASE;
		return { id, s_terrainPaths[offset / T3D_RD_MATERIAL_SHADER_TRIAL_COUNT],
			s_materialTrialVersions[offset % T3D_RD_MATERIAL_SHADER_TRIAL_COUNT],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	}
	if ( id >= T3D_RD_SHADER_INSTANCE_ALPHA_TRIAL_BASE && id < T3D_RD_SHADER_WORLD_ALPHA_TRIAL_BASE ) {
		return { id, "res://addons/terrain_3d/rd/terrain_instances.glsl",
			s_alphaSamplingTrialVersions[( uint32_t ) id - T3D_RD_SHADER_INSTANCE_ALPHA_TRIAL_BASE],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_INSTANCE, false };
	}
	if ( id >= T3D_RD_SHADER_WORLD_ALPHA_TRIAL_BASE && id < T3D_RD_SHADER_VT_SAMPLING_TRIAL_BASE ) {
		return { id, "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
			s_alphaSamplingTrialVersions[( uint32_t ) id - T3D_RD_SHADER_WORLD_ALPHA_TRIAL_BASE],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, false };
	}
	if ( id >= T3D_RD_SHADER_VT_SAMPLING_TRIAL_BASE && id < T3D_RD_SHADER_OCEAN_PREPARED_COLOR ) {
		const uint32_t offset = ( uint32_t ) id - T3D_RD_SHADER_VT_SAMPLING_TRIAL_BASE;
		return { id, s_terrainPaths[offset / T3D_RD_VT_SAMPLING_TRIAL_COUNT],
			s_vtSamplingTrialVersions[offset % T3D_RD_VT_SAMPLING_TRIAL_COUNT],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	}
	// compact forward lighting: cold-only compact-list consumers. Existing native variants remain unchanged.
	if ( id >= T3D_RD_SHADER_TERRAIN_COMPACT_BASE && id < T3D_RD_SHADER_INSTANCE_COMPACT_BASE ) {
		const uint32_t offset = ( uint32_t ) id - T3D_RD_SHADER_TERRAIN_COMPACT_BASE;
		const uint32_t path = offset / ( 3u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT );
		const uint32_t tier = offset / T3D_RD_FORWARD_COLOR_VARIANT_COUNT % 3u;
		const uint32_t variant = offset % T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
		static const char *const versions[T3D_TERRAIN_SHADER_PATH_COUNT][T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
			{ "color_native_compact", "color_specular_compact", "color_motion_compact", "color_specular_motion_compact",
			  "color_native_radiance_array_compact", "color_specular_radiance_array_compact",
			  "color_motion_radiance_array_compact", "color_specular_motion_radiance_array_compact" },
			{ "compat_color_native_compact", "compat_color_specular_compact", "compat_color_motion_compact", "compat_color_specular_motion_compact",
			  "compat_color_native_radiance_array_compact", "compat_color_specular_radiance_array_compact",
			  "compat_color_motion_radiance_array_compact", "compat_color_specular_motion_radiance_array_compact" },
			{ "editor_color_native_compact", "editor_color_specular_compact", "editor_color_motion_compact", "editor_color_specular_motion_compact",
			  "editor_color_native_radiance_array_compact", "editor_color_specular_radiance_array_compact",
			  "editor_color_motion_radiance_array_compact", "editor_color_specular_motion_radiance_array_compact" }
		};
		return { id, s_terrainPaths[tier], versions[path][variant], T3D_RD_SHADER_GLOBAL_DEVICE,
			T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	}
	if ( id >= T3D_RD_SHADER_INSTANCE_COMPACT_BASE && id < T3D_RD_SHADER_WORLD_STRIP_COMPACT_BASE ) {
		static const char *const versions[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
			"color_native_compact", "color_specular_compact", "color_motion_compact", "color_specular_motion_compact",
			"color_native_radiance_array_compact", "color_specular_radiance_array_compact",
			"color_motion_radiance_array_compact", "color_specular_motion_radiance_array_compact"
		};
		return { id, "res://addons/terrain_3d/rd/terrain_instances.glsl",
			versions[( uint32_t ) id - T3D_RD_SHADER_INSTANCE_COMPACT_BASE],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_INSTANCE, false };
	}
	if ( id >= T3D_RD_SHADER_WORLD_STRIP_COMPACT_BASE && id < T3D_RD_SHADER_SCREEN_SPACE_COVERAGE ) {
		const bool strip = id < T3D_RD_SHADER_WORLD_MESH_COMPACT_BASE;
		const uint32_t offset = ( uint32_t ) id - ( strip ? T3D_RD_SHADER_WORLD_STRIP_COMPACT_BASE : T3D_RD_SHADER_WORLD_MESH_COMPACT_BASE );
		static const char *const stripVersions[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
			"terrain_strip_color_native_compact", "terrain_strip_color_specular_compact",
			"terrain_strip_color_motion_compact", "terrain_strip_color_specular_motion_compact",
			"terrain_strip_color_native_radiance_array_compact", "terrain_strip_color_specular_radiance_array_compact",
			"terrain_strip_color_motion_radiance_array_compact", "terrain_strip_color_specular_motion_radiance_array_compact"
		};
		static const char *const meshVersions[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
			"mesh_color_native_compact", "mesh_color_specular_compact", "mesh_color_motion_compact", "mesh_color_specular_motion_compact",
			"mesh_color_native_radiance_array_compact", "mesh_color_specular_radiance_array_compact",
			"mesh_color_motion_radiance_array_compact", "mesh_color_specular_motion_radiance_array_compact"
		};
		return { id, "res://addons/terrain_3d/rd/terrain_world_raster.glsl", strip ? stripVersions[offset] : meshVersions[offset],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, false };
	}
	if ( id == T3D_RD_SHADER_OCEAN_PREPARED_COLOR )
		return { id, "res://addons/terrain_3d/rd/terrain_ocean.glsl", "color_prepared",
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_OCEAN, false };
	if ( id == T3D_RD_SHADER_SCREEN_SPACE_COVERAGE || id == T3D_RD_SHADER_SCREEN_SPACE_SHADE )
		return { id, "res://addons/terrain_3d/rd/terrain_clipmap_screen_space.glsl",
			id == T3D_RD_SHADER_SCREEN_SPACE_COVERAGE ? "coverage" : "shade",
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	if ( id == T3D_RD_SHADER_CLUSTER_COMPACT )
		return { id, "res://addons/terrain_3d/rd/terrain_cluster_compact.glsl", "compact",
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_COMPUTE, false };
	if ( id == T3D_RD_SHADER_VT_FEEDBACK_MULTI )
		return { id, "res://addons/terrain_3d/rd/terrain_vt_feedback_multi_writer.glsl",
			"reduce_multi", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_COMPUTE, false };
	if ( id == T3D_RD_SHADER_PAGE_DERIVED_GPU )
		return { id,
				 "res://addons/terrain_3d/compute/terrain_page_derived.glsl",
				 "",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_COMPUTE,
				 false };
	const uint32_t value = ( uint32_t ) id;
	if ( id == T3D_RD_SHADER_PAGE_COMPUTE_R32F || id == T3D_RD_SHADER_PAGE_COMPUTE_RGBA8 ||
		 id == T3D_RD_SHADER_PAGE_COMPUTE_R16UI || id == T3D_RD_SHADER_PAGE_COMPUTE_R32UI ) {
		return { id,
				 "res://addons/terrain_3d/compute/terrain_page_storage.glsl",
				 id == T3D_RD_SHADER_PAGE_COMPUTE_R32F
					 ? "r32f"
					 : ( id == T3D_RD_SHADER_PAGE_COMPUTE_RGBA8
							 ? "rgba8"
							 : ( id == T3D_RD_SHADER_PAGE_COMPUTE_R16UI ? "r16ui" : "r32ui" ) ),
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_COMPUTE,
				 false };
	}
	if ( id >= T3D_RD_SHADER_PAGE_COMPUTE_R32F_REDUCE && id <= T3D_RD_SHADER_PAGE_COMPUTE_R32UI_REDUCE ) {
		static const char *const versions[] = { "r32f_reduce", "rgba8_reduce", "r16ui_reduce", "r32ui_reduce" };
		return { id, "res://addons/terrain_3d/compute/terrain_page_storage.glsl",
				 versions[value - T3D_RD_SHADER_PAGE_COMPUTE_R32F_REDUCE],
				 T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_COMPUTE, false };
	}
	if ( id >= T3D_RD_SHADER_PAGE_COMPUTE_R32F_PARTIAL && id <= T3D_RD_SHADER_PAGE_COMPUTE_R32UI_PARTIAL ) {
		static const char *const versions[] = { "r32f_partial", "rgba8_partial", "r16ui_partial", "r32ui_partial" };
		return { id, "res://addons/terrain_3d/compute/terrain_page_storage.glsl",
				 versions[value - T3D_RD_SHADER_PAGE_COMPUTE_R32F_PARTIAL],
				 T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_COMPUTE, false };
	}
	if ( id == T3D_RD_SHADER_PAGE_COMPUTE_REDUCE_FINAL )
		return { id, "res://addons/terrain_3d/compute/terrain_page_storage.glsl", "reduce_final",
				 T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_COMPUTE, false };
	// CS18 opt-in strip variants; the depth-only shader retains its original contract.
	if ( value >= T3D_RD_SHADER_WORLD_CS_BASE && value < T3D_RD_SHADER_WORLD_CS_END ) {
		static const char *const s_composedVersions[T3D_RD_FORWARD_SHADER_VARIANT_MAX - 1u] = {
			"cs_terrain_strip_color_native", "cs_terrain_strip_color_specular",
			"cs_terrain_strip_color_motion", "cs_terrain_strip_color_specular_motion",
			"cs_terrain_strip_color_native_radiance_array",
			"cs_terrain_strip_color_specular_radiance_array",
			"cs_terrain_strip_color_motion_radiance_array",
			"cs_terrain_strip_color_specular_motion_radiance_array",
			"cs_terrain_strip_depth_normal_roughness",
			"cs_terrain_strip_depth_normal_roughness_voxel_gi"
		};
		return { id, "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
				 s_composedVersions[value - T3D_RD_SHADER_WORLD_CS_BASE],
				 T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, false };
	}
	if ( value >= T3D_RD_SHADER_TERRAIN_FEEDBACK_BASE && value < T3D_RD_SHADER_WORLD_CS_BASE ) {
		const bool one_sun = value >= T3D_RD_SHADER_TERRAIN_FEEDBACK_ONE_SUN_BASE;
		const uint32_t offset = value - ( one_sun ? T3D_RD_SHADER_TERRAIN_FEEDBACK_ONE_SUN_BASE : T3D_RD_SHADER_TERRAIN_FEEDBACK_BASE );
		const uint32_t tier = offset / T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
		const uint32_t variant = offset % T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
		static const char *const s_feedbackVersions[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
			"feedback_color_native", "feedback_color_specular", "feedback_color_motion", "feedback_color_specular_motion",
			"feedback_color_native_radiance_array", "feedback_color_specular_radiance_array",
			"feedback_color_motion_radiance_array", "feedback_color_specular_motion_radiance_array"
		};
		static const char *const s_feedbackOneSunVersions[T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
			"feedback_color_native_one_sun", "feedback_color_specular_one_sun",
			"feedback_color_motion_one_sun", "feedback_color_specular_motion_one_sun",
			"feedback_color_native_one_sun_radiance_array", "feedback_color_specular_one_sun_radiance_array",
			"feedback_color_motion_one_sun_radiance_array", "feedback_color_specular_motion_one_sun_radiance_array"
		};
		return { id, s_terrainPaths[tier], one_sun ? s_feedbackOneSunVersions[variant] : s_feedbackVersions[variant],
				 T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	}
	if ( value >= T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE && value < T3D_RD_SHADER_PAGE_COMPUTE_R32F ) {
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
				 s_worldMeshFeedbackVersions[value - T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE],
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 false };
	}
	if ( value >= T3D_RD_SHADER_INSTANCE_FEEDBACK_BASE && value < T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE ) {
		return { id,
				 "res://addons/terrain_3d/rd/terrain_instances.glsl",
				 s_instanceFeedbackVersions[value - T3D_RD_SHADER_INSTANCE_FEEDBACK_BASE],
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_INSTANCE,
				 false };
	}
	if ( value < T3D_RD_SHADER_TERRAIN_ONE_SUN_BASE ) {
		const uint32_t group = value / T3D_RD_FORWARD_SHADER_VARIANT_MAX;
		const uint32_t path = group / 3u;
		return { id,
				 s_terrainPaths[path],
				 s_forwardVersions[path][value % T3D_RD_FORWARD_SHADER_VARIANT_MAX],
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_TERRAIN,
				 path == T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL };
	}
	if ( value < T3D_RD_SHADER_NEAR_ALIAS ) {
		const uint32_t offset = value - T3D_RD_SHADER_TERRAIN_ONE_SUN_BASE;
		const uint32_t group = offset / T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
		return { id,
				 s_terrainPaths[group / 3u],
				 s_oneSunVersions[group / 3u][offset % T3D_RD_FORWARD_COLOR_VARIANT_COUNT],
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_TERRAIN,
				 false };
	}
	if ( value >= T3D_RD_SHADER_INSTANCE_FORWARD_BASE && value < T3D_RD_SHADER_INSTANCE_ALIAS ) {
		return { id,
				 "res://addons/terrain_3d/rd/terrain_instances.glsl",
				 s_forwardVersions[0][value - T3D_RD_SHADER_INSTANCE_FORWARD_BASE],
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_INSTANCE,
				 true };
	}
	if ( value >= T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE && value < T3D_RD_SHADER_WORLD_RASTER_MESH_BASE ) {
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
				 s_worldStripVersions[value - T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE],
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 true };
	}
	if ( value >= T3D_RD_SHADER_WORLD_RASTER_MESH_BASE && value < T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW ) {
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
				 s_worldMeshVersions[value - T3D_RD_SHADER_WORLD_RASTER_MESH_BASE],
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 true };
	}
	switch ( id ) {
	case T3D_RD_SHADER_NEAR_ALIAS:
		return { id, s_terrainPaths[0], "color", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, true };
	case T3D_RD_SHADER_MIDDLE_ALIAS:
		return { id, s_terrainPaths[1], "color", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, true };
	case T3D_RD_SHADER_FAR_ALIAS:
		return { id, s_terrainPaths[2], "color", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, true };
	case T3D_RD_SHADER_TERRAIN_CULL:
		return { id, NULL, "", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, true };
	case T3D_RD_SHADER_INSTANCE_ALIAS:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_instances.glsl",
				 "color",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_INSTANCE,
				 true };
	case T3D_RD_SHADER_INSTANCE_OCCLUDER_DEPTH:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_instance_occluder_depth.glsl",
				 "",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_INSTANCE,
				 true };
	case T3D_RD_SHADER_HLOD_CULL:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_hlod_cull.glsl",
				 "",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_INSTANCE,
				 true };
	case T3D_RD_SHADER_INSTANCE_CULL:
	case T3D_RD_SHADER_INSTANCE_CULL_GROUPED:
	case T3D_RD_SHADER_INSTANCE_CULL_COMMAND_OWNER:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_instance_cull.glsl",
				 id == T3D_RD_SHADER_INSTANCE_CULL_COMMAND_OWNER ? "command_owner" :
				 ( id == T3D_RD_SHADER_INSTANCE_CULL_GROUPED ? "grouped" : "legacy" ),
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_INSTANCE,
				 true };
	case T3D_RD_SHADER_WORLD_HIZ_REDUCE:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_hiz_reduce.glsl",
				 "",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 true };
	case T3D_RD_SHADER_WORLD_VISIBILITY_RESET:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_visibility.glsl",
				 "reset",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 true };
	case T3D_RD_SHADER_WORLD_VISIBILITY_CULL:
	case T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED:
	case T3D_RD_SHADER_WORLD_VISIBILITY_CULL_SMALL:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_visibility.glsl",
				 id == T3D_RD_SHADER_WORLD_VISIBILITY_CULL_SMALL ? "cull_small" :
					 ( id == T3D_RD_SHADER_WORLD_VISIBILITY_CULL_GROUPED ? "cull_grouped" : "cull" ),
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 true };
	case T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_CLASSIFY:
	case T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_PREFIX:
	case T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_SCATTER:
	case T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_FINALIZE:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_visibility.glsl",
				 id == T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_CLASSIFY ? "scan_classify" :
				 ( id == T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_PREFIX ? "scan_prefix" :
				 ( id == T3D_RD_SHADER_WORLD_VISIBILITY_SCAN_SCATTER ? "scan_scatter" : "scan_finalize" ) ),
				 T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
	case T3D_RD_SHADER_WORLD_VISIBILITY_FINALIZE:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_visibility.glsl",
				 "finalize",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 true };
	case T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
				 "terrain_strip_shadow",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 true };
	case T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
				 "mesh_shadow",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_WORLD,
				 true };
	case T3D_RD_SHADER_OCEAN_COLOR:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_ocean.glsl",
				 "color_native",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_OCEAN,
				 true };
	case T3D_RD_SHADER_OCEAN_SHADOW:
		return { id,
				 "res://addons/terrain_3d/rd/terrain_ocean.glsl",
				 "shadow",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_OCEAN,
				 true };
	case T3D_RD_SHADER_BRUSH_COMPUTE:
		return { id,
				 "res://addons/terrain_3d/compute/terrain_brush.glsl",
				 "",
				 T3D_RD_SHADER_LOCAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_COMPUTE,
				 false };
	case T3D_RD_SHADER_DERIVED_COMPUTE:
		return { id,
				 "res://addons/terrain_3d/compute/terrain_derived.glsl",
				 "",
				 T3D_RD_SHADER_LOCAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_COMPUTE,
				 false };
	case T3D_RD_SHADER_DISPLACEMENT_COMPUTE:
		return { id,
				 "res://addons/terrain_3d/compute/terrain_displacement.glsl",
				 "",
				 T3D_RD_SHADER_GLOBAL_DEVICE,
				 T3D_RD_SHADER_CATEGORY_COMPUTE,
				 false };
	default:
		return { id, NULL, NULL, T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	}
}
