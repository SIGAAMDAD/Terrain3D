// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Varyings & Types

struct material {
	vec4 albedo_height;
	vec4 normal_rough;
	float normal_map_depth;
	float ao;
	float ao_affect;
	float metallic;
	float total_weight;
};

#include "res://addons/terrain_3d/shaders/includes/terrain_render_varyings.gdshaderinc"
