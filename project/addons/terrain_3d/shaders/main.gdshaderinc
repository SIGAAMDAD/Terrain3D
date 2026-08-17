#ifdef T3D_TEMPLATE_CONTEXT
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx, skip_vertex_transform;

/* The terrain depends on this shader to function. Don't change most things in vertex() or
 * terrain normal calculations in fragment(). You probably only want to customize the
 * material calculation and PBR application in fragment().
 *
 * Uniforms that begin with _ are private and will not display in the inspector. However,
 * you can set them via code. You are welcome to create more of your own hidden uniforms.
 *
 * Legacy packing supports albedo, height, normal, AO and roughness. Packed-PBR/R8 mode
 * stores NXY, roughness and AO in a hardware-filtered RGBA8 surface array, keeps metallic as a
 * per-material scalar, and enables the R8_UNORM displacement sampler only for tessellated variants.
 *
 */

// Defined Constants
#define COLOR_MAP_DEF vec4(1.0, 1.0, 1.0, 0.5)
#define DIV_255 0.003921568627450 // 1. / 255.
#define DIV_1024 0.0009765625 // 1. / 1024.
#define TAU_16TH -0.392699081698724 // -TAU / 16.

// Inline Functions
#define DECODE_BLEND(control) float(control >>14u & 0xFFu) * DIV_255
#define DECODE_AUTO(control) bool(control & 0x1u)
#define DECODE_BASE(control) int(control >>27u & 0x1Fu)
#define DECODE_OVER(control) int(control >>22u & 0x1Fu)
#define DECODE_ANGLE(control) float(control >>10u & 0xFu) * TAU_16TH
// This math recreates the scale value directly rather than using an 8 float const array.
#define DECODE_SCALE(control) (0.9 - float(((control >>7u & 0x7u) + 3u) % 8u + 1u) * 0.1)
#define DECODE_HOLE(control) bool(control >>2u & 0x1u)
#define CONTROL_NON_MATERIAL_MASK 0x3FFFu

// Use a portable arithmetic expansion on every renderer. Some low-end/mobile
// shader compilers reject the fma() builtin even when the same arithmetic is valid.
#define fma(a, b, c) ((a) * (b) + (c))
#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
    #define dFdxCoarse(a) dFdx(a)
    #define dFdyCoarse(a) dFdy(a)
#endif

// Private uniforms
group_uniforms shader_uniforms;
uniform vec3 _target_pos = vec3(0.f);
uniform float _mesh_size = 48.f;
uniform float _subdiv = 1.f;
uniform uint _mouse_layer = 0x80000000u; // Layer 32
uniform float _vertex_spacing = 1.0;
uniform float _vertex_density = 1.0; // = 1./_vertex_spacing
uniform float _region_size = 1024.0;
uniform float _region_texel_size = 0.0009765625; // = 1./region_size
uniform vec2 _height_decode = vec2(-384.0, 1152.0);
uniform int _region_map_size = 32;
uniform sampler2D _region_map_texture : filter_nearest, repeat_disable;
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
uniform highp sampler2D _far_height_map : filter_linear, repeat_disable;
uniform highp sampler2D _far_color_map : source_color, filter_nearest, repeat_disable;

float decode_terrain_height(float encoded) {
	return fma(encoded, _height_decode.y, _height_decode.x);
}

uint decode_far_surface_bits(float packed_alpha) {
	return uint(round(clamp(packed_alpha, 0.0, 1.0) * 255.0));
}

bool far_surface_is_solid(float packed_alpha) {
	return (decode_far_surface_bits(packed_alpha) & 0x80u) != 0u;
}

int decode_far_material_id(float packed_alpha) {
	return int((decode_far_surface_bits(packed_alpha) >> 2u) & 0x1Fu);
}

float decode_far_surface_alpha(float packed_alpha) {
	return float(decode_far_surface_bits(packed_alpha) & 0x3u) * (1.0 / 3.0);
}
#if !T3D_CONTROL_MAPS_IMPLICIT
uniform highp sampler2DArray _control_maps : repeat_disable;
#endif
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_linear_anisotropic.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_linear.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_nearest_anisotropic.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_nearest.gdshaderinc"
#if !T3D_COLOR_MAPS_IMPLICIT
uniform highp sampler2DArray _color_maps : source_color, FILTER_METHOD, repeat_disable;
#endif
uniform float _region_color_max_lod = 0.0;
#if T3D_TERRAIN_NORMAL_MODE == 3
uniform highp sampler2DArray _derived_maps : FILTER_METHOD, repeat_disable;
#endif

uint read_terrain_control(ivec3 index) {
#if T3D_CONTROL_MAPS_IMPLICIT
	return 1u;
#else
	return index.z >= 0 ? floatBitsToUint(texelFetch(_control_maps, index, 0).r) : 1u;
#endif
}

vec4 sample_terrain_color(vec3 region_uv, float region_mip) {
#if T3D_COLOR_MAPS_IMPLICIT
	return COLOR_MAP_DEF;
#else
	return region_uv.z >= 0.0 ? textureLod(_color_maps, region_uv, clamp(region_mip, 0.0, _region_color_max_lod)) : COLOR_MAP_DEF;
#endif
}
uniform highp sampler2DArray _texture_array_albedo : source_color, FILTER_METHOD, repeat_enable;
#include "res://addons/terrain_3d/shaders/includes/legacy_pbr_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/surface_pbr_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/surface_displacement_uniform.gdshaderinc"
#if T3D_OUTPUT_SPECULAR
uniform vec3 _light_direction = vec3(0., 0., 0);
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

varying vec3 v_vertex;
varying vec3 v_main_camera_position;
varying float v_vertex_xz_dist;

////////////////////////
// Vertex
////////////////////////

