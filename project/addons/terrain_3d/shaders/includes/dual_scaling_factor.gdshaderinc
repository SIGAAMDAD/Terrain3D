// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DUAL_SCALING_FACTOR
	far_factor = clamp(smoothstep(dual_scale_near, dual_scale_far, length(v_vertex - v_main_camera_position)), 0.0, 1.0);
#endif
