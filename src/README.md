# Terrain3D native source layout

The `src` root is intentionally limited to the extension entry points and global integration headers. Native implementation files live under the subsystem that owns them.

| Directory | Responsibility |
| --- | --- |
| `assets/` | Terrain assets, mesh assets, texture assets, generated texture resources |
| `authoring/` | Editor tools, World scene authoring, erosion, autoshader and landform baking |
| `core/` | Low-level reusable utilities, logging, compiler/configuration helpers and EASTL runtime support |
| `diagnostics/` | Diagnostics resources, runtime diagnostics publication and native unit-test integration |
| `integration/` | Top-level Terrain3D Godot bindings, resource wiring and runtime frame scheduling |
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

The repository carries both the native `SConstruct` and the top-level `Terrain3D.vcxproj` / `Terrain3D.sln`. `SConstruct` discovers authored `src/**/*.cpp` files, excluding generated implementation directories, and builds the GDExtension against `godot-cpp` 10.0.0-stable targeting its bundled 4.7 API, plus `thirdparty/EASTL` and `thirdparty/EABase`. The build validates the required persistent-buffer RenderingDevice surface before generating bindings, so an incompatible stock or stale godot-cpp checkout fails before compilation. The Visual Studio project mirrors the same authored source tree for MSVC development. New implementation files belong directly in the subsystem that owns them rather than re-expanding the `src` root.

The managed addon project is `project/Terrain3D.csproj` and targets `Godot.NET.Sdk/4.7.2` / `net8.0`.


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

## Repository sustainability verification

Run the final P00-P16 / R01-R28 structural acceptance gate with:

```text
python3 tools/verification/verify_repo_sustainability.py
python3 tools/verification/verify_compatibility_ledger.py
python3 tools/csharp/generate_wrappers.py --check
```

The sustainability gate verifies canonical control/image cooking and square-ring ownership, shared residency and desired-region candidate collection, instance traversal, Hi-Z, Forward+ variant selection, HLOD descriptor/validation infrastructure, the canonical erosion configuration snapshot, shared transactional editor publication and editor utility helpers, retirement of the legacy parallel static-world publication ABI, preview-service ownership, source-tree responsibilities, generated C# bindings, compatibility ownership, and the absence of repository verification scripts from runtime source directories, canonical VT quality-to-base-mip ownership, R20 dead-static cleanup, canonical workbench checkout-state serialization, clipmap terrain-binding fallback resolution, VT cold residency/streaming initialization, canonical RD instance-record packing, canonical World cluster finalization, complete retirement of the former parallel static-world authoring/runtime subsystem, B11 final `Terrain3DRdGpuBuffer` ownership/lifecycle acceptance, and T09 final `Terrain3DRdTexture` owned-texture lifecycle acceptance.

### RenderingDevice owned-resource boundaries

`Terrain3DRdGpuBuffer` is the sole physical buffer implementation: one object retains logical configuration plus physical RID/capacity/used/generation/persistence state and owns create/grow/replace/slice construction/I/O/release. The shared fixed-indirect arena is also a `Terrain3DRdGpuBuffer`; World, Ocean, and clipmap retain non-owning owner pointers plus slices derived by that object. Multi-buffer batching only coordinates canonical buffer writes, while POD growth/alignment arithmetic remains in `terrain_3d_rd_gpu_buffer_types.h`. `Terrain3DRdTexture` is the sole RenderingDevice texture authority and owns physical texture identity, layout/byte accounting, tracked create/shared-view create, update/copy/readback, borrowed-texture inspection, format capability checks, generation, swap, and release. Renderer/domain code retains descriptor/framebuffer dependency ordering and borrowed texture RIDs, but all direct `RenderingDevice::texture_*` calls stay inside `Terrain3DRdTexture`. `Terrain3DRdTextureManager` is a scheduling/batching bridge for cross-thread generated-texture work and never stores or mutates raw texture RIDs.

## Generated C# wrappers

`tools/csharp/wrapper_manifest.json` is the source of truth for mechanical GDExtension wrapper binding glue. Regenerate after changing the mechanical wrapper surface with:

```text
python3 tools/csharp/generate_wrappers.py
```

The generated output is `project/addons/terrain_3d/csharp/Terrain3D.Bindings.Generated.cs`. Domain-specific wrapper behavior remains handwritten.
