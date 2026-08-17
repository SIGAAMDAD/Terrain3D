// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_DISPLACEMENT_BUFFER
	// Show displacement buffer
	#ifdef DISPLACEMENT
	{
		float scale = MODEL_MATRIX[0][0];
		float vertex_lerp = smoothstep(0.55, 0.95, (v_vertex_xz_dist / scale - _mesh_size - 4.0) / (_mesh_size - 2.0));
		float disp = dot(w_normal, mix(get_displacement(uv, scale), get_displacement(uv, scale * 2.0), vertex_lerp));
		// Red values lower than collision, Green values above. Black if no deviation.
		ALBEDO = vec3(-disp, disp, 0.);
		ROUGHNESS = 0.7;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
	#endif
#endif
