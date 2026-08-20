// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#if T3D_TEXTURE_SURFACE_MODE == 1
	// Solid diagnostic surface. Geometry, holes, terrain normals and editor
	// overlays remain active, while authored material texture-array sampling
	// and material blending below are compiled out. Painted terrain color remains visible.
	ALBEDO = silhouette_color.rgb * mix(vec3(1.0), color_map.rgb, _tier_authored_color_contribution);
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
	AO = 1.0;
	METALLIC = 0.0;
#else
	// Page-miss path: one coarse material sample, no detailed control/PBR work.
	material mat = material(vec4(0.0), vec4(0.0), 0.0, 0.0, 0.0, 0.0, 0.0);
	if (far_cached_region) {
		mat.albedo_height = vec4(sample_far_cached_material(far_cached_material_id, uv), 1.0);
		mat.normal_rough = vec4(0.0, 1.0, 0.0, 1.0);
		mat.normal_map_depth = 0.0;
		mat.ao = 1.0;
		mat.ao_affect = 0.0;
		mat.metallic = 0.0;
		mat.total_weight = 1.0;
	} else {
#include "res://addons/terrain_3d/shaders/includes/near/near_detailed_material.gdshaderinc"
	}

	// Normalize only the values consumed by the selected PBR outputs.
	float weight_inv = 1.0 / max(mat.total_weight, 1e-8);
	mat.albedo_height *= weight_inv;
#if T3D_OUTPUT_NORMAL_MAP || T3D_OUTPUT_ROUGHNESS || T3D_OUTPUT_SPECULAR
	mat.normal_rough *= weight_inv;
#endif
#if T3D_OUTPUT_NORMAL_MAP
	mat.normal_map_depth *= weight_inv;
#endif
#if T3D_OUTPUT_AO
	mat.ao *= weight_inv;
	mat.ao_affect *= weight_inv;
#endif
	mat.metallic *= weight_inv;

#if T3D_OUTPUT_NORMAL_MAP
	vec3 normal_map = fma(normalize(mat.normal_rough.xzy), vec3(0.5), vec3(0.5));
	float distant_normal_amplifier = clamp(sqrt(base_footprint_sq), 1.0, distant_normal_scale);
	mat.normal_map_depth *= distant_normal_amplifier;
#endif

#include "res://addons/terrain_3d/shaders/includes/macro_variation.gdshaderinc"

#if T3D_OUTPUT_ROUGHNESS || T3D_OUTPUT_SPECULAR
	// Wetness is evaluated only when roughness or specular output consumes it.
	float wetness = fma(color_map.a, -2.0, 1.0) * _tier_wetness_contribution;
#endif
#if T3D_OUTPUT_ROUGHNESS
	float roughness = clamp(mat.normal_rough.a - wetness, 0.0, 1.0);
#endif
#if T3D_OUTPUT_SPECULAR
	// Suppress dry specular on non-light-facing terrain, then restore wet sky response.
	float terrain_facing_light = clamp(dot(w_normal, _light_direction), 0.0, 1.0);
	float specular = 1.0 - mat.normal_rough.a;
	specular *= terrain_facing_light;
	specular = clamp(specular + wetness, 0.0, 1.0);
#endif

	// Apply PBR
#include "res://addons/terrain_3d/shaders/includes/near/near_pbr_outputs.gdshaderinc"
#endif
