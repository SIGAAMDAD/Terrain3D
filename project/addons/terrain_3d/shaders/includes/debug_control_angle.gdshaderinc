// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_CONTROL_ANGLE
	// Show control map texture angle
	{
		ivec3 __auv = get_index_coord(floor(uv));
		uint __a_control = read_terrain_control(__auv);
		uint __angle = (__a_control >>10u & 0xFu);
		vec3 __a_colors[16] = {
			vec3(1., .2, .0), vec3(.8, 0., .2), vec3(.6, .0, .4), vec3(.4, .0, .6),
			vec3(.2, 0., .8), vec3(.1, .1, .8), vec3(0., .2, .8), vec3(0., .4, .6),
			vec3(0., .6, .4), vec3(0., .8, .2), vec3(0., 1., 0.), vec3(.2, 1., 0.),
			vec3(.4, 1., 0.), vec3(.6, 1., 0.), vec3(.8, .6, 0.), vec3(1., .4, 0.)
		};
		ALBEDO = __a_colors[__angle];
		ROUGHNESS = 1.;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
