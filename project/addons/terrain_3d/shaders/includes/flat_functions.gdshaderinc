// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_FLAT_FUNCTIONS
// Exact presence lookup used only for map-edge fallback.
float check_region_cell(const ivec2 cell) {
	return float(get_logical_region_present(cell + (_region_map_size / 2)));
}

// Bilinear presence lookup. The fractional coordinate is supplied explicitly so
// callers retain their existing transition curve while texture hardware combines
// the four binary map cells in one filtered fetch.
float sample_region_presence(const ivec2 base_cell, const vec2 w) {
	ivec2 map_base = base_cell + (_region_map_size / 2);
	ivec2 map_last = map_base + ivec2(1);
	if (all(greaterThanEqual(map_base, ivec2(0))) && all(lessThan(map_last, ivec2(_region_map_size)))) {
		vec2 map_uv = (vec2(map_base) + vec2(0.5) + w) / float(_region_map_size);
		return textureLod(_logical_region_map_texture, map_uv, 0.0).r;
	}
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
