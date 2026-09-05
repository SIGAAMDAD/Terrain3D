// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_FLAT_FRAGMENT
	float blend = get_region_blend(world_texel * T3D_REGION_INV_SIZE);
	height = mix(height, ground_level, smoothstep(0., 1., blend));
#endif
