// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_HEIGHTMAP_SETUP
group_uniforms shader_uniforms.debug_heightmap;
uniform float heightmap_black_height: hint_range(-2048.,2048.,.5) = -100.0;
uniform float heightmap_white_height: hint_range(-2048.,2048.,.5) = 300.0;
group_uniforms;
#endif
