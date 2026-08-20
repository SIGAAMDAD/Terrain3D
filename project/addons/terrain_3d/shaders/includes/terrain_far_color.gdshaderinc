// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

vec2 get_far_atlas_uv(const vec2 world_texel) {
	float world_size = _region_size * float(_region_map_size);
	return clamp((world_texel + vec2(world_size * 0.5)) / world_size, vec2(0.0), vec2(1.0));
}

vec4 sample_far_color_nearest_unchecked(const vec2 world_texel) {
	ivec2 atlas_size = textureSize(_far_color_map, 0);
	vec2 atlas_uv = get_far_atlas_uv(world_texel);
	ivec2 atlas_coord = clamp(
		ivec2(floor(atlas_uv * vec2(atlas_size))),
		ivec2(0),
		atlas_size - ivec2(1));
	return texelFetch(_far_color_map, atlas_coord, 0);
}
