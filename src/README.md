# Terrain3D native source layout

The `src` root is intentionally limited to the extension entry points and global integration headers. Native implementation files live under the subsystem that owns them.

| Directory | Responsibility |
| --- | --- |
| `assets/` | Terrain assets, mesh assets, texture assets, generated texture resources |
| `authoring/` | Editor tools, erosion, autoshader and landform baking |
| `core/` | Engine-independent utilities, diagnostics, logging, compatibility and EASTL runtime support |
| `data/` | Region/data ownership, sampling, serialization codecs and runtime page format |
| `hlod/` | HLOD package, manifest, builder and streamer |
| `instances/` | Instance storage, page codec, cache/layout policy and instancing runtime |
| `lighting/` | Regional lighting policy, lightmap streaming and region-lighting resources |
| `memory/` | Fixed arena, active storage, resident pool and memory budgeting |
| `physics/` | Terrain collision integration |
| `rendering/` | Meshing, material, compute and shader specialization |
| `simd/` | Portable SIMD dispatch, parallel kernels and ISA-specific backends |
| `streaming/` | Runtime page preparation, residency coordination/policies, profiles and node streaming |

## Include convention

Authored headers use paths relative to `src`, for example:

```cpp
#include "data/terrain_3d_region.h"
#include "memory/terrain_3d_memory_arena.h"
#include "simd/terrain_3d_simd.h"
```

This keeps includes stable across sibling subsystems and requires only `src` on the authored include path.

## Build discovery

`SConstruct` discovers authored `.cpp` files recursively. Visual Studio and validation manifests use the same physical subsystem paths. New implementation files should be placed directly in the owning subsystem rather than re-expanding the `src` root.


## Owner-controlled allocation

Terrain3D runtime phase labels are diagnostic telemetry only. No allocator derives permission from `HOT_FRAME`, `HOT_RENDER`, or a cold phase label. Zone-backed owners carry their allocator domain explicitly, and the subsystem that owns retained backing decides whether a capacity change is legal.

Backing-growth legality still belongs to the object that owns the storage:

- fixed buffers and slabs declare `PersistentResourceOwned`, `RuntimeZoneRequired`, or parent-bound storage; a runtime-zone-required owner cannot seal or grow while bootstrap-backed;
- hot paths reuse or mutate established backing and report an owner capacity miss instead of growing implicitly;
- cold rebuild/materialization services are the only owner APIs that establish or enlarge retained backing;
- `Terrain3DEastlAllocator` carries an explicit `Terrain3DMemoryZone *` for Terrain3D-instance-owned storage, while Resource/authoring state that may outlive a Terrain3D node remains bootstrap-owned;
- allocator and zone layers validate mechanical size/alignment/capacity/overflow rules but never infer allocation permission from runtime phase;
- allocation phase telemetry records successful requests so hot-path ownership regressions remain observable without making the allocator a policy engine.

A steady-state regression is an **owner backing-growth/capacity miss or unexpected allocation event** during `HOT_FRAME`/`HOT_RENDER`.

Run the owner-allocation verification gates after memory/lifetime changes:

```text
python tools/verify_owner_controlled_allocation_abc.py
python tools/verify_owner_controlled_allocation_def.py
python tools/verify_owner_controlled_allocation_g.py
python tools/verify_ready_cold_phase_boundaries.py
python tools/verify_initialization_residency_ownership.py
```
