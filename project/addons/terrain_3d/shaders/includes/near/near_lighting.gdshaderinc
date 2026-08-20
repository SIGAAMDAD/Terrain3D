// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#if T3D_SHADING_MODE == 5
#include "res://addons/terrain_3d/shaders/includes/blinn_phong.gdshaderinc"

void light() {
#include "res://addons/terrain_3d/shaders/includes/wrapped_diffuse_light.gdshaderinc"

	if (ROUGHNESS < 0.999) {
		vec4 blinn = terrain3d_blinn_terms(NORMAL, LIGHT, VIEW);
		float gloss = clamp(1.0 - ROUGHNESS, 0.0, 1.0);
		float highlight = mix(blinn.y, blinn.w, gloss);
		float specular_strength = mix(0.035, 0.22, gloss);
		SPECULAR_LIGHT += LIGHT_COLOR * highlight * specular_strength * ATTENUATION / PI;
	}
}
#endif
