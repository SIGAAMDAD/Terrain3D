// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_CHECKERED
	// Show a checkered grid
	{
		vec2 __p = uv * 1.0; // scale
		vec2 __ddx = dFdx(__p);
		vec2 __ddy = dFdy(__p);
		vec2 __w = max(abs(__ddx), abs(__ddy)) + 0.01;
		vec2 __i = 2.0 * (abs(fract((__p - 0.5 * __w) / 2.0) - 0.5) - abs(fract((__p + 0.5 * __w) / 2.0) - 0.5)) / __w;
		ALBEDO = vec3((0.5 - 0.5 * __i.x * __i.y) * 0.2 + 0.2);
		ROUGHNESS = 0.7;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
