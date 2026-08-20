// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

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
		depth_blur + 1.0,
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
		uint far_surface_bits = decode_far_surface_bits(color_map.a);
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && !far_surface_bits_is_solid(far_surface_bits)) {
			discard;
		}
		// Preserve dominant material identity while the detailed page streams.
		far_cached_material_id = far_surface_bits_material_id(far_surface_bits);
		color_map.a = far_surface_bits_alpha(far_surface_bits);
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
