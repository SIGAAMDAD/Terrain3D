#ifdef T3D_TEMPLATE_CONTEXT
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx, skip_vertex_transform;


#define fma(a, b, c) ((a) * (b) + (c))

group_uniforms shader_uniforms;
uniform vec3 _target_pos = vec3(0.0);
uniform float _mesh_size = 48.0;
uniform float _subdiv = 1.0;
uniform uint _mouse_layer = 0x80000000u;
uniform float _vertex_spacing = 1.0;
uniform float _vertex_density = 1.0;
uniform float _region_size = 1024.0;
uniform float _region_texel_size = 0.0009765625;
uniform vec2 _height_decode = vec2(-384.0, 1152.0);
uniform int _region_map_size = 32;
uniform highp sampler2D _far_height_map : filter_linear, repeat_disable;
uniform highp sampler2D _far_color_map : source_color, filter_nearest, repeat_disable;
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_linear_anisotropic.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_linear.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_nearest_anisotropic.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_nearest.gdshaderinc"
uniform highp sampler2DArray _texture_array_albedo : source_color, FILTER_METHOD, repeat_enable;
uniform vec4 _texture_color_array[32];
uniform float _texture_uv_scale_array[32];
uniform int _texture_color_count = 0;
uniform int _texture_array_layer_count = 1;
#include "res://addons/terrain_3d/shaders/includes/runtime_material_streaming.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/material_layer_map.gdshaderinc"
uniform float _far_material_max_lod = 0.0;
uniform float _tier_material_lod_bias : hint_range(-2.0, 4.0, 0.25) = -0.5;
uniform float _far_quality_lod_offset : hint_range(0.0, 3.0, 1.0) = 0.0;
uniform float _tier_authored_color_contribution : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform float _tier_wetness_contribution : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform float _tier_ambient_floor = 0.12;
uniform float _tier_shadow_floor = 0.28;
uniform vec4 silhouette_color : source_color = vec4(0.04, 0.04, 0.04, 1.0);
group_uniforms;

#include "res://addons/terrain_3d/shaders/includes/flat_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_uniforms.gdshaderinc"

varying vec3 v_vertex;
varying vec3 v_main_camera_position;
varying float v_vertex_xz_dist;
varying vec3 v_far_normal;

float decode_terrain_height(float encoded) {
return fma(encoded, _height_decode.y, _height_decode.x);
}

bool far_atlas_contains(const vec2 world_texel) {
float half_world_size = _region_size * float(_region_map_size) * 0.5;
return all(greaterThanEqual(world_texel, vec2(-half_world_size))) &&
all(lessThan(world_texel, vec2(half_world_size)));
}

vec2 get_far_atlas_uv(const vec2 world_texel) {
float world_size = _region_size * float(_region_map_size);
return clamp((world_texel + vec2(world_size * 0.5)) / world_size, vec2(0.0), vec2(1.0));
}

vec2 get_far_height_atlas_uv(const vec2 world_texel) {
	float world_size = _region_size * float(_region_map_size);
	ivec2 atlas_size = textureSize(_far_height_map, 0);
	vec2 sample_coord = ((world_texel + vec2(world_size * 0.5)) / world_size) * vec2(atlas_size);
	vec2 centered = (sample_coord + vec2(0.5)) / vec2(atlas_size);
	vec2 half_texel = vec2(0.5) / vec2(atlas_size);
	return clamp(centered, half_texel, vec2(1.0) - half_texel);
}

float sample_far_height(const vec2 world_texel) {
if (!far_atlas_contains(world_texel)) {
return 0.0;
}
return decode_terrain_height(textureLod(_far_height_map, get_far_height_atlas_uv(world_texel), 0.0).r);
}

vec4 sample_far_color(const vec2 world_texel) {
if (!far_atlas_contains(world_texel)) {
return vec4(1.0, 1.0, 1.0, 0.0);
}
ivec2 atlas_size = textureSize(_far_color_map, 0);
vec2 atlas_uv = get_far_atlas_uv(world_texel);
vec2 texel = atlas_uv * vec2(atlas_size) - vec2(0.5);
ivec2 atlas_coord = clamp(ivec2(floor(texel + vec2(0.5))), ivec2(0), atlas_size - ivec2(1));
vec4 nearest_sample = texelFetch(_far_color_map, atlas_coord, 0);
#if T3D_COLOR_SAMPLING_MODE == 2
ivec2 base = ivec2(floor(texel));
vec2 f = fract(texel);
ivec2 max_coord = atlas_size - ivec2(1);
vec3 c00 = texelFetch(_far_color_map, clamp(base, ivec2(0), max_coord), 0).rgb;
vec3 c10 = texelFetch(_far_color_map, clamp(base + ivec2(1, 0), ivec2(0), max_coord), 0).rgb;
vec3 c01 = texelFetch(_far_color_map, clamp(base + ivec2(0, 1), ivec2(0), max_coord), 0).rgb;
vec3 c11 = texelFetch(_far_color_map, clamp(base + ivec2(1, 1), ivec2(0), max_coord), 0).rgb;
nearest_sample.rgb = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
#endif
return nearest_sample;
}

