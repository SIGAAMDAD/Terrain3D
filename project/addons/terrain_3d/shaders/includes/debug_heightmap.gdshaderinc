// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_HEIGHTMAP
	// Show heightmap
	{
		float __lo = min(heightmap_black_height, heightmap_white_height);
		float __hi = max(heightmap_black_height, heightmap_white_height);
		float __factor = clamp((v_vertex.y - __lo) / max(__hi - __lo, 1e-6), 0.0, 1.0);
		__factor = mix(__factor, 1.0 - __factor, float(heightmap_white_height < heightmap_black_height));
		ALBEDO = vec3(smoothstep(0.0, 1.0, __factor));
		ROUGHNESS = 0.7;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
