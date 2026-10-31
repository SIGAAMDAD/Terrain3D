# Terrain3D native source layout

The `src` root is intentionally limited to the extension entry points and global integration/data-contract headers. Native implementation files live under the subsystem that owns them, with a second directory level used to separate responsibilities inside larger subsystems.

## Top-level ownership

| Directory | Responsibility |
| --- | --- |
| `assets/` | Terrain assets, import profiles, mesh/texture/material resources and the asset catalog |
| `authoring/` | Editor tools, baking/cooking, erosion, virtual-texture authoring and World scene authoring |
| `core/` | Low-level configuration, containers, EASTL/native runtime support and reusable utilities |
| `data/` | Terrain/region model, sampling, editing, cooking, persistence and runtime artifact/page formats |
| `diagnostics/` | Diagnostics/runtime publication and native tests |
| `hlod/` | HLOD building, serialized formats and runtime streaming |
| `instances/` | Instance contracts, storage/publication, physics/occlusion and instancing runtime |
| `integration/` | Godot bindings/resource wiring and top-level runtime scheduling |
| `lighting/` | Lighting policy and regional/lightmap runtime systems |
| `memory/` | Arena/zone ownership, storage pools and memory policy/budgeting |
| `physics/` | Terrain collision building and runtime collision integration |
| `rendering/` | Front-end compute/geometry/material/surface code and RenderingDevice backends |
| `simd/` | SIMD dispatch/core helpers and ISA-specific backends |
| `streaming/` | Cache, policy, residency and runtime streaming coordination |

## Responsibility groups

### `assets/`

- `catalog/` — asset registry/catalog behavior.
- `import/` — world import profiles.
- `resources/` — concrete Godot resource types for textures, meshes and materials.

### `authoring/`

- `baking/` — autoshader, landform and cook-database baking.
- `editor/` — editor-facing orchestration.
- `erosion/` — erosion configuration, settings and jobs.
- `vt/build/` — virtual-texture page/reference/cook generation.
- `vt/io/` — VT pack writing and Godot-facing cooker integration.
- `world/core/` — world authoring/controller ownership.
- `world/conversion/` — Godot/world conversion and scene/material compilation.

### `core/`

- `config/` — compiler compatibility, limits, numeric constants and startup state.
- `containers/` — low-level byte/fixed/slab storage helpers.
- `runtime/` — allocation domains, EASTL integration and native backing.
- `utility/` — logging, hashing, synchronization, ownership and general utilities.

### `data/`

- `model/` — `Terrain3DData`, region ownership and data access.
- `formats/` — compact primitive data encodings.
- `sampling/` — samplers and sampling storage.
- `editing/` — map editing services.
- `cooking/` — control/region image cooking.
- `persistence/` — region codecs, repository and persistence.
- `runtime/` — runtime page/world formats, bake codecs, residency adapters and artifacts.

### `rendering/`

- `compute/` — front-end compute integration.
- `geometry/` — meshing and clipmap geometry contracts.
- `material/` — material schema/runtime and shader specialization.
- `surface/` — far-surface atlas and ocean settings.
- `integration/` — rendering subsystem integration entry points.

`rendering/rd/` is split further by RenderingDevice responsibility:

- `core/` — RD context, coordinator, publication and lifecycle state.
- `clipmap/` — clipmap renderer.
- `compositor/` — compositor effect integration.
- `forward/` — forward/Forward+ pass preparation, variants and lighting.
- `instances/` — RD instance records, renderer, culling, frustum and mesh cache.
- `material/` — RD material-state construction.
- `ocean/` — ocean renderer.
- `preview/` — preview service/results.
- `resources/` — higher-level RD resource managers.
- `shaders/` — shader catalog/library and pipeline ABI.
- `infrastructure/buffers/` — canonical GPU-buffer ownership/batching.
- `infrastructure/descriptors/` — bindings, descriptor cache and uniform sets.
- `infrastructure/pipeline/` — RD pipeline objects/cache.
- `infrastructure/resources/` — physical RD textures, samplers and shaders.
- `infrastructure/targets/` — framebuffer, geometry-view and render-target pooling.
- `infrastructure/visibility/` — Hi-Z infrastructure.
- `infrastructure/telemetry/` — render telemetry contracts.
- `vt/core/` — VT contracts, identities and stable hashing.
- `vt/gpu/` — VT GPU resources/bindings.
- `vt/io/` — runtime VT pack decoding.
- `vt/planning/` — VT resource planning.
- `vt/runtime/` — virtual/mega texture runtime and fixed profiles.
- `vt/streaming/` — VT residency/streaming.
- `world/core/` — world rendering contracts, limits and authority.
- `world/build/` — world cluster/static builders.
- `world/geometry/` — world geometry generation/validation.
- `world/resources/` — world GPU/runtime resources.
- `world/runtime/` — world publication/runtime state.
- `world/visibility/` — world Hi-Z/raster/visibility GPU work.

