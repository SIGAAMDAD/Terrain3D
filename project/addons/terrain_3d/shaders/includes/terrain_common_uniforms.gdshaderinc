// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Geometry/page-table state shared by near, middle, far, and shadow shaders.
uniform vec3 _target_pos = vec3(0.0);
uniform float _mesh_size = 48.0;
uniform float _subdiv = 1.0;
uniform float _vertex_spacing = 1.0;
uniform float _vertex_density = 1.0;
const int T3D_REGION_SIZE = 128;
const int T3D_REGION_SHIFT = 7;
const int T3D_REGION_MASK = 127;
const float T3D_REGION_SIZE_F = 128.0;
const float T3D_REGION_INV_SIZE = 0.0078125;
const int T3D_REGION_MAP_SIZE = 32;
const int T3D_REGION_MAP_HALF = 16;

ivec2 t3d_region_cell_from_grid(const ivec2 grid) {
	return ivec2(floor(vec2(grid) * T3D_REGION_INV_SIZE));
}

ivec2 t3d_region_local_from_grid(const ivec2 grid, const ivec2 region_cell) {
	return grid - region_cell * T3D_REGION_SIZE;
}
uniform vec2 _height_decode = vec2(-384.0, 1152.0);
uniform highp sampler2D _far_height_map : filter_linear, repeat_disable;
