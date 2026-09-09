# P1 Forward+ Lane-Aware Diagnostics — 2026-09-08

## Scope

Implements the second P0 item from the 2026-09-08 Godot 4.7.2 / Terrain3D bridge audit:

> A successful bridge PREPARE/SUBMIT must not be mistaken for proof that the Terrain3D instance lane initialized, prepared draw packets, or submitted draws.

Terrain remains the authoritative functional lane. This patch deliberately does **not** make instance failure suppress valid terrain rendering. It makes each lane independently observable.

## Source changes

### `src/rendering/rd/terrain_3d_render_coordinator.h/.cpp`

Adds fixed, allocation-free hot-path lane state for:

- terrain;
- instances/HLOD;
- ocean;
- RenderWorld.

For every native Forward+ pass the coordinator records:

```text
PREPARE requested lanes
PREPARE ready lanes
PREPARE failed lanes
SUBMIT attempted lanes
SUBMIT succeeded lanes
SUBMIT failed lanes
```

Instance evidence is additionally recorded as an explicit chain:

```text
published instance records/cells
        |
instance renderer initialized?
        |
PREPARE attempted?
        |
PREPARE succeeded?
        |
prepared draw packets exist?
        |
SUBMIT attempted?
        |
SUBMIT succeeded?
        |
actual submitted draw delta
```

An expected instance lane whose renderer is not initialized now increments `instance_lane_unavailable_prepares` and sets the instance bit in `prepare_failed_mask`. A successful instance PREPARE that produces no draw packets is counted separately as `instance_prepared_without_draws`; this is evidence, not automatically an error because culling/pass filtering can legitimately produce zero draws.

`getForwardLaneDiagnostics()` exposes this state without recursively invoking the coordinator's full diagnostics path.

### `src/rendering/rd/terrain_3d_rd_instance_renderer.h`

Adds allocation-free inspection helpers:

- `hasForwardPreparedDraws()`
- `getForwardSubmittedDrawCount(pass)`

These allow the coordinator to prove that SUBMIT actually increased the pass-specific instance draw counter instead of treating a boolean callback return as draw evidence.

### `src/rendering/rd/terrain_3d_rd_forward_plus_bridge.cpp`

`forward_plus_bridge` diagnostics now include `coordinator_lanes`, an array containing the lane state for every attached coordinator slot.

This makes the existing bridge packet counters interpretable correctly:

```text
packets_accepted > 0
```

means the coordinator callback contract was accepted. To prove the instance lane rendered, inspect the matching coordinator lane state and require the expected instance lane to progress through PREPARE and SUBMIT with actual draw evidence.

## Lane mask values

```text
1  terrain
2  instances/HLOD
4  ocean
8  RenderWorld
```

The numeric values are also emitted in the diagnostic dictionary so tools do not need to duplicate them.

## Failure isolation retained

This patch does not change the existing policy that companion instance/HLOD, ocean, or RenderWorld failure must not suppress the functional terrain height lane.

The bridge may therefore remain accepted while `prepare_failed_mask` contains the instance bit. That state is now explicit instead of being hidden behind the aggregate bridge token.

## Regression gate

Added:

`tools/verify_p1_forward_lane_diagnostics.py`

It verifies:

1. all four lane bits and PREPARE/SUBMIT masks exist;
2. published instances make the instance lane explicitly expected;
3. expected-but-uninitialized instance state is visible;
4. prepared draw-packet evidence is collected;
5. actual SUBMIT draw deltas are measured;
6. bridge diagnostics enumerate coordinator lane state;
7. terrain failure isolation remains unchanged.

## Deliberately not included

This patch does not yet implement:

- pass-specific borrowed-resource prerequisites;
- unconditional Terrain3D MOTION-hook scheduling for split-motion Forward+;
- multi-coordinator partial-success submission masks;
- engine-side SUBMIT return-value enforcement.
