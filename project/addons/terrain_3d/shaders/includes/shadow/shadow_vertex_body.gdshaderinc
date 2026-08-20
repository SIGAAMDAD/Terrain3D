// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	vec3 world_vertex = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float scale = clipmap_lod_state.z;
	vec3 clipmap_morph = terrain3d_clipmap_morph(world_vertex.xz, clipmap_grid_vertex.xz, scale);
	vec2 shift_world = clipmap_morph.xy * scale;
	world_vertex.xz -= shift_world * clipmap_morph.z;

	vec2 world_texel = world_vertex.xz * _vertex_density;
	world_vertex.y = _shadow_height_source == 0 ? shadow_page_height(world_texel, false) :
		(_shadow_height_source == 1 ? shadow_page_height(world_texel, true) : shadow_far_height(world_texel));
	VERTEX = (VIEW_MATRIX * vec4(world_vertex, 1.0)).xyz;
