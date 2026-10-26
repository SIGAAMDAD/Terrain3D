#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

static const char *const s_terrainPaths[3] = {
	"res://addons/terrain_3d/rd/terrain_clipmap_near.glsl",
	"res://addons/terrain_3d/rd/terrain_clipmap_middle.glsl",
	"res://addons/terrain_3d/rd/terrain_clipmap_far.glsl",
};

static const char *const s_otherGlobalPaths[] = {
	"res://addons/terrain_3d/rd/terrain_cull.glsl",
	"res://addons/terrain_3d/rd/terrain_instances.glsl",
	"res://addons/terrain_3d/rd/terrain_instance_occluder_depth.glsl",
	"res://addons/terrain_3d/rd/terrain_hlod_cull.glsl",
	"res://addons/terrain_3d/rd/terrain_instance_cull.glsl",
	"res://addons/terrain_3d/rd/terrain_world_hiz_reduce.glsl",
	"res://addons/terrain_3d/rd/terrain_world_visibility.glsl",
	"res://addons/terrain_3d/rd/terrain_world_raster.glsl",
	"res://addons/terrain_3d/rd/terrain_ocean.glsl",
	"res://addons/terrain_3d/compute/terrain_displacement.glsl",
};

static const char *const s_localPaths[] = {
	"res://addons/terrain_3d/compute/terrain_brush.glsl",
	"res://addons/terrain_3d/compute/terrain_derived.glsl",
};

uint32_t t3dRdShaderCatalogGlobalSourceCount() {
	return 3u + ( uint32_t )( sizeof( s_otherGlobalPaths ) / sizeof( s_otherGlobalPaths[0] ) );
}

const char *t3dRdShaderCatalogGlobalSourcePath( const uint32_t index ) {
	if ( index < 3u ) { return s_terrainPaths[index]; }
	const uint32_t other = index - 3u;
	return other < sizeof( s_otherGlobalPaths ) / sizeof( s_otherGlobalPaths[0] ) ? s_otherGlobalPaths[other] : NULL;
}

uint32_t t3dRdShaderCatalogSourceCount() {
	return t3dRdShaderCatalogGlobalSourceCount() + ( uint32_t )( sizeof( s_localPaths ) / sizeof( s_localPaths[0] ) );
}

const char *t3dRdShaderCatalogSourcePath( const uint32_t index ) {
	const uint32_t global_count = t3dRdShaderCatalogGlobalSourceCount();
	if ( index < global_count ) { return t3dRdShaderCatalogGlobalSourcePath( index ); }
	const uint32_t local = index - global_count;
	return local < sizeof( s_localPaths ) / sizeof( s_localPaths[0] ) ? s_localPaths[local] : NULL;
}

static const char *const s_forwardVersions[T3D_TERRAIN_SHADER_PATH_COUNT][T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
	{ "color_native", "color_specular", "color_motion", "color_specular_motion",
	  "color_native_radiance_array", "color_specular_radiance_array", "color_motion_radiance_array",
	  "color_specular_motion_radiance_array", "depth", "depth_normal_roughness", "depth_normal_roughness_voxel_gi" },
	{ "compat_color_native", "compat_color_specular", "compat_color_motion", "compat_color_specular_motion",
	  "compat_color_native_radiance_array", "compat_color_specular_radiance_array", "compat_color_motion_radiance_array",
	  "compat_color_specular_motion_radiance_array", "compat_depth", "compat_depth_normal_roughness",
	  "compat_depth_normal_roughness_voxel_gi" },
	{ "editor_color_native", "editor_color_specular", "editor_color_motion", "editor_color_specular_motion",
	  "editor_color_native_radiance_array", "editor_color_specular_radiance_array", "editor_color_motion_radiance_array",
	  "editor_color_specular_motion_radiance_array", "editor_depth", "editor_depth_normal_roughness",
	  "editor_depth_normal_roughness_voxel_gi" },
};

