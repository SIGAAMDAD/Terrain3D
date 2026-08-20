// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

float decode_terrain_height(const float encoded) {
	return fma(encoded, _height_decode.y, _height_decode.x);
}
