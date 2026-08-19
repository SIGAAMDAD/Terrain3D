#ifdef T3D_TEMPLATE_CONTEXT
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx, skip_vertex_transform;


#define COLOR_MAP_DEF vec4(1.0, 1.0, 1.0, 0.5)
#define DECODE_BLEND(control) float(control >> 14u & 0xFFu) * 0.003921568627450
#define DECODE_BASE(control) int(control >> 27u & 0x1Fu)
#define DECODE_OVER(control) int(control >> 22u & 0x1Fu)
#define DECODE_HOLE(control) bool(control >> 2u & 0x1u)
#define DECODE_AUTO(control) bool(control & 0x1u)
#define DECODE_SCALE(control) (0.9 - float(((control >> 7u & 0x7u) + 3u) % 8u + 1u) * 0.1)
#define fma(a, b, c) ((a) * (b) + (c))

#include "res://addons/terrain_3d/shaders/includes/texture_samplers_linear_anisotropic.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_linear.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_nearest_anisotropic.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/texture_samplers_nearest.gdshaderinc"

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
uniform sampler2D _region_map_texture : filter_nearest, repeat_disable;
uniform sampler2D _logical_region_map_texture : filter_nearest, repeat_disable;
uniform sampler2D _middle_height_region_map_texture : filter_nearest, repeat_disable;
uniform highp sampler2DArray _middle_height_maps : filter_linear, repeat_disable;
#if !T3D_CONTROL_MAPS_IMPLICIT
uniform highp sampler2DArray _control_maps : repeat_disable;
#endif
#if T3D_TERRAIN_SAMPLING_MODE != 2 && !T3D_COLOR_MAPS_IMPLICIT
uniform highp sampler2DArray _color_maps : source_color, FILTER_METHOD, repeat_disable;
#endif
uniform highp sampler2DArray _derived_maps : FILTER_METHOD, repeat_disable;
uniform highp sampler2D _far_height_map : filter_linear, repeat_disable;
uniform highp sampler2D _far_color_map : source_color, filter_nearest, repeat_disable;
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
#if T3D_AUTO_SHADER_ENABLED
group_uniforms shader_uniforms.auto_shader;
uniform float auto_slope : hint_range(0, 10) = 1.0;
uniform float auto_height_reduction : hint_range(0, 1) = 0.1;
uniform int auto_base_texture : hint_range(0, 31) = 0;
uniform int auto_overlay_texture : hint_range(0, 31) = 1;
#endif
group_uniforms;

#include "res://addons/terrain_3d/shaders/includes/flat_uniforms.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_uniforms.gdshaderinc"

varying vec3 v_vertex;
varying vec3 v_main_camera_position;
varying float v_vertex_xz_dist;
varying vec3 v_middle_normal;

float decode_terrain_height(float encoded) {
	return fma(encoded, _height_decode.y, _height_decode.x);
}

int get_region_layer(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return -1;
	}
	return int(round(texelFetch(_region_map_texture, map_pos, 0).r * 65535.0)) - 1;
}

int get_middle_height_layer(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return -1;
	}
	return int(round(texelFetch(_middle_height_region_map_texture, map_pos, 0).r * 65535.0)) - 1;
}

bool get_logical_region_present(const ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return false;
	}
	return texelFetch(_logical_region_map_texture, map_pos, 0).r > 0.5;
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
	ivec2 atlas_coord = clamp(ivec2(floor(atlas_uv * vec2(atlas_size))), ivec2(0), atlas_size - ivec2(1));
	return texelFetch(_far_color_map, atlas_coord, 0);
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

vec3 sample_texture_tint(const int texture_id) {
#if T3D_TERRAIN_SAMPLING_MODE == 2
	return vec3(1.0);
#else
	if (texture_id < 0 || texture_id >= _texture_color_count) {
		return vec3(1.0);
	}
	return _texture_color_array[texture_id].rgb;
#endif
}

vec3 missing_texture_checker(const vec2 world_texel) {
	vec2 cell = floor(world_texel * 0.125);
	float checker = mod(cell.x + cell.y, 2.0);
	return mix(vec3(0.24), vec3(0.42), checker);
}