static const char *const s_oneSunVersions[T3D_TERRAIN_SHADER_PATH_COUNT][T3D_RD_FORWARD_COLOR_VARIANT_COUNT] = {
	{ "color_native_one_sun", "color_specular_one_sun", "color_motion_one_sun", "color_specular_motion_one_sun",
	  "color_native_one_sun_radiance_array", "color_specular_one_sun_radiance_array",
	  "color_motion_one_sun_radiance_array", "color_specular_motion_one_sun_radiance_array" },
	{ "compat_color_native_one_sun", "compat_color_specular_one_sun", "compat_color_motion_one_sun",
	  "compat_color_specular_motion_one_sun", "compat_color_native_one_sun_radiance_array",
	  "compat_color_specular_one_sun_radiance_array", "compat_color_motion_one_sun_radiance_array",
	  "compat_color_specular_motion_one_sun_radiance_array" },
	{ "editor_color_native_one_sun", "editor_color_specular_one_sun", "editor_color_motion_one_sun",
	  "editor_color_specular_motion_one_sun", "editor_color_native_one_sun_radiance_array",
	  "editor_color_specular_one_sun_radiance_array", "editor_color_motion_one_sun_radiance_array",
	  "editor_color_specular_motion_one_sun_radiance_array" },
};

static const char *const s_worldStripVersions[T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
	"terrain_strip_color_native", "terrain_strip_color_specular", "terrain_strip_color_motion",
	"terrain_strip_color_specular_motion", "terrain_strip_color_native_radiance_array",
	"terrain_strip_color_specular_radiance_array", "terrain_strip_color_motion_radiance_array",
	"terrain_strip_color_specular_motion_radiance_array", "terrain_strip_depth",
	"terrain_strip_depth_normal_roughness", "terrain_strip_depth_normal_roughness_voxel_gi",
};

static const char *const s_worldMeshVersions[T3D_RD_FORWARD_SHADER_VARIANT_MAX] = {
	"mesh_color_native", "mesh_color_specular", "mesh_color_motion", "mesh_color_specular_motion",
	"mesh_color_native_radiance_array", "mesh_color_specular_radiance_array", "mesh_color_motion_radiance_array",
	"mesh_color_specular_motion_radiance_array", "mesh_depth", "mesh_depth_normal_roughness",
	"mesh_depth_normal_roughness_voxel_gi",
};

