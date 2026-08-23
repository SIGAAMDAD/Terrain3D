# Batch F7 — Multiview and viewport/render-target lifecycle

F7 closes the remaining native Forward+ viewport state for the RenderingDevice branch. The bridge is **ABI 7**. F8 is not implemented here.

## Native multiview

The bridge carries a bounded view count (`1..2`), current and previous jittered view-projection matrices for each eye, and Godot's per-eye offsets. The raw shaders enable `GL_EXT_multiview` and select eye-specific projection, previous projection, and camera position with `gl_ViewIndex`. The common main-camera view matrix remains shared, matching Godot 4.7.1's Forward+ convention; the eye offset changes the eye position/view direction rather than creating a second world transform.

Terrain patch visibility is the union of both eye frusta. Regular instances use the same union rule. HLOD retains the existing GPU culler for a single view, while multiview uses the already-batched CPU union result so no eye can disappear because a single-eye compute dispatch overwrote the indirect count. Shared world residency and LOD state are not redundantly recomputed per eye.

## Resize, render scale, MSAA, and recreation

ABI 7 adds `render_frame`, stable `viewport_key`, and actual `sample_count`. A fixed eight-slot render-thread lifecycle table keys each native viewport by framebuffer RID, framebuffer format, render size, render region, view count, and sample count. Resize, internal render scale, MSAA changes, or native framebuffer recreation invalidate temporal history for every pass in that render frame; compatible history becomes eligible on the following frame. Cached borrowed descriptor sets are invalidated at reconfiguration while static Terrain3D mesh, texture, and material ownership is preserved.

Forward pipeline keys already include native framebuffer format, pass kind, sample state, view count, reverse culling, and depth-prepass behavior. F7 additionally cross-checks packet sample metadata against the framebuffer format and bounds each terrain/instance pipeline cache to 64 compatibility keys, preventing indefinite growth during repeated configuration changes.

## Temporal and multiple cameras

Camera temporal validity is tracked per stable viewport key. Instance/HLOD transform history is scene-global and now advances by the engine rasterizer render-frame number, not `scene_pass`; multiple cameras/passes in the same engine frame therefore cannot prematurely advance stopped-object history. Current/previous camera matrices still come directly from Godot for each viewport/camera. Reconfigured viewports emit neutral motion until compatible history exists.

PREPARE records both scene-pass and viewport identity. SUBMIT must match both, rejecting stale borrowed native state. Renderer/bridge teardown clears lifecycle slots, prepared viewport identities, and borrowed descriptor sets. The path is renderer-level and does not depend on finding a `Camera3D`, so editor and runtime Forward+ viewports use the same bridge.

## Validation contract

`tools/rd.py` contains `rd_batch_f7_multiview_lifecycle_check()`. It guards ABI 7 lifecycle fields, bounded view/sample validation, per-eye shader ABI, union culling, HLOD multiview behavior, render-frame transform history, fixed viewport state, resize/render scale/MSAA invalidation, descriptor invalidation, actual native pipeline identity, stale-submit rejection, bounded caches, teardown, and the no-allocation native SUBMIT rule.

F8 remains the later consolidation/finalization slice and is intentionally outside this batch.