// Takes in world space XZ (UV) coordinates
// Returns ivec3 with:
// XY: (0 to _region_size - 1) coordinates within a region
// Z: layer index used for texturearrays, -1 if not in a region
int get_region_layer(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return -1;
	}
	return int(round(texelFetch(_region_map_texture, map_pos, 0).r * 65535.0)) - 1;
}

bool get_logical_region_present(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return false;
	}
	return texelFetch(_logical_region_map_texture, map_pos, 0).r > 0.5;
}

vec2 get_far_atlas_uv(const vec2 world_texel) {
	float world_size = _region_size * float(_region_map_size);
	return clamp((world_texel + vec2(world_size * 0.5)) / world_size,
		vec2(0.0), vec2(1.0));
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
	return decode_terrain_height(textureLod(_far_height_map, get_far_height_atlas_uv(world_texel), 0.0).r);
}

vec4 sample_far_color(const vec2 world_texel) {
	ivec2 atlas_size = textureSize(_far_color_map, 0);
	vec2 atlas_uv = get_far_atlas_uv(world_texel);
	ivec2 atlas_coord = clamp(ivec2(floor(atlas_uv * vec2(atlas_size))), ivec2(0), atlas_size - ivec2(1));
	return texelFetch(_far_color_map, atlas_coord, 0);
}

#include "res://addons/terrain_3d/shaders/includes/near_far_cached_material.gdshaderinc"

vec3 sample_far_normal(const vec2 world_texel, const float sample_step) {
	float h0 = sample_far_height(world_texel);
	float hx = sample_far_height(world_texel + vec2(sample_step, 0.0));
	float hz = sample_far_height(world_texel + vec2(0.0, sample_step));
	float world_step = sample_step / max(_vertex_density, 1e-6);
	return normalize(vec3(h0 - hx, world_step, h0 - hz));
}

float sample_terrain_height(const ivec3 index, const vec2 world_texel) {
	return index.z >= 0 ? decode_terrain_height(texelFetch(_height_maps, index, 0).r) :
		sample_far_height(world_texel);
}

// Takes in world space XZ (UV) coordinates.
// Returns local texel XY and the texture-array layer in Z.
ivec3 get_index_coord(const vec2 uv) {
	vec2 r_uv = round(uv);
	ivec2 map_pos = ivec2(floor(r_uv * _region_texel_size)) + (_region_map_size / 2);
	return ivec3(ivec2(mod(r_uv, _region_size)), get_region_layer(map_pos));
}

// Build the common 2x2 sample quad with one region-map lookup for interior texels.
// Boundary texels retain the original cross-region behavior.
void get_quad_indices(const vec2 index_id, out ivec3 i0, out ivec3 i1, out ivec3 i2, out ivec3 i3) {
	i3 = get_index_coord(index_id);
	int last_texel = int(_region_size) - 1;
	if (i3.z >= 0 && i3.x < last_texel && i3.y < last_texel) {
		i2 = ivec3(i3.xy + ivec2(1, 0), i3.z);
		i0 = ivec3(i3.xy + ivec2(0, 1), i3.z);
		i1 = ivec3(i3.xy + ivec2(1, 1), i3.z);
	} else {
		i0 = get_index_coord(index_id + vec2(0.0, 1.0));
		i1 = get_index_coord(index_id + vec2(1.0, 1.0));
		i2 = get_index_coord(index_id + vec2(1.0, 0.0));
	}
}

// Resolve a nearby sample from an already resolved base texel. The region map
// is touched only when the offset crosses a region boundary.
ivec3 get_offset_index(const vec2 index_id, const ivec3 base_index, const ivec2 offset) {
	ivec2 local = base_index.xy + offset;
	int region_size_i = int(_region_size);
	if (base_index.z >= 0 && uint(local.x | local.y) < uint(region_size_i)) {
		return ivec3(local, base_index.z);
	}
	return get_index_coord(index_id + vec2(offset));
}

// Takes in descaled world-to-region coordinates. Local UVs are the fractional
// component, so no dynamic region-location uniform lookup is required.
vec3 get_index_uv(const vec2 uv2) {
	vec2 region_pos = floor(uv2);
	ivec2 map_pos = ivec2(region_pos) + (_region_map_size / 2);
	return vec3(uv2 - region_pos, float(get_region_layer(map_pos)));
}

vec4 read_terrain_color_exact_texel(ivec2 local_coord, ivec2 region_cell, const ivec2 map_size) {
#if T3D_COLOR_MAPS_IMPLICIT
	return COLOR_MAP_DEF;
#else
	ivec2 region_delta = ivec2(0);
	if (local_coord.x < 0) {
		local_coord.x += map_size.x;
		region_delta.x = -1;
	} else if (local_coord.x >= map_size.x) {
		local_coord.x -= map_size.x;
		region_delta.x = 1;
	}
	if (local_coord.y < 0) {
		local_coord.y += map_size.y;
		region_delta.y = -1;
	} else if (local_coord.y >= map_size.y) {
		local_coord.y -= map_size.y;
		region_delta.y = 1;
	}
	int layer = get_region_layer(region_cell + region_delta + (_region_map_size / 2));
	return layer >= 0 ? texelFetch(_color_maps, ivec3(local_coord, layer), 0) : COLOR_MAP_DEF;
#endif
}

bool terrain_color_filter_crosses_region(const vec2 world_region_uv) {
#if T3D_COLOR_MAPS_IMPLICIT
	return false;
#else
	ivec2 map_size = textureSize(_color_maps, 0).xy;
	vec2 local_uv = world_region_uv - floor(world_region_uv);
	ivec2 base = ivec2(floor(local_uv * vec2(map_size) - vec2(0.5)));
	return base.x < 0 || base.y < 0 || base.x + 1 >= map_size.x || base.y + 1 >= map_size.y;
#endif
}

