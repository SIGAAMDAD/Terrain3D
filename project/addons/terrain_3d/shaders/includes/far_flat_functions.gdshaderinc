// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_FAR_FLAT_FUNCTIONS
float check_region_cell(const ivec2 cell) {
	ivec2 map_pos = cell + (_region_map_size / 2);
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return 0.0;
	}
	vec2 world_center = (vec2(cell) + vec2(0.5)) * _region_size;
	return float(far_surface_is_solid(sample_far_color(world_center).a));
}

float sample_region_presence(const ivec2 base_cell, const vec2 w) {
	float a = check_region_cell(base_cell);
	float b = check_region_cell(base_cell + ivec2(1, 0));
	float c = check_region_cell(base_cell + ivec2(0, 1));
	float d = check_region_cell(base_cell + ivec2(1, 1));
	return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}

float get_region_blend(vec2 uv2) {
	uv2 -= 0.5011;
	ivec2 base_cell = ivec2(floor(uv2));
	vec2 f = fract(uv2);
	float blend_factor = 2.0 + 126.0 * (1.0 - region_blend);
	float half_width = clamp(0.75 / blend_factor, 1.0 / 1024.0, 0.499);
	vec2 w = smoothstep(vec2(0.5 - half_width), vec2(0.5 + half_width), f);
	return (1.0 - sample_region_presence(base_cell, w)) * 2.0;
}
#endif
