// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

int shadow_page_layer(const bool middle, const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return -1;
	}

	float encoded = middle ?
		texelFetch(_middle_height_region_map_texture, map_pos, 0).r :
		texelFetch(_region_map_texture, map_pos, 0).r;
	return int(round(encoded * 65535.0)) - 1;
}

float shadow_far_height(const vec2 world_texel) {
	return far_atlas_contains(world_texel) ? sample_far_height_unchecked(world_texel) : 0.0;
}

float shadow_page_height(const vec2 world_texel, const bool middle) {
	vec2 rounded_uv = round(world_texel);
	ivec2 map_pos = ivec2(floor(rounded_uv * _region_texel_size)) + (_region_map_size / 2);
	int layer = shadow_page_layer(middle, map_pos);
	if (layer < 0) {
		return middle ? shadow_far_height(world_texel) : 0.0;
	}

	vec2 local_texel = mod(rounded_uv, _region_size);
	ivec2 page_size = middle ? textureSize(_middle_height_maps, 0).xy : textureSize(_height_maps, 0).xy;
	vec2 page_uv = (local_texel * vec2(page_size) / _region_size + vec2(0.5)) / vec2(page_size);
	float encoded = middle ?
		textureLod(_middle_height_maps, vec3(page_uv, float(layer)), 0.0).r :
		textureLod(_height_maps, vec3(page_uv, float(layer)), 0.0).r;
	return decode_terrain_height(encoded);
}
