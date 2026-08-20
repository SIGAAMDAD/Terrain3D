// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Private uniforms shared by all visible terrain tiers.
uniform uint _mouse_layer = 0x80000000u; // Layer 32
uniform highp sampler2D _far_color_map : source_color, filter_nearest, repeat_disable;
