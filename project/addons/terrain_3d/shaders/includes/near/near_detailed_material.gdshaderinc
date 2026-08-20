// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	// Resolve the control data required by the selected static sampling mode.
	uvec4 control = uvec4(read_terrain_control(index[3]));
#if T3D_TERRAIN_SAMPLING_MODE == 0 || T3D_TERRAIN_SAMPLING_MODE == 3 || T3D_TERRAIN_SAMPLING_MODE == 4 || T3D_TERRAIN_SAMPLING_MODE == 5
	if (bilerp) {
		control = uvec4(
			read_terrain_control(index[0]),
			read_terrain_control(index[1]),
			read_terrain_control(index[2]),
			control[3]);
	}
#elif T3D_TERRAIN_SAMPLING_MODE == 1
	control = uvec4(
		read_terrain_control(index[0]),
		read_terrain_control(index[1]),
		read_terrain_control(index[2]),
		control[3]);
#endif

#include "res://addons/terrain_3d/shaders/includes/auto_shader.gdshaderinc"

	// Cooked controls use canonical material-pair order for coherent array access.
	ivec4 material_ids_by_slot[2] = {
		ivec4(control >> uvec4(27u) & uvec4(0x1Fu)),
		ivec4(control >> uvec4(22u) & uvec4(0x1Fu)),
	};
	bool all_resident = all(greaterThanEqual(
		ivec4(index[0].z, index[1].z, index[2].z, index[3].z), ivec4(0)));
	bool same_base_layer = all(equal(material_ids_by_slot[0], material_ids_by_slot[0].xxxx));
	bool same_overlay_layer = all(equal(material_ids_by_slot[1], material_ids_by_slot[1].xxxx));
	uvec4 non_material = control & uvec4(CONTROL_NON_MATERIAL_MASK);
	bool same_transform = all(equal(non_material, non_material.xxxx));
	bool uniform_control = bilerp && all_resident && all(equal(control, control.xxxx));
	bool coherent_pair = bilerp && all_resident && same_base_layer && same_overlay_layer && same_transform;

	ivec2 corner_material_ids[4] = ivec2[4](
		ivec2(material_ids_by_slot[0].x, material_ids_by_slot[1].x),
		ivec2(material_ids_by_slot[0].y, material_ids_by_slot[1].y),
		ivec2(material_ids_by_slot[0].z, material_ids_by_slot[1].z),
		ivec2(material_ids_by_slot[0].w, material_ids_by_slot[1].w));

	// Base/overlay weights are initially local to each corner.
	vec4 overlay_weights = vec4(control >> uvec4(14u) & uvec4(0xFFu)) * DIV_255;
	vec4 base_weights = 1.0 - overlay_weights;
	vec2 corner_material_weights[4] = vec2[4](
		vec2(base_weights[0], overlay_weights[0]),
		vec2(base_weights[1], overlay_weights[1]),
		vec2(base_weights[2], overlay_weights[2]),
		vec2(base_weights[3], overlay_weights[3]));

	// Interpolate controls only for modes that can use multi-corner sampling.
	vec2 coherent_pair_weights = corner_material_weights[3];
#if T3D_TERRAIN_SAMPLING_MODE != 2
	if (coherent_pair) {
		float overlay_weight = dot(overlay_weights, weights);
		coherent_pair_weights = vec2(1.0 - overlay_weight, overlay_weight);
	} else if (bilerp && !uniform_control) {
		corner_material_weights = {vec2(0.0), vec2(0.0), vec2(0.0), vec2(0.0)};
		base_weights *= weights;
		overlay_weights *= weights;
		for (int i = 0; i < 4; i++) {
			vec2 base_weight = vec2(base_weights[i]);
			vec2 overlay_weight = vec2(overlay_weights[i]);
			ivec2 base_id = corner_material_ids[i].xx;
			ivec2 overlay_id = corner_material_ids[i].yy;
			corner_material_weights[0] += fma(
				base_weight, vec2(equal(corner_material_ids[0], base_id)),
				overlay_weight * vec2(equal(corner_material_ids[0], overlay_id)));
			corner_material_weights[1] += fma(
				base_weight, vec2(equal(corner_material_ids[1], base_id)),
				overlay_weight * vec2(equal(corner_material_ids[1], overlay_id)));
			corner_material_weights[2] += fma(
				base_weight, vec2(equal(corner_material_ids[2], base_id)),
				overlay_weight * vec2(equal(corner_material_ids[2], overlay_id)));
			corner_material_weights[3] += fma(
				base_weight, vec2(equal(corner_material_ids[3], base_id)),
				overlay_weight * vec2(equal(corner_material_ids[3], overlay_id)));
		}
	}
#endif

	float far_factor = 0.0;
#include "res://addons/terrain_3d/shaders/includes/dual_scaling_factor.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_material_sampling.gdshaderinc"
