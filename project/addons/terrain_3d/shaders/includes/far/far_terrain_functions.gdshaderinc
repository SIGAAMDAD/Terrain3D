// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "res://addons/terrain_3d/shaders/includes/terrain_render_varyings.gdshaderinc"
#if T3D_TERRAIN_NORMAL_MODE != 2
varying vec3 v_far_normal;
#endif

#include "res://addons/terrain_3d/shaders/includes/terrain_height_decode.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_surface.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_height.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_atlas_bounds.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_color.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_color_filtered.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_missing_texture.gdshaderinc"

float sample_far_height(const vec2 world_texel) {
	return far_atlas_contains(world_texel) ? sample_far_height_unchecked(world_texel) : 0.0;
}

vec4 sample_far_color(const vec2 world_texel) {
	return far_atlas_contains(world_texel) ?
		sample_far_color_filtered_unchecked(world_texel) : vec4(1.0, 1.0, 1.0, 0.0);
}

#include "res://addons/terrain_3d/shaders/includes/terrain_far_normal.gdshaderinc"

vec3 sample_far_material(const int texture_id, const vec2 world_texel) {
	if (_far_albedo_layer_count <= 0) {
		return missing_texture_checker(world_texel);
	}

	int id = clamp(texture_id, 0, _far_albedo_layer_count - 1);
	float uv_scale = _texture_uv_scale_array[id];
	if (!(uv_scale > 0.00001)) {
		uv_scale = 0.1;
	}
	vec3 tint = id < _texture_color_count ? _texture_color_array[id].rgb : vec3(1.0);

#if T3D_TERRAIN_SAMPLING_MODE == 2
	return tint;
#else
	float material_lod = clamp(
		_tier_material_lod_bias + _far_quality_lod_offset,
		0.0,
		max(_far_material_max_lod, 0.0));
	vec2 uv = world_texel * (_vertex_spacing * uv_scale);

#if T3D_DETILING_MODE != 2
	ivec2 cell = ivec2(floor(world_texel * 0.0625));
	int rotation = (cell.x ^ cell.y) & 3;
	uv = rotation == 1 ? vec2(-uv.y, uv.x) :
		(rotation == 2 ? -uv : (rotation == 3 ? vec2(uv.y, -uv.x) : uv));
#endif

	return textureLod(_far_albedo_array, vec3(uv, float(id)), material_lod).rgb * tint;
#endif
}

#include "res://addons/terrain_3d/shaders/includes/far_none_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/far_flat_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_functions.gdshaderinc"
