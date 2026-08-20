// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#if T3D_SHADING_MODE == 2
void light() {
#include "res://addons/terrain_3d/shaders/includes/wrapped_diffuse_light.gdshaderinc"
}
#elif T3D_SHADING_MODE == 5
#include "res://addons/terrain_3d/shaders/includes/blinn_phong.gdshaderinc"

void light() {
#include "res://addons/terrain_3d/shaders/includes/wrapped_diffuse_light.gdshaderinc"

	vec4 blinn = terrain3d_blinn_terms(NORMAL, LIGHT, VIEW);
	float gloss = clamp(1.0 - ROUGHNESS, 0.0, 1.0);
	SPECULAR_LIGHT += LIGHT_COLOR * blinn.y * mix(0.02, 0.12, gloss) * ATTENUATION / PI;
}
#endif
