// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_SURFACE_PBR_DISPLACEMENT_FUNCTIONS
void sample_displacement_data(const vec2 uv, const int id, out float height, out vec3 normal_value) {
	if (!t3d_mat_resident(id)) {
		height = 0.5;
		normal_value = vec3(0.0, 1.0, 0.0);
		return;
	}
	height = textureLod(_texture_array_displacement, vec3(uv, float(t3d_mat_layer(id))), 0.0).r;
	vec2 encoded_normal = textureLod(_texture_array_surface, vec3(uv, float(t3d_mat_layer(id))), 0.0).rg;
	normal_value = decode_surface_normal(encoded_normal);
}

float sample_displacement_height(const vec2 uv, const int id) {
	if (!t3d_mat_resident(id)) {
		return 0.5;
	}
	return textureLod(_texture_array_displacement, vec3(uv, float(t3d_mat_layer(id))), 0.0).r;
}
#endif
