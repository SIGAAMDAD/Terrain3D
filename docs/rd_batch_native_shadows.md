# Batch F3 — Native Forward+ shadows

F3 routes Terrain3D terrain, regular instances, and HLOD into Godot 4.7.1 Forward+ native directional-shadow draw lists. Godot owns the shadow pass/atlas/list; Terrain3D retains its own mesh, material, visibility, uniform, and indirect resources.

## ABI 3

ABI 3 extends the F2 synchronous PREPARE/SUBMIT bridge with exact shadow-pass state: light instance/type, cascade/pass index, combined light view-projection, caster/light masks, bias/range metadata, atlas rect/size, reverse-cull state, and directional-shadow resources. PREPARE remains before `draw_list_begin`; SUBMIT remains inside the native draw list.

## Terrain

Terrain uses the established depth shader for shadow submission so displacement, holes, clipmap stitching, and silhouette match the depth surface. Each shadow pass is culled against the exact light VP. Existing global and near/middle/far shadow LOD/enabled settings filter casters before indirect upload.

## Instances and HLOD

Shadow packets reuse the existing RD mesh/material cache and active published LOD/HLOD state. Shadow visibility and indirect commands use dedicated persistent buffers so cascades cannot overwrite main depth/opaque visibility. Alpha-scissor materials retain shader discard behavior; pure alpha blend is excluded unless the surface explicitly participates in the alpha depth prepass. Layer/caster masks and single/double/front-face material culling are respected.

## Native proxy clean break

For every RenderingDevice production backend, `Terrain3DMesher::setRdPresentation(false, false)` is enforced. Public proxy settings remain for compatibility, but cannot reactivate visual or shadow proxy geometry. RenderingServer backend behavior is unchanged.

F3 does not claim F4 shadow receiving/clustered lighting or later motion/GI/lifecycle work.
