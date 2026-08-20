// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	// Static fast path: one control and one terrain material evaluation.
	accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
		control[3], corner_material_weights[3], corner_material_ids[3], index_normal[3], h[3], mat);