vec4 sample_terrain_color_exact(const vec2 world_region_uv) {
#if T3D_COLOR_MAPS_IMPLICIT
	return COLOR_MAP_DEF;
#else
	ivec2 map_size = textureSize(_color_maps, 0).xy;
	vec2 region_pos = floor(world_region_uv);
	vec2 local_uv = world_region_uv - region_pos;
	vec2 texel = local_uv * vec2(map_size) - vec2(0.5);
	ivec2 base = ivec2(floor(texel));
	vec2 f = fract(texel);
	ivec2 region_cell = ivec2(region_pos);
	vec4 c00 = read_terrain_color_exact_texel(base, region_cell, map_size);
	vec4 c10 = read_terrain_color_exact_texel(base + ivec2(1, 0), region_cell, map_size);
	vec4 c01 = read_terrain_color_exact_texel(base + ivec2(0, 1), region_cell, map_size);
	vec4 c11 = read_terrain_color_exact_texel(base + ivec2(1, 1), region_cell, map_size);
	return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
#endif
}

float interpolated_height(vec2 pos) {
	vec2 index_id = floor(pos);
	ivec3 index[4];
	get_quad_indices(index_id, index[0], index[1], index[2], index[3]);
	float h0 = sample_terrain_height(index[0], index_id + vec2(0.0, 1.0));
	float h1 = sample_terrain_height(index[1], index_id + vec2(1.0, 1.0));
	float h2 = sample_terrain_height(index[2], index_id + vec2(1.0, 0.0));
	float h3 = sample_terrain_height(index[3], index_id);
	vec2 f = fract(pos);
	vec2 i = 1.0 - f;
	vec4 w = vec4(i.x * f.y, f.x * f.y, f.x * i.y, i.x * i.y);
	float h = h0 * w[0] + h1 * w[1] + h2 * w[2] + h3 * w[3];
	return h;
}

#include "res://addons/terrain_3d/shaders/includes/displacement_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/none_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/flat_functions.gdshaderinc"
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
	// MAIN_CAM_INV_VIEW_MATRIX is vertex-only; pass its position to fragment helpers.
	v_main_camera_position = MAIN_CAM_INV_VIEW_MATRIX[3].xyz;

	// Get vertex of flat plane in world coordinates and set world UV
	v_vertex = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;

	// Distance from target node to vertex on a flat plane
	v_vertex_xz_dist = length(v_vertex.xz - _target_pos.xz);

	// Geomorph vertex across clipmap LODs, set end and start for linear height interpolate
	float scale = clipmap_lod_state.z;
	float inv_scale = 1.0 / scale;
	float max_xz = max(abs(v_vertex.x - _target_pos.x), abs(v_vertex.z - _target_pos.z));
	float vertex_lerp = smoothstep(0.0, 1.0, (max_xz * inv_scale - _mesh_size - 4.0) / (_mesh_size - 4.0));
	vec2 vertex_fract = fract(clipmap_grid_vertex.xz * 0.5) * 2.0;
	// For LOD0 morph from a regular grid to an alternating grid to align with LOD1+
	vec2 shift = (scale < _vertex_spacing / _subdiv + 1e-6) ? // LOD0 or not
		// Shift from regular to symmetric
		mix(vertex_fract, vec2(vertex_fract.x, -vertex_fract.y),
			round(fract(round(mod(v_vertex.z * inv_scale, 4.0)) *
			round(mod(v_vertex.x * inv_scale, 4.0)) * 0.25))) :
		// Symmetric shift
		vertex_fract * round((fract(v_vertex.xz * 0.25 * inv_scale) - 0.5) * 4.0);
	vec2 start_pos = v_vertex.xz * _vertex_density;
	vec2 end_pos = (v_vertex.xz - shift * scale) * _vertex_density;
	v_vertex.xz -= shift * scale * vertex_lerp;

	// UV coordinates in region space. 0-1 covers 1 region, 1-2 is the next region, etc.
	UV = v_vertex.xz * _vertex_density;

	// UV coordinates in region space + texel offset. Values are 0 to 1 within regions
	UV2 = fma(UV, vec2(_region_texel_size), vec2(0.5 * _region_texel_size));

	ivec3 v_region = get_index_coord(start_pos);
	uint control = read_terrain_control(v_region);
	bool hole = DECODE_HOLE(control);

	vec3 displacement = vec3(0.);
	// Show holes to all cameras except mouse camera (on exactly 1 layer)
	if ( !(CAMERA_VISIBLE_LAYERS == _mouse_layer) && (hole
#include "res://addons/terrain_3d/shaders/includes/none_check.gdshaderinc"
		)){
		v_vertex.x = 0. / 0.;
	} else {
		float h;
		// This branch is static for each of the clipmap segments.
		if (scale < _vertex_spacing) {
			h = interpolated_height(UV);
		} else {
			ivec3 coord_b = get_index_coord(end_pos);
			h = mix(sample_terrain_height(v_region, start_pos),
				sample_terrain_height(coord_b, end_pos), vertex_lerp);
		}

#include "res://addons/terrain_3d/shaders/includes/flat_vertex.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_vertex.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/displacement_vertex.gdshaderinc"
		v_vertex.y = h;
	}


	// Convert model space to view space w/ skip_vertex_transform render mode
	// Include displacement without modifying v_vertex.
	VERTEX = (VIEW_MATRIX * vec4(v_vertex + displacement, 1.0)).xyz;
}

////////////////////////
// Fragment
////////////////////////

float random(in vec2 xy) {
	return fract(sin(dot(xy, vec2(12.9898, 78.233))) * 43758.5453);
}

