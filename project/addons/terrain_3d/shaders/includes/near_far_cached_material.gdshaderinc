vec3 sample_far_cached_material(const int texture_id, const vec2 world_texel) {
	if (_texture_array_layer_count <= 0) return vec3(1.0);
	int id = clamp(texture_id, 0, _texture_array_layer_count - 1);
	vec3 tint = _texture_color_array[id].rgb;
	if (!t3d_mat_resident(id)) return t3d_runtime_average_material(id, tint);
	vec2 material_uv = world_texel * (_vertex_spacing * _texture_uv_scale_array[id]);
	float material_lod = min(4.0, max(_material_max_lod, 0.0));
	return textureLod(_texture_array_albedo, vec3(material_uv, float(t3d_mat_layer(id))), material_lod).rgb * tint;
}
