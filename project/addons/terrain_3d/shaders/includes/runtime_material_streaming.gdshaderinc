uniform bool _runtime_material_streaming_active = false;
uniform vec4 _runtime_material_average_albedo[32];

vec3 t3d_runtime_average_material(const int logical_id, const vec3 tint) {
	return _runtime_material_average_albedo[clamp(logical_id, 0, 31)].rgb * tint;
}