uint detile_hash(const ivec2 p) {
	uvec2 q = uvec2(p);
	uint h = q.x * 0x9E3779B9u ^ q.y * 0x85EBCA6Bu;
	h ^= h >> 16u;
	h *= 0x7FEB352Du;
	h ^= h >> 15u;
	h *= 0x846CA68Bu;
	return h ^ (h >> 16u);
}

// Returns cos(angle), sin(angle), and scalar UV shift. The selected implementation
// is compiled from the sidebar detiling mode; no runtime mode branch remains.
vec3 get_detile_transform(const vec2 uv_center, const int texture_id) {
	// Godot rejects a dynamically indexed uniform-array element when it is passed
	// directly as a custom-function argument on some shader backends. Resolve it
	// inside the function so the call site passes an ordinary local integer.
	vec2 strength = _texture_detile_array[texture_id];
#if T3D_DETILING_MODE == 2
	return vec3(1.0, 0.0, 0.0);
#elif T3D_DETILING_MODE == 3
	uint h = detile_hash(ivec2(uv_center));
	float shift = (float((h >> 8u) & 0xFFFFu) * (2.0 / 65535.0) - 1.0) * strength.y * TAU;
	return vec3(1.0, 0.0, shift);
#elif T3D_DETILING_MODE == 1
	uint h = detile_hash(ivec2(uv_center));
	float enabled = step(0.000001, strength.x);
	vec2 cs = mix(vec2(1.0, 0.0), _detile_rotation_cs[int((h & 7u) << 1u)], enabled);
	float shift = (float((h >> 8u) & 0xFFFFu) * (2.0 / 65535.0) - 1.0) * strength.y * TAU;
	return vec3(cs, shift);
#elif T3D_DETILING_MODE == 4
	uint h = detile_hash(ivec2(uv_center));
	float enabled = step(0.000001, strength.x);
	vec2 cs = mix(vec2(1.0, 0.0), _detile_rotation_cs[int((h & 3u) << 2u)], enabled);
	float shift = (float((h >> 8u) & 0xFFFFu) * (2.0 / 65535.0) - 1.0) * strength.y * TAU;
	return vec3(cs, shift);
#elif T3D_DETILING_MODE == 5
	uint h = detile_hash(ivec2(uv_center));
	float enabled = step(0.000001, strength.x);
	vec2 cs = mix(vec2(1.0, 0.0), _detile_rotation_cs[int(h & 15u)], enabled);
	float shift = (float((h >> 8u) & 0xFFFFu) * (2.0 / 65535.0) - 1.0) * strength.y * TAU;
	return vec3(cs, shift);
#else
	vec2 detile = fma(random(uv_center), 2.0, -1.0) * strength * TAU;
	return vec3(cos(detile.x), sin(detile.x), detile.y);
#endif
}

vec2 rotate_vec2(const vec2 v, const vec2 cs) {
	return vec2(fma(cs.x, v.x,  cs.y * v.y), fma(cs.x, v.y, -cs.y * v.x));
}

#include "res://addons/terrain_3d/shaders/includes/blend_weight_functions.gdshaderinc"

// 2-4 lookups ( 2-6 with dual scaling )
#include "res://addons/terrain_3d/shaders/includes/legacy_pbr_material_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/surface_pbr_core_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/surface_pbr_material_functions.gdshaderinc"

