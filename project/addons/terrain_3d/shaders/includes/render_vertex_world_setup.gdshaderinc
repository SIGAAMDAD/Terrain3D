// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Shared world-space setup for render tiers. Included after clipmap placement.
v_main_camera_position = MAIN_CAM_INV_VIEW_MATRIX[3].xyz;
v_vertex = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
v_vertex_xz_dist = length(v_vertex.xz - _target_pos.xz);
