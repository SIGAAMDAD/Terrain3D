// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

group_uniforms shader_uniforms;
#include "res://addons/terrain_3d/shaders/includes/terrain_common_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_render_common_uniforms.gdshaderinc"
uniform highp sampler2DArray _far_albedo_array : source_color, FILTER_METHOD, repeat_enable;
uniform vec4 _texture_color_array[32];
uniform float _texture_uv_scale_array[32];
uniform int _texture_color_count = 0;
uniform int _far_albedo_layer_count = 1;
uniform float _far_material_max_lod = 0.0;
uniform float _tier_material_lod_bias : hint_range(-2.0, 4.0, 0.25) = -0.5;
uniform float _far_quality_lod_offset : hint_range(0.0, 3.0, 1.0) = 0.0;
uniform float _tier_authored_color_contribution : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform float _tier_wetness_contribution : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform vec4 silhouette_color : source_color = vec4(0.04, 0.04, 0.04, 1.0);
group_uniforms;

#include "res://addons/terrain_3d/shaders/includes/flat_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_uniforms.gdshaderinc"
