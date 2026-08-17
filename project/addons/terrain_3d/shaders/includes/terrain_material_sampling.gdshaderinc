// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// T3D_STANDALONE_DEFAULTS_BEGIN
// ShaderInclude resources are parsed independently by the editor. Runtime-generated
// shaders inject the selected values before these guards, so local defaults only
// apply when this module is parsed without its Terrain3D parent shader.
#ifndef T3D_TERRAIN_SAMPLING_MODE
#define T3D_TERRAIN_SAMPLING_MODE 0
#endif
// T3D_STANDALONE_DEFAULTS_END


#ifdef T3D_ENABLE_TERRAIN_MATERIAL_SAMPLING
#if T3D_TERRAIN_SAMPLING_MODE == 0
	if (coherent_pair) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], coherent_pair_weights, texture_ids[3], w_normal, terrain_height, mat);
	} else if (uniform_control) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], t_weights[3], texture_ids[3], w_normal, terrain_height, mat);
	} else {
		accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
			control[3], t_weights[3], texture_ids[3], index_normal[3], h[3], mat);
		if (bilerp) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
				control[2], t_weights[2], texture_ids[2], index_normal[2], h[2], mat);
			accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
				control[1], t_weights[1], texture_ids[1], index_normal[1], h[1], mat);
			accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
				control[0], t_weights[0], texture_ids[0], index_normal[0], h[0], mat);
		}
	}
#elif T3D_TERRAIN_SAMPLING_MODE == 1
	// Cinematic retains exact four-corner fallback only for genuinely different
	// material pairs/transforms. Smooth blend-only transitions use two layer reads.
	if (coherent_pair) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], coherent_pair_weights, texture_ids[3], w_normal, terrain_height, mat);
	} else {
	accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
		control[3], t_weights[3], texture_ids[3], index_normal[3], h[3], mat);
	accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
		control[2], t_weights[2], texture_ids[2], index_normal[2], h[2], mat);
	accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
		control[1], t_weights[1], texture_ids[1], index_normal[1], h[1], mat);
	accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
		control[0], t_weights[0], texture_ids[0], index_normal[0], h[0], mat);
	}
#elif T3D_TERRAIN_SAMPLING_MODE == 3
	// Adaptive weighted: preserve the adaptive distance/uniform-control behavior,
	// but avoid full material evaluation for corners below 2% bilinear influence.
	if (coherent_pair) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], coherent_pair_weights, texture_ids[3], w_normal, terrain_height, mat);
	} else if (uniform_control || !bilerp) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], t_weights[3], texture_ids[3], w_normal, terrain_height, mat);
	} else {
		if (weights[3] >= 0.02) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
				control[3], t_weights[3], texture_ids[3], index_normal[3], h[3], mat);
		}
		if (weights[2] >= 0.02) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
				control[2], t_weights[2], texture_ids[2], index_normal[2], h[2], mat);
		}
		if (weights[1] >= 0.02) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
				control[1], t_weights[1], texture_ids[1], index_normal[1], h[1], mat);
		}
		if (weights[0] >= 0.02) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
				control[0], t_weights[0], texture_ids[0], index_normal[0], h[0], mat);
		}
	}
#elif T3D_TERRAIN_SAMPLING_MODE == 4
	// Distance adaptive: exact four-corner material evaluation near the camera,
	// contribution-pruned four-corner evaluation in the middle, and one sample
	// once the control footprint no longer benefits from bilinear transitions.
	if (coherent_pair) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], coherent_pair_weights, texture_ids[3], w_normal, terrain_height, mat);
	} else if (uniform_control || !bilerp) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], t_weights[3], texture_ids[3], w_normal, terrain_height, mat);
	} else if (region_mip < 1.5) {
		accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
			control[3], t_weights[3], texture_ids[3], index_normal[3], h[3], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
			control[2], t_weights[2], texture_ids[2], index_normal[2], h[2], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
			control[1], t_weights[1], texture_ids[1], index_normal[1], h[1], mat);
		accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
			control[0], t_weights[0], texture_ids[0], index_normal[0], h[0], mat);
	} else {
		if (weights[3] >= 0.04) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
				control[3], t_weights[3], texture_ids[3], index_normal[3], h[3], mat);
		}
		if (weights[2] >= 0.04) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
				control[2], t_weights[2], texture_ids[2], index_normal[2], h[2], mat);
		}
		if (weights[1] >= 0.04) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
				control[1], t_weights[1], texture_ids[1], index_normal[1], h[1], mat);
		}
		if (weights[0] >= 0.04) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
				control[0], t_weights[0], texture_ids[0], index_normal[0], h[0], mat);
		}
	}
#elif T3D_TERRAIN_SAMPLING_MODE == 5
	// Adaptive two-corner: evaluate only the two strongest bilinear contributors
	// on transition pixels. Ties may evaluate more than two corners to avoid
	// directional seams at exact 50/50 or four-way intersections.
	if (coherent_pair) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], coherent_pair_weights, texture_ids[3], w_normal, terrain_height, mat);
	} else if (uniform_control || !bilerp) {
		float terrain_height = dot(vec4(h[0], h[1], h[2], h[3]), weights);
		accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
			control[3], t_weights[3], texture_ids[3], w_normal, terrain_height, mat);
	} else {
		float pair_hi_01 = max(weights[0], weights[1]);
		float pair_lo_01 = min(weights[0], weights[1]);
		float pair_hi_23 = max(weights[2], weights[3]);
		float pair_lo_23 = min(weights[2], weights[3]);
		float second_weight = max(min(pair_hi_01, pair_hi_23), max(pair_lo_01, pair_lo_23));
		float threshold = max(second_weight, 0.00001);
		if (weights[3] >= threshold) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[3], far_factor, index[3], index_id + offsets.xx,
				control[3], t_weights[3], texture_ids[3], index_normal[3], h[3], mat);
		}
		if (weights[2] >= threshold) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[2], far_factor, index[2], index_id + offsets.yx,
				control[2], t_weights[2], texture_ids[2], index_normal[2], h[2], mat);
		}
		if (weights[1] >= threshold) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[1], far_factor, index[1], index_id + offsets.yy,
				control[1], t_weights[1], texture_ids[1], index_normal[1], h[1], mat);
		}
		if (weights[0] >= threshold) {
			accumulate_material(base_ddx, base_ddy, TNB, weights[0], far_factor, index[0], index_id + offsets.xy,
				control[0], t_weights[0], texture_ids[0], index_normal[0], h[0], mat);
		}
	}
#else
	// Static fast path: one control and one terrain material evaluation.
	accumulate_material(base_ddx, base_ddy, TNB, 1.0, far_factor, index[3], uv,
		control[3], t_weights[3], texture_ids[3], index_normal[3], h[3], mat);
#endif
#endif
