# Terrain3D — Memory Budget Zone-Containment P1 Fix

**Date:** 2026-09-09  
**Build ID:** `memory-budget-zone-containment-p1-2026-09-09`  
**Parent:** `instancer-rd-arraymesh-surface-binding-p0-2026-09-09`

## Fault

The total-memory coordinator reserved the complete Terrain3D runtime allocator zone and also charged several logical capacities backed by that same zone as fixed memory. Confirmed duplicate charges were:

- resident sampling-map backing;
- retained instancer EASTL containers;
- node-streamer activation/movable-node backing.

The previous policy therefore had this shape:

```text
logical fixed capacity
  + zone-backed resident/instancer/node capacity
  + full runtime-zone reserve
  + transient reserve
  + managed budgets
```

The zone-backed items were already physically contained by the runtime-zone reservation, so they reduced the managed budget twice.

## Repair

`t3dMemoryBudgetInput_t` and `t3dMemoryBudgetResult_t` now carry `m_nAllocatorContainedFixedBytes` explicitly.

`Terrain3D::calculateFixedMemoryBytes()` remains the logical fixed-footprint telemetry source. A new `calculateAllocatorContainedFixedMemoryBytes()` identifies the confirmed fixed-capacity consumers whose physical backing belongs to the runtime zone.

The coordinator converts the logical fixed footprint into the actual externally charged fixed footprint:

```text
allocator_contained = min(logical_contained, logical_fixed, allocator_reserve)
external_fixed      = logical_fixed - allocator_contained

budget charge = external_fixed + allocator_reserve + transient_reserve + managed
```

Clamping containment to the allocator reserve matters in editor builds where the EASTL allocator may overflow to heap. Bytes beyond the physical zone reserve are not hidden as allocator-contained.

## Ownership classification added

`Terrain3DData::getMemoryAllocatorContainedRepositoryBytes()` classifies the resident sampling pool separately from the active-height backing:

- active-height backing remains fixed/external to the generic runtime zone;
- resident map backing is allocator-contained.

The instancer retained-container capacity and node-streamer retained activation capacity are likewise classified as allocator-contained for budget purposes while remaining visible in diagnostics.

## Diagnostics

Memory budget diagnostics now expose:

- `fixed_bytes` — externally charged fixed bytes;
- `fixed_logical_bytes` — complete logical fixed footprint;
- `allocator_contained_fixed_bytes` — logical fixed bytes covered by the allocator reserve;
- `allocator_reserve_bytes` — physical reserved zone capacity.

The detailed fixed-residency dictionary also exposes `budget_external_bytes` and `allocator_contained_bytes` without deleting the existing logical subsystem telemetry.

## Example

For a 512 MiB total budget with:

```text
logical fixed          100 MiB
allocator-contained     20 MiB
allocator reserve       64 MiB
transient reserve       32 MiB
```

previous managed budget:

```text
512 - 100 - 64 - 32 = 316 MiB
```

correct managed budget:

```text
512 - (100 - 20) - 64 - 32 = 336 MiB
```

The 20 MiB contained inside the 64 MiB runtime-zone image is charged once.

## Verification

Added:

- `tools/test_memory_budget_allocator_containment.cpp`
- `tools/verify_memory_budget_zone_containment_p1.py`

The standalone test covers:

1. normal allocator containment;
2. containment larger than logical fixed bytes;
3. containment larger than the physical allocator reserve.

Also passed after the patch:

- owner-controlled allocation A/B/C, D/E/F and G gates;
- initialization/residency ownership gate;
- runtime-page transient-grant accounting gate;
- GPU budget/residency minimum gate;
- instancer P0 ArrayMesh surface-binding regression gate;
- authoritative-store publication and RD-port integration gates;
- native/GDScript API contract;
- systems-C ownership and style gates;
- remaining cumulative P06-P12 / Forward+ / runtime-page verification gates exercised in this tree;
- C++17 syntax compilation of `terrain_3d_memory_policy.cpp` and `terrain_3d_data.cpp` against the supplied godot-cpp/EASTL headers.

A complete SCons build was not available in this environment because `scons` is not installed.

## Scope

This P1 changes memory-budget ownership accounting only. It does not de-globalize active storage, change runtime-zone capacity, change allocator overflow policy, or move additional subsystems out of the generic zone.
