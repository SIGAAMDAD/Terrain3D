// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// T3D_STANDALONE_DEFAULTS_BEGIN
// ShaderInclude resources are parsed independently by the editor. Runtime-generated
// shaders inject the selected values before these guards, so local defaults only
// apply when this module is parsed without its Terrain3D parent shader.
#ifndef T3D_WORLD_NOISE_SHADER_MODE
#define T3D_WORLD_NOISE_SHADER_MODE 0
#endif
// T3D_STANDALONE_DEFAULTS_END


#ifdef T3D_ENABLE_WORLD_NOISE_UNIFORMS
group_uniforms shader_uniforms.world_background_noise;
#if T3D_WORLD_NOISE_SHADER_MODE == 0
uniform bool world_noise_fragment_normals = false;
// Dynamic mode preserves the original runtime controls. Static sidebar modes
// compile both branches away.
uniform bool world_noise_cache_enabled = true;
#endif
uniform float world_noise_cache_period : hint_range(8.0, 256.0, 1.0) = 64.0;
uniform int world_noise_max_octaves : hint_range(0, 15) = 4;
uniform int world_noise_min_octaves : hint_range(0, 15) = 2;
uniform float world_noise_lod_distance : hint_range(0., 40000., 1.) = 7500.;
uniform float world_noise_scale : hint_range(0.25, 20, 0.01) = 5.0;
uniform float world_noise_height : hint_range(-1000., 1000., 0.1) = 32.0;
uniform vec3 world_noise_offset = vec3(0.0);
group_uniforms;
uniform sampler2D _world_noise_cache : repeat_enable, filter_linear_mipmap;
varying vec2 world_noise_ddxy;
#endif
