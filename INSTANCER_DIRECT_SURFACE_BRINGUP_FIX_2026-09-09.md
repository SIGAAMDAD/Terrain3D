# Terrain3D Instancer Direct-Surface Bring-Up Fix — 2026-09-09

## Build contract

Native/script build ID:

`instancer-direct-surface-bringup-2026-09-09`

This patch replaces the previous "minimal" instance renderer with an actually isolated raster bring-up path. The previous path still depended on the production shared mesh arenas, imported material texture synchronization, indirect-command buffers, GPU-cull resources, HLOD/shadow support and production descriptor layouts. Those dependencies made a failed bring-up ambiguous.

## Minimal instance path after this patch

```text
authoritative Terrain3D instance store
    -> LOD0 CPU mesh snapshot
    -> exact standalone GPU vertex/index buffer per surface
    -> exact vertex/index array per surface
    -> packed instance records
    -> CPU/reference visible-index list
    -> Forward+ opaque callback
    -> plain indexed draw_list_draw()
    -> compile-time bring-up shader
    -> solid magenta fragment output
```

The bring-up path does not depend on:

- shared mesh vertex/index arenas;
- indirect draw commands;
- GPU instance culling;
- HLOD descriptors/culling;
- shadow or motion-history instance buffers;
- imported material textures;
- material table/set 1;
- native Forward+ lighting/set 2;
- alpha/scissor routing;
- transparent/depth/shadow/motion instance passes;
- depth testing or face culling.

## Direct surface GPU storage

`Terrain3DRdMeshCache` now owns a minimal-only `t3dRdDirectSurfaceGpu_t` per published surface. Each record owns:

- exact surface vertex buffer;
- vertex-array view over that exact buffer;
- exact surface index buffer;
- index-array view over that exact buffer.

Minimal `Terrain3DRdMeshCache::upload()` returns through this path before texture synchronization, shared arena allocation, shared array reconstruction or arena dirty-range uploads. This is intentional isolation, not the production storage design.

Resource teardown is child-before-parent. Shared-arena replacement also destroys array views before replacing their parent buffers, preventing stale tracked RIDs after Godot invalidates dependent views.

## Dedicated bring-up shader ABI

`terrain_instances.glsl` now provides the `bringup` version with `T3D_INSTANCE_BRINGUP`.

The compiled bring-up descriptor ABI contains only the resources the raster test needs:

- set 0 / binding 0: frame data;
- set 0 / binding 1: instance records;
- set 0 / binding 2: visible indices.

Material buffers, previous-motion records, material textures and native Forward+ lighting are absent from the bring-up shader at compile time. The fragment path writes solid magenta directly.

`Terrain3DRdShaderLibrary` loads the bring-up instance shader for the minimal lane and does not require the HLOD or instance-cull compute shaders for minimal readiness.

## Direct draw submission

The minimal Forward+ opaque lane binds the packet's exact standalone vertex/index arrays and calls:

`draw_list_draw(draw_list, true, packet_instance_count, 0)`

The push constant uses the packet's `firstVisible` value so `gl_InstanceIndex` addresses the CPU-uploaded visible-index list without base-instance/indirect semantics.

Production/non-minimal code retains shared arenas, production material/light bindings and indexed-indirect submission.

## Upload/runtime dependency stripping

Minimal scene upload now provisions only:

- frame UBO;
- instance-record SSBO;
- visible-index SSBO;
- CPU packet/reference storage required to build direct draws.

It no longer requires indirect buffers, shadow indirect buffers, HLOD descriptor buffers, GPU-cull descriptor buffers or previous-motion storage.

Minimal visibility upload writes only the visible-index buffer.

## Preview path

Editor mesh preview uses the same standalone direct-surface path in minimal mode. It skips material upload and uses ordinary indexed draws with depth/culling disabled. Preview framebuffer and pipeline creation/registration failure paths now directly free an unregistered RID instead of leaving a retry hazard in the shared RD context.

## Retry/resource lifetime repairs

Additional fault-line repairs include:

- child vertex/index arrays are destroyed before shared arena parent buffers;
- HLOD/instance-cull shader and pipeline retries no longer overwrite a surviving tracked shader RID after a pipeline-only failure;
- cull uniform-set registration failure directly frees the unregistered RID;
- preview framebuffer/pipeline registration failure directly frees the unregistered RID.

These changes prevent an initial failure from manufacturing a second shared-context ownership failure on retry.

## Editor Jolt isolation

While `T3D_INSTANCER_MINIMAL_RENDER_PATH` is active, instance placement does not call `PhysicsDirectSpaceState3D` from the editor GUI-input path. It uses Terrain3D's own height/normal sampler instead. This removes the Jolt `Space state is inaccessible right now` / `Invalid PhysicsDirectSpaceState3D` error from the bring-up placement path.

Physics-aware placement remains a production feature and is only bypassed for this diagnostic renderer.

## Verification completed

C++17 front-end syntax checks passed in both configurations for all five touched translation units:

- debug/tools: `GDEXTENSION + DEBUG_ENABLED + TOOLS_ENABLED`;
- release-style: `GDEXTENSION`.

Translation units:

- `src/rendering/rd/terrain_3d_rd_mesh_cache.cpp`;
- `src/rendering/rd/terrain_3d_rd_instance_renderer.cpp`;
- `src/rendering/rd/terrain_3d_render_coordinator.cpp`;
- `src/instances/terrain_3d_instancer.cpp`;
- `src/rendering/rd/terrain_3d_rd_shader_library.cpp`.

Only the pre-existing generated godot-cpp `INT64_MIN` warning was emitted.

18 focused cumulative regression verifiers passed:

1. `verify_instancer_direct_surface_bringup.py`
2. `verify_instancer_minimal_render_path.py`
3. `verify_instance_fault_reporting_texture_fallback.py`
4. `verify_instance_indirect_buffer_usage.py`
5. `verify_instance_forward_submission.py`
6. `verify_instance_first_population_render.py`
7. `verify_instance_pipeline_recovery.py`
8. `verify_instance_preview_and_lazy_forward_fix.py`
9. `verify_p0_forward_viewport_lifecycle.py`
10. `verify_p1_forward_lane_diagnostics.py`
11. `verify_p2_forward_pass_resource_prerequisites.py`
12. `verify_p09_gpu_indirect_visibility.py`
13. `verify_p11_instance_gpu_culling.py`
14. `verify_script_native_contract.py`
15. `verify_gpu_page_tier_nondestructive.py`
16. `verify_p10_vt_streaming.py`
17. `verify_systems_c_style.py`
18. `verify_systems_c_ownership.py`

Shader conditional-balance inspection passed. No `glslangValidator`, `glslc`, `spirv-val`, or Godot runtime executable is available in this container. `tools/format_shaders.py --check` reports pre-existing formatting differences in three unrelated clipmap/VT files and does not report `terrain_instances.glsl`.

## Runtime expectation

The expected visible result with the build ID above is solid magenta instance geometry. That result deliberately proves only:

- authoritative instance publication;
- standalone mesh GPU upload;
- instance-record upload;
- visible-index upload;
- Forward+ opaque callback;
- vertex transform and raster submission.

It does not depend on the production material, texture, lighting, culling, HLOD or indirect-draw systems. No runtime viewport smoke test was possible in the container, so runtime visibility must still be confirmed in the source-matched Godot 4.7.2 editor.
