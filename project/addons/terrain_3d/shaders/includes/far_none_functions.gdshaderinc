// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_FAR_NONE_FUNCTIONS
bool is_none_bg(const vec2 uv) {
	return !far_surface_is_solid(sample_far_color(uv - vec2(0.5, 0.0)).a) ||
		!far_surface_is_solid(sample_far_color(uv - vec2(0.0, 0.5)).a) ||
		!far_surface_is_solid(sample_far_color(uv + vec2(1.0, 0.0)).a) ||
		!far_surface_is_solid(sample_far_color(uv + vec2(0.0, 1.0)).a);
}
#endif
