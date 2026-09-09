# Terrain3D P2 — Instance-Local Active Storage

**Date:** 2026-09-09  
**Build ID:** `instance-local-active-storage-p2-2026-09-09`  
**Base:** `Terrain3D_Full_Memory_Budget_Zone_Containment_P1_Fix_2026-09-09.zip`

## Scope

This patch implements remediation item #2 from the 2026-09-09 memory/instancer audit: remove the process-global active-height storage owner and make active terrain sampling storage an instance-owned subsystem.

It intentionally does **not** implement the next lifecycle patch that delays runtime-memory activation until the Terrain3D enters the active world. Each Terrain3D may still establish its own runtime image during the existing initialization phase; the important change here is that those images no longer contend for one global active-storage singleton.

## Defect

The previous active-height store mixed two incompatible ownership models:

- backing bytes came from each `Terrain3D` object's private `Terrain3DMemoryArena`;
- physical page descriptors, logical-to-physical mappings, writer mutex, owner generation, counters, and owner token were process-static globals in `terrain_3d_active_storage.cpp`.

The global owner token rejected a second Terrain3D object while the first remained bound, even though the second object had its own valid backing allocation. Editor construction/deserialization can overlap multiple Terrain3D objects, so this produced the observed bind failure before the second object's instancer could initialize.

## Implementation

### `Terrain3DActiveStorage`

`terrain_3d_active_storage.h/.cpp` now defines an explicit non-copyable `Terrain3DActiveStorage` object containing:

- 32 physical active-height page descriptors;
- the fixed logical-to-physical page table;
- active-height backing pointer and ownership flag;
- active/free/draining state;
- publication generation;
- reader-retry and publication counters;
- clock-hand state;
- writer mutex.

No mutable active-store state remains process-global.

### `Terrain3DSamplingStorage`

Each `Terrain3DSamplingStorage` owns:

```cpp
mutable Terrain3DActiveStorage m_activeStorage;
```

The existing sampling-storage boundary remains authoritative. `Terrain3DData`, samplers, streaming caches, and rendering code continue to talk to `Terrain3DSamplingStorage`; they do not gain a second ownership path.

### Lease ownership

`Terrain3DActiveMapLease` now records the exact `Terrain3DActiveStorage*` and physical page it pinned. Lease destruction decrements that instance's page pin count directly.

This prevents one Terrain3D object's release/reset from touching another object's reader state.

### Backing ownership

The normal path still binds the active-height bank to the per-Terrain3D runtime-arena slice. The legacy cold fallback `claim()` remains instance-local and may allocate its own backing only when no arena binding has occurred, preserving existing behavior without reintroducing global ownership.

## Resulting ownership

```text
Terrain3D A
  RuntimeMemoryArena A
    ActiveHeightSlice A
        |
        v
  Terrain3DData A
    Terrain3DSamplingStorage A
      Terrain3DActiveStorage A
        pages/map/mutex/generation A

Terrain3D B
  RuntimeMemoryArena B
    ActiveHeightSlice B
        |
        v
  Terrain3DData B
    Terrain3DSamplingStorage B
      Terrain3DActiveStorage B
        pages/map/mutex/generation B
```

There is no process-wide owner arbitration between A and B.

## Verification performed

- P2 focused static regression verifier passes.
- Object-symbol inspection confirms the active-storage object file no longer exports/emits the old process-static page bank, logical map, owner pointer, or writer mutex.
- Syntax compilation passes for:
  - `terrain_3d_active_storage.cpp`
  - `terrain_3d_sampling_storage.cpp`
  - `terrain_3d_data.cpp`
  - `terrain_3d_memory_policy.cpp`
  - `terrain_3d_sampler.cpp`
  - `terrain_3d_middle_height_region_cache.cpp`
  - `terrain_3d_far_surface_atlas_cache.cpp`
  - `terrain_3d_material.cpp`
  - `terrain_3d_render_coordinator.cpp`
- Cumulative repository verification passes for the P0 instancer mesh-source fix, P1 memory-budget containment fix, runtime-page contracts, GPU residency contracts, owner-controlled allocation gates, API contract, systems-C ownership/style, and render-lane checks.

The environment does not contain SCons, so a complete GDExtension DLL link build was not performed here.

## Expected runtime change

The editor may construct overlapping Terrain3D objects without the second object failing because another Terrain3D owns the active-height singleton. A failure after this patch should therefore come from that object's own arena/zone/backing state rather than a process-global active-store ownership collision.
