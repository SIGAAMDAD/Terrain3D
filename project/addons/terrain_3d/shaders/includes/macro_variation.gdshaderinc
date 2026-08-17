// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_MACRO_VARIATION
	// Macro variation. 2 lookups
	{
		vec2 noise1_uv = fma(uv, vec2(noise1_scale * .1), noise1_offset);
		vec2 noise1_rotation = _noise1_rotation;
		vec2 rotated_noise1_uv = rotate_vec2(noise1_uv, noise1_rotation);
		float noise1 = texture(noise_texture, rotated_noise1_uv).r;
		float noise2 = texture(noise_texture, uv * noise2_scale * .1).r;
		vec3 macrov = mix(macro_variation1, vec3(1.), noise1);
		macrov *= mix(macro_variation2, vec3(1.), noise2);
		mat.albedo_height.rgb *= mix(vec3(1.0), macrov, clamp(w_normal.y + macro_variation_slope, 0., 1.));
	}
#endif
