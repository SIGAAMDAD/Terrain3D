// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_EDITOR_NAVIGATION
	// Show navigation
	{
		uint __control = read_terrain_control(get_index_coord(floor(uv + 0.5)));
		if (bool(__control >> 1u & 0x1u)) {
			ALBEDO *= vec3(.5, .0, .85);
		}
	}
#endif
