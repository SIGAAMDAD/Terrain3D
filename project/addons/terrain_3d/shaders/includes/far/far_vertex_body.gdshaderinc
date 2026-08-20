// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	float scale = clipmap_lod_state.z;
	vec3 clipmap_morph = terrain3d_clipmap_morph(v_vertex.xz, clipmap_grid_vertex.xz, scale);
	vec2 shift_world = clipmap_morph.xy * scale;
	v_vertex.xz -= shift_world * clipmap_morph.z;

	UV = v_vertex.xz * _vertex_density;
	UV2 = fma(UV, vec2(_region_texel_size), vec2(0.5 * _region_texel_size));

	bool hidden = false
#include "res://addons/terrain_3d/shaders/includes/none_check.gdshaderinc"
	;
	if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && hidden) {
		v_vertex.x = 0.0 / 0.0;
#if T3D_TERRAIN_NORMAL_MODE != 2
		v_far_normal = vec3(0.0, 1.0, 0.0);
#endif
	} else {
		float h = sample_far_height(UV);
#include "res://addons/terrain_3d/shaders/includes/flat_vertex.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_vertex.gdshaderinc"
		v_vertex.y = h;
#if T3D_TERRAIN_NORMAL_MODE != 2
		float normal_step = max(1.0, scale * _vertex_density);
		v_far_normal = sample_far_normal_from_height(UV, normal_step, h);
#endif
	}

	VERTEX = (VIEW_MATRIX * vec4(v_vertex, 1.0)).xyz;
