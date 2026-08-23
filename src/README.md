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
