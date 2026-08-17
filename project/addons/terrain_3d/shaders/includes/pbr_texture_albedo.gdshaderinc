// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_PBR_TEXTURE_ALBEDO
	// Show albedo textures
	{
		ALBEDO = mat.albedo_height.rgb;
		ROUGHNESS = 0.7;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
