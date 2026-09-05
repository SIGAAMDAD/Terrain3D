// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

int get_region_layer(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(T3D_REGION_MAP_SIZE)) {
		return -1;
	}
	return int(round(texelFetch(_region_map_texture, map_pos, 0).r * 65535.0)) - 1;
}

bool get_logical_region_present(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(T3D_REGION_MAP_SIZE)) {
		return false;
	}
	return texelFetch(_logical_region_map_texture, map_pos, 0).r > 0.5;
}

ivec3 get_index_coord(const vec2 uv) {
	ivec2 grid = ivec2(round(uv));
	ivec2 region_cell = t3d_region_cell_from_grid(grid);
	ivec2 map_pos = region_cell + ivec2(T3D_REGION_MAP_HALF);
	return ivec3(t3d_region_local_from_grid(grid, region_cell), get_region_layer(map_pos));
}

vec3 get_index_uv(const vec2 uv2) {
	vec2 region_pos = floor(uv2);
	ivec2 map_pos = ivec2(region_pos) + T3D_REGION_MAP_HALF;
	return vec3(uv2 - region_pos, float(get_region_layer(map_pos)));
}
