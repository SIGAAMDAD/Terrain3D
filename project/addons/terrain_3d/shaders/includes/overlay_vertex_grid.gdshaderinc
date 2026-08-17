// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_OVERLAY_VERTEX_GRID
	// Show vertex grids
	{
		vec3 __pixel_pos = (INV_VIEW_MATRIX * vec4(VERTEX,1.0)).xyz;
		vec3 __camera_pos = INV_VIEW_MATRIX[3].xyz;
		float __grid_line = 0.05;		// Vertex grid line thickness
		float __grid_step = 1.0;			// Vertex grid size, 1.0 == integer units
		float __vertex_size = 4.;		// Size of vertices
		float __view_distance = 300.0;	// Visible distance of grid
		vec3 __vertex_mul = vec3(0.);
		vec3 __vertex_add = vec3(0.);
		float __distance_factor = clamp(1.-length(__camera_pos - __pixel_pos)/__view_distance, 0., 1.);
		// Draw vertex grid
		if ( mod(__pixel_pos.x * _vertex_density + __grid_line*.5, __grid_step) < __grid_line ||
			mod(__pixel_pos.z * _vertex_density + __grid_line*.5, __grid_step) < __grid_line ) {
			__vertex_mul = vec3(0.5) * __distance_factor;
		}
		// Draw Vertices
		if ( mod(UV.x + __grid_line*__vertex_size*.5, __grid_step) < __grid_line*__vertex_size &&
			mod(UV.y + __grid_line*__vertex_size*.5, __grid_step) < __grid_line*__vertex_size ) {
			__vertex_add = vec3(0.15) * __distance_factor;
		}
		ALBEDO = fma(ALBEDO, 1.-__vertex_mul, __vertex_add);
	}
#endif