uint decode_far_surface_bits(const float packed_alpha) {
return uint(round(clamp(packed_alpha, 0.0, 1.0) * 255.0));
}

bool far_surface_is_solid(const float packed_alpha) {
return (decode_far_surface_bits(packed_alpha) & 0x80u) != 0u;
}

int decode_far_material_id(const float packed_alpha) {
return int((decode_far_surface_bits(packed_alpha) >> 2u) & 0x1Fu);
}

float decode_far_surface_alpha(const float packed_alpha) {
return float(decode_far_surface_bits(packed_alpha) & 0x3u) * (1.0 / 3.0);
}

vec3 missing_texture_checker(const vec2 world_texel) {
vec2 cell = floor(world_texel * 0.125);
float checker = mod(cell.x + cell.y, 2.0);
return mix(vec3(0.24), vec3(0.42), checker);
}

vec3 sample_far_material(const int texture_id, const vec2 world_texel) {
if (_texture_array_layer_count <= 0) {
return missing_texture_checker(world_texel);
}
int id = clamp(texture_id, 0, _texture_array_layer_count - 1);
float uv_scale = _texture_uv_scale_array[id];
if (!(uv_scale > 0.00001)) {
uv_scale = 0.1;
}
vec3 tint = id < _texture_color_count ? _texture_color_array[id].rgb : vec3(1.0);
if (_runtime_material_streaming_active) {
return t3d_runtime_average_material(id, tint);
}
// FAST is metadata/tint-only. FULL performs the dominant material fetch.
#if T3D_TERRAIN_SAMPLING_MODE == 2
return tint;
#else
float material_lod = clamp(4.0 + _tier_material_lod_bias + _far_quality_lod_offset, 0.0, max(_far_material_max_lod, 0.0));
vec2 uv = world_texel * (_vertex_spacing * uv_scale);
#if T3D_DETILING_MODE != 2
ivec2 cell = ivec2(floor(world_texel * 0.0625));
int rotation = (cell.x ^ cell.y) & 3;
uv = rotation == 1 ? vec2(-uv.y, uv.x) :
    (rotation == 2 ? -uv : (rotation == 3 ? vec2(uv.y, -uv.x) : uv));
#endif
vec3 albedo = textureLod(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), material_lod).rgb;
return albedo * tint;
#endif
}

vec3 sample_far_normal(const vec2 world_texel, const float sample_step, const float h0) {
float hx = sample_far_height(world_texel + vec2(sample_step, 0.0));
float hz = sample_far_height(world_texel + vec2(0.0, sample_step));
float world_step = sample_step / max(_vertex_density, 1e-6);
return normalize(vec3(h0 - hx, world_step, h0 - hz));
}

#include "res://addons/terrain_3d/shaders/includes/far_none_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/far_flat_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_functions.gdshaderinc"

#include "res://addons/terrain_3d/shaders/includes/clipmap_batching_functions.gdshaderinc"

#include "res://addons/terrain_3d/shaders/includes/editor_setup.gdshaderinc"

void vertex() {
vec2 clipmap_vertex_metadata = UV2;
vec4 clipmap_lod_state = terrain3d_clipmap_lod_state(clipmap_vertex_metadata.y);
vec2 clipmap_stitch_state = terrain3d_clipmap_stitch_state(clipmap_lod_state.w);
vec3 clipmap_grid_vertex = VERTEX;
clipmap_grid_vertex.xz += terrain3d_clipmap_edge_offset(clipmap_vertex_metadata.x, clipmap_stitch_state);
VERTEX.xz = clipmap_grid_vertex.xz * clipmap_lod_state.z + clipmap_lod_state.xy;
v_main_camera_position = MAIN_CAM_INV_VIEW_MATRIX[3].xyz;
v_vertex = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
v_vertex_xz_dist = length(v_vertex.xz - _target_pos.xz);

float scale = clipmap_lod_state.z;
float inv_scale = 1.0 / scale;
float max_xz = max(abs(v_vertex.x - _target_pos.x), abs(v_vertex.z - _target_pos.z));
float vertex_lerp = smoothstep(0.0, 1.0, (max_xz * inv_scale - _mesh_size - 4.0) / (_mesh_size - 4.0));
vec2 vertex_fract = fract(clipmap_grid_vertex.xz * 0.5) * 2.0;
vec2 shift = scale < _vertex_spacing / _subdiv + 1e-6 ?
mix(vertex_fract, vec2(vertex_fract.x, -vertex_fract.y),
round(fract(round(mod(v_vertex.z * inv_scale, 4.0)) * round(mod(v_vertex.x * inv_scale, 4.0)) * 0.25))) :
vertex_fract * round((fract(v_vertex.xz * 0.25 * inv_scale) - 0.5) * 4.0);
vec2 start_pos = v_vertex.xz * _vertex_density;
vec2 end_pos = (v_vertex.xz - shift * scale) * _vertex_density;
v_vertex.xz -= shift * scale * vertex_lerp;
UV = v_vertex.xz * _vertex_density;
UV2 = fma(UV, vec2(_region_texel_size), vec2(0.5 * _region_texel_size));

bool hidden = false
#include "res://addons/terrain_3d/shaders/includes/none_check.gdshaderinc"
;
if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && hidden) {
v_vertex.x = 0.0 / 0.0;
v_far_normal = vec3(0.0, 1.0, 0.0);
} else {
float h = sample_far_height(UV);
float terrain_center_height = h;
#include "res://addons/terrain_3d/shaders/includes/flat_vertex.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_vertex.gdshaderinc"
v_vertex.y = h;
float normal_step = max(1.0, scale * _vertex_density);
v_far_normal = sample_far_normal(UV, normal_step, terrain_center_height);
}
VERTEX = (VIEW_MATRIX * vec4(v_vertex, 1.0)).xyz;
}

