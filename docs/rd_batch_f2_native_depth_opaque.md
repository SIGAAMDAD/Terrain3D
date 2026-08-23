# Batch F2 — Native Forward+ depth and opaque submission

## Status

F2 moves production terrain, regular-instance, and HLOD depth/opaque raster submission from the diagnostic compositor timing path into Godot 4.7.1 Forward+ native draw lists. Terrain3D keeps ownership of its vertex/index/storage/uniform/indirect resources; Godot keeps ownership of the native pass and draw-list lifetime.

## ABI 2 PREPARE / SUBMIT split

RenderingDevice does not permit buffer updates, compute dispatches, or persistent pipeline creation while a draw list is active. ABI 2 therefore invokes the same synchronous Terrain3D callback twice at each selected native pass:

1. **PREPARE**, immediately before `draw_list_begin`: update frame/scene buffers, perform CPU/GPU culling, update HLOD indirect counts, and create any missing framebuffer-compatible cached pipelines.
2. **SUBMIT**, while Godot's native draw list is open: bind existing Terrain3D pipelines/descriptors/mesh buffers and issue direct/indirect draws only.

There is no deferred queue and no frame of latency between phases.

## Native pipeline compatibility

Terrain and instance/HLOD pipeline caches are keyed by native framebuffer format, Forward+ shader/output variant, sample count, view count, reverse-cull state, and depth-prepass state. Shader variants cover color, optional specular/motion attachments, depth-only, depth+normal/roughness, and depth+normal/roughness+VoxelGI attachment signatures.

Main native depth follows Godot 4.7.1 reversed-Z: `GREATER_OR_EQUAL`. When the opaque color pass follows an active depth prepass, pipelines use `EQUAL` with depth writes disabled. The legacy self-owned compositor/debug pipelines are isolated and were not globally rewritten.

## Geometry participation

Terrain depth uses the same near/middle/far clipmap topology, patch records, height/displacement inputs, hole/background discard behavior, stitching, and indirect culling stream as visible terrain. The depth shader preserves middle-composite solid coverage before returning through its depth-only variant.

Regular instances and HLOD use the existing shared RD mesh/material cache and the same sorted draw-packet/indirect stream. Opaque surfaces participate in depth and opaque color. Alpha-scissored materials keep their material discard behavior. Alpha-blended surfaces are excluded from opaque color; surfaces explicitly requesting alpha depth prepass still enter depth with the existing depth-prepass threshold flag.

## F2 evidence

- bridge ABI/types: `src/rendering/rd/terrain_3d_rd_types.h`, `terrain_3d_rd_forward_plus_bridge.*`;
- native terrain prepare/submit/cache: `terrain_3d_rd_clipmap_renderer.*`;
- native regular-instance/HLOD prepare/submit/cache: `terrain_3d_rd_instance_renderer.*`;
- routing: `terrain_3d_render_coordinator.*`;
- versioned raw RD shaders: `project/addons/terrain_3d/rd/terrain_clipmap_{near,middle,far}.glsl`, `terrain_instances.glsl`;
- engine PREPARE/SUBMIT hook: `engine_patch/godot-4.7.1-terrain3d-forward-plus.patch`;
- regression gate: `tools/rd.py` (`rd_batch_f2_native_depth_opaque_check`).

F2 does not claim F3 shadows, F4 clustered/local lighting, F5 real motion vectors, F6 GI/decal/probe parity, or F7 multiview/lifecycle completion.

## Native depth-consumer compatibility

Normal/roughness depth variants transform Terrain3D world-space normals into the active camera view space before encoding them into Godot Forward+ normal/roughness attachments. This matches the coordinate space expected by native depth consumers while retaining the existing Terrain3D material roughness and alpha-scissor behavior.
