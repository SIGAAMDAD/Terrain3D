// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_LEGACY_PBR_DISPLACEMENT_FUNCTIONS
void sample_displacement_data(const vec2 uv, const int id, out float height, out vec3 normal_value) {
	if (!t3d_mat_resident(id)) {
		height = 0.5;
		normal_value = vec3(0.0, 1.0, 0.0);
		return;
	}
	height = textureLod(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), 0.0).a;
	vec4 normal_sample = textureLod(_texture_array_normal, vec3(uv, float(t3d_mat_layer(id))), 0.0);
	normal_value = normalize(fma(normal_sample.xzy, vec3(2.0), vec3(-1.0)));
}

float sample_displacement_height(const vec2 uv, const int id) {
	if (!t3d_mat_resident(id)) {
		return 0.5;
	}
	return textureLod(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), 0.0).a;
}
#endif