vec2 rotate_compact_uv(const vec2 uv, const vec2 cs) {
#if T3D_DETILING_MODE == 2
	return uv;
#else
	return vec2(cs.x * uv.x + cs.y * uv.y, cs.x * uv.y - cs.y * uv.x);
#endif
}

vec3 sample_middle_material(const int texture_id, const vec2 world_texel, const float control_scale, const vec2 control_rotation) {
#if T3D_TERRAIN_SAMPLING_MODE == 2
	return vec3(1.0);
#else
	if (_texture_array_layer_count <= 0) {
		return missing_texture_checker(world_texel);
	}
	int id = clamp(texture_id, 0, _texture_array_layer_count - 1);
	float asset_scale = _texture_uv_scale_array[id];
	vec3 tint = sample_texture_tint(id);
	if (!t3d_mat_resident(id)) {
		return t3d_runtime_average_material(id, tint);
	}
	vec2 world_xz = world_texel * _vertex_spacing;
	vec2 uv = rotate_compact_uv(world_xz * control_scale, control_rotation) * asset_scale;
	return texture(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), _tier_material_lod_bias).rgb * tint;
#endif
}

vec3 sample_middle_far_material(const int texture_id, const vec2 world_texel) {
#if T3D_TERRAIN_SAMPLING_MODE == 2
	return vec3(1.0);
#else
	if (_texture_array_layer_count <= 0) {
		return missing_texture_checker(world_texel);
	}
	int id = clamp(texture_id, 0, _texture_array_layer_count - 1);
	vec3 tint = sample_texture_tint(id);
	if (_runtime_material_streaming_active) {
		return t3d_runtime_average_material(id, tint);
	}
	vec2 uv = world_texel * (_vertex_spacing * _texture_uv_scale_array[id]);
	float material_lod = clamp(max(2.0, 3.0 + _tier_material_lod_bias), 0.0, max(_material_max_lod, 0.0));
	return textureLod(_texture_array_albedo, vec3(uv, float(t3d_mat_layer(id))), material_lod).rgb * tint;
#endif
}

vec3 sample_far_normal(const vec2 world_texel, const float sample_step) {
	float h0 = sample_far_height(world_texel);
	float hx = sample_far_height(world_texel + vec2(sample_step, 0.0));
	float hz = sample_far_height(world_texel + vec2(0.0, sample_step));
	float world_step = sample_step / max(_vertex_density, 1e-6);
	return normalize(vec3(h0 - hx, world_step, h0 - hz));
}

uint read_terrain_control(const ivec3 index) {
#if T3D_CONTROL_MAPS_IMPLICIT
	return 1u;
#else
	return index.z >= 0 ? floatBitsToUint(texelFetch(_control_maps, index, 0).r) : 1u;
#endif
}

#include "res://addons/terrain_3d/shaders/includes/none_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/flat_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_functions.gdshaderinc"

ivec3 get_index_coord(const vec2 uv) {
	vec2 rounded_uv = round(uv);
	ivec2 map_pos = ivec2(floor(rounded_uv * _region_texel_size)) + (_region_map_size / 2);
	return ivec3(ivec2(mod(rounded_uv, _region_size)), get_region_layer(map_pos));
}

void get_quad_indices(const vec2 base, out ivec3 i00, out ivec3 i10, out ivec3 i01, out ivec3 i11) {
	i00 = get_index_coord(base);
	int last_texel = int(_region_size) - 1;
	if (i00.z >= 0 && i00.x < last_texel && i00.y < last_texel) {
		i10 = ivec3(i00.xy + ivec2(1, 0), i00.z);
		i01 = ivec3(i00.xy + ivec2(0, 1), i00.z);
		i11 = ivec3(i00.xy + ivec2(1, 1), i00.z);
	} else {
		i10 = get_index_coord(base + vec2(1.0, 0.0));
		i01 = get_index_coord(base + vec2(0.0, 1.0));
		i11 = get_index_coord(base + vec2(1.0, 1.0));
	}
}

