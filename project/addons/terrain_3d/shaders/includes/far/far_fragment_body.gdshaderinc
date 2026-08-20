// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	vec2 fragment_uv = UV;
	vec4 far_surface = sample_far_color(fragment_uv);
#include "res://addons/terrain_3d/shaders/includes/far_none_fragment.gdshaderinc"

#if T3D_TERRAIN_NORMAL_MODE == 2
	vec3 terrain_geometry_normal = normalize(cross(dFdyCoarse(v_vertex), dFdxCoarse(v_vertex)));
	NORMAL = normalize(mat3(VIEW_MATRIX) * terrain_geometry_normal);
#else
	NORMAL = normalize(mat3(VIEW_MATRIX) * v_far_normal);
#endif

#if T3D_TEXTURE_SURFACE_MODE == 1
	ALBEDO = silhouette_color.rgb * far_surface.rgb;
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
	AO = 1.0;
#else
	uint far_surface_bits = decode_far_surface_bits(far_surface.a);
	bool surface_present = far_surface_bits_is_solid(far_surface_bits);
	float surface_alpha = surface_present ? far_surface_bits_alpha(far_surface_bits) : 0.5;
	float wetness = fma(surface_alpha, -2.0, 1.0) * _tier_wetness_contribution;
	float wet_amount = clamp(wetness, 0.0, 1.0);
	int material_id = surface_present ? far_surface_bits_material_id(far_surface_bits) : 0;
	vec3 material_albedo = sample_far_material(material_id, fragment_uv);
	vec3 authored_color = mix(vec3(1.0), far_surface.rgb, _tier_authored_color_contribution);
	ALBEDO = authored_color * material_albedo * mix(1.0, 0.86, wet_amount);
#if T3D_SAMPLE_SURFACE_DATA
	ROUGHNESS = clamp(1.0 - wetness, 0.0, 1.0);
#else
	ROUGHNESS = mix(1.0, 0.84, wet_amount);
#endif
	SPECULAR = 0.0;
	AO = 1.0;
#endif
