// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_CONTROL_SCALE
	// Show control map texture scale
	{
		ivec3 __suv = get_index_coord(floor(uv));
		uint __s_control = read_terrain_control(__suv);
		uint __scale = (__s_control >>7u & 0x7u);
		vec3 __s_colors[8] = vec3[8](
			vec3(.5, .5, .5), vec3(.675, .25, .375), vec3(.75, .125, .25), vec3(.875, .0, .125), vec3(1., 0., 0.),
			vec3(0., 0., 1.), vec3(.0, .166, .833), vec3(.166, .333, .666)
		);
		ALBEDO = __s_colors[__scale];
		ROUGHNESS = 1.;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5 ,0.5 ,1.0);
		AO = 1.0;
	}
#endif
