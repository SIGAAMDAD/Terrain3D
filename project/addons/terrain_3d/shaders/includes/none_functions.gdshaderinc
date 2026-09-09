// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_NONE_FUNCTIONS
// Takes in world space XZ (UV) and returns if the coordinate should be part of the NONE background.
bool is_none_bg(const vec2 uv) {
	ivec2 half_map = ivec2(T3D_REGION_MAP_HALF);
	ivec2 a = ivec2(floor((uv - vec2(0.5, 0.0)) * T3D_REGION_INV_SIZE)) + half_map;
	ivec2 b = ivec2(floor((uv - vec2(0.0, 0.5)) * T3D_REGION_INV_SIZE)) + half_map;
	ivec2 c = ivec2(floor((uv + vec2(1.0, 0.0)) * T3D_REGION_INV_SIZE)) + half_map;
	ivec2 d = ivec2(floor((uv + vec2(0.0, 1.0)) * T3D_REGION_INV_SIZE)) + half_map;
	return !get_logical_region_present(a) || !get_logical_region_present(b) ||
		!get_logical_region_present(c) || !get_logical_region_present(d);
}
#endif
