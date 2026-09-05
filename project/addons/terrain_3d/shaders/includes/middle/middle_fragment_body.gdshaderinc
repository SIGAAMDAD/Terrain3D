// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

	vec2 fragment_uv = UV;
	vec2 fragment_uv2 = UV2;
	vec3 region_uv = get_index_uv(fragment_uv2);
	bool logical_region_present = region_uv.z >= 0.0;
	if (region_uv.z < 0.0) {
		ivec2 logical_map_pos = ivec2(floor(fragment_uv2)) + T3D_REGION_MAP_HALF;
		logical_region_present = get_logical_region_present(logical_map_pos);
#include "res://addons/terrain_3d/shaders/includes/middle_none_fragment.gdshaderinc"
	}
#if T3D_TERRAIN_SAMPLING_MODE == 2
	vec4 macro_surface = region_uv.z >= 0.0 ? vec4(1.0) : sample_far_color(fragment_uv);
#else
	vec4 macro_surface = region_uv.z >= 0.0 ?
		sample_terrain_color(fragment_uv2, region_uv) : sample_far_color(fragment_uv);
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
			float transition = smoothstep(0.12, 0.88, blend);
			surface_material = mix(base_material, overlay_material, transition);
		} else {
			surface_material = base_material;
		}
#endif
	} else {
		uint far_surface_bits = decode_far_surface_bits(macro_surface.a);
		bool coarse_surface_present = far_surface_bits_is_solid(far_surface_bits);
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && logical_region_present && !coarse_surface_present) {
			discard;
		}
		surface_alpha = coarse_surface_present ? far_surface_bits_alpha(far_surface_bits) : 0.5;
		int coarse_material_id = coarse_surface_present ? far_surface_bits_material_id(far_surface_bits) : 0;
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