t3dRdShaderCatalogEntry_t t3dRdShaderCatalogGet( const t3dRdShaderId_t id ) {
	const uint32_t value = ( uint32_t )id;
	if ( value < T3D_RD_SHADER_TERRAIN_ONE_SUN_BASE ) {
		const uint32_t group = value / T3D_RD_FORWARD_SHADER_VARIANT_MAX;
		const uint32_t path = group / 3u;
		return { id, s_terrainPaths[path], s_forwardVersions[path][value % T3D_RD_FORWARD_SHADER_VARIANT_MAX],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, path == T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL };
	}
	if ( value < T3D_RD_SHADER_NEAR_ALIAS ) {
		const uint32_t offset = value - T3D_RD_SHADER_TERRAIN_ONE_SUN_BASE;
		const uint32_t group = offset / T3D_RD_FORWARD_COLOR_VARIANT_COUNT;
		return { id, s_terrainPaths[group / 3u], s_oneSunVersions[group / 3u][offset % T3D_RD_FORWARD_COLOR_VARIANT_COUNT],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	}
	if ( value >= T3D_RD_SHADER_INSTANCE_FORWARD_BASE && value < T3D_RD_SHADER_INSTANCE_ALIAS ) {
		return { id, "res://addons/terrain_3d/rd/terrain_instances.glsl",
			s_forwardVersions[0][value - T3D_RD_SHADER_INSTANCE_FORWARD_BASE],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_INSTANCE, true };
	}
	if ( value >= T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE && value < T3D_RD_SHADER_WORLD_RASTER_MESH_BASE ) {
		return { id, "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
			s_worldStripVersions[value - T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
	}
	if ( value >= T3D_RD_SHADER_WORLD_RASTER_MESH_BASE && value < T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW ) {
		return { id, "res://addons/terrain_3d/rd/terrain_world_raster.glsl",
			s_worldMeshVersions[value - T3D_RD_SHADER_WORLD_RASTER_MESH_BASE],
			T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
	}
	switch ( id ) {
		case T3D_RD_SHADER_NEAR_ALIAS: return { id, s_terrainPaths[0], "color", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, true };
		case T3D_RD_SHADER_MIDDLE_ALIAS: return { id, s_terrainPaths[1], "color", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, true };
		case T3D_RD_SHADER_FAR_ALIAS: return { id, s_terrainPaths[2], "color", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, true };
		case T3D_RD_SHADER_TERRAIN_CULL: return { id, "res://addons/terrain_3d/rd/terrain_cull.glsl", "", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, true };
		case T3D_RD_SHADER_INSTANCE_ALIAS: return { id, "res://addons/terrain_3d/rd/terrain_instances.glsl", "color", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_INSTANCE, true };
		case T3D_RD_SHADER_INSTANCE_OCCLUDER_DEPTH: return { id, "res://addons/terrain_3d/rd/terrain_instance_occluder_depth.glsl", "", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_INSTANCE, true };
		case T3D_RD_SHADER_HLOD_CULL: return { id, "res://addons/terrain_3d/rd/terrain_hlod_cull.glsl", "", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_INSTANCE, true };
		case T3D_RD_SHADER_INSTANCE_CULL: return { id, "res://addons/terrain_3d/rd/terrain_instance_cull.glsl", "", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_INSTANCE, true };
		case T3D_RD_SHADER_WORLD_HIZ_REDUCE: return { id, "res://addons/terrain_3d/rd/terrain_world_hiz_reduce.glsl", "", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
		case T3D_RD_SHADER_WORLD_VISIBILITY_RESET: return { id, "res://addons/terrain_3d/rd/terrain_world_visibility.glsl", "reset", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
		case T3D_RD_SHADER_WORLD_VISIBILITY_CULL: return { id, "res://addons/terrain_3d/rd/terrain_world_visibility.glsl", "cull", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
		case T3D_RD_SHADER_WORLD_VISIBILITY_FINALIZE: return { id, "res://addons/terrain_3d/rd/terrain_world_visibility.glsl", "finalize", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
		case T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW: return { id, "res://addons/terrain_3d/rd/terrain_world_raster.glsl", "terrain_strip_shadow", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
		case T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW: return { id, "res://addons/terrain_3d/rd/terrain_world_raster.glsl", "mesh_shadow", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_WORLD, true };
		case T3D_RD_SHADER_OCEAN_COLOR: return { id, "res://addons/terrain_3d/rd/terrain_ocean.glsl", "color_native", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_OCEAN, true };
		case T3D_RD_SHADER_OCEAN_SHADOW: return { id, "res://addons/terrain_3d/rd/terrain_ocean.glsl", "shadow", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_OCEAN, true };
		case T3D_RD_SHADER_BRUSH_COMPUTE: return { id, "res://addons/terrain_3d/compute/terrain_brush.glsl", "", T3D_RD_SHADER_LOCAL_DEVICE, T3D_RD_SHADER_CATEGORY_COMPUTE, false };
		case T3D_RD_SHADER_DERIVED_COMPUTE: return { id, "res://addons/terrain_3d/compute/terrain_derived.glsl", "", T3D_RD_SHADER_LOCAL_DEVICE, T3D_RD_SHADER_CATEGORY_COMPUTE, false };
		case T3D_RD_SHADER_DISPLACEMENT_COMPUTE: return { id, "res://addons/terrain_3d/compute/terrain_displacement.glsl", "", T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_COMPUTE, false };
		default: return { id, NULL, NULL, T3D_RD_SHADER_GLOBAL_DEVICE, T3D_RD_SHADER_CATEGORY_TERRAIN, false };
	}
}
