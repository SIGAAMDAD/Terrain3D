// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

uint read_terrain_control(const ivec3 index) {
#if T3D_CONTROL_MAPS_IMPLICIT
	return 1u;
#else
	return index.z >= 0 ? floatBitsToUint(texelFetch(_control_maps, index, 0).r) : 1u;
#endif
}
