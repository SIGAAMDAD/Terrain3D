// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DISPLACEMENT_UNIFORMS
uniform float _displacement_scale : hint_range(0.0, 2.0, 0.01) = 1.0;
uniform highp sampler2D _displacement_buffer : repeat_disable, filter_linear;
#endif
