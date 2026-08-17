// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_FAR_NONE_FRAGMENT
if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer) && !far_surface_is_solid(far_surface.a)) {
	discard;
}
#endif
