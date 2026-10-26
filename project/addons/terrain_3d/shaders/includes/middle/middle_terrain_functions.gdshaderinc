// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "res://addons/terrain_3d/shaders/includes/terrain_render_varyings.gdshaderinc"
varying vec3 v_middle_normal;

#include "res://addons/terrain_3d/shaders/includes/terrain_height_decode.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_surface.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_region_lookup.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_height.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_atlas_bounds.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_color.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_missing_texture.gdshaderinc"

float sample_far_height(const vec2 world_texel) {
	return far_atlas_contains(world_texel) ? sample_far_height_unchecked(world_texel) : 0.0;
}

vec4 sample_far_color(const vec2 world_texel) {
	return far_atlas_contains(world_texel) ?
		sample_far_color_nearest_unchecked(world_texel) : vec4(1.0, 1.0, 1.0, 0.0);
}

#include "res://addons/terrain_3d/shaders/includes/terrain_far_normal.gdshaderinc"
int get_middle_height_layer(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(T3D_REGION_MAP_SIZE)) {
		return -1;
	}
	return int(round(texelFetch(_middle_height_region_map_texture, map_pos, 0).r * 65535.0)) - 1;
}

vec3 sample_texture_tint(const int texture_id) {
#if T3D_TERRAIN_SAMPLING_MODE == 2
	return vec3(1.0);
#else
	if (texture_id < 0 || texture_id >= _texture_color_count) {
		return vec3(1.0);
	}
	return _texture_color_array[texture_id].rgb;
#endif
}

vec2 rotate_compact_uv(const vec2 uv, const vec2 cs) {
#if T3D_DETILING_MODE == 2
	return uv;
#else
	return vec2(cs.x * uv.x + cs.y * uv.y, cs.x * uv.y - cs.y * uv.x);
#endif
}

vec3 sample_middle_material(
	const int texture_id,
	const vec2 world_texel,
	const float control_scale,
	const vec2 control_rotation
) {
#if T3D_TERRAIN_SAMPLING_MODE == 2
	return vec3(1.0);
#else
	if (_texture_array_layer_count <= 0) {
		return missing_texture_checker(world_texel);
	}
	int id = clamp(texture_id, 0, _texture_array_layer_count - 1);
	float asset_scale = _texture_uv_scale_array[id];
	vec3 tint = sample_texture_tint(id);
	if (!t3d_mat_resident(id)) {
		return t3d_runtime_average_material(id, tint);
	}
	vec2 world_xz = world_texel * _vertex_spacing;
	vec2 uv = rotate_compact_uv(world_xz * control_scale, control_rotation) * asset_scale;
	return texture(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), _tier_material_lod_bias).rgb * tint;
#endif
}

vec3 sample_middle_far_material(const int texture_id, const vec2 world_texel) {
#if T3D_TERRAIN_SAMPLING_MODE == 2
	return vec3(1.0);
#else
	if (_texture_array_layer_count <= 0) {
		return missing_texture_checker(world_texel);
	}
	int id = clamp(texture_id, 0, _texture_array_layer_count - 1);
	vec3 tint = sample_texture_tint(id);
	vec2 uv = world_texel * (_vertex_spacing * _texture_uv_scale_array[id]);
	float material_lod = clamp(max(2.0, 3.0 + _tier_material_lod_bias), 0.0, max(_material_max_lod, 0.0));
	return textureLod(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), material_lod).rgb * tint;
#endif
}

#include "res://addons/terrain_3d/shaders/includes/none_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/flat_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_functions.gdshaderinc"
#if T3D_TERRAIN_SAMPLING_MODE != 2
#include "res://addons/terrain_3d/shaders/includes/middle_color_functions.gdshaderinc"
#endif

float sample_terrain_height(const ivec3 index, const vec2 world_texel) {
	ivec2 grid = ivec2(round(world_texel));
	ivec2 region_cell = t3d_region_cell_from_grid(grid);
	ivec2 map_pos = region_cell + ivec2(T3D_REGION_MAP_HALF);
	int height_layer = get_middle_height_layer(map_pos);
	if (height_layer >= 0) {
		ivec2 local_coord = t3d_region_local_from_grid(grid, region_cell);
		ivec2 height_size = textureSize(_middle_height_maps, 0).xy;
		if (height_size.x == T3D_REGION_SIZE && height_size.y == T3D_REGION_SIZE) {
			return decode_terrain_height(texelFetch(_middle_height_maps, ivec3(local_coord, height_layer), 0).r);
		}
		vec2 reduced_texel = vec2(local_coord) * vec2(height_size) / T3D_REGION_SIZE_F;
		vec2 reduced_uv = (reduced_texel + vec2(0.5)) / vec2(height_size);
		return decode_terrain_height(textureLod(_middle_height_maps, vec3(reduced_uv, float(height_layer)), 0.0).r);
	}
	return sample_far_height(world_texel);
}

#include "res://addons/terrain_3d/shaders/includes/terrain_height_interpolation.gdshaderinc"