void accumulate_material(vec3 base_ddx, vec3 base_ddy, const mat3 TNB, const float weight, const float far_factor,
			const ivec3 index, const vec2 world_texel, const uint control, const vec2 texture_weight,
			const ivec2 texture_id, const vec3 i_normal, float h, inout material mat) {

	// Applying scaling before projection reduces the number of multiplys ops required.
	vec3 i_vertex = v_vertex;

	// Control map scale
	float control_scale = DECODE_SCALE(control);
	base_ddx *= control_scale;
	base_ddy *= control_scale;
	i_vertex *= control_scale;
	h *= control_scale;

	// World texel position for detiling. Passing it from fragment() avoids a
	// dynamic lookup into the 1,024-entry region-location array per blend.
	vec2 i_pos = world_texel * (_vertex_spacing * control_scale);

	// Projection
	vec2 i_uv = i_vertex.xz;
	vec4 i_dd = vec4(base_ddx.xz, base_ddy.xz);
	mat2 p_align = mat2(1.);
#include "res://addons/terrain_3d/shaders/includes/projection.gdshaderinc"

	// Control map rotation. Must be applied seperatley from detiling to maintain UV continuity.
	vec2 c_cs_angle = _control_rotation_cs[int(control >> 10u & 0xFu)];
	i_uv = rotate_vec2(i_uv, c_cs_angle);
	i_pos = rotate_vec2(i_pos, c_cs_angle);

	// Cooked runtime pairs are canonicalized, so material weighting must be
	// independent of which member happens to be stored first. The fourth albedo
	// component is the material blend-height term: legacy packing supplies packed
	// height, while packed-PBR/R8 supplies neutral 0.5 unless the tessellated
	// material-height path explicitly samples the displacement array.
	float sharpness = fma(60., blend_sharpness, 4.);

#include "res://addons/terrain_3d/shaders/includes/dual_scaling.gdshaderinc"

	// 1st Texture Asset ID
	if (texture_weight[0] > 0.0005 && texture_id[0] >= 0 && texture_id[0] < _texture_array_layer_count
	#include "res://addons/terrain_3d/shaders/includes/dual_scaling_condition_0.gdshaderinc"
		) {
		int id = texture_id[0];
		float id_w = texture_weight[0];
		float id_scale = _texture_uv_scale_array[id];
		vec4 id_dd = i_dd * id_scale;

		// Detiling and Control map rotation
		vec2 uv_center = floor(fma(i_pos, vec2(id_scale), vec2(0.5)));
		vec3 id_detile = get_detile_transform(uv_center, id);
		vec2 id_cs_angle = id_detile.xy;
		// Apply UV rotation and shift around pivot.
		vec2 id_uv = rotate_vec2(fma(i_uv, vec2(id_scale), -uv_center), id_cs_angle) + uv_center + id_detile.z - 0.5;
		// Manual transpose to rotate derivatives and normals counter to uv rotation whilst also
		// including control map rotation. avoids extra matrix op, and sin/cos calls.
		id_cs_angle = vec2(
			fma(id_cs_angle.x, c_cs_angle.x, -id_cs_angle.y * c_cs_angle.y),
			fma(id_cs_angle.y, c_cs_angle.x, id_cs_angle.x * c_cs_angle.y));
		// Align derivatives for correct anisotropic filtering
		id_dd.xy = rotate_vec2(id_dd.xy, id_cs_angle);
		id_dd.zw = rotate_vec2(id_dd.zw, id_cs_angle);

		vec4 alb;
		vec4 nrm;
		float ao;
		float metallic;
		sample_texture_data(id_uv, id, id_dd.xy, id_dd.zw, alb, nrm, ao, metallic);
		nrm.xz = rotate_vec2(nrm.xz, id_cs_angle) * p_align;

#include "res://addons/terrain_3d/shaders/includes/dual_scaling_mix.gdshaderinc"
		float id_weight = terrain_blend_power(weight + id_w + alb.a, sharpness) * weight;
		mat.albedo_height = fma(alb, vec4(id_weight), mat.albedo_height);
		mat.normal_rough = fma(nrm, vec4(id_weight), mat.normal_rough);
#if T3D_OUTPUT_NORMAL_MAP
		mat.normal_map_depth = fma(_texture_normal_depth_array[id], id_weight, mat.normal_map_depth);
#endif
#if T3D_OUTPUT_AO
		mat.ao = fma(ao, id_weight, mat.ao);
		mat.ao_affect = fma(_texture_ao_affect_array[id], id_weight, mat.ao_affect);
#endif
		mat.metallic = fma(metallic, id_weight, mat.metallic);
		mat.total_weight += id_weight;
	}

	// 2nd Texture Asset ID
	if (texture_weight[1] > 0.0005 && texture_id[1] != texture_id[0] &&
		texture_id[1] >= 0 && texture_id[1] < _texture_array_layer_count
#include "res://addons/terrain_3d/shaders/includes/dual_scaling_condition_1.gdshaderinc"
		) {
		int id = texture_id[1];
		float id_w = texture_weight[1];
		float id_scale = _texture_uv_scale_array[id];
		vec4 id_dd = i_dd * id_scale;

		// Detiling and Control map rotation
		vec2 uv_center = floor(fma(i_pos, vec2(id_scale), vec2(0.5)));
		vec3 id_detile = get_detile_transform(uv_center, id);
		vec2 id_cs_angle = id_detile.xy;
		// Apply UV rotation and shift around pivot.
		vec2 id_uv = rotate_vec2(fma(i_uv, vec2(id_scale), -uv_center), id_cs_angle) + uv_center + id_detile.z - 0.5;
		// Manual transpose to rotate derivatives and normals counter to uv rotation whilst also
		// including control map rotation. avoids extra matrix op, and sin/cos calls.
		id_cs_angle = vec2(
			fma(id_cs_angle.x, c_cs_angle.x, -id_cs_angle.y * c_cs_angle.y),
			fma(id_cs_angle.y, c_cs_angle.x, id_cs_angle.x * c_cs_angle.y));
		// Align derivatives for correct anisotropic filtering
		id_dd.xy = rotate_vec2(id_dd.xy, id_cs_angle);
		id_dd.zw = rotate_vec2(id_dd.zw, id_cs_angle);

		vec4 alb;
		vec4 nrm;
		float ao;
		float metallic;
		sample_texture_data(id_uv, id, id_dd.xy, id_dd.zw, alb, nrm, ao, metallic);
		nrm.xz = rotate_vec2(nrm.xz, id_cs_angle) * p_align;

#include "res://addons/terrain_3d/shaders/includes/dual_scaling_mix.gdshaderinc"
		float id_weight = terrain_blend_power(weight + id_w + alb.a, sharpness) * weight;
		mat.albedo_height = fma(alb, vec4(id_weight), mat.albedo_height);
		mat.normal_rough = fma(nrm, vec4(id_weight), mat.normal_rough);
#if T3D_OUTPUT_NORMAL_MAP
		mat.normal_map_depth = fma(_texture_normal_depth_array[id], id_weight, mat.normal_map_depth);
#endif
#if T3D_OUTPUT_AO
		mat.ao = fma(ao, id_weight, mat.ao);
		mat.ao_affect = fma(_texture_ao_affect_array[id], id_weight, mat.ao_affect);
#endif
		mat.metallic = fma(metallic, id_weight, mat.metallic);
		mat.total_weight += id_weight;
	}
}

float get_height(const ivec3 index, const vec2 world_texel) {
	float height = sample_terrain_height(index, world_texel);
#include "res://addons/terrain_3d/shaders/includes/flat_fragment.gdshaderinc"
	return height;
}


