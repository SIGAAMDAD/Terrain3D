// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Shared clipmap placement prelude. Included at the start of vertex().
vec2 clipmap_vertex_metadata = UV2;
vec4 clipmap_lod_state = terrain3d_clipmap_lod_state(clipmap_vertex_metadata.y);
vec2 clipmap_stitch_state = terrain3d_clipmap_stitch_state(clipmap_lod_state.w);
vec3 clipmap_grid_vertex = VERTEX;
clipmap_grid_vertex.xz += terrain3d_clipmap_edge_offset(clipmap_vertex_metadata.x, clipmap_stitch_state);
VERTEX.xz = clipmap_grid_vertex.xz * clipmap_lod_state.z + clipmap_lod_state.xy;
