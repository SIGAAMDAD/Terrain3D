// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_PBR_TEXTURE_NORMAL
	// Show normal map textures
	{
		ALBEDO = fma(normalize(mat.normal_rough.xzy), vec3(0.5), vec3(0.5));
		ROUGHNESS = 0.7;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
