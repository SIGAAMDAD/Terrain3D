// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DISPLACEMENT_VERTEX
		if (!(CAMERA_VISIBLE_LAYERS == _mouse_layer)) {
			displacement = mix(get_displacement(start_pos, scale), get_displacement(end_pos, scale * 2.0), vertex_lerp);
		}
#endif
