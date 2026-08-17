// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_EDITOR_REGION_GRID
	// Show region grid
	{
		vec3 __boundary_color = pow(vec3(0.095, 0.328, 0.56), vec3(2.2)); // Medium dark blue, hue 210, converted to linear
		vec3 __active_color = vec3(1.0);
		vec3 __inactive_color = vec3(0.1);
		float __line_thickness = 0.05 * sqrt(-VERTEX.z);
		vec3 __pixel_pos = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz * _vertex_density;
		vec2 __p = __pixel_pos.xz;
		// Region Grid
		vec2 __g = abs(fract((__p + _region_size * 0.5) / _region_size) - 0.5) * _region_size;
		float __grid_d = min(__g.x, __g.y);
		float __grid_mask = 1.0 - smoothstep(__line_thickness - fwidth(__grid_d), __line_thickness + fwidth(__grid_d), __grid_d);
		// Region Map Boundry
		float __hmap = _region_size * 16.0;
		vec2 __bp = abs(__p) - __hmap;
		float __box_d = abs(max(__bp.x, __bp.y));
		float __box_mask = 1.0 - smoothstep( __line_thickness - fwidth(__box_d), __line_thickness + fwidth(__box_d), __box_d);
		// Clip Grid at Boundary
		float __b_line = __hmap - __line_thickness - fwidth(__box_d);
		__grid_mask *= step(abs(__p.x), __b_line) * step(abs(__p.y), __b_line);
		vec3 __grid_color = mix(__inactive_color, __active_color, float(clamp(get_index_coord(__pixel_pos.xz - 0.5).z + 1, 0, 1)));
		ALBEDO = mix(ALBEDO, __grid_mask * __grid_color + __box_mask * __boundary_color, max(__grid_mask, __box_mask));
	}
#endif