vec3 get_index_uv(const vec2 uv2) {
	vec2 region_pos = floor(uv2);
	ivec2 map_pos = ivec2(region_pos) + (_region_map_size / 2);
	return vec3(uv2 - region_pos, float(get_region_layer(map_pos)));
}

#if T3D_TERRAIN_SAMPLING_MODE != 2
#include "res://addons/terrain_3d/shaders/includes/middle_color_functions.gdshaderinc"
#endif

float sample_terrain_height(const ivec3 index, const vec2 world_texel) {
	vec2 rounded_uv = round(world_texel);
	ivec2 map_pos = ivec2(floor(rounded_uv * _region_texel_size)) + (_region_map_size / 2);
	int height_layer = get_middle_height_layer(map_pos);
	if (height_layer >= 0) {
		ivec2 local_coord = ivec2(mod(rounded_uv, _region_size));
		ivec2 height_size = textureSize(_middle_height_maps, 0).xy;
		if (height_size.x == int(_region_size) && height_size.y == int(_region_size)) {
			return decode_terrain_height(texelFetch(_middle_height_maps, ivec3(local_coord, height_layer), 0).r);
		}
		vec2 reduced_texel = vec2(local_coord) * vec2(height_size) / _region_size;
		vec2 reduced_uv = (reduced_texel + vec2(0.5)) / vec2(height_size);
		return decode_terrain_height(textureLod(_middle_height_maps, vec3(reduced_uv, float(height_layer)), 0.0).r);
	}
	return sample_far_height(world_texel);
}

float interpolated_height(const vec2 pos) {
	vec2 base = floor(pos);
	ivec3 i00;
	ivec3 i10;
	ivec3 i01;
	ivec3 i11;
	get_quad_indices(base, i00, i10, i01, i11);
	vec2 f = fract(pos);
	float h00 = sample_terrain_height(i00, base);
	float h10 = sample_terrain_height(i10, base + vec2(1.0, 0.0));
	float h01 = sample_terrain_height(i01, base + vec2(0.0, 1.0));
	float h11 = sample_terrain_height(i11, base + vec2(1.0, 1.0));
	return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
}


#include "res://addons/terrain_3d/shaders/includes/clipmap_batching_functions.gdshaderinc"

#include "res://addons/terrain_3d/shaders/includes/editor_setup.gdshaderinc"
void vertex() {
	vec2 clipmap_vertex_metadata=UV2;
	vec4 clipmap_lod_state = terrain3d_clipmap_lod_state(clipmap_vertex_metadata.y);
	vec2 clipmap_stitch_state = terrain3d_clipmap_stitch_state(clipmap_lod_state.w);
	vec3 clipmap_grid_vertex = VERTEX;
	clipmap_grid_vertex.xz += terrain3d_clipmap_edge_offset(clipmap_vertex_metadata.x, clipmap_stitch_state);
	VERTEX.xz = clipmap_grid_vertex.xz * clipmap_lod_state.z + clipmap_lod_state.xy;
	v_main_camera_position=MAIN_CAM_INV_VIEW_MATRIX[3].xyz;
	v_vertex = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_vertex_xz_dist=length(v_vertex.xz-_target_pos.xz);

	float scale=clipmap_lod_state.z;
	float inv_scale=1.0/scale;
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

	ivec3 region = get_index_coord(start_pos);
	uint control = read_terrain_control(region);
	bool hole = DECODE_HOLE(control);
	if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && (hole
#include "res://addons/terrain_3d/shaders/includes/none_check.gdshaderinc"
	)) {
		v_vertex.x = 0.0 / 0.0;
		v_middle_normal = vec3(0.0, 1.0, 0.0);
	} else {
		float h;
		if (scale < _vertex_spacing) {
			h = interpolated_height(UV);
		} else {
			ivec3 coord_b = get_index_coord(end_pos);
			h = mix(sample_terrain_height(region, start_pos), sample_terrain_height(coord_b, end_pos), vertex_lerp);
		}
#include "res://addons/terrain_3d/shaders/includes/flat_vertex.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/world_noise_vertex.gdshaderinc"
		v_vertex.y = h;

		vec3 region_uv = get_index_uv(UV2);
#if T3D_TERRAIN_NORMAL_MODE == 2
		v_middle_normal = vec3(0.0, 1.0, 0.0);
#else
		if (region_uv.z >= 0.0) {
			vec2 encoded_xz = textureLod(_derived_maps, region_uv, 0.0).rg;
			vec2 normal_xz = encoded_xz * 2.0 - 1.0;
			float normal_y = sqrt(max(1.0 - dot(normal_xz, normal_xz), 0.0));
			v_middle_normal = normalize(vec3(normal_xz.x, normal_y, normal_xz.y));
		} else {
			float normal_step = max(1.0, scale * _vertex_density);
			v_middle_normal = sample_far_normal(UV, normal_step);
		}
#endif
	}
	VERTEX = (VIEW_MATRIX * vec4(v_vertex, 1.0)).xyz;
}

