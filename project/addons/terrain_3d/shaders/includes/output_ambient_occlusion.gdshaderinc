// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_OUTPUT_AMBIENT_OCCLUSION
	AO = clamp(mat.ao, 0., 1.);
	AO_LIGHT_AFFECT = mat.ao_affect;
#endif
