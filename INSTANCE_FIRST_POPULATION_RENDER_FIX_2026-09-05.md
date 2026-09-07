# Terrain3D First-Population Instancing Render Fix

**Date:** 2026-09-05  
**Base:** P01 -> P07, including the repaired P06 generation gates

## Symptom

A scene/region that begins with zero instances accepts new placements and updates the logical instance count, but the new instances never render.

## Root cause

The instance-region cache intentionally does not spend a resident slot on an empty region. `t3dBeginInstancePageAccess()` therefore treats the first edit of an empty cached region as a temporary page access.

The placement path then populated the native instance store, but still executed the temporary cleanup unchanged:

```text
empty region -> demand access returns ready without cache attachment
             -> bTemporary = true
             -> append transforms / count becomes nonzero
             -> temporary cleanup evicts the newly populated store
             -> temporary regions are excluded from render-update list
             -> no render cells / RD instance scene records are published
```

This exactly permits the editor/API to report that instances were added while the renderer receives zero usable instance records.

## Fix

When a temporary page becomes populated by an operation that requested a render update, the placement path now re-enters the instance-cache owner and promotes the already-resident store into a real cache slot before temporary cleanup. The region is then scheduled through the normal `updateMmis()` path.

The patch does not pin the region permanently. Normal instance-region residency policy still owns later eviction.

## Dynamic-capacity companion fix

An initially empty scene establishes only mutation headroom for fixed instancer publication storage. A sufficiently large first placement can exceed that sealed capacity.

Placement now preflights the projected logical record count. If it exceeds the fixed publication image, the instancer queues its existing owner-controlled cold rebuild. `Terrain3D::physicsProcess()` services that rebuild before hot instance updates, after which the render coordinator independently observes the larger publication requirement and resizes its own fixed banks in the same cold-service phase.

No hot-path backing growth was added.

If logical-cell metadata reaches the old capacity before the cold service runs, the transient entry is deferred rather than treated as a permanent placement failure; the already-existing rebuild-all path reconstructs it from the authoritative region store after capacity is established.

## Preserved invariants

- P06 generation-gating/material publication fix remains unchanged.
- P07 GPU upload accounting remains unchanged.
- Empty regions still consume no instance-cache slot.
- Runtime mutation does not directly allocate/grow fixed renderer backing in the hot path.
- Resident instance storage remains the source used by RD snapshot publication.

As a fallback, any instancer fixed-capacity miss now also arms the same cold rebuild instead of remaining a permanent dead-end. The denied hot growth is still counted; the actual backing change remains owner-controlled and cold.
