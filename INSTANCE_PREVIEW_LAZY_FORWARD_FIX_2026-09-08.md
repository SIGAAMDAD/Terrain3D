# Terrain3D Instance Preview / Lazy Forward Capability Fix

Date: 2026-09-08

## Scope

This patch is intentionally conservative. It repairs only source-proven failure gates found during the end-to-end instance/alpha-scissor investigation. It does not bypass GPU culling, change authored transforms, alter alpha-scissor thresholds, change visibility layers, introduce a CPU rendering fallback, or rewrite instance publication.

## Fixed defects

### 1. RD preview clip-depth mismatch

The editor preview built an orthographic projection in Godot/OpenGL-style clip depth (-1..1), while the RenderingDevice preview/culling path interpreted depth as 0..1. Valid preview geometry could therefore be rejected/clipped before rasterization.

The preview now applies `Projection::set_depth_correction(false, false, true)` and uses the same corrected view-projection for both CPU packet culling and rasterization. Reverse-Z is intentionally disabled because the preview pipeline uses conventional `LESS_OR_EQUAL` depth testing and clears depth to 1.0.

### 2. Preview initialization depended on unrelated runtime capabilities

The preview renderer previously used the full runtime `initialize()` path, which required Forward+ instance variants, HLOD compute, and runtime instance-cull compute even though preview cards use only the base raster/mesh/material path.

A dedicated `initializePreview()` now requires only the shared instance raster shader/device. The coordinator preview readiness check likewise depends on the base instance shader rather than global shader-library readiness.

### 3. Normal live instances depended on HLOD compute at initialization

The runtime instance renderer previously refused to initialize if the HLOD cull shader/pipeline could not be created, even for scenes containing only regular instances. HLOD compute is now retained as source SPIR-V and instantiated only when HLOD descriptors actually exist.

### 4. Every Forward+ instance shader RID was required up front

The runtime instance renderer previously created every Forward+ shader RID during initialization. Failure of one unused variant could disable all instance rendering.

The Forward+ SPIR-V bundle is now retained and the selected variant is materialized on demand during the corresponding PREPARE pass. A bad variant therefore fails its own pass instead of preventing unrelated variants from rendering.

## New diagnostics

`Terrain3DRdInstanceRenderer::getDiagnostics()` now includes:

- `instance_forward_shader_ready_mask`
- `hlod_cull_shader_instantiated`

These show which Forward+ shader RIDs have actually been materialized and whether HLOD compute has been instantiated.

## Explicitly not changed

The following remain unchanged pending runtime evidence:

- logical instance -> render-cell materialization
- immutable coordinator publication
- mesh source-key resolution
- GPU instance-cull visibility rules
- indirect command layout or submission
- alpha-scissor cutoff values
- material cull mode / double-sided policy
- transform packing/unpacking
- visibility-layer masks
- CPU fallback rendering

Use the existing P1 diagnostics to locate the first zero/failure in the live path before modifying these systems.

## Validation

- 44/44 `tools/verify_*.py` scripts pass.
- New `verify_instance_preview_and_lazy_forward_fix.py` passes.
- `terrain_3d_rd_instance_renderer.cpp` passes `clang++ -fsyntax-only` with GDExtension debug/tools defines.
- `terrain_3d_render_coordinator.cpp` passes the same syntax check.
- Release-style syntax check also passes.
- Only the pre-existing generated `godot-cpp` `INT64_MIN` literal warning remains.
