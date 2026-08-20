// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "res://addons/terrain_3d/shaders/includes/blend_weight_functions.gdshaderinc"

// 2-4 material lookups; 2-6 when dual scaling is active.
#include "res://addons/terrain_3d/shaders/includes/legacy_pbr_material_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/surface_pbr_core_functions.gdshaderinc"
#include "res://addons/terrain_3d/shaders/includes/surface_pbr_material_functions.gdshaderinc"

void accumulate_material(
	vec3 base_ddx,
	vec3 base_ddy,
	const mat3 TNB,
	const float weight,
	const float far_factor,
	const ivec3 index,
	const vec2 world_texel,
	const uint control,
	const vec2 texture_weight,
	const ivec2 texture_id,
	const vec3 i_normal,
	float h,
	inout material mat) {

	// Apply control scaling before projection so the projection path does less arithmetic.
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
	mat2 p_align = mat2(1.0);
#include "res://addons/terrain_3d/shaders/includes/projection.gdshaderinc"

	// Apply control-map rotation separately from detiling to preserve UV continuity.
	vec2 c_cs_angle = _control_rotation_cs[int(control >> 10u & 0xFu)];
	i_uv = rotate_vec2(i_uv, c_cs_angle);
	i_pos = rotate_vec2(i_pos, c_cs_angle);

	// Cooked runtime pairs are canonicalized, so material weighting must be
	// independent of which member happens to be stored first. The fourth albedo
	// component is the material blend-height term: legacy packing supplies packed
	// height, while packed-PBR/R8 supplies neutral 0.5 unless the tessellated
	// material-height path explicitly samples the displacement array.
	float sharpness = fma(60.0, blend_sharpness, 4.0);

#include "res://addons/terrain_3d/shaders/includes/dual_scaling.gdshaderinc"

	// Base texture asset.
#ifdef T3D_ENABLE_DUAL_SCALING_CONDITION_0
	if (texture_weight[0] > 0.0005 && texture_id[0] >= 0 && texture_id[0] < _texture_array_layer_count &&
		!(far_factor == 1.0 && texture_id[0] == dual_scale_texture)) {
#else
	if (texture_weight[0] > 0.0005 && texture_id[0] >= 0 && texture_id[0] < _texture_array_layer_count) {
#endif
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
		// Compose detile and control rotations without building another matrix or recomputing sin/cos.
		id_cs_angle = vec2(
			fma(id_cs_angle.x, c_cs_angle.x, -id_cs_angle.y * c_cs_angle.y),
			fma(id_cs_angle.y, c_cs_angle.x, id_cs_angle.x * c_cs_angle.y));
		// Align derivatives for anisotropic filtering.
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

	// Overlay texture asset.
#ifdef T3D_ENABLE_DUAL_SCALING_CONDITION_1
	if (texture_weight[1] > 0.0005 && texture_id[1] != texture_id[0] &&
		texture_id[1] >= 0 && texture_id[1] < _texture_array_layer_count &&
		!(far_factor == 1.0 && texture_id[1] == dual_scale_texture)) {
#else
	if (texture_weight[1] > 0.0005 && texture_id[1] != texture_id[0] &&
		texture_id[1] >= 0 && texture_id[1] < _texture_array_layer_count) {
#endif
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
		// Compose detile and control rotations without building another matrix or recomputing sin/cos.
		id_cs_angle = vec2(
			fma(id_cs_angle.x, c_cs_angle.x, -id_cs_angle.y * c_cs_angle.y),
			fma(id_cs_angle.y, c_cs_angle.x, id_cs_angle.x * c_cs_angle.y));
		// Align derivatives for anisotropic filtering.
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
