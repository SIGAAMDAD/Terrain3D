# Terrain3D Instance Pipeline Recovery / Depth Fallback Fix — 2026-09-08

## Basis

This patch is applied on top of `Terrain3D_P01_P12_2026-09-07_rd_initial_texture_upload_fixed.zip`.
It does not modify the GDScript API, editor scripts, or native API revision/build-id contract.

## Problems fixed

### 1. Companion instance renderer could remain permanently disabled

`Terrain3DRenderCoordinator::initializeRendererOnRenderThread()` previously returned immediately once the terrain renderer was initialized. If the instance renderer failed during that first initialization attempt, `m_bRendererInitialized` became true while `m_bInstanceRendererInitialized` remained false, and the instance renderer was never retried.

The coordinator now treats terrain, instances/HLOD, and ocean as independent initialization lanes. The instance renderer is retried at most once per render frame while unavailable. Failed partial initialization is shut down before the next retry so stale RD resources are not retained. Diagnostics now expose initialization attempts, failures, and recoveries.

### 2. GPU packet fade state could encode zero visibility

GPU packet topology marked fade-capable packets with `T3D_RD_INSTANCE_FADE_ACTIVE` before a real camera distance was known, but left the encoded 16-bit fade field at zero. The fragment shader interprets that value as zero visibility and can discard the complete surface.

GPU topology now initializes active fade packets to full visibility (`0xffff`) and the Forward+ draw path always rewrites an explicit fade value from a 1.0 default. This prevents an active fade flag from ever implicitly meaning invisible.

### 3. CPU reference/direct-debug draw addressed the wrong arena slice

The `TERRAIN3D_RD_INSTANCE_DIRECT_DRAW=1` diagnostic lane used `draw_list_draw()` while a global mesh/index arena was bound. That call cannot express each packet's `firstIndex` and `vertexOffset`, so non-zero-offset surfaces could draw the wrong geometry or nothing.

The diagnostic mode now means **CPU visibility/reference culling** while submission still uses the packet's CPU-populated indirect command. This preserves exact arena addressing and still bypasses P11 GPU visibility compute, which is the useful isolation boundary.

### 4. Native depth-prepass flag can no longer make instances disappear without an instance depth submit

Opaque/motion instance pipelines used `COMPARE_OP_EQUAL` whenever the engine bridge reported a native depth prepass. That is only valid if Terrain3D instances successfully submitted their own depth for the same scene pass.

The coordinator now records the last successful instance depth submit. If an opaque/motion PREPARE arrives with `m_bDepthPrepassActive=true` but no successful Terrain3D instance depth submit exists for that scene pass, the instance lane receives an effective context with `m_bDepthPrepassActive=false`. This selects the normal depth-tested color pipeline instead of an equality test against depth the instances never wrote.

A sparse warning is emitted when this fallback is used:

`[RD instances] Native depth prepass active without a successful instance depth submit ...; using depth-tested color fallback`

### 5. Pipeline-stage diagnostics

`getRdRendererDiagnostics()` now exposes enough state to locate the next failing boundary in one run.

Coordinator gauges/counters include:

- `published_instance_records`
- `published_instance_cells`
- `published_instance_scene_generation`
- `published_mesh_lods`
- `published_mesh_surfaces`
- `published_mesh_vertex_bytes`
- `published_mesh_index16_bytes`
- `published_mesh_index32_bytes`
- `published_mesh_generation`
- `instance_initialization_attempts`
- `instance_initialization_failures`
- `instance_initialization_recoveries`
- `instance_depth_prepare_attempts`
- `instance_depth_prepare_successes`
- `instance_depth_submit_attempts`
- `instance_depth_submit_successes`
- `instance_opaque_prepare_attempts`
- `instance_opaque_prepare_successes`
- `instance_opaque_submit_attempts`
- `instance_opaque_submit_successes`
- `instance_depth_prepass_fallbacks`
- `instance_opaque_depth_prepass_requested_prepares`
- `instance_opaque_depth_prepass_used_prepares`
- `instance_last_successful_depth_scene_pass`

Instance-renderer gauges/counters include:

- `instance_mesh_lods`
- `instance_mesh_surfaces`
- `instance_packet_build_input_cells`
- `instance_resolved_mesh_cells`
- `instance_unresolved_mesh_cells`
- `instance_draw_packets`
- `depth_packets`
- `instance_gpu_cull_descriptors`
- `instance_depth_submitted_draws`
- `instance_opaque_submitted_draws`
- `instance_shadow_submitted_draws`
- `instance_motion_submitted_draws`
- `instance_transparent_submitted_draws`
- `instance_cpu_visibility_reference`

## Diagnostic interpretation

For a scene with placed meshes, the useful progression is:

1. `published_instance_records > 0`
2. `published_instance_cells > 0`
3. `published_mesh_lods > 0`
4. `published_mesh_surfaces > 0`
5. `instance_renderer_initialized == true`
6. `instance_resolved_mesh_cells > 0`
7. `instance_draw_packets > 0`
8. `instance_gpu_cull_descriptors > 0` in production GPU-visibility mode
9. `instance_depth_prepare_successes > 0` and/or `instance_opaque_prepare_successes > 0`
10. `instance_depth_submitted_draws > 0` and `instance_opaque_submitted_draws > 0`

If `instance_unresolved_mesh_cells` is non-zero while published mesh LODs/surfaces are non-zero, mesh source-key resolution is the next boundary to inspect.

If opaque depth-prepass requests increase while `instance_depth_submit_successes` remains zero, the new fallback should keep color geometry renderable and `instance_depth_prepass_fallbacks` should increase.

For GPU-cull isolation, set `TERRAIN3D_RD_INSTANCE_DIRECT_DRAW=1`. In this patch that mode uses CPU visibility/reference culling but preserves correct indirect arena submission.

## Verification

- 35/35 `tools/verify_*.py` regression gates pass.
- Added `tools/verify_instance_pipeline_recovery.py`.
- Updated older verification gates only where exact-text assumptions needed to accept the new effective instance context / independent terrain initialization semantics.
- `clang++ -std=c++17 -DGDEXTENSION -DDEBUG_ENABLED -DTOOLS_ENABLED -fsyntax-only` passes all 11 translation units that directly include the modified coordinator/instance/mesh-cache headers.
- Only existing generated `INT64_MIN` warnings are emitted by `godot-cpp`.
- No GDScript files were changed.

## Runtime limitation

The environment used for this patch does not contain SCons, a Godot executable, or `glslangValidator`, so a linked GDExtension build and live Godot 4.7.2 Forward+ smoke test were not possible here.
