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


#ifdef T3D_ENABLE_SURFACE_PBR_MATERIAL_FUNCTIONS
void sample_texture_data(const vec2 uv, const int id, const vec2 ddx_uv, const vec2 ddy_uv,
		out vec4 albedo_height, out vec4 normal_rough, out float ao, out float metallic) {
	if (!t3d_mat_resident(id)) {
		albedo_height = vec4(t3d_runtime_average_material(id, _texture_color_array[id].rgb), 0.5);
		normal_rough = vec4(0.0, 1.0, 0.0, 1.0);
		ao = 1.0;
		metallic = _texture_metallic_array[id];
		return;
	}
	vec4 albedo = textureGrad(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), ddx_uv, ddy_uv);
	vec4 surface = vec4(0.5, 1.0, 1.0, 1.0);
#if T3D_SAMPLE_SURFACE_DATA
	surface = textureGrad(_texture_array_surface, vec3(uv, float(t3d_mat_layer(id))), ddx_uv, ddy_uv);
#endif
	// Packed-PBR albedo is BC1/RGB-style and carries no displacement alpha.
	// Keep the blend-height term neutral unless tessellated material-height mode
	// explicitly binds and samples the dedicated R8 displacement array.
	float height = 0.5;
#ifdef T3D_ENABLE_SURFACE_DISPLACEMENT_UNIFORM
	height = textureGrad(_texture_array_displacement, vec3(uv, float(t3d_mat_layer(id))), ddx_uv, ddy_uv).r;
#endif
	albedo_height = vec4(albedo.rgb * _texture_color_array[id].rgb, height);
#if T3D_SAMPLE_SURFACE_DATA
#if T3D_OUTPUT_ROUGHNESS || T3D_OUTPUT_SPECULAR
	float roughness = clamp(surface.b + _texture_roughness_mod_array[id], 0.0, 1.0);
#else
	float roughness = 1.0;
#endif
#if T3D_OUTPUT_NORMAL_MAP
	normal_rough = vec4(decode_surface_normal(surface.rg), roughness);
#else
	normal_rough = vec4(0.0, 1.0, 0.0, roughness);
#endif
#if T3D_OUTPUT_AO
	ao = surface.a * _texture_ao_strength_array[id] + 1.0 - _texture_ao_strength_array[id];
#else
	ao = 1.0;
#endif
#else
	normal_rough = vec4(0.0, 1.0, 0.0, 1.0);
	ao = 1.0;
#endif
#ifdef T3D_ENABLE_SURFACE_DISPLACEMENT_UNIFORM
	// Preserve Terrain3D's historical suppression of texture AO on raised
	// displacement while compiling the entire dependency out without tessellation.
	ao = mix(ao, 1.0, height * height);
#endif
	metallic = _texture_metallic_array[id];
}
#endif