void fragment() {
vec2 fragment_uv = UV;
// Far atlas is authoritative beyond Middle.
vec4 far_surface = sample_far_color(fragment_uv);
#include "res://addons/terrain_3d/shaders/includes/far_none_fragment.gdshaderinc"
#if T3D_TERRAIN_NORMAL_MODE == 2
NORMAL = normalize(cross(dFdyCoarse(VERTEX), dFdxCoarse(VERTEX)));
#else
NORMAL = normalize(mat3(VIEW_MATRIX) * v_far_normal);
#endif
#if T3D_TEXTURE_SURFACE_MODE == 1
ALBEDO = silhouette_color.rgb * far_surface.rgb;
ROUGHNESS = 1.0;
SPECULAR = 0.0;
AO = 1.0;
float hemisphere = mix(0.65, 1.0, clamp(v_far_normal.y * 0.5 + 0.5, 0.0, 1.0));
EMISSION = ALBEDO * (_tier_ambient_floor * hemisphere);
#else
bool coarse_surface_present = far_surface_is_solid(far_surface.a);
float surface_alpha = coarse_surface_present ? decode_far_surface_alpha(far_surface.a) : 0.5;
float wetness = fma(surface_alpha, -2.0, 1.0) * _tier_wetness_contribution;
float wet_amount = clamp(wetness, 0.0, 1.0);
int coarse_material_id = coarse_surface_present ? decode_far_material_id(far_surface.a) : 0;
vec3 material_albedo = sample_far_material(coarse_material_id, fragment_uv);
vec3 authored_color = mix(vec3(1.0), far_surface.rgb, _tier_authored_color_contribution);
ALBEDO = authored_color * material_albedo * mix(1.0, 0.86, wet_amount);
#if T3D_SAMPLE_SURFACE_DATA
ROUGHNESS = clamp(1.0 - wetness, 0.0, 1.0);
#else
ROUGHNESS = mix(1.0, 0.84, wet_amount);
#endif
SPECULAR = 0.0;
AO = 1.0;
float hemisphere = mix(0.65, 1.0, clamp(v_far_normal.y * 0.5 + 0.5, 0.0, 1.0));
EMISSION = ALBEDO * (_tier_ambient_floor * hemisphere);
#endif
#include "res://addons/terrain_3d/shaders/includes/editor_fragment.gdshaderinc"

}

#if T3D_SHADING_MODE == 2
void light() {
float ndotl = max(dot(NORMAL, LIGHT), 0.0);
float wrapped_diffuse = clamp((ndotl + 0.20) / 1.20, 0.0, 1.0);
float shadow_attenuation = max(ATTENUATION, _tier_shadow_floor);
DIFFUSE_LIGHT += LIGHT_COLOR * wrapped_diffuse * shadow_attenuation / PI;
}
#elif T3D_SHADING_MODE == 5
void light() {
float ndotl = max(dot(NORMAL, LIGHT), 0.0);
float wrapped_diffuse = clamp((ndotl + 0.20) / 1.20, 0.0, 1.0);
float shadow_attenuation = max(ATTENUATION, _tier_shadow_floor);
DIFFUSE_LIGHT += LIGHT_COLOR * wrapped_diffuse * shadow_attenuation / PI;
vec3 half_vector = normalize(LIGHT + VIEW);
float nh = max(dot(NORMAL, half_vector), 0.0);
float nh2 = nh * nh;
float nh4 = nh2 * nh2;
float nh8 = nh4 * nh4;
float gloss = clamp(1.0 - ROUGHNESS, 0.0, 1.0);
SPECULAR_LIGHT += LIGHT_COLOR * nh8 * mix(0.02, 0.12, gloss) * shadow_attenuation / PI;
}
#endif
#endif // T3D_TEMPLATE_CONTEXT
