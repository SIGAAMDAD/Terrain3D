// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	// Geomorph vertex across clipmap LODs, set end and start for linear height interpolate
	float scale = clipmap_lod_state.z;
	vec3 clipmap_morph = terrain3d_clipmap_morph(v_vertex.xz, clipmap_grid_vertex.xz, scale);
	vec2 shift_world = clipmap_morph.xy * scale;
	float vertex_lerp = clipmap_morph.z;
	vec2 start_pos = v_vertex.xz * _vertex_density;
	vec2 end_pos = (v_vertex.xz - shift_world) * _vertex_density;
	v_vertex.xz -= shift_world * vertex_lerp;

	// UV coordinates in region space. 0-1 covers 1 region, 1-2 is the next region, etc.
	UV = v_vertex.xz * _vertex_density;

	// UV coordinates in region space + texel offset. Values are 0 to 1 within regions
	UV2 = fma(UV, vec2(T3D_REGION_INV_SIZE), vec2(0.5 * T3D_REGION_INV_SIZE));

	ivec3 v_region = get_index_coord(start_pos);
	uint control = read_terrain_control(v_region);
	bool hole = DECODE_HOLE(control);

	vec3 displacement = vec3(0.0);
	// Show holes to all cameras except mouse camera (on exactly 1 layer)
	if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && (hole
#include "res://addons/terrain_3d/shaders/includes/none_check.gdshaderinc"
	)) {
		v_vertex.x = 0.0 / 0.0;
	} else {
		float h;
		// This branch is static for each of the clipmap segments.
		if (scale < _vertex_spacing) {
			h = interpolated_height(UV);
		} else {
			ivec3 coord_b = get_index_coord(end_pos);
			h = mix(sample_terrain_height(v_region, start_pos),
				sample_terrain_height(coord_b, end_pos), vertex_lerp);
		}

#include "res://addons/terrain_3d/shaders/includes/flat_vertex.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_vertex.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/displacement_vertex.gdshaderinc"
		v_vertex.y = h;
	}


	// Convert model space to view space w/ skip_vertex_transform render mode
	// Include displacement without modifying v_vertex.
	VERTEX = (VIEW_MATRIX * vec4(v_vertex + displacement, 1.0)).xyz;
