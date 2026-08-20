// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_CLIPMAP_BATCHING_FUNCTIONS
uniform vec4 _clipmap_lod_states[16];

vec4 terrain3d_clipmap_lod_state(float lod_code) {
	return _clipmap_lod_states[clamp(int(round(lod_code)), 0, 15)];
}

vec2 terrain3d_clipmap_stitch_state(float packed_code) {
	int packed = clamp(int(round(packed_code)), 0, 8);
	return vec2(float(packed % 3), float(packed / 3));
}

float terrain3d_clipmap_edge_axis_offset(int edge_index, int test_axis) {
	if (edge_index == 0) {
		return test_axis == 2 ? -(4.0 * _mesh_size + 6.0) : 0.0;
	}
	if (test_axis == 0) {
		return 4.0 * _mesh_size + 8.0;
	}
	return test_axis == 2 ? -2.0 : 0.0;
}

vec2 terrain3d_clipmap_edge_offset(float part_code, vec2 stitch_state) {
	int part = int(round(part_code));
	if (part == 0) {
		return vec2(0.0);
	}
	int test_x = clamp(int(round(stitch_state.x)), 0, 2);
	int test_z = clamp(int(round(stitch_state.y)), 0, 2);
	if (part == 1) {
		return vec2(terrain3d_clipmap_edge_axis_offset(0, test_x), -2.0 * float(test_z - 1));
	}
	if (part == 2) {
		return vec2(terrain3d_clipmap_edge_axis_offset(1, test_x), -2.0 * float(test_z - 1));
	}
	if (part == 3) {
		return vec2(0.0, terrain3d_clipmap_edge_axis_offset(0, test_z));
	}
	if (part == 4) {
		return vec2(0.0, terrain3d_clipmap_edge_axis_offset(1, test_z));
	}
	return vec2(0.0);
}

vec3 terrain3d_clipmap_morph(const vec2 world_xz, const vec2 grid_xz, const float scale) {
	float inv_scale = 1.0 / scale;
	float max_xz = max(abs(world_xz.x - _target_pos.x), abs(world_xz.y - _target_pos.z));
	float morph = smoothstep(0.0, 1.0, (max_xz * inv_scale - _mesh_size - 4.0) / (_mesh_size - 4.0));
	vec2 vertex_fract = fract(grid_xz * 0.5) * 2.0;
	vec2 shift;

	if (scale < _vertex_spacing / _subdiv + 1e-6) {
		float alternate = round(fract(
			round(mod(world_xz.y * inv_scale, 4.0)) *
			round(mod(world_xz.x * inv_scale, 4.0)) * 0.25));
		shift = mix(vertex_fract, vec2(vertex_fract.x, -vertex_fract.y), alternate);
	} else {
		shift = vertex_fract * round((fract(world_xz * 0.25 * inv_scale) - 0.5) * 4.0);
	}

	return vec3(shift, morph);
}

#endif
