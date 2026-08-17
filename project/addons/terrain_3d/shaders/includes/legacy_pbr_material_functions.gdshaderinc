// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// T3D_STANDALONE_DEFAULTS_BEGIN
// ShaderInclude resources are parsed independently by the editor. Runtime-generated
// shaders inject the selected values before these guards, so local defaults only
// apply when this module is parsed without its Terrain3D parent shader.
#ifndef T3D_SAMPLE_SURFACE_DATA
#define T3D_SAMPLE_SURFACE_DATA 0
#endif
#ifndef T3D_OUTPUT_ROUGHNESS
#define T3D_OUTPUT_ROUGHNESS 0
#endif
#ifndef T3D_OUTPUT_SPECULAR
#define T3D_OUTPUT_SPECULAR 0
#endif
#ifndef T3D_OUTPUT_NORMAL_MAP
#define T3D_OUTPUT_NORMAL_MAP 0
#endif
#ifndef T3D_OUTPUT_AO
#define T3D_OUTPUT_AO 0
#endif
// T3D_STANDALONE_DEFAULTS_END


#ifdef T3D_ENABLE_LEGACY_PBR_MATERIAL_FUNCTIONS
void sample_texture_data(const vec2 uv, const int id, const vec2 ddx_uv, const vec2 ddy_uv,
		out vec4 albedo_height, out vec4 normal_rough, out float ao, out float metallic) {
	if (!t3d_mat_resident(id)) {
		albedo_height = vec4(t3d_runtime_average_material(id, _texture_color_array[id].rgb), 0.5);
		normal_rough = vec4(0.0, 1.0, 0.0, 1.0);
		ao = 1.0;
		metallic = 0.0;
		return;
	}
	albedo_height = textureGrad(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), ddx_uv, ddy_uv);
	albedo_height.rgb *= _texture_color_array[id].rgb;
#if T3D_SAMPLE_SURFACE_DATA
	vec4 normal_sample = textureGrad(_texture_array_normal, vec3(uv, float(t3d_mat_layer(id))), ddx_uv, ddy_uv);
#if T3D_OUTPUT_ROUGHNESS || T3D_OUTPUT_SPECULAR
	float roughness = clamp(normal_sample.a + _texture_roughness_mod_array[id], 0.0, 1.0);
#else
	float roughness = 1.0;
#endif
	normal_rough = vec4(0.0, 1.0, 0.0, roughness);
#if T3D_OUTPUT_NORMAL_MAP || T3D_OUTPUT_AO
	vec3 decoded_normal = fma(normal_sample.xzy, vec3(2.0), vec3(-1.0));
#if T3D_OUTPUT_AO
	float ao_raw = length(decoded_normal) * 2.0 - 1.0;
	ao = mix(ao_raw * ao_raw * _texture_ao_strength_array[id] + 1.0 - _texture_ao_strength_array[id],
			1.0, albedo_height.a * albedo_height.a);
#else
	ao = 1.0;
#endif
#if T3D_OUTPUT_NORMAL_MAP
	normal_rough.xyz = normalize(decoded_normal);
#endif
#else
	ao = 1.0;
#endif
#else
	normal_rough = vec4(0.0, 1.0, 0.0, 1.0);
	ao = 1.0;
#endif
	metallic = 0.0;
}
#endif
