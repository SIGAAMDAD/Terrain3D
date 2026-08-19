// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// T3D_STANDALONE_DEFAULTS_BEGIN
#ifndef T3D_COLOR_MAPS_IMPLICIT
#define T3D_COLOR_MAPS_IMPLICIT 0
#endif
#ifndef T3D_COLOR_SAMPLING_MODE
#define T3D_COLOR_SAMPLING_MODE 0
#endif
// T3D_STANDALONE_DEFAULTS_END

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
	int layer = get_region_layer(region_cell + region_delta + (_region_map_size / 2));
	return layer >= 0 ? texelFetch(_color_maps, ivec3(local_coord, layer), 0) : COLOR_MAP_DEF;
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

vec4 sample_terrain_color(const vec2 world_region_uv, const vec3 region_uv) {
#if T3D_COLOR_MAPS_IMPLICIT
	return COLOR_MAP_DEF;
#else
	if (region_uv.z < 0.0) {
		return COLOR_MAP_DEF;
	}
#if T3D_COLOR_SAMPLING_MODE == 2
	return sample_terrain_color_exact(world_region_uv);
#else
	return texture(_color_maps, region_uv);
#endif
#endif
}
