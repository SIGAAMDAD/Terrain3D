// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_AUTOSHADER
	// Show where autoshader enabled
	{
		ivec3 __ruv = get_index_coord(floor(uv));
		uint __control = read_terrain_control(__ruv);
		float __autoshader = float( bool(__control & 0x1u) || __ruv.z<0 );
		ALBEDO = vec3(__autoshader);
		ROUGHNESS = 1.;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
