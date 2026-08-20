// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

vec3 missing_texture_checker(const vec2 world_texel) {
	vec2 cell = floor(world_texel * 0.125);
	float checker = mod(cell.x + cell.y, 2.0);
	return mix(vec3(0.24), vec3(0.42), checker);
}
