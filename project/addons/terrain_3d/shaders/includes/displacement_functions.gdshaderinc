// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DISPLACEMENT_FUNCTIONS
vec3 get_displacement(vec2 pos, float scale) {
	float s = floor(log2(1.0 / (scale * _vertex_density)));
	scale = pow(2.0, s) * 0.5;
	vec2 d_uv = (scale * pos - round(_target_pos.xz * _vertex_density * scale)) / (_mesh_size * 2.0) + 0.5;
	d_uv.x += s - 1.;
	d_uv.x /= log2(_subdiv);
	highp vec3 disp = vec3(0.);
	if (all(greaterThanEqual(d_uv, vec2(0.0))) && all(lessThanEqual(d_uv, vec2(1.0)))) {
		disp = textureLod(_displacement_buffer, d_uv, 0.).rgb * 2.0 - 1.0;
		disp *= _displacement_scale;
	}
	return disp;
}
#endif
