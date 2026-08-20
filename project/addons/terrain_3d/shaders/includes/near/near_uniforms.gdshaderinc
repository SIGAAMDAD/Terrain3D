// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Private uniforms
group_uniforms shader_uniforms;
#include "res://addons/terrain_3d/shaders/includes/terrain_common_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_region_map_uniform.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_render_common_uniforms.gdshaderinc"
uniform sampler2D _logical_region_map_texture : filter_linear, repeat_disable;
uniform vec2 _control_rotation_cs[16];
#if T3D_DETILING_MODE == 1 || T3D_DETILING_MODE == 4 || T3D_DETILING_MODE == 5
uniform vec2 _detile_rotation_cs[16];
#endif
#if T3D_OUTPUT_NORMAL_MAP
uniform float _texture_normal_depth_array[32];
#endif
#if T3D_OUTPUT_AO
uniform float _texture_ao_strength_array[32];
uniform float _texture_ao_affect_array[32];
#endif
#if T3D_OUTPUT_ROUGHNESS || T3D_OUTPUT_SPECULAR
uniform float _texture_roughness_mod_array[32];
#endif
uniform float _texture_uv_scale_array[32];
uniform vec2 _texture_detile_array[32];
uniform vec4 _texture_color_array[32];
uniform int _texture_array_layer_count = 1;
uniform float _material_max_lod = 0.0;
#include "res://addons/terrain_3d/shaders/includes/runtime_material_streaming.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/material_layer_map.gdshaderinc"
uniform highp sampler2DArray _height_maps : repeat_disable;
#include "res://addons/terrain_3d/shaders/includes/terrain_height_decode.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/terrain_far_surface.gdshaderinc"
#if !T3D_CONTROL_MAPS_IMPLICIT
uniform highp sampler2DArray _control_maps : repeat_disable;
#endif
#include "res://addons/terrain_3d/shaders/includes/terrain_control_lookup.gdshaderinc"
#if !T3D_COLOR_MAPS_IMPLICIT
uniform highp sampler2DArray _color_maps : source_color, FILTER_METHOD, repeat_disable;
#endif
uniform float _region_color_max_lod = 0.0;
#if T3D_TERRAIN_NORMAL_MODE == 3
uniform highp sampler2DArray _derived_maps : FILTER_METHOD, repeat_disable;
#endif
uniform highp sampler2DArray _texture_array_albedo : source_color, FILTER_METHOD, repeat_enable;
#include "res://addons/terrain_3d/shaders/includes/legacy_pbr_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/surface_pbr_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/surface_displacement_uniform.gdshaderinc"
#if T3D_OUTPUT_SPECULAR
uniform vec3 _light_direction = vec3(0.0, 0.0, 0);
#endif
group_uniforms;

// Public uniforms
group_uniforms shader_uniforms.general;
#include "res://addons/terrain_3d/shaders/includes/flat_uniforms.gdshaderinc"
#if T3D_TERRAIN_NORMAL_MODE == 0
uniform bool flat_terrain_normals = false;
#endif
#if T3D_OUTPUT_NORMAL_MAP
uniform float distant_normal_scale : hint_range(1.0, 10.0, 0.1) = 2.0;
#endif
uniform float blend_sharpness : hint_range(0, 1) = 0.5;
uniform vec4 silhouette_color : source_color = vec4(0.04, 0.04, 0.04, 1.0);
uniform float _tier_authored_color_contribution : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform float _tier_wetness_contribution : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform float _tier_material_lod_bias : hint_range(-2.0, 4.0, 0.25) = 0.0;
group_uniforms;

#include "res://addons/terrain_3d/shaders/includes/auto_shader_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/displacement_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/dual_scaling_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/macro_variation_uniforms.gdshaderinc"

group_uniforms shader_uniforms.mipmaps;
uniform float bias_distance : hint_range(0.0, 16384.0, 0.1) = 512.0;
uniform float mipmap_bias : hint_range(0.5, 1.5, 0.01) = 1.0;
uniform float depth_blur : hint_range(0.0, 35.0, 0.1) = 0.0;
group_uniforms;

#include "res://addons/terrain_3d/shaders/includes/world_noise_uniforms.gdshaderinc"