void fragment() {
	// Recover UVs
	vec2 uv = UV;
	vec2 uv2 = UV2;

	// Lookup offsets, ID and blend weight
	vec3 region_uv = get_index_uv(uv2);
	bool far_cached_region = false;
	int far_cached_material_id = 0;
	if (region_uv.z < 0.0) {
		ivec2 logical_map_pos = ivec2(floor(uv2)) + (_region_map_size / 2);
		far_cached_region = get_logical_region_present(logical_map_pos);
		if (!far_cached_region) {
			discard;
		}
	}
	const vec3 offsets = vec3(0, 1, 2);
	vec2 index_id = floor(uv);
	vec2 weight = fract(uv);
	vec2 invert = 1.0 - weight;
	vec4 weights = vec4(
		invert.x * weight.y, // 0
		weight.x * weight.y, // 1
		weight.x * invert.y, // 2
		invert.x * invert.y  // 3
	);

	ivec3 index[4];
	// One region-map lookup for the normal interior case; boundary pixels use
	// the original per-corner resolution to preserve cross-region blending.
	get_quad_indices(index_id, index[0], index[1], index[2], index[3]);

	vec3 base_ddx = dFdxCoarse(v_vertex);
	vec3 base_ddy = dFdyCoarse(v_vertex);
	// Calculate the effective mipmap for regionspace, and when less than 0,
	// skip all extra lookups required for bilinear blend.
	float base_footprint_sq = max(dot(base_ddx.xz, base_ddx.xz), dot(base_ddy.xz, base_ddy.xz));
	float region_mip = 0.5 * log2(max(base_footprint_sq * _vertex_density * _vertex_density, 1e-16));
#if T3D_TERRAIN_SAMPLING_MODE == 0 || T3D_TERRAIN_SAMPLING_MODE == 3 || T3D_TERRAIN_SAMPLING_MODE == 4 || T3D_TERRAIN_SAMPLING_MODE == 5
	bool bilerp = region_mip < 4.0 && any(greaterThan(ivec4(index[0].z, index[1].z, index[2].z, index[3].z), ivec4(-1)));
#elif T3D_TERRAIN_SAMPLING_MODE == 1
	const bool bilerp = true;
#else
	const bool bilerp = false;
#endif

	// Terrain normals. Compile-time normal modes prevent unused height taps from
	// surviving in low-cost variants.
	vec3 index_normal[4];
	float h[4] = float[4](v_vertex.y, v_vertex.y, v_vertex.y, v_vertex.y);
	float u = 0.0;
	float v = 0.0;

#include "res://addons/terrain_3d/shaders/includes/world_noise_fragment.gdshaderinc"

	vec3 w_normal = vec3(0.0, 1.0, 0.0);
#if T3D_TERRAIN_NORMAL_MODE == 2
	// Geometry derivative: no fragment height-map reads.
	w_normal = normalize(cross(dFdyCoarse(v_vertex), dFdxCoarse(v_vertex)));
	index_normal[0] = w_normal;
	index_normal[1] = w_normal;
	index_normal[2] = w_normal;
	index_normal[3] = w_normal;
#elif T3D_TERRAIN_NORMAL_MODE == 3
	// Compute-derived normal cache: one filtered regional lookup.
	if (region_uv.z > -1.0) {
		vec2 encoded_xz = textureLod(_derived_maps, region_uv, 0.0).rg;
		vec2 normal_xz = encoded_xz * 2.0 - 1.0;
		float normal_y = sqrt(max(1.0 - dot(normal_xz, normal_xz), 0.0));
		w_normal = normalize(vec3(normal_xz.x, normal_y, normal_xz.y));
	}
	index_normal[0] = w_normal;
	index_normal[1] = w_normal;
	index_normal[2] = w_normal;
	index_normal[3] = w_normal;
#else
	// Three height taps provide the near-corner terrain normal.
	h[3] = get_height(index[3], index_id + offsets.xx);
	h[2] = get_height(index[2], index_id + offsets.yx);
	h[0] = get_height(index[0], index_id + offsets.xy);
	index_normal[3] = normalize(vec3(h[3] - h[2] + u, _vertex_spacing, h[3] - h[0] + v));
	w_normal = index_normal[3];
	index_normal[0] = w_normal;
	index_normal[1] = w_normal;
	index_normal[2] = w_normal;
#endif

	// Adjust derivatives for mipmap bias and depth blur effect.
	float bias = mix(mipmap_bias,
		depth_blur + 1.,
		smoothstep(0.0, 1.0, (v_vertex_xz_dist - bias_distance) * DIV_1024));
	base_ddx *= bias;
	base_ddy *= bias;
	float tier_lod_scale = exp2(_tier_material_lod_bias);
	base_ddx *= tier_lod_scale;
	base_ddy *= tier_lod_scale;

	vec4 color_map = COLOR_MAP_DEF;
	if (region_uv.z >= 0.0) {
#if T3D_COLOR_SAMPLING_MODE == 1
		color_map = sample_terrain_color(region_uv, region_mip);
#elif T3D_COLOR_SAMPLING_MODE == 0
		color_map = terrain_color_filter_crosses_region(uv2) ?
			sample_terrain_color_exact(uv2) : sample_terrain_color(region_uv, region_mip);
#else
		color_map = sample_terrain_color_exact(uv2);
#endif
	}

#if T3D_TERRAIN_SAMPLING_MODE == 0 || T3D_TERRAIN_SAMPLING_MODE == 3 || T3D_TERRAIN_SAMPLING_MODE == 4 || T3D_TERRAIN_SAMPLING_MODE == 5
	if (bilerp) {
#elif T3D_TERRAIN_SAMPLING_MODE == 1
	{
#endif
#if T3D_TERRAIN_SAMPLING_MODE != 2
#if T3D_TERRAIN_NORMAL_MODE == 0 || T3D_TERRAIN_NORMAL_MODE == 1
		// Eight-tap height reconstruction is retained only for adaptive/quality
		// normal modes.
		h[1] = get_height(index[1], index_id + offsets.yy);
		float h_4 = get_height(get_offset_index(index_id, index[3], ivec2(1, 2)), index_id + offsets.yz);
		float h_5 = get_height(get_offset_index(index_id, index[3], ivec2(2, 1)), index_id + offsets.zy);
		float h_6 = get_height(get_offset_index(index_id, index[3], ivec2(2, 0)), index_id + offsets.zx);
		float h_7 = get_height(get_offset_index(index_id, index[3], ivec2(0, 2)), index_id + offsets.xz);
		index_normal[0] = normalize(vec3(h[0] - h[1] + u, _vertex_spacing, h[0] - h_7 + v));
		index_normal[1] = normalize(vec3(h[1] - h_5 + u, _vertex_spacing, h[1] - h_4 + v));
		index_normal[2] = normalize(vec3(h[2] - h_6 + u, _vertex_spacing, h[2] - h[1] + v));
		w_normal = normalize(index_normal[0] * weights[0] + index_normal[1] * weights[1] +
			index_normal[2] * weights[2] + index_normal[3] * weights[3]);
#endif
	}
#endif
	// Detailed regional color is optional outside the physical near cache. The
	// coarse atlas preserves authored macro tint and roughness across far LODs.
	if (region_uv.z < 0.0) {
		color_map = sample_far_color(uv);
	}
	if (far_cached_region) {
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && !far_surface_is_solid(color_map.a)) {
			discard;
		}
		// Preserve dominant material identity while the detailed page streams.
		far_cached_material_id = decode_far_material_id(color_map.a);
		color_map.a = decode_far_surface_alpha(color_map.a);
	}

#if T3D_PROJECTION_MODE == 1
	// Shared projection basis: all material corners use one fragment normal.
	index_normal[0] = w_normal;
	index_normal[1] = w_normal;
	index_normal[2] = w_normal;
	index_normal[3] = w_normal;
#endif

	vec3 w_tangent = normalize(cross(w_normal, vec3(0.0, 0.0, 1.0)));
	vec3 w_binormal = normalize(cross(w_normal, w_tangent));
	mat3 TNB = mat3(w_tangent, w_normal, w_binormal);

	// Apply terrain normals. Static modes compile the public uniform branch away.
#if T3D_TERRAIN_NORMAL_MODE == 0
	if (flat_terrain_normals) {
		NORMAL = normalize(cross(dFdyCoarse(VERTEX), dFdxCoarse(VERTEX)));
		TANGENT = normalize(cross(NORMAL, VIEW_MATRIX[2].xyz));
		BINORMAL = normalize(cross(NORMAL, TANGENT));
	} else {
		NORMAL = mat3(VIEW_MATRIX) * w_normal;
		TANGENT = mat3(VIEW_MATRIX) * w_tangent;
		BINORMAL = mat3(VIEW_MATRIX) * w_binormal;
	}
#elif T3D_TERRAIN_NORMAL_MODE == 2
	NORMAL = normalize(cross(dFdyCoarse(VERTEX), dFdxCoarse(VERTEX)));
	TANGENT = normalize(cross(NORMAL, VIEW_MATRIX[2].xyz));
	BINORMAL = normalize(cross(NORMAL, TANGENT));
#else
	NORMAL = mat3(VIEW_MATRIX) * w_normal;
	TANGENT = mat3(VIEW_MATRIX) * w_tangent;
	BINORMAL = mat3(VIEW_MATRIX) * w_binormal;
#endif



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
	material mat = material(vec4(0.0), vec4(0.0), 0., 0., 0., 0., 0.);
	if (far_cached_region) {
		mat.albedo_height = vec4(sample_far_cached_material(far_cached_material_id, uv), 1.0);
		mat.normal_rough = vec4(0.0, 1.0, 0.0, 1.0);
		mat.normal_map_depth = 0.0;
		mat.ao = 1.0;
		mat.ao_affect = 0.0;
		mat.metallic = 0.0;
		mat.total_weight = 1.0;
	} else {
	// Get index control data. Static modes remove the bilerp branch.
	uvec4 control = uvec4(read_terrain_control(index[3]));
#if T3D_TERRAIN_SAMPLING_MODE == 0 || T3D_TERRAIN_SAMPLING_MODE == 3 || T3D_TERRAIN_SAMPLING_MODE == 4 || T3D_TERRAIN_SAMPLING_MODE == 5
	if (bilerp) {
		control = uvec4(
			read_terrain_control(index[0]),
			read_terrain_control(index[1]),
			read_terrain_control(index[2]),
			control[3]);
	}
#elif T3D_TERRAIN_SAMPLING_MODE == 1
	control = uvec4(
		read_terrain_control(index[0]),
		read_terrain_control(index[1]),
		read_terrain_control(index[2]),
		control[3]);
#endif

#include "res://addons/terrain_3d/shaders/includes/auto_shader.gdshaderinc"

	// Cooked controls use canonical material-pair order for coherent array access.
	ivec4 t_id[2] = {ivec4(control >> uvec4(27u) & uvec4(0x1Fu)),
		ivec4(control >> uvec4(22u) & uvec4(0x1Fu))};
	bool all_resident = all(greaterThanEqual(ivec4(index[0].z, index[1].z, index[2].z, index[3].z), ivec4(0)));
	bool same_base_layer = all(equal(t_id[0], t_id[0].xxxx));
	bool same_overlay_layer = all(equal(t_id[1], t_id[1].xxxx));
	uvec4 non_material = control & uvec4(CONTROL_NON_MATERIAL_MASK);
	bool same_transform = all(equal(non_material, non_material.xxxx));
	bool uniform_control = bilerp && all_resident && all(equal(control, control.xxxx));
	// Collapse a shared pair/transform to one material-pair evaluation.
	bool coherent_pair = bilerp && all_resident && same_base_layer && same_overlay_layer && same_transform;

	// Texture weights
	ivec2 texture_ids[4] = ivec2[4](
		ivec2(t_id[0].x, t_id[1].x),
		ivec2(t_id[0].y, t_id[1].y),
		ivec2(t_id[0].z, t_id[1].z),
		ivec2(t_id[0].w, t_id[1].w));

	// uninterpolated weights.
	vec4 weights_id_1 = vec4(control >> uvec4(14u) & uvec4(0xFFu)) * DIV_255;
	vec4 weights_id_0 = 1.0 - weights_id_1;
	vec2 t_weights[4] = vec2[4](
				vec2(weights_id_0[0], weights_id_1[0]),
				vec2(weights_id_0[1], weights_id_1[1]),
				vec2(weights_id_0[2], weights_id_1[2]),
				vec2(weights_id_0[3], weights_id_1[3]));
	// Interpolate corner controls only in the adaptive transition path or the
	// always-smooth quality path. Single-sample variants compile this out.
	vec2 coherent_pair_weights = t_weights[3];
#if T3D_TERRAIN_SAMPLING_MODE != 2
	if (coherent_pair) {
		float overlay_weight = dot(weights_id_1, weights);
		coherent_pair_weights = vec2(1.0 - overlay_weight, overlay_weight);
	} else if (bilerp && !uniform_control) {
		t_weights = {vec2(0), vec2(0), vec2(0), vec2(0)};
		weights_id_0 *= weights;
		weights_id_1 *= weights;
		for (int i = 0; i < 4; i++) {
			vec2 w_0 = vec2(weights_id_0[i]);
			vec2 w_1 = vec2(weights_id_1[i]);
			ivec2 id_0 = texture_ids[i].xx;
			ivec2 id_1 = texture_ids[i].yy;
			t_weights[0] += fma(w_0, vec2(equal(texture_ids[0], id_0)), w_1 * vec2(equal(texture_ids[0], id_1)));
			t_weights[1] += fma(w_0, vec2(equal(texture_ids[1], id_0)), w_1 * vec2(equal(texture_ids[1], id_1)));
			t_weights[2] += fma(w_0, vec2(equal(texture_ids[2], id_0)), w_1 * vec2(equal(texture_ids[2], id_1)));
			t_weights[3] += fma(w_0, vec2(equal(texture_ids[3], id_0)), w_1 * vec2(equal(texture_ids[3], id_1)));
		}
	}
#endif

	float far_factor = 0.0;
#include "res://addons/terrain_3d/shaders/includes/dual_scaling_factor.gdshaderinc"

	// Struct storage was initialized before the far-field branch so the cheap
	// path and detailed path converge on the same output code.

#include "res://addons/terrain_3d/shaders/includes/terrain_material_sampling.gdshaderinc"
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
	float distant_normal_amplifier = clamp(sqrt(base_footprint_sq), 1., distant_normal_scale);
	mat.normal_map_depth *= distant_normal_amplifier;
#endif

	#include "res://addons/terrain_3d/shaders/includes/macro_variation.gdshaderinc"

#if T3D_OUTPUT_ROUGHNESS || T3D_OUTPUT_SPECULAR
	// Wetness is evaluated only when roughness or specular output consumes it.
	float wetness = fma(color_map.a, -2., 1.) * _tier_wetness_contribution;
#endif
#if T3D_OUTPUT_ROUGHNESS
	float roughness = clamp(mat.normal_rough.a - wetness, 0., 1.);
#endif
#if T3D_OUTPUT_SPECULAR
	// Suppress dry specular on non-light-facing terrain, then restore wet sky response.
	float terrain_facing_light = clamp(dot(w_normal, _light_direction), 0.0, 1.0);
	float specular = 1. - mat.normal_rough.a;
	specular *= terrain_facing_light;
	specular = clamp(specular + wetness, 0., 1.);
#endif

	// Apply PBR
#include "res://addons/terrain_3d/shaders/includes/output_albedo.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/output_albedo_grey.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/output_roughness.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/output_specular.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/output_specular_none.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/output_normal_map.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/output_ambient_occlusion.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/output_metallic.gdshaderinc"
#endif

#include "res://addons/terrain_3d/shaders/includes/editor_fragment.gdshaderinc"

}

#if T3D_XBOX_360_AUTHENTIC_LIGHTING
// Approximate the common sixth/seventh-generation terrain response: diffuse-heavy
// Lambert lighting, a compact Blinn-Phong highlight, baked AO, and no metallic BRDF.
// This is intentionally simpler than Godot's GGX path and uses the packed roughness
// channel only as a gloss mask. The material SPECULAR output stays at zero to
// suppress the modern reflection path; this loop supplies the fixed direct highlight.
void light() {
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	float wrapped_diffuse = clamp((ndotl + 0.12) / 1.12, 0.0, 1.0);
	DIFFUSE_LIGHT += LIGHT_COLOR * wrapped_diffuse * ATTENUATION / PI;
	if (ROUGHNESS < 0.999) {
		vec3 half_vector = normalize(LIGHT + VIEW);
		float gloss = clamp(1.0 - ROUGHNESS, 0.0, 1.0);
		float nh = max(dot(NORMAL, half_vector), 0.0);
		// Fixed-power lobes avoid variable-exponent pow().
		float nh2 = nh * nh;
		float nh4 = nh2 * nh2;
		float nh8 = nh4 * nh4;
		float nh16 = nh8 * nh8;
		float nh32 = nh16 * nh16;
		float highlight = mix(nh8, nh32, gloss);
		float specular_strength = mix(0.035, 0.22, gloss);
		SPECULAR_LIGHT += LIGHT_COLOR * highlight * specular_strength * ATTENUATION / PI;
	}
}
#endif
#endif // T3D_TEMPLATE_CONTEXT
