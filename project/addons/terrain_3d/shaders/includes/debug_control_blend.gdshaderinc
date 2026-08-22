// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_CONTROL_BLEND
	// Show control map blend values
	{
		ivec3 __uv = get_index_coord(floor(uv));
		uint __control = read_terrain_control(__uv);
        float __ctrl_blend = float(__control >>14u & 0xFFu) * 0.003921568627450; // 1.0/255.0
		float __is_auto = 0.;
#if T3D_AUTO_SHADER_ENABLED
		__is_auto = float(bool(__control & 0x1u) || __uv.z < 0);
#endif
		ALBEDO = vec3(__ctrl_blend) * (1.0 - __is_auto) + vec3(0.2, 0.0, 0.0) * __is_auto;
		ROUGHNESS = 1.;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
