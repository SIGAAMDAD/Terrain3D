// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// T3D_STANDALONE_DEFAULTS_BEGIN
// ShaderInclude resources are parsed independently by the editor. Runtime-generated
// shaders inject the selected values before these guards, so local defaults only
// apply when this module is parsed without its Terrain3D parent shader.
#ifndef T3D_WORLD_NOISE_SHADER_MODE
#define T3D_WORLD_NOISE_SHADER_MODE 0
#endif
// T3D_STANDALONE_DEFAULTS_END


#ifdef T3D_ENABLE_WORLD_NOISE_VERTEX
		// The NOISE variant is compile-time specialized; no background-mode branch remains.
		vec2 nuv_a = start_pos * _region_texel_size;
		vec2 nuv_b = end_pos * _region_texel_size;
		// The +/- one-texel normal probes share the center sample's region coverage.
		// Reusing it cuts logical-region-map reads from 24 to 8 per morphed vertex
		// in cached vertex-normal modes while changing only a one-texel border ramp.
		float noise_weight_a = get_noise_region_blend(nuv_a);
		float noise_weight_b = get_noise_region_blend(nuv_b);
		float nh = mix(get_noise_height_with_region_weight(nuv_a, noise_weight_a),
			get_noise_height_with_region_weight(nuv_b, noise_weight_b), vertex_lerp);
#if T3D_WORLD_NOISE_SHADER_MODE == 0 || T3D_WORLD_NOISE_SHADER_MODE == 1 || T3D_WORLD_NOISE_SHADER_MODE == 3
		float nu = mix(get_noise_height_with_region_weight(nuv_a + vec2(_region_texel_size, 0.0), noise_weight_a),
			get_noise_height_with_region_weight(nuv_b + vec2(_region_texel_size, 0.0), noise_weight_b), vertex_lerp);
		float nv = mix(get_noise_height_with_region_weight(nuv_a + vec2(0.0, _region_texel_size), noise_weight_a),
			get_noise_height_with_region_weight(nuv_b + vec2(0.0, _region_texel_size), noise_weight_b), vertex_lerp);
		world_noise_ddxy = vec2(nh - nu, nh - nv);
#endif
		h += nh;
#endif
