// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	// Distance adaptive: exact four-corner material evaluation near the camera,
	// contribution-pruned four-corner evaluation in the middle, and one sample
	// once the control footprint no longer benefits from bilinear transitions.
	if (coherent_pair) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], coherent_pair_weights, corner_material_ids[3], w_normal, terrain_height, mat);
	} else if (uniform_control || !bilerp) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], corner_material_weights[3], corner_material_ids[3], w_normal, terrain_height, mat);
	} else if (region_mip < 1.5) {
		accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
			control[3], corner_material_weights[3], corner_material_ids[3], index_normal[3], h[3], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
			control[2], corner_material_weights[2], corner_material_ids[2], index_normal[2], h[2], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
			control[1], corner_material_weights[1], corner_material_ids[1], index_normal[1], h[1], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
			control[0], corner_material_weights[0], corner_material_ids[0], index_normal[0], h[0], mat);
	} else {
		if (weights[3] >= 0.04) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
				control[3], corner_material_weights[3], corner_material_ids[3], index_normal[3], h[3], mat);
		}
		if (weights[2] >= 0.04) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
				control[2], corner_material_weights[2], corner_material_ids[2], index_normal[2], h[2], mat);
		}
		if (weights[1] >= 0.04) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
				control[1], corner_material_weights[1], corner_material_ids[1], index_normal[1], h[1], mat);
		}
		if (weights[0] >= 0.04) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
				control[0], corner_material_weights[0], corner_material_ids[0], index_normal[0], h[0], mat);
		}
	}
