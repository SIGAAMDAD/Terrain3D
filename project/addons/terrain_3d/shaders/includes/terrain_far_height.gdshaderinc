// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

vec2 get_far_height_atlas_uv(const vec2 world_texel) {
	float world_size = _region_size * float(_region_map_size);
	vec2 atlas_size = vec2(textureSize(_far_height_map, 0));
	vec2 half_texel = vec2(0.5) / atlas_size;
	vec2 atlas_uv = (world_texel + vec2(world_size * 0.5)) / world_size + half_texel;
	return clamp(atlas_uv, half_texel, vec2(1.0) - half_texel);
}

float sample_far_height_unchecked(const vec2 world_texel) {
	return decode_terrain_height(textureLod(_far_height_map, get_far_height_atlas_uv(world_texel), 0.0).r);
}
