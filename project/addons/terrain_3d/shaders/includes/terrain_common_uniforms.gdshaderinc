// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Geometry/page-table state shared by near, middle, far, and shadow shaders.
uniform vec3 _target_pos = vec3(0.0);
uniform float _mesh_size = 48.0;
uniform float _subdiv = 1.0;
uniform float _vertex_spacing = 1.0;
uniform float _vertex_density = 1.0;
uniform float _region_size = 1024.0;
uniform float _region_texel_size = 0.0009765625;
uniform vec2 _height_decode = vec2(-384.0, 1152.0);
uniform int _region_map_size = 32;
uniform highp sampler2D _far_height_map : filter_linear, repeat_disable;
