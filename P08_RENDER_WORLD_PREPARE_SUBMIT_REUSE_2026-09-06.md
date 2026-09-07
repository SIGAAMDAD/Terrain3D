# P08 RenderWorld Prepare/Submit Reuse

## Scope

P08 splits RenderWorld's main-camera work into a pass-independent compute preparation stage and a pass-specific raster submission stage. The purpose is to avoid re-running visibility, Hi-Z/VT request work, retire servicing, and bounded VT upload admission when equivalent OPAQUE and MOTION callbacks describe the same prepared view.

## Contract

`Terrain3DRenderWorld::prepare_camera_visibility_hot()` owns pass-independent work:

- retired VT-slot servicing;
- bounded VT upload service;
- GPU visibility dispatch;
- Hi-Z preparation/dispatch when enabled;
- VT feedback request/readback scheduling;
- compute-prepare telemetry.

`Terrain3DRenderWorld::submit_camera_pass_hot()` owns pass-local work:

- obtaining the current three-tier raster bindings;
- raster draw submission to the callback's draw list;
- draw-count and raster-submit telemetry.

The compatibility wrapper `render_main_camera_hot()` remains for callers that require the complete prepare+submit transaction.

## Prepared-view reuse key

The runtime caches the prepared compute state only when all view/state generations match:

- render frame;
- viewport key;
- visibility-frame hash;
- scene publication generation;
- terrain publication generation;
- culling/resource generation;
- view count.

Pass kind and draw-list identity are intentionally excluded. They belong to raster submission, not compute visibility preparation.

A scene release invalidates the prepared view immediately. Scene/terrain/resource changes change the corresponding generation and therefore prevent stale reuse.

## Compatibility with the current P01-P07 line

This integration is layered on top of the repaired P06/P07 contracts rather than replacing them. It preserves:

- P06 source-owned publication generations;
- P07 exact GPU page accounting and elapsed-time upload admission;
- Editor Authoring live-page fallback for missing/stale/incompatible cooked pages;
- the later adjustable memory budgets and height-minimum render behavior;
- runtime-page authoring cook layout fixes.

P08 does not alter runtime-page formats, terrain-region serialization, GPU-page capacity policy, or ClassDB/GDScript method signatures.

## Performance effect

For equivalent OPAQUE/MOTION callbacks in the same view, compute preparation can be reused while each callback still receives an independent raster submission. This removes redundant visibility/VT/Hi-Z work without merging pass-local draw semantics.

The hot path adds only fixed-size stack state, integer/hash comparisons, and counters. It does not add dynamic allocation or a new container owner.

## Diagnostics

`Terrain3DRenderCoordinator::getDiagnostics()` now exposes:

- `render_world_compute_prepares`;
- `render_world_compute_reuses`;
- `render_world_raster_submits`.

These make the optimization measurable at runtime instead of inferred from code structure.

## Compatibility audit of the supplied P06/P07/P08 archive

The supplied `Terrain3D_P06_P07_P08_Performance_128m_Memory_Patched_ShaderFix_2026-09-05.zip` was not merged wholesale. Its P08-specific contract is concentrated in the RenderWorld prepare/submit split, while several other files in that archive predate the current repaired P06/P07 line.

The integration therefore intentionally imports the P08 RenderWorld changes and telemetry but retains the current tree's newer behavior for:

- P07 Editor Authoring live fallback instead of the supplied archive's older fail-closed prepared-page rule;
- exact resident-layer capacity/grant arithmetic repaired after the original P07/P08 archive;
- adjustable GPU budgets and non-destructive render-tier policy;
- pre-initialization GPU-cache load safety;
- height-minimum terrain rendering and companion-lane isolation;
- render-startup recovery;
- runtime-page legacy/incompatible recooking;
- runtime-page Authoring Middle-height divisor generation and world-edge collision handling.

This is a selective P08 forward-port, not a rollback to the September 5 P06/P07 state.
