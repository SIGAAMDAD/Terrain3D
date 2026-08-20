// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

int get_region_layer(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return -1;
	}
	return int(round(texelFetch(_region_map_texture, map_pos, 0).r * 65535.0)) - 1;
}

bool get_logical_region_present(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return false;
	}
	return texelFetch(_logical_region_map_texture, map_pos, 0).r > 0.5;
}

ivec3 get_index_coord(const vec2 uv) {
	vec2 rounded_uv = round(uv);
	ivec2 map_pos = ivec2(floor(rounded_uv * _region_texel_size)) + (_region_map_size / 2);
	return ivec3(ivec2(mod(rounded_uv, _region_size)), get_region_layer(map_pos));
}

vec3 get_index_uv(const vec2 uv2) {
	vec2 region_pos = floor(uv2);
	ivec2 map_pos = ivec2(region_pos) + (_region_map_size / 2);
	return vec3(uv2 - region_pos, float(get_region_layer(map_pos)));
}
