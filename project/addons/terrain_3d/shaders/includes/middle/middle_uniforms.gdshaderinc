// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

group_uniforms shader_uniforms;
#include "res://addons/terrain_3d/shaders/includes/terrain_common_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_region_map_uniform.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_middle_height_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_render_common_uniforms.gdshaderinc"
uniform sampler2D _logical_region_map_texture : filter_nearest, repeat_disable;
#if !T3D_CONTROL_MAPS_IMPLICIT
uniform highp sampler2DArray _control_maps : repeat_disable;
#endif
#include "res://addons/terrain_3d/shaders/includes/terrain_control_lookup.gdshaderinc"
#if T3D_TERRAIN_SAMPLING_MODE != 2 && !T3D_COLOR_MAPS_IMPLICIT
uniform highp sampler2DArray _color_maps : source_color, FILTER_METHOD, repeat_disable;
#endif
uniform highp sampler2DArray _derived_maps : FILTER_METHOD, repeat_disable;
#if T3D_TERRAIN_SAMPLING_MODE == 2
uniform highp sampler2DArray _middle_composite_maps : source_color, filter_nearest, repeat_disable;
#else
uniform highp sampler2DArray _texture_array_albedo : source_color, FILTER_METHOD, repeat_enable;
uniform vec2 _control_rotation_cs[16];
uniform vec4 _texture_color_array[32];
uniform float _texture_uv_scale_array[32];
uniform int _texture_color_count = 0;
uniform int _texture_array_layer_count = 1;
uniform float _material_max_lod = 0.0;
#include "res://addons/terrain_3d/shaders/includes/runtime_material_streaming.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/material_layer_map.gdshaderinc"
#endif
uniform float _tier_material_lod_bias : hint_range(-2.0, 4.0, 0.25) = 0.0;
uniform float _tier_authored_color_contribution : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform float _tier_wetness_contribution : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform vec4 silhouette_color : source_color = vec4(0.04, 0.04, 0.04, 1.0);
#include "res://addons/terrain_3d/shaders/includes/auto_shader_uniforms.gdshaderinc"
group_uniforms;

#include "res://addons/terrain_3d/shaders/includes/flat_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_uniforms.gdshaderinc"
