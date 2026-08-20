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
	// The far-height sampler clamps to the atlas edge. Keep the shadow caster
	// continuous there instead of manufacturing a Y=0 cliff.
	return sample_far_height_unchecked(world_texel);
}

float shadow_middle_height(const vec2 world_texel) {
	vec2 rounded_uv = round(world_texel);
	ivec2 map_pos = ivec2(floor(rounded_uv * _region_texel_size)) + (_region_map_size / 2);
	int layer = shadow_page_layer(true, map_pos);
	if (layer < 0) {
		return shadow_far_height(world_texel);
	}

	vec2 local_texel = mod(rounded_uv, _region_size);
	ivec2 page_size = textureSize(_middle_height_maps, 0).xy;
	vec2 page_uv = (local_texel * vec2(page_size) / _region_size + vec2(0.5)) / vec2(page_size);
	float encoded = textureLod(_middle_height_maps, vec3(page_uv, float(layer)), 0.0).r;
	return decode_terrain_height(encoded);
}

float shadow_page_height(const vec2 world_texel, const bool middle) {
	if (middle) {
		return shadow_middle_height(world_texel);
	}

	vec2 rounded_uv = round(world_texel);
	ivec2 map_pos = ivec2(floor(rounded_uv * _region_texel_size)) + (_region_map_size / 2);
	int layer = shadow_page_layer(false, map_pos);
	if (layer < 0) {
		// Match the visible terrain's coherent near -> middle -> far fallback.
		return shadow_middle_height(world_texel);
	}

	vec2 local_texel = mod(rounded_uv, _region_size);
	ivec2 page_size = textureSize(_height_maps, 0).xy;
	vec2 page_uv = (local_texel * vec2(page_size) / _region_size + vec2(0.5)) / vec2(page_size);
	float encoded = textureLod(_height_maps, vec3(page_uv, float(layer)), 0.0).r;
	return decode_terrain_height(encoded);
}
