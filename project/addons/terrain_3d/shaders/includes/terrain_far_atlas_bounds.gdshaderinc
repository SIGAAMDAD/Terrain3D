// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

bool far_atlas_contains(const vec2 world_texel) {
	float half_world_size = T3D_REGION_SIZE_F * float(T3D_REGION_MAP_SIZE) * 0.5;
	return all(greaterThanEqual(world_texel, vec2(-half_world_size))) &&
		all(lessThan(world_texel, vec2(half_world_size)));
}
