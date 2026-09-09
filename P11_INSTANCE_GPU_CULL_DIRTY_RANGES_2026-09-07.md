# P11 Instance Dirty Publication, GPU Culling, and Motion-Range Update

Date: 2026-09-07

## Lineage

This is a selective forward-port of the P11 portion of `Terrain3D_P09_P10_P11_Performance_128m_Memory_Patched_2026-09-04.zip` onto the repaired cumulative P01-P10 September 7 tree. P01-P10 and the September 5-7 startup, ownership, authoring, page-cache, and RenderWorld fixes remain authoritative wherever the older cumulative archive differs.

## Implemented P11 contract

- Instance scene publication now carries owner-known dirty instance ranges, dirty cell indices, and an explicit topology-change bit.
- Stable-topology scene uploads update only the published dirty ranges instead of rediscovering changes by comparing every instance record with `memcmp`.
- Stable-topology motion history advances only changed ranges while topology changes retain the full remap/reference path.
- Production Forward+ instance visibility uses a compute cull shader that writes visible-index buffers and indexed-indirect instance counts on the GPU.
- CPU packet/frustum visibility remains the explicit direct-draw/debug reference path.
- Equivalent main-view GPU visibility work is reused across compatible passes in the same render frame.
- Instance cull descriptors are retained in a fixed publication-time GPU buffer and dirty cells update only their descriptor spans.

## Compatibility repair relative to the old P11 archive

The old P11 archive added dirty metadata to `t3dRdInstanceSceneSnapshot_t` but its coordinator cold-capacity service still reserved only instance/cell payload arrays. This port extends the owner capacity checks and `serviceColdCapacity()` storage establishment to reserve dirty-range and dirty-cell backing for cached scenes and publication banks as part of the same explicit cold boundary.

The current coordinator's post-P11 terrain-primary companion-renderer behavior is preserved. P11 only adds the instance-cull shader argument to the existing instance and preview renderer initialization paths; it does not replace the newer terrain/startup recovery logic with the older cumulative coordinator.

## Validation target

`tools/verify_p11_instance_gpu_culling.py` verifies the P11 ownership, GPU cull, indirect draw, debug/reference split, motion-range, shader-library, and coordinator-capacity contracts. Existing P01-P10/regression verifiers remain the authoritative guards for earlier patches.

## Validation completed in this patch workspace

- `28/28` repository `tools/verify_*.py` contract/regression gates pass.
- `verify_p11_instance_gpu_culling.py` passes.
- `verify_systems_c_style.py` and `verify_systems_c_ownership.py` pass after replacing the old P11 renderer's local `auto` lambdas with explicit member helpers.
- `clang++ -std=c++17 -fsyntax-only` passes for:
  - `src/rendering/rd/terrain_3d_rd_instance_renderer.cpp`
  - `src/rendering/rd/terrain_3d_rd_shader_library.cpp`
  - `src/rendering/rd/terrain_3d_render_coordinator.cpp`
  - `src/instances/terrain_3d_instancer.cpp`
  - `src/hlod/terrain_3d_hlod_streamer.cpp`
- SCons, a Godot executable, and `glslangValidator` are not installed in this sandbox, so a full GDExtension link, Godot import/runtime smoke test, and engine-side compute-shader compilation were not executable here.
