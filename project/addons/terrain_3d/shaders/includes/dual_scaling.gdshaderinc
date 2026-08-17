// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DUAL_SCALING
	// dual scaling
	vec4 far_alb = vec4(0.);
	vec4 far_nrm = vec4(0.);
	float far_ao = 1.0;
	float far_metallic = 0.0;
	if (far_factor > 0. && any(equal(texture_id, ivec2(dual_scale_texture)))) {
		int far_texture_id = dual_scale_texture;
		float far_scale = _texture_uv_scale_array[far_texture_id] * dual_scale_reduction;
		if (index.z < 0) {
			far_scale *= tri_scale_reduction;
		}
		vec4 far_dd = i_dd * far_scale;

		// Detiling and Control map rotation
		vec2 uv_center = floor(fma(i_pos, vec2(far_scale), vec2(0.5)));
		vec3 far_detile = get_detile_transform(uv_center, far_texture_id);
		vec2 far_cs_angle = far_detile.xy;
		// Apply UV rotation and shift around pivot.
		vec2 far_uv = rotate_vec2(fma(i_uv, vec2(far_scale), -uv_center), far_cs_angle) + uv_center + far_detile.z - 0.5;
		// Manual transpose to rotate derivatives and normals counter to uv rotation whilst also
		// including control map rotation. avoids extra matrix op, and sin/cos calls.
		far_cs_angle = vec2(
			fma(far_cs_angle.x, c_cs_angle.x, -far_cs_angle.y * c_cs_angle.y),
			fma(far_cs_angle.y, c_cs_angle.x, far_cs_angle.x * c_cs_angle.y));
		// Align derivatives for correct anisotropic filtering
		far_dd.xy = rotate_vec2(far_dd.xy, far_cs_angle);
		far_dd.zw = rotate_vec2(far_dd.zw, far_cs_angle);

		sample_texture_data(far_uv, far_texture_id, far_dd.xy, far_dd.zw, far_alb, far_nrm, far_ao, far_metallic);
		far_nrm.xz = rotate_vec2(far_nrm.xz, far_cs_angle) * p_align;

		// apply weighting when far_factor == 1.0 as the later lookup will be skipped.
		if (far_factor == 1.0) {
			float id_w = texture_id[0] == dual_scale_texture ? texture_weight[0] : texture_weight[1];
			float id_weight = terrain_blend_power(weight + id_w + far_alb.a, sharpness) * weight;
			mat.albedo_height = fma(far_alb, vec4(id_weight), mat.albedo_height);
			mat.normal_rough = fma(far_nrm, vec4(id_weight), mat.normal_rough);
			mat.normal_map_depth = fma(_texture_normal_depth_array[far_texture_id], id_weight, mat.normal_map_depth);
			mat.ao = fma(far_ao, id_weight, mat.ao);
			mat.ao_affect = fma(_texture_ao_affect_array[far_texture_id], id_weight, mat.ao_affect);
			mat.metallic = fma(far_metallic, id_weight, mat.metallic);
			mat.total_weight += id_weight;
		}
	}
#endif
