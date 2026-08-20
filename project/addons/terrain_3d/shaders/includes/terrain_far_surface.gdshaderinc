// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

uint decode_far_surface_bits(const float packed_alpha) {
	return uint(round(clamp(packed_alpha, 0.0, 1.0) * 255.0));
}

bool far_surface_bits_is_solid(const uint bits) {
	return (bits & 0x80u) != 0u;
}

int far_surface_bits_material_id(const uint bits) {
	return int((bits >> 2u) & 0x1Fu);
}

float far_surface_bits_alpha(const uint bits) {
	return float(bits & 0x3u) * (1.0 / 3.0);
}

bool far_surface_is_solid(const float packed_alpha) {
	return far_surface_bits_is_solid(decode_far_surface_bits(packed_alpha));
}

int decode_far_material_id(const float packed_alpha) {
	return far_surface_bits_material_id(decode_far_surface_bits(packed_alpha));
}

float decode_far_surface_alpha(const float packed_alpha) {
	return far_surface_bits_alpha(decode_far_surface_bits(packed_alpha));
}
