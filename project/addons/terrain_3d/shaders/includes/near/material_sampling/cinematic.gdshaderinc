// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	// Cinematic retains exact four-corner fallback only for genuinely different
	// material pairs/transforms. Smooth blend-only transitions use two layer reads.
	if (coherent_pair) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], coherent_pair_weights, corner_material_ids[3], w_normal, terrain_height, mat);
	} else {
		accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
			control[3], corner_material_weights[3], corner_material_ids[3], index_normal[3], h[3], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
			control[2], corner_material_weights[2], corner_material_ids[2], index_normal[2], h[2], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
			control[1], corner_material_weights[1], corner_material_ids[1], index_normal[1], h[1], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
			control[0], corner_material_weights[0], corner_material_ids[0], index_normal[0], h[0], mat);
	}
