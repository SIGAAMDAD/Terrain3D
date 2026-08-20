// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

vec4 sample_far_color_filtered_unchecked(const vec2 world_texel) {
#if T3D_COLOR_SAMPLING_MODE == 2
	ivec2 atlas_size = textureSize(_far_color_map, 0);
	ivec2 max_coord = atlas_size - ivec2(1);
	vec2 texel = get_far_atlas_uv(world_texel) * vec2(atlas_size) - vec2(0.5);
	ivec2 base = ivec2(floor(texel));
	vec2 f = fract(texel);

	vec4 s00 = texelFetch(_far_color_map, clamp(base, ivec2(0), max_coord), 0);
	vec4 s10 = texelFetch(_far_color_map, clamp(base + ivec2(1, 0), ivec2(0), max_coord), 0);
	vec4 s01 = texelFetch(_far_color_map, clamp(base + ivec2(0, 1), ivec2(0), max_coord), 0);
	vec4 s11 = texelFetch(_far_color_map, clamp(base + ivec2(1, 1), ivec2(0), max_coord), 0);

	vec3 rgb = mix(mix(s00.rgb, s10.rgb, f.x), mix(s01.rgb, s11.rgb, f.x), f.y);
	float alpha = f.x < 0.5 ? (f.y < 0.5 ? s00.a : s01.a) : (f.y < 0.5 ? s10.a : s11.a);
	return vec4(rgb, alpha);
#else
	return sample_far_color_nearest_unchecked(world_texel);
#endif
}
