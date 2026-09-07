# Terrain3D render-startup recovery fix

**Date:** 2026-09-06

## Symptom

Terrain data loaded without the previous GPU-capacity warning, but the direct-RD terrain remained completely invisible.

## Root causes addressed

1. The first cooked-page residency pass ran before `Terrain3DAssets::initialize(Terrain3D*)`. Runtime-page material signatures include owner-dependent Terrain3D settings, so a freshly cooked page could be validated against a provisional ownerless signature during startup.
2. Near GPU height-array readiness and physical page-table publication were not one coherent state. The renderer could observe a physical page map while the corresponding height RID was unavailable during a cache rebuild.
3. A temporarily unavailable Near height RID was treated as a render failure. Eight callbacks set the coordinator's sticky failure latch, disabled submission, and later valid texture publication did not clear the latch.

## Fix

- Initialize/bind `Terrain3DAssets` after data repository activation but before the first residency/page-source pass.
- Resolve the Near height RID before packing the immutable region map. While it is unavailable, publish logical region presence but zero physical Near layers. Republish when height-RID readiness changes.
- Bind the renderer's existing neutral array descriptor for the unavailable Near height sampler. Because physical layers are zero during that state, vertex sampling falls through to Middle/Far/background and never indexes the neutral array as a resident page.
- Recover a latched renderer when terrain/material texture generations advance.
- Expose `published_height_texture_valid`, `region_map_height_texture_ready`, and `renderer_recoveries` in runtime diagnostics.

The patch does not restore the legacy RenderingServer terrain path and does not make allocation/resource creation hot-path work.
