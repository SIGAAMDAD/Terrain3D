# Terrain3D Instance Single-LOD Target Synchronization Regression

**Date:** 2026-09-05  
**Affected lineage:** P01 -> P07, after the first-population instance residency repair  
**Symptom:** Instance populations render using stale/wrong LOD selection after scene load or editor-camera handoff, then snap to the expected appearance after a forced instance/mesh update. Terrain control-map material selection can already be correct.

## Root cause

Editor Authoring mode intentionally disables cell streaming through `effectiveInstancerCellStreamingEnabled()`. Single-LOD instance buffers remain enabled by default.

`Terrain3DInstancer::updateStreaming()` already supports this configuration: when cell streaming is disabled but single-LOD buffers are enabled it updates the target and queues only LOD transitions. The Terrain3D frame loop did not call it in that configuration. It gated the entire call on `effectiveInstancerCellStreamingEnabled()`.

This left `Terrain3DInstancer::m_lastStreamPosition` and the per-cell selected LOD tied to the target captured during initialization. A late editor-camera acquisition or later editor-camera movement updated terrain clipmap placement but not instance LOD selection. A broad `updateMmis()` later rebuilt cells and made the visual state appear to wake up.

There was a second lifecycle hole in the same target domain: `Terrain3D::synchronizeClipmapNow()` immediately resynchronized terrain/ocean/collision state on camera or explicit-target handoff, but did not publish that target to the instancer.

## Fix

1. The physics frame services `Terrain3DInstancer::updateStreaming()` whenever either cell streaming is active **or** single-LOD instance buffers require target tracking.
2. Runtime boundary deferral remains attached only to actual cell-streaming work. Single-LOD-only editor selection is not accidentally disabled by the Authoring streaming policy.
3. `Terrain3D::synchronizeClipmapNow()` now forwards camera/explicit-target handoffs to the instancer.
4. `Terrain3DInstancer::synchronizeStreamingTarget()` commits the authoritative target and requeues resident cells once on a lifecycle target change. This is change-time work, not a per-frame full rebuild.

## Preserved invariants

- No hot-path allocation was introduced.
- Incremental single-LOD transition buckets remain the normal moving-camera path.
- Full resident-cell re-evaluation happens only on explicit camera/clipmap-target synchronization.
- P06 material/control-map generation fixes remain unchanged.
- P07 GPU upload accounting/throttling remains unchanged.

## Runtime validation target

```text
open an existing authoring scene
-> do not touch mesh assets or terrain
-> acquire/move the editor camera normally
-> near instance cells select the same LODs they select after a forced update
-> no forced mesh/instance update changes the visible population
-> move across LOD boundaries and verify transitions continue without manual invalidation
```
