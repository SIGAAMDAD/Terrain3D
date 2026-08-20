// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#if T3D_TERRAIN_NORMAL_MODE != 2
vec3 sample_far_normal_from_height(const vec2 world_texel, const float sample_step, const float h0) {
	float hx = sample_far_height(world_texel + vec2(sample_step, 0.0));
	float hz = sample_far_height(world_texel + vec2(0.0, sample_step));
	float world_step = sample_step / max(_vertex_density, 1e-6);
	return normalize(vec3(h0 - hx, world_step, h0 - hz));
}

vec3 sample_far_normal(const vec2 world_texel, const float sample_step) {
	return sample_far_normal_from_height(world_texel, sample_step, sample_far_height(world_texel));
}
#endif
