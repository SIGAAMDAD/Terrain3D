// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

////////////////////////
// Shared terrain lookup
////////////////////////

#include "res://addons/terrain_3d/shaders/includes/terrain_region_lookup.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_height.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_color.gdshaderinc"

float sample_far_height(const vec2 world_texel) {
	return sample_far_height_unchecked(world_texel);
}

vec4 sample_far_color(const vec2 world_texel) {
	return sample_far_color_nearest_unchecked(world_texel);
}

vec4 sample_terrain_color(vec3 region_uv, float region_mip) {
#if T3D_COLOR_MAPS_IMPLICIT
	return COLOR_MAP_DEF;
#else
	return region_uv.z >= 0.0 ?
		textureLod(_color_maps, region_uv, clamp(region_mip, 0.0, _region_color_max_lod)) : COLOR_MAP_DEF;
#endif
}

#include "res://addons/terrain_3d/shaders/includes/near_far_cached_material.gdshaderinc"
float sample_terrain_height(const ivec3 index, const vec2 world_texel) {
	return index.z >= 0 ? decode_terrain_height(texelFetch(_height_maps, index, 0).r) :
		sample_far_height(world_texel);
}

#include "res://addons/terrain_3d/shaders/includes/terrain_height_interpolation.gdshaderinc"

// Resolve a nearby sample from an already resolved base texel. The region map
// is touched only when the offset crosses a region boundary.
ivec3 get_offset_index(const vec2 index_id, const ivec3 base_index, const ivec2 offset) {
	ivec2 local = base_index.xy + offset;
	int region_size_i = int(T3D_REGION_SIZE_F);
	if (base_index.z >= 0 && uint(local.x | local.y) < uint(region_size_i)) {
		return ivec3(local, base_index.z);
	}
	return get_index_coord(index_id + vec2(offset));
}

// Takes in descaled world-to-region coordinates. Local UVs are the fractional
// component, so no dynamic region-location uniform lookup is required.
vec4 read_terrain_color_exact_texel(ivec2 local_coord, ivec2 region_cell, const ivec2 map_size) {
#if T3D_COLOR_MAPS_IMPLICIT
	return COLOR_MAP_DEF;
#else
	ivec2 region_delta = ivec2(0);
	if (local_coord.x < 0) {
		local_coord.x += map_size.x;
		region_delta.x = -1;
	} else if (local_coord.x >= map_size.x) {
		local_coord.x -= map_size.x;
		region_delta.x = 1;
	}
	if (local_coord.y < 0) {
		local_coord.y += map_size.y;
		region_delta.y = -1;
	} else if (local_coord.y >= map_size.y) {
		local_coord.y -= map_size.y;
		region_delta.y = 1;
	}
	int layer = get_region_layer(region_cell + region_delta + T3D_REGION_MAP_HALF);
	return layer >= 0 ? texelFetch(_color_maps, ivec3(local_coord, layer), 0) : COLOR_MAP_DEF;
#endif
}

bool terrain_color_filter_crosses_region(const vec2 world_region_uv) {
#if T3D_COLOR_MAPS_IMPLICIT
	return false;
#else
	ivec2 map_size = textureSize(_color_maps, 0).xy;
	vec2 local_uv = world_region_uv - floor(world_region_uv);
	ivec2 base = ivec2(floor(local_uv * vec2(map_size) - vec2(0.5)));
	return base.x < 0 || base.y < 0 || base.x + 1 >= map_size.x || base.y + 1 >= map_size.y;
#endif
}

vec4 sample_terrain_color_exact(const vec2 world_region_uv) {
#if T3D_COLOR_MAPS_IMPLICIT
	return COLOR_MAP_DEF;
#else
	ivec2 map_size = textureSize(_color_maps, 0).xy;
	vec2 region_pos = floor(world_region_uv);
	vec2 local_uv = world_region_uv - region_pos;
	vec2 texel = local_uv * vec2(map_size) - vec2(0.5);
	ivec2 base = ivec2(floor(texel));
	vec2 f = fract(texel);
	ivec2 region_cell = ivec2(region_pos);
	vec4 c00 = read_terrain_color_exact_texel(base, region_cell, map_size);
	vec4 c10 = read_terrain_color_exact_texel(base + ivec2(1, 0), region_cell, map_size);
	vec4 c01 = read_terrain_color_exact_texel(base + ivec2(0, 1), region_cell, map_size);
	vec4 c11 = read_terrain_color_exact_texel(base + ivec2(1, 1), region_cell, map_size);
	return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
#endif
}
#include "res://addons/terrain_3d/shaders/includes/displacement_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/none_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/flat_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_functions.gdshaderinc"
