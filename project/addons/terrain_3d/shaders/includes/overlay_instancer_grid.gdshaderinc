// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_OVERLAY_INSTANCER_GRID
	// Show instancer grid
	{
		vec3 __grid_color = vec3(.05);
		float __line_thickness = 0.01 * sqrt(-VERTEX.z);
		vec3 __pixel_pos = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz * _vertex_density;
		vec2 __p = __pixel_pos.xz;
		// Instancer Grid
		vec2 __g = abs(fract((__p + _instancer_cell_size * 0.5) / _instancer_cell_size) - 0.5) * _instancer_cell_size;
		float __grid_d = min(__g.x, __g.y);
		float __grid_mask = 1.0 - smoothstep(__line_thickness - fwidth(__grid_d), __line_thickness + fwidth(__grid_d), __grid_d);
		// Clip Grid outside regions
		__grid_mask *= float(clamp(get_index_coord(__pixel_pos.xz - 0.5).z + 1, 0, 1));
		ALBEDO = mix(ALBEDO, __grid_mask * __grid_color, __grid_mask);
	}
#endif
