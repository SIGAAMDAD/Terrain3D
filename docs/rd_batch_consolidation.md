# Batch F8 — Forward+ consolidation and static/compile validation

Batch F8 converges the F1–F7 Forward+ work into the canonical Batch F implementation for Godot 4.7.1. Production RenderingDevice rendering remains Terrain3D-owned and submits terrain, regular instances, and HLOD into Godot-owned native Forward+ passes through `Terrain3DForwardPlusBridge` ABI 7.

## Sections 17–22 convergence

- **17 Shadows:** native Terrain3D terrain/instance/HLOD shadow submission remains the production path. RD production keeps both native visual and shadow proxy geometry disabled.
- **18 Lighting:** native directional and clustered local-light resources, native shadow atlases, ambient/fog/AO/cavity behavior, and the regional baked-lighting path remain integrated through the persistent Forward+ lighting bindings.
- **19 Forward+ bridge:** the packaged Godot 4.7.1 patch owns the native draw list and invokes Terrain3D synchronously for PREPARE/SUBMIT. Terrain3D owns its buffers, textures, mesh/material data, pipelines, and descriptors.
- **20 Motion:** current/previous per-view state and native velocity output remain integrated for terrain, instances, and HLOD, including history reset and clipmap-recenter rules.
- **21 Depth:** terrain, instances, HLOD, and alpha-scissored materials use the supplied native depth/opaque lists with reversed-Z Forward+ pipeline state.
- **22 Lifecycle:** two-view state, framebuffer/sample/view-count pipeline identity, resize/render-scale/MSAA changes, viewport recreation, multiple-camera history, and borrowed RID invalidation remain represented by the F7 lifecycle model.

## F8 defects corrected

1. `Terrain3DRenderCoordinator::shutdown()` no longer unconditionally restores native visual terrain. RenderingServer presentation is restored only when the selected backend is actually `T3D_RENDER_BACKEND_RENDERING_SERVER`; RD teardown/reinitialization leaves native Terrain3D proxy geometry disabled.
2. The renderer failure state is now diagnosed as a failure latch rather than a renderer fallback. A latched RD failure disables RD submission and the compositor debug lane without resurrecting native Terrain3D rendering.
3. Coordinator diagnostics now report observed native depth, opaque, shadow, and motion callbacks, bridge ABI/registration, current native view/sample/framebuffer state, clustered-light resource readiness, and borrowed scene-resource generation changes. `native_shadow_proxy_available` remains false for the RD clean-break path.
4. The runtime profiler now describes the production Forward+ route and failure-latch semantics instead of claiming compositor production submission or native-renderer restoration.

## Static source audit

The F8 audit re-ran the F1–F7 contract checks and manually reviewed Batch F-relevant occurrences of `RenderingServer`, `MultiMesh`, native shadow proxy state, `Hybrid`, `TODO`, `FIXME`, `placeholder`, `draw_list_begin`, `draw_list_end`, `LESS_OR_EQUAL`, `ForwardPlusBridge`, and compositor code.

- The production bridge submit functions do not begin/end the Godot draw list and do not allocate persistent GPU resources during SUBMIT.
- `draw_list_begin/end` in Terrain3D RD renderers belongs to the explicitly gated self-owned compositor/debug lane; the engine patch's begin/end calls remain Godot-owned around the synchronous Terrain3D SUBMIT callback.
- Conventional `LESS_OR_EQUAL` depth comparisons remain only in the separate self-owned debug/compositor raster path. Native Forward+ terrain and instance pipelines use reversed-Z `GREATER_OR_EQUAL`, or `EQUAL` when consuming an existing depth prepass.
- `TERRAIN3D_RD_DEBUG_COMPOSITOR=1` is the explicit opt-in for the compositor diagnostic lane. Bridge absence does not silently activate it or native proxy rendering.
- No `TODO`, `FIXME`, or `placeholder` marker remains in Batch F-owned RD/lighting/engine-patch source.
- Remaining repository-wide `RenderingServer`, `MultiMesh`, Hybrid, and legacy proxy API references are outside the Batch F production submission architecture and remain owned by later cleanup/parity batches where applicable; F8 does not delete public compatibility surface assigned to Batch G/H.

## Engine patch audit

`engine_patch/godot-4.7.1-terrain3d-forward-plus.patch` remains targeted at Godot `4.7.1-stable`, registers and tears down `Terrain3DForwardPlusBridge`, carries ABI 7, exposes depth/opaque/shadow/motion native pass kinds, invokes PREPARE before Godot opens the draw list, invokes SUBMIT while that native list is open, and leaves draw-list ownership with Godot. The patch is syntax-checked with `git apply --stat` as part of the F8 static contract.

## Validation scope for this F8 handoff

The requested validation scope is intentionally limited to **static audit and C++ compilation**. Runtime, GPU, shader-runtime, editor-smoke, and patched-engine execution are therefore not claimed for F8.

Compilation evidence for the supplied environment:

- all 80 Terrain3D C++ translation units were compiler-accepted with `g++`/C++17 against bindings generated from the repository's exact Godot 4.7.1 extension API;
- the retained `godot-cpp` source used for binding/header generation was compared with the supplied `godot-cpp-10.0.0-rc1` archive and all 189 common source files were byte-identical;
- F8-modified translation units are recompiled after the consolidation edits;
- SCons is not installed in the supplied execution environment and outbound package installation is unavailable, so this validation uses direct compiler invocations rather than the repository SCons driver.

This document records validation evidence only. It does not upgrade static/compile evidence into runtime or GPU evidence.
