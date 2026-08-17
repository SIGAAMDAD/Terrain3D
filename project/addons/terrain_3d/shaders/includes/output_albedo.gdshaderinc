// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_OUTPUT_ALBEDO
	ALBEDO = mat.albedo_height.rgb * mix(vec3(1.0), color_map.rgb, _tier_authored_color_contribution);
#endif
