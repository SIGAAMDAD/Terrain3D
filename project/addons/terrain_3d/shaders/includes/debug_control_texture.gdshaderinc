// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_CONTROL_TEXTURE
	// Show control map texture selection
	{
		vec3 __t_colors[32];
		__t_colors[0] = vec3(1.0, 0.0, 0.0);
		__t_colors[1] = vec3(0.0, 1.0, 0.0);
		__t_colors[2] = vec3(0.0, 0.0, 1.0);
		__t_colors[3] = vec3(1.0, 0.0, 1.0);
		__t_colors[4] = vec3(0.0, 1.0, 1.0);
		__t_colors[5] = vec3(1.0, 1.0, 0.0);
		__t_colors[6] = vec3(0.2, 0.0, 0.0);
		__t_colors[7] = vec3(0.0, 0.2, 0.0);
		__t_colors[8] = vec3(0.0, 0.0, 0.35);
		__t_colors[9] = vec3(0.2, 0.0, 0.2);
		__t_colors[10] = vec3(0.0, 0.2, 0.2);
		__t_colors[11] = vec3(0.2, 0.2, 0.0);
		__t_colors[12] = vec3(0.1, 0.0, 0.0);
		__t_colors[13] = vec3(0.0, 0.1, 0.0);
		__t_colors[14] = vec3(0.0, 0.0, 0.15);
		__t_colors[15] = vec3(0.1, 0.0, 0.1);
		__t_colors[16] = vec3(0.0, 0.1, 0.1);
		__t_colors[17] = vec3(0.1, 0.1, 0.0);
		__t_colors[18] = vec3(0.2, 0.05, 0.05);
		__t_colors[19] = vec3(0.1, 0.3, 0.1);
		__t_colors[20] = vec3(0.05, 0.05, 0.2);
		__t_colors[21] = vec3(0.1, 0.05, 0.2);
		__t_colors[22] = vec3(0.05, 0.15, 0.2);
		__t_colors[23] = vec3(0.2, 0.2, 0.1);
		__t_colors[24] = vec3(1.0);
		__t_colors[25] = vec3(0.5);
		__t_colors[26] = vec3(0.35);
		__t_colors[27] = vec3(0.25);
		__t_colors[28] = vec3(0.15);
		__t_colors[29] = vec3(0.1);
		__t_colors[30] = vec3(0.05);
		__t_colors[31] = vec3(0.0125);
		ivec3 __uv = get_index_coord(floor(uv));
		uint __control = read_terrain_control(__uv);
		vec3 __ctrl_base = __t_colors[int(__control >>27u & 0x1Fu)];
		vec3 __ctrl_over = __t_colors[int(__control >>22u & 0x1Fu)];
		float __blend = float(__control >>14u & 0xFFu) * 0.003921568627450; // 1.0/255.0
		float base_over = (length(fract(uv) - 0.5) < fma(__blend, 0.45, 0.1) ? 1.0 : 0.0);
		ALBEDO = mix(__ctrl_base, __ctrl_over, base_over);
		ROUGHNESS = 1.0;
		SPECULAR = 0.0;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