void fragment() {
	vec2 fragment_uv = UV;
	vec2 fragment_uv2 = UV2;
	vec3 region_uv = get_index_uv(fragment_uv2);
	bool logical_region_present = region_uv.z >= 0.0;
	if (region_uv.z < 0.0) {
		ivec2 logical_map_pos = ivec2(floor(fragment_uv2)) + (_region_map_size / 2);
		logical_region_present = get_logical_region_present(logical_map_pos);
#include "res://addons/terrain_3d/shaders/includes/middle_none_fragment.gdshaderinc"
	}
#if T3D_TERRAIN_SAMPLING_MODE == 2
	vec4 macro_surface = region_uv.z >= 0.0 ? vec4(1.0) : sample_far_color(fragment_uv);
#else
	vec4 macro_surface = region_uv.z >= 0.0 ? sample_terrain_color(fragment_uv2, region_uv) : sample_far_color(fragment_uv);
#endif
#if T3D_TERRAIN_NORMAL_MODE == 2
	NORMAL = normalize(cross(dFdyCoarse(VERTEX), dFdxCoarse(VERTEX)));
#else
	NORMAL = normalize(mat3(VIEW_MATRIX) * v_middle_normal);
#endif
#if T3D_TEXTURE_SURFACE_MODE == 1
	if (region_uv.z >= 0.0) {
		ivec3 control_index = get_index_coord(floor(fragment_uv));
		uint control = read_terrain_control(control_index);
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && DECODE_HOLE(control)) {
			discard;
		}
	} else if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && logical_region_present &&
		!far_surface_is_solid(macro_surface.a)) {
		discard;
	}
	ALBEDO = silhouette_color.rgb * macro_surface.rgb;
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
	AO = 1.0;
#else
	vec3 surface_material = vec3(1.0);
	float surface_alpha = macro_surface.a;

#if T3D_TERRAIN_SAMPLING_MODE == 2
	if (region_uv.z >= 0.0) {
		vec4 baked_surface = texture(_middle_composite_maps, region_uv);
		uint baked_surface_bits = uint(round(clamp(baked_surface.a, 0.0, 1.0) * 255.0));
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && (baked_surface_bits & 0x80u) == 0u) {
			discard;
		}
		ALBEDO = baked_surface.rgb;
		surface_alpha = float(baked_surface_bits & 0x7Fu) * (1.0 / 127.0);
	} else {
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && logical_region_present &&
			!far_surface_is_solid(macro_surface.a)) {
			discard;
		}
		ALBEDO = macro_surface.rgb;
	}
#else
	if (region_uv.z >= 0.0) {
		ivec3 control_index = get_index_coord(floor(fragment_uv));
		uint control = read_terrain_control(control_index);
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && DECODE_HOLE(control)) {
			discard;
		}
		int base_id = DECODE_BASE(control);
		int overlay_id = DECODE_OVER(control);
		float blend = DECODE_BLEND(control);
