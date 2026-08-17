// Logical Terrain3D material IDs are stable authored values. Runtime material
// streaming remaps them onto a smaller physical Texture2DArray working set.
uniform int _texture_layer_map[32];

int t3d_mat_layer(const int logical_id) {
	return max(_texture_layer_map[clamp(logical_id, 0, 31)], 0);
}

bool t3d_mat_resident(const int logical_id) {
	return _texture_layer_map[clamp(logical_id, 0, 31)] >= 0;
}
