# Terrain3D GPU Page Functional-Minimum / Tier-Layout Regression Fix

**Date:** 2026-09-05

## Symptom

Startup repeatedly emitted:

```text
Clamped detailed terrain range to 0 clipmap levels because the configured GPU page capacity cannot cover the requested footprint
```

and terrain could disappear after the resulting tier-layout mutation.

## Root cause A: the managed GPU-page "minimum" was not functionally usable

`Terrain3DGpuRegionCache::getMinimumBytes()` reserved one mapped page plus the legal spare-page set. A clipmap level is a spatial footprint, not a single page. With the fixed 128 m region geometry and the default 48-cell clipmap mesh, the first detailed level spans a 3x3 region footprint (nine mapped pages). Under aggregate memory pressure the budget coordinator was therefore allowed to return a grant that met the declared minimum while being incapable of rendering even one detailed level.

P07's exact resident-layer accounting (including Middle composite and terrain shadow storage) made this mismatch visible more consistently.

### Fix

The GPU terrain-page minimum now covers:

```text
first detailed clipmap core footprint
+ legal minimum spare-page set
+ shared page-table/default-map storage
```

The active-region count bounds the minimum for tiny datasets.

## Root cause B: zero has incompatible meanings across the two tier controls

`gpu_region_detail_lods == 0` means "all clipmap levels". `runtime_near_shader_lods == 0` means literally no Near shader levels.

`normalizeRuntimeTierLayout()` previously wrote `supported_lods == 0` into both when the page grant could not fit even LOD1. That simultaneously expanded GPU detail demand back to the full footprint and removed the Near shader tier. The result was self-contradictory and could collapse terrain presentation.

### Fix

A zero supported-LOD result is no longer published into either setting. The configured functional layout is retained, the aggregate memory policy is marked dirty, and the owner-controlled budget path is allowed to establish the functional minimum. Positive supported-LOD clamps continue to work normally.

## Root cause C: startup normalized before finalizing the topology-aware memory grant

The first tier normalization ran immediately after data activation, before forcing the aggregate memory policy to recompute against the now-loaded region topology and exact GPU-page minimum.

### Fix

Startup now services the aggregate memory budget immediately after data initialization and before the first tier normalization/residency selection.

## Preserved behavior

- P07 exact byte accounting remains intact.
- P07 byte/call upload throttling remains intact.
- P06 generation gating remains intact.
- Page capacity still never exceeds the owner-controlled aggregate grant.
- Positive detail-LOD clamping remains available when the grant supports some, but not all, configured detail levels.
