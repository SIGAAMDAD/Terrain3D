// Logical Terrain3D material IDs map directly to static editor/legacy arrays.
// Cooked runtime material residency is owned exclusively by sparse VT V3.
int t3d_mat_layer(const int logical_id) {
	return clamp(logical_id, 0, 31);
}

bool t3d_mat_resident(const int logical_id) {
	return logical_id >= 0 && logical_id < 32;
}
