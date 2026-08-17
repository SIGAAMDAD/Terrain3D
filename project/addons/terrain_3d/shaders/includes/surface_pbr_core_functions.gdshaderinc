// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_SURFACE_PBR_CORE_FUNCTIONS
vec3 decode_surface_normal(const vec2 encoded_xy) {
	vec2 normal_xy = fma(encoded_xy, vec2(2.0), vec2(-1.0));
	float xy_length_squared = dot(normal_xy, normal_xy);
	if (xy_length_squared > 1.0) {
		normal_xy *= inversesqrt(xy_length_squared);
		xy_length_squared = 1.0;
	}
	// Terrain3D tangent convention: source X, reconstructed Z, source Y.
	return vec3(normal_xy.x, sqrt(max(1.0 - xy_length_squared, 0.0)), normal_xy.y);
}
#endif
