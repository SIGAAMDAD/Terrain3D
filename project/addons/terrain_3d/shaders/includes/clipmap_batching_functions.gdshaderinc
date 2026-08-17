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
#endif
