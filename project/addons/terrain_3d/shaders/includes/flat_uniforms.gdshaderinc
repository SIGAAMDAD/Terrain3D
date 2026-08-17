// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_FLAT_UNIFORMS
uniform float ground_level : hint_range(-1000., 1000.) = 0.0;
uniform float region_blend : hint_range(.001, 1., 0.001) = 0.25;
#endif
