// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// T3D_STANDALONE_DEFAULTS_BEGIN
// ShaderInclude resources are parsed independently by the editor. Runtime-generated
// shaders inject the selected values before these guards, so local defaults only
// apply when this module is parsed without its Terrain3D parent shader.
#ifndef T3D_WORLD_NOISE_SHADER_MODE
#define T3D_WORLD_NOISE_SHADER_MODE 0
#endif
// T3D_STANDALONE_DEFAULTS_END


#ifdef T3D_ENABLE_WORLD_NOISE_FRAGMENT
	// Static modes remove this branch entirely. Fragment-normal modes pay three
	// height evaluations; vertex-normal modes consume the interpolated varying.
#if T3D_WORLD_NOISE_SHADER_MODE == 0
	if (world_noise_fragment_normals) {
		float noise_region_weight = get_noise_region_blend(uv2);
		float noise_height = get_noise_height_with_region_weight(uv2, noise_region_weight);
		u += noise_height - get_noise_height_with_region_weight(uv2 + vec2(T3D_REGION_INV_SIZE, 0.0), noise_region_weight);
		v += noise_height - get_noise_height_with_region_weight(uv2 + vec2(0.0, T3D_REGION_INV_SIZE), noise_region_weight);
	} else {
		u += world_noise_ddxy.x;
		v += world_noise_ddxy.y;
	}
#elif T3D_WORLD_NOISE_SHADER_MODE == 2 || T3D_WORLD_NOISE_SHADER_MODE == 4
	float noise_region_weight = get_noise_region_blend(uv2);
	float noise_height = get_noise_height_with_region_weight(uv2, noise_region_weight);
	u += noise_height - get_noise_height_with_region_weight(uv2 + vec2(T3D_REGION_INV_SIZE, 0.0), noise_region_weight);
	v += noise_height - get_noise_height_with_region_weight(uv2 + vec2(0.0, T3D_REGION_INV_SIZE), noise_region_weight);
#else
	u += world_noise_ddxy.x;
	v += world_noise_ddxy.y;
#endif
#endif