#if T3D_AUTO_SHADER_ENABLED
		if (DECODE_AUTO(control)) {
			int auto_layer_max = max(_texture_color_count - 1, 0);
			base_id = clamp(auto_base_texture, 0, auto_layer_max);
			overlay_id = clamp(auto_overlay_texture, 0, auto_layer_max);
			blend = clamp(fma(auto_slope * 2.0, (normalize(v_middle_normal).y - 1.0), 1.0)
				- auto_height_reduction * 0.01 * v_vertex.y, 0.0, 1.0);
		}
#endif
		float control_scale = DECODE_SCALE(control);
		vec2 control_rotation = _control_rotation_cs[int(control >> 10u & 0xFu)];
		vec3 base_material;
#if T3D_TERRAIN_SAMPLING_MODE == 2
		if (overlay_id != base_id && blend >= 0.5) {
			base_id = overlay_id;
		}
		base_material = sample_middle_material(base_id, fragment_uv, control_scale, control_rotation);
		surface_material = base_material;
#else
		base_material = sample_middle_material(base_id, fragment_uv, control_scale, control_rotation);
		if (overlay_id != base_id && blend > 0.001) {
			vec3 overlay_material = sample_middle_material(overlay_id, fragment_uv, control_scale, control_rotation);
			// Middle's optimized albedo array is BC1/RGB-style. Its alpha channel is not
			// displacement, so use the cooked control weight directly rather than
			// advertising or approximating a height-aware blend.
			float transition = smoothstep(.12, .88, blend);
			surface_material = mix(base_material, overlay_material, transition);
		} else {
			surface_material = base_material;
		}
#endif
	} else {
		bool coarse_surface_present = far_surface_is_solid(macro_surface.a);
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && logical_region_present && !coarse_surface_present) {
			discard;
		}
		surface_alpha = coarse_surface_present ? decode_far_surface_alpha(macro_surface.a) : 0.5;
		int coarse_material_id = coarse_surface_present ? decode_far_material_id(macro_surface.a) : 0;
		surface_material = sample_middle_far_material(coarse_material_id, fragment_uv);
	}
#endif

	float wetness = fma(surface_alpha, -2.0, 1.0) * _tier_wetness_contribution;
	float wet_amount = clamp(wetness, 0.0, 1.0);
#if T3D_TERRAIN_SAMPLING_MODE != 2
	vec3 authored_color = mix(vec3(1.0), macro_surface.rgb, _tier_authored_color_contribution);
	ALBEDO = authored_color * surface_material * mix(1.0, 0.86, wet_amount);
#else
	ALBEDO *= mix(1.0, 0.86, wet_amount);
#endif
#if T3D_SAMPLE_SURFACE_DATA
	ROUGHNESS = clamp(0.9 - wetness, 0.0, 1.0);
#else
	ROUGHNESS = mix(1.0, 0.82, wet_amount);
#endif
	SPECULAR = 0.0;
	AO = 1.0;
#endif
#include "res://addons/terrain_3d/shaders/includes/editor_fragment.gdshaderinc"

}

#if T3D_SHADING_MODE == 2
void light() {
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	float wrapped_diffuse = clamp((ndotl + 0.12) / 1.12, 0.0, 1.0);
	DIFFUSE_LIGHT += LIGHT_COLOR * wrapped_diffuse * ATTENUATION / PI;
}
#elif T3D_SHADING_MODE == 5
void light() {
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	float wrapped_diffuse = clamp((ndotl + 0.12) / 1.12, 0.0, 1.0);
	DIFFUSE_LIGHT += LIGHT_COLOR * wrapped_diffuse * ATTENUATION / PI;
	vec3 half_vector = normalize(LIGHT + VIEW);
	float nh = max(dot(NORMAL, half_vector), 0.0);
	float nh2 = nh * nh;
	float nh4 = nh2 * nh2;
	float nh8 = nh4 * nh4;
	float nh16 = nh8 * nh8;
	float gloss = clamp(1.0 - ROUGHNESS, 0.0, 1.0);
	float highlight = mix(nh8, nh16, gloss);
	SPECULAR_LIGHT += LIGHT_COLOR * highlight * mix(0.025, 0.16, gloss) * ATTENUATION / PI;
}
#endif
#endif // T3D_TEMPLATE_CONTEXT
