// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Quad order matches the near-tier fragment arrays: 01, 11, 10, 00.
void get_quad_indices(const vec2 base, out ivec3 i01, out ivec3 i11, out ivec3 i10, out ivec3 i00) {
	i00 = get_index_coord(base);
	int last_texel = int(T3D_REGION_SIZE_F) - 1;
	if (i00.z >= 0 && i00.x < last_texel && i00.y < last_texel) {
		i10 = ivec3(i00.xy + ivec2(1, 0), i00.z);
		i01 = ivec3(i00.xy + ivec2(0, 1), i00.z);
		i11 = ivec3(i00.xy + ivec2(1, 1), i00.z);
	} else {
		i01 = get_index_coord(base + vec2(0.0, 1.0));
		i11 = get_index_coord(base + vec2(1.0, 1.0));
		i10 = get_index_coord(base + vec2(1.0, 0.0));
	}
}

float interpolated_height(const vec2 pos) {
	vec2 base = floor(pos);
	ivec3 i01;
	ivec3 i11;
	ivec3 i10;
	ivec3 i00;
	get_quad_indices(base, i01, i11, i10, i00);

	float h00 = sample_terrain_height(i00, base);
	float h10 = sample_terrain_height(i10, base + vec2(1.0, 0.0));
	float h01 = sample_terrain_height(i01, base + vec2(0.0, 1.0));
	float h11 = sample_terrain_height(i11, base + vec2(1.0, 1.0));
	vec2 f = fract(pos);
	return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
}