### `streaming/`

- `cache/` — CPU/GPU/instance/middle-height caches.
- `policy/` — budgets, profiles and cache/streaming policies.
- `residency/` — active-height and region residency state/selection.
- `runtime/` — node streaming, runtime-page store and upload bridge.
- `integration/` — streaming subsystem integration entry point.
- `utility/` — local streaming helpers.

### Remaining subsystems

- `memory/arena`, `memory/storage`, `memory/policy`, `memory/core` separate ownership/storage/policy concerns.
- `instances/contracts`, `instances/storage`, `instances/physics`, `instances/runtime` separate data contracts from retained state and runtime nodes.
- `hlod/build`, `hlod/format`, `hlod/runtime` separate build-time, serialized and live HLOD paths.
- `integration/godot` and `integration/runtime` separate engine bindings from frame scheduling.
- `physics/build` and `physics/runtime` separate collision construction from runtime collision.
- `simd/core` and `simd/backends` separate dispatch/contracts from ISA-specific translation units.
- `diagnostics/core` and `diagnostics/tests` separate product diagnostics from native test code.

## Include convention

Authored headers use paths relative to `src`, for example:

```cpp
#include "data/model/terrain_3d_region.h"
#include "memory/arena/terrain_3d_memory_arena.h"
#include "rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer.h"
```

This keeps dependency ownership visible at the include site and requires only `src` on the authored include path.

## Build discovery

This repository includes `SConstruct` and the Visual Studio NMake project. The P00/P01
migration freezes the native translation units in `build/source_sets.py` instead of
performing an unrestricted source glob. `build/tu_ownership_review.csv` records the
planned runtime/editor source ownership but does not exclude units until P06.

The native dependencies are supplied separately as `terrain3d_dependencies.zip`
(`godot-cpp/` and `thirdparty/`). Extract it at the native build root before
building. See `tools/dual_dll/README.md` for the three Windows x86-64 targets,
ClassDB capture, and P00/P01 validation commands.

The managed addon project is `project/Terrain3D.csproj` and targets `Godot.NET.Sdk/4.7.2` / `net8.0`; `project/Terrain3D.sln` resolves that project directly.

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

## Source-tree verification

The reconstructable checks included with this snapshot are:

```text
python3 tools/dual_dll/p00_snapshot.py --check
python3 tools/dual_dll/verify.py
```

The source-level checks above do not compile or link the three native DLLs.
Run the Windows/MSVC build matrix and the Godot host/ClassDB checks described
in `tools/dual_dll/README.md` before treating the P01 runtime gate as verified.

### RenderingDevice owned-resource boundaries

`Terrain3DRdGpuBuffer` is the sole physical buffer implementation: one object retains logical configuration plus physical RID/capacity/used/generation/persistence state and owns create/grow/replace/slice construction/I/O/release. The shared fixed-indirect arena is also a `Terrain3DRdGpuBuffer`; World, Ocean, and clipmap retain non-owning owner pointers plus slices derived by that object. Multi-buffer batching only coordinates canonical buffer writes, while POD growth/alignment arithmetic remains in `rendering/rd/infrastructure/buffers/terrain_3d_rd_gpu_buffer_types.h`. `Terrain3DRdTexture` is the sole RenderingDevice texture authority and owns physical texture identity, layout/byte accounting, tracked create/shared-view create, update/copy/readback, borrowed-texture inspection, format capability checks, generation, swap, and release. Renderer/domain code retains descriptor/framebuffer dependency ordering and borrowed texture RIDs, but all direct `RenderingDevice::texture_*` calls stay inside `Terrain3DRdTexture`. `Terrain3DRdTextureManager` is a scheduling/batching bridge for cross-thread generated-texture work and never stores or mutates raw texture RIDs.

## Generated C# wrappers

The source README describes the C# wrapper-generation tooling used in the original outer repository. That tooling is not bundled with this source snapshot. In the complete repository, regenerate after changing the mechanical wrapper surface with:

```text
python3 tools/csharp/generate_wrappers.py
```

The generated output is `project/addons/terrain_3d/csharp/Terrain3D.Bindings.Generated.cs`. Domain-specific wrapper behavior remains handwritten.
