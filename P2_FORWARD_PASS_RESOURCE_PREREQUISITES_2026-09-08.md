# P2 — Forward+ Pass-Specific Borrowed-Resource Prerequisites

**Date:** 2026-09-08  
**Base:** P0 viewport-lifecycle fix + P1 lane-aware Forward+ diagnostics

## Problem

The ABI-11 ordinary Forward+ callback adapter previously required a valid borrowed-resource packet (or a cached resource generation) for every ordinary pass. That included the depth prepass.

Godot's Forward+ bridge can invoke Terrain3D DEPTH before the renderer publishes the borrowed lighting/cluster/reflection/decal packet for the scene. DEPTH does not consume those resources. On startup or after the adapter resource cache is reset, the old contract therefore rejected an otherwise valid depth callback solely because `m_nCachedResourceGeneration == 0`.

For the clipmap path this can be materially wrong when the following opaque pipeline expects a successful native depth prepass and uses depth equality. The instance renderer has a recovery path, but the adapter should not manufacture a depth failure in the first place.

## Change

`Terrain3DRdForwardPlusBridgeAdapter::prepare()` now separates packet validation from pass prerequisites:

```text
DEPTH        frame + pass
OPAQUE       frame + pass + borrowed scene resources
MOTION       frame + pass + borrowed scene resources
TRANSPARENT  frame + pass + borrowed scene resources
SHADOW       dedicated shadow packet path
```

A supplied resource packet is still always validated, including on DEPTH. DEPTH simply does not require a resource packet to have been supplied or cached.

Resource-requiring passes continue to accept the cached validated packet when Godot omits an unchanged resource payload.

## Files changed

- `src/rendering/rd/terrain_3d_rd_forward_plus_bridge.cpp`
- `tools/verify_p2_forward_pass_resource_prerequisites.py`

## Non-goals

This patch does not change:

- engine-side resource re-publication on callback re-registration;
- Forward+ motion-pass scheduling;
- lane failure-isolation policy;
- shadow packet semantics;
- the GPU instance culling/rendering implementation.

Those are independent bridge/runtime issues.

## Validation

The P2 verifier enforces:

- DEPTH is excluded from borrowed-resource prerequisites;
- OPAQUE, MOTION and TRANSPARENT still require them;
- supplied resource packets are still validated;
- cached resources remain usable by resource-requiring passes;
- pass conversion occurs before resource-prerequisite evaluation;
- the previous unconditional ordinary-pass resource gate cannot return.
