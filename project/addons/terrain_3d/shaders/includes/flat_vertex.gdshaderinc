// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_FLAT_VERTEX
		// Apply background ground level and region blend, dont include texel offset
		h = mix(h, ground_level, smoothstep(0.0, 1.0, get_region_blend(UV * _region_texel_size)));
#endif
