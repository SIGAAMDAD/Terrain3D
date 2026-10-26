// Compatibility fallback for legacy shader call sites. Runtime material image
// residency is owned by sparse VT V1; there is no secondary streamed array.
vec3 t3d_runtime_average_material(const int logical_id, const vec3 tint) {
	return tint;
}
