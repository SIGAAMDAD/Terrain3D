# P0 Forward+ Viewport Lifecycle Fix — 2026-09-08

## Scope

Implements the first P0 item from the Godot 4.7.2 / Terrain3D bridge audit:

> Native pass framebuffer RID/format must not participate in Terrain3D's viewport lifecycle identity.

Godot Forward+ legitimately uses different native framebuffer RIDs/formats for depth, opaque, motion, and transparent passes belonging to the same viewport. Treating those per-pass framebuffers as viewport reconfiguration caused false reconfiguration events during normal rendering.

## Source changes

### `src/rendering/rd/terrain_3d_render_coordinator.h`

Removed these fields from `t3dRdForwardViewportState_t`:

- `m_hFramebuffer`
- `m_nFramebufferFormat`

The viewport lifecycle state now contains only stable viewport/render-target configuration:

- viewport key;
- render size;
- render region;
- view count;
- sample count;
- lifecycle timestamps/validity.

### `src/rendering/rd/terrain_3d_render_coordinator.cpp`

`prepareForwardLifecycle()` no longer compares or stores the native pass framebuffer RID/format when deciding whether a viewport was reconfigured.

Framebuffer validity is still checked on the incoming pass context, and framebuffer format remains part of the clipmap/instance/render-world pass and pipeline compatibility keys. This patch changes lifecycle identity only; it does not weaken native framebuffer/pipeline validation.

## Fixed failure mode

Before:

```text
DEPTH framebuffer A       -> viewport reconfigured
OPAQUE framebuffer B      -> viewport reconfigured
MOTION framebuffer C      -> viewport reconfigured
TRANSPARENT framebuffer D -> viewport reconfigured
next frame DEPTH A        -> viewport reconfigured again
```

This repeatedly invalidated temporal history and could repeatedly invalidate borrowed Forward+ lighting state during ordinary pass transitions.

After:

```text
first pass for viewport/config -> reconfigured once
later passes, same viewport    -> same lifecycle
later frames, same config      -> temporal history may remain valid
actual size/region/view/sample change -> reconfigured
```

## Regression gate

Added:

`tools/verify_p0_forward_viewport_lifecycle.py`

It verifies that:

1. framebuffer RID/format are absent from viewport lifecycle storage;
2. lifecycle equality uses only stable viewport configuration;
3. temporal-history gating remains intact;
4. clipmap and instance raster pipelines remain keyed by native framebuffer format.

## Deliberately not included

This patch does not implement the other audit P0 items yet:

- lane-aware bridge diagnostics;
- pass-specific borrowed-resource prerequisites;
- unconditional Terrain3D motion-hook scheduling when Forward+ uses a split motion pass.

Those are separate changes and should remain independently testable.
