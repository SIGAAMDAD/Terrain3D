// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	float scale = clipmap_lod_state.z;
	vec3 clipmap_morph = terrain3d_clipmap_morph(v_vertex.xz, clipmap_grid_vertex.xz, scale);
	vec2 shift_world = clipmap_morph.xy * scale;
	float vertex_lerp = clipmap_morph.z;
	vec2 start_pos = v_vertex.xz * _vertex_density;
	vec2 end_pos = (v_vertex.xz - shift_world) * _vertex_density;
	v_vertex.xz -= shift_world * vertex_lerp;

	UV = v_vertex.xz * _vertex_density;
	UV2 = fma(UV, vec2(_region_texel_size), vec2(0.5 * _region_texel_size));

	ivec3 region = get_index_coord(start_pos);
	uint control = read_terrain_control(region);
	bool hole = DECODE_HOLE(control);
	if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && (hole
#include "res://addons/terrain_3d/shaders/includes/none_check.gdshaderinc"
	)) {
		v_vertex.x = 0.0 / 0.0;
		v_middle_normal = vec3(0.0, 1.0, 0.0);
	} else {
		float h;
		if (scale < _vertex_spacing) {
			h = interpolated_height(UV);
		} else {
			ivec3 coord_b = get_index_coord(end_pos);
			h = mix(sample_terrain_height(region, start_pos), sample_terrain_height(coord_b, end_pos), vertex_lerp);
		}
#include "res://addons/terrain_3d/shaders/includes/flat_vertex.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_vertex.gdshaderinc"
		v_vertex.y = h;

		vec3 region_uv = get_index_uv(UV2);
#if T3D_TERRAIN_NORMAL_MODE == 2
		v_middle_normal = vec3(0.0, 1.0, 0.0);
#else
		if (region_uv.z >= 0.0) {
			vec2 encoded_xz = textureLod(_derived_maps, region_uv, 0.0).rg;
			vec2 normal_xz = encoded_xz * 2.0 - 1.0;
			float normal_y = sqrt(max(1.0 - dot(normal_xz, normal_xz), 0.0));
			v_middle_normal = normalize(vec3(normal_xz.x, normal_y, normal_xz.y));
		} else {
			float normal_step = max(1.0, scale * _vertex_density);
			v_middle_normal = sample_far_normal(UV, normal_step);
		}
#endif
	}
	VERTEX = (VIEW_MATRIX * vec4(v_vertex, 1.0)).xyz;
