// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "res://addons/terrain_3d/shaders/includes/terrain_common_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_region_map_uniform.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_middle_height_uniforms.gdshaderinc"
uniform int _shadow_height_source = 0;
uniform highp sampler2DArray _height_maps : filter_linear, repeat_disable;

#include "res://addons/terrain_3d/shaders/includes/terrain_height_decode.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_height.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_atlas_bounds.gdshaderinc"
