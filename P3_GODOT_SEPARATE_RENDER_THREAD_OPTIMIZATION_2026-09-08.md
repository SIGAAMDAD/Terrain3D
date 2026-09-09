# P3 — Godot Separate Render-Thread Bridge Optimization

**Target:** Terrain3D P0 + P1 + P2 tree / Godot 4.7.2 ABI 11 Forward+ bridge
**Date:** 2026-09-08

## Goal

Make the plugin side match Godot's dedicated render-thread ownership model: scene/lifecycle mutation is cold, the render callback sees immutable publication, PREPARE/SUBMIT use identical recipients, and hot callback dispatch avoids per-coordinator atomic pointer traffic.

## Changes

### 1. Coordinator table is cold-owned and release-published

The bridge previously stored every coordinator in an atomic pointer and performed an acquire-load for every active coordinator on every PREPARE and SUBMIT.

It now uses:

```text
cold/main-thread mutation:
    coordinator pointer array
    + mutex
    + release publication through one atomic bit mask

render thread:
    one acquire mask load per PREPARE
    plain immutable pointer reads for set bits
```

Attach writes the pointer first, then release-publishes its bit. Detach clears the bit first, calls `RenderingServer::force_sync()` through `Terrain3DRdContext`, then clears the pointer after all in-flight render work has completed.

### 2. PREPARE and SUBMIT reuse the exact coordinator mask

Previously PREPARE and SUBMIT each loaded the current coordinator set. A coordinator attachment or removal between those calls could therefore make SUBMIT target a different set than PREPARE.

The adapter now snapshots `m_nPreparedCoordinatorMask` in PREPARE and uses that exact mask in SUBMIT.

This pairs cleanly with Godot's own engine-side callback-slot snapshot.

### 3. Removed redundant shutdown synchronization

`disconnectEngineBridge()` already unregisters the native callback, synchronizes the Godot render thread, and collects the retired engine callback slot. `shutdownSingleton()` no longer performs a second unconditional `force_sync()` immediately afterward.

### 4. Cached frame generation short-circuit moved before expensive validation

If an ABI frame packet repeats the already-consumed generation, conversion exits after the fixed ABI/size/generation checks instead of re-running the floating-point environment validation.

The optimized engine normally omits unchanged frame packets entirely, so this also serves as a defensive low-cost fast path.

## Hot-path synchronization before / after

### Before

For N active coordinators, an ordinary pass performed roughly:

```text
engine PREPARE:  active callback atomic acquire
plugin PREPARE:  bridge-ready acquire + coordinator-mask acquire + N coordinator-pointer acquires
engine SUBMIT:   active callback atomic acquire
plugin SUBMIT:   coordinator-mask acquire + N coordinator-pointer acquires
```

### After

```text
scene boundary:  engine callback-slot acquire once
plugin PREPARE:  bridge-ready acquire + coordinator-mask acquire once
plugin dispatch: plain pointer reads
engine SUBMIT:   same captured callback slot
plugin SUBMIT:   same captured coordinator mask, no coordinator atomics
```

The RenderingDevice work itself remains render-thread-owned and synchronous with Godot's native Forward+ pass.

## Validation

- `verify_render_thread_bridge_optimization.py`: PASS
- P0 viewport lifecycle gate: PASS
- P1 lane diagnostics gate: PASS
- P2 pass resource prerequisites gate: PASS
- Forward+ instance submission gate: PASS
- P11 instance GPU culling gate: PASS
- P08 RenderWorld prepare/submit gate: PASS
- systems-C ownership gate: PASS
- systems-C style gate: PASS
- `clang++ -std=c++17 -DGDEXTENSION -DDEBUG_ENABLED -DTOOLS_ENABLED -fsyntax-only`:
  - `terrain_3d_rd_forward_plus_bridge.cpp`: PASS
  - `terrain_3d_render_coordinator.cpp`: PASS
  - `register_types.cpp`: PASS

The only compiler warning remains the pre-existing generated godot-cpp `INT64_MIN` literal warning.
