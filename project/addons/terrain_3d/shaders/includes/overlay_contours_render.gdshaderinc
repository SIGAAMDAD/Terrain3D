// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_OVERLAY_CONTOURS_RENDER
	// Show contour lines
	{
		vec3 __pixel_pos = (INV_VIEW_MATRIX * vec4(VERTEX,1.0)).xyz;
		vec3 __camera_pos = INV_VIEW_MATRIX[3].xyz;
		vec3 __base_ddx = dFdxCoarse(__pixel_pos);
		vec3 __base_ddy = dFdyCoarse(__pixel_pos);
		vec3 __w_normal = normalize(cross(__base_ddy, __base_ddx));
		float __line = fractal_contour_lines(contour_thickness, contour_interval, __pixel_pos, __w_normal, __base_ddx, __base_ddy, __camera_pos);
		ALBEDO = mix(ALBEDO, contour_color.rgb, (1.-__line) * contour_color.a);
	}
#endif
