# Terrain3D RenderingDevice Revamp - Part-by-Part Implementation Plan

## Scope

This document turns the RenderingDevice conversion specification into an implementation sequence mapped to the current Terrain3D repository. The conversion preserves the existing CPU terrain, region residency, streaming, authoring, collision, persistence, baking, HLOD selection and instance-storage systems while replacing the live GPU submission path in controlled stages.

The first shipping objective is RenderingServer parity with explicit RenderingDevice ownership. Later parts add packed topology, explicit instance geometry arenas, GPU culling, indirect draws and optional native Forward+ integration.

Target: Godot 4.7.1, C++17 GDExtension, Forward+ first, D3D12 and Vulkan validation.

---

## Part 0 - Compile-clean baseline

### Goal
Establish a source tree that compiles before adding the RD backend.

### Work
- Repair stale identifiers and declaration/definition mismatches introduced by the recent source split and naming migration.
- Verify `src/**/*.cpp` discovery against `Terrain3D.vcxproj` generation.
- Check every extracted implementation file against `src/terrain_3d.h` declarations.
- Check public bindings against renamed C++ methods.
- Check MSVC-sensitive string concatenation, integer overloads, enum conversions and template deduction.
- Check private-access violations introduced by extracted helper/service classes.
- Check include completeness after file splitting.
- Regenerate `Terrain3D.vcxproj`, `.filters`, and solution metadata after source changes.

### Acceptance
- SCons build succeeds for the available target toolchain.
- MSVC project generator succeeds.
- No missing source/header entries in generated project files.
- No stale renamed C++ symbol references remain.

---

## Part 1 - RD shared types and backend selection

### New files
- `src/rendering/rd/terrain_3d_rd_types.h`
- `src/rendering/terrain_3d_render_coordinator.h`
- `src/rendering/terrain_3d_render_coordinator.cpp`

### New types
```cpp
typedef enum t3dRenderBackend_e {
    T3D_RENDER_BACKEND_RENDERING_SERVER = 0,
    T3D_RENDER_BACKEND_RENDERING_DEVICE = 1,
} t3dRenderBackend_t;
```

Add POD types for:
- frame generation;
- view state;
- terrain-global state;
- clipmap patch state;
- topology ranges;
- mesh handles;
- surface ranges;
- framebuffer keys;
- pipeline keys.

### `Terrain3DRenderCoordinator`
Responsibilities:
- own backend selection;
- initialize and destroy backend-specific state;
- keep RenderingServer and RenderingDevice lifecycle branching out of `Terrain3D`;
- publish immutable frame snapshots;
- coordinate full renderer rebuilds;
- expose backend readiness/failure state;
- route render-thread work.

### Existing files modified
- `src/terrain_3d.h`
- `src/terrain_3d.cpp`
- `src/rendering/terrain_3d_render_integration.cpp`
- `src/core/terrain_3d_bindings.cpp`

### Acceptance
- RenderingServer behavior unchanged.
- RD backend can be selected but may render nothing yet.
- Backend initialization/destruction can repeat without leaked state.

---

## Part 2 - `Terrain3DRdContext`

### New files
- `src/rendering/rd/terrain_3d_rd_context.h`
- `src/rendering/rd/terrain_3d_rd_context.cpp`

### Responsibilities
- hold the non-owning global `RenderingDevice *`;
- track device generation and initialized state;
- expose common samplers and per-frame buffers;
- expose render-thread assertions/checks;
- provide shared creation/update helpers used by the RD renderer;
- centralize device shutdown/reacquisition.

### Interaction with `Terrain3DCompute`
Do not create a second independent global-RD convention. Reuse the existing `Terrain3DCompute` assumptions for global-device access and render-thread dispatch, then move common low-level helpers into the RD context where both systems can share them.

### Acceptance
- Context obtains the global RD only where supported.
- Compatibility/headless failure leaves the RS renderer usable.
- Reinitialization creates a new generation and invalidates stale RD state.

---

## Part 3 - `Terrain3DRdResourceOwner`

### New files
- `src/rendering/rd/terrain_3d_rd_resource_owner.h`
- `src/rendering/rd/terrain_3d_rd_resource_owner.cpp`

### Responsibilities
Central ownership record for Terrain3D-created RD resources:
- buffers;
- shaders;
- pipelines;
- samplers;
- uniform sets;
- framebuffers;
- texture RIDs owned directly by Terrain3D.

Each tracked record stores:
- RID;
- resource class;
- byte size where meaningful;
- creation generation;
- diagnostic name;
- owner/category;
- live/deferred-free state.

### Rules
- No individual clipmap patch owns a VB/IB RID.
- No ordinary frame path creates topology resources.
- Teardown order is deterministic.
- Borrowed Godot textures are recorded as borrowed and are never freed by the RD owner.

### Acceptance
- Zero live owned RIDs after renderer destruction.
- Double-free and cross-generation use are detected in diagnostics builds.

---

## Part 4 - Render snapshot boundary

### Files
- `src/rendering/rd/terrain_3d_rd_types.h`
- `src/rendering/terrain_3d_render_coordinator.*`

### Data
Introduce double- or triple-buffered `t3dRdFrameSnapshot_t` storage containing POD only.

Snapshot contents:
- current GPU-frame generation;
- camera matrices;
- previous camera matrices;
- camera world position;
- terrain target/origin;
- clipmap state generation;
- patch dirty ranges;
- material-state generation;
- region-cache texture generations;
- instance/HLOD state generations;
- viewport-independent render flags.

### Publication model
```text
main/streaming thread
    builds inactive snapshot
        -> atomic publish generation/index
            -> render thread reads latest complete snapshot
```

### Acceptance
- Render thread never observes partially written state.
- No `Ref<>` mutation crosses the snapshot boundary.
- Main thread never directly mutates RD resources.

---

## Part 5 - `Terrain3DRdTopology`

### New files
- `src/rendering/rd/terrain_3d_rd_topology.h`
- `src/rendering/rd/terrain_3d_rd_topology.cpp`

### Existing source retained
Keep the CPU topology generation in `Terrain3DMesher`:
- `generateOffsetData()`;
- `generateLodCache()`;
- `generateCombinedGeometry()`;
- `appendGrid()`;
- `t3dWeldClipmapGeometry()`;
- cooked/runtime geometry-pack support.

### Initial RD representation
Upload canonical geometry once:
- LOD0 position VB;
- LOD0 metadata VB;
- LOD0 IB;
- outer-ring position VB;
- outer-ring metadata VB;
- outer-ring IB.

At upload:
- inspect maximum welded index;
- select uint16 when possible;
- retain uint32 otherwise;
- store explicit index ranges.

### Key behavior change
Do not reproduce `instantiateStateMesh()` geometry duplication. Near/middle/far state becomes draw/patch state while all tiers reference shared canonical topology.

### Acceptance
- CPU and RD vertex/index counts match.
- Uploaded topology checksum matches source geometry.
- Camera movement creates no VB/IB resources.

---

## Part 6 - `Terrain3DRdShaderLibrary`

### New files
- `src/rendering/rd/terrain_3d_rd_shader_library.h`
- `src/rendering/rd/terrain_3d_rd_shader_library.cpp`

### New raw shaders
Under `project/addons/terrain_3d/rd/`:
- `terrain_clipmap_near.glsl`;
- `terrain_clipmap_middle.glsl`;
- `terrain_clipmap_far.glsl`;
- `terrain_depth.glsl`;
- `terrain_shadow.glsl`;
- `terrain_cull.glsl`;
- `instance_cull.glsl`;
- `instance_surface.glsl`.

### Responsibilities
- load/compile raw GLSL/SPIR-V;
- create shader RIDs;
- create raster/compute pipeline variants;
- cache by explicit pipeline key;
- rebuild only state dependent on target format/MSAA/view count;
- expose stable shader/pipeline generation numbers.

### Acceptance
- Near/middle/far pipelines compile independently.
- Pipeline cache returns stable RIDs for unchanged keys.
- Resize alone does not rebuild shader modules.

---

## Part 7 - `Terrain3DRdMaterialState`

### New files
- `src/rendering/rd/terrain_3d_rd_material_state.h`
- `src/rendering/rd/terrain_3d_rd_material_state.cpp`

### Existing `Terrain3DMaterial`
Keep it as:
- public/editor material configuration;
- RenderingServer backend implementation;
- source of production material state.

### RD representation
Create fixed-layout GPU blocks:
- per-frame/per-view UBO;
- terrain-global UBO;
- material layer SSBO;
- clipmap state SSBO;
- patch/instance SSBOs.

Bridge current resident Godot textures using `texture_get_rd_texture()` for parity instead of duplicating texture memory.

### Uniform-set policy
Recreate only when dependent buffer/texture RIDs change. Camera movement updates buffer contents only.

### Acceptance
- No normal-frame uniform-set creation.
- Material generation changes invalidate only dependent bindings.
- Borrowed terrain textures are not double-counted or double-freed.

---

## Part 8 - `Terrain3DRdClipmapRenderer`

### New files
- `src/rendering/rd/terrain_3d_rd_clipmap_renderer.h`
- `src/rendering/rd/terrain_3d_rd_clipmap_renderer.cpp`

### Responsibilities
- own GPU patch descriptor storage;
- consume canonical topology;
- upload dirty patch ranges;
- maintain near/middle/far draw ranges;
- bind material/pipeline state;
- emit direct indexed terrain draws;
- later emit indirect draws;
- expose depth/shadow draw entry points.

### Patch descriptor
Initial naturally aligned representation:
```cpp
typedef struct t3dRdPatch_s {
    float m_fOriginX;
    float m_fOriginZ;
    float m_fScale;
    uint32_t m_nLod;
    uint32_t m_nTier;
    uint32_t m_nStitchState;
    uint32_t m_nFlags;
    uint32_t m_nReserved;
} t3dRdPatch_t;
```

### Existing `Terrain3DMesher`
Its CPU clipmap calculations remain authoritative. RS-specific batches become fallback-only while RD consumes patch descriptors.

### Acceptance
- Clipmap alignment and snap behavior match RS.
- Stitch states produce no cracks.
- Near/middle/far boundaries match reference output.

---

## Part 9 - `Terrain3DRdCompositorEffect`

### New files
- `src/rendering/rd/terrain_3d_rd_compositor_effect.h`
- `src/rendering/rd/terrain_3d_rd_compositor_effect.cpp`

### Responsibilities
- run in the selected compositor callback;
- obtain `RenderSceneData` and `RenderSceneBuffersRD`;
- resolve color/depth RIDs;
- resolve camera/view state;
- update frame UBO;
- obtain framebuffer variant;
- invoke clipmap/instance renderers.

It must not perform:
- region streaming;
- topology generation;
- HLOD selection;
- instance page loading;
- CPU culling work unrelated to submission.

### Framebuffer cache
Key on:
- color RID;
- depth RID;
- sample count;
- view count;
- framebuffer/attachment format.

### Acceptance
- viewport resize is safe;
- render scaling changes are safe;
- MSAA changes are safe;
- no callback-lifetime `RenderData` pointer is retained.

---

## Part 10 - Raster parity milestone

### Work
Port only enough shader behavior to establish production terrain parity:
- height/displacement;
- holes/background behavior;
- near/middle/far tier material behavior;
- normal/roughness/AO outputs required by the selected lighting path;
- fog/environment approximation needed by the compositor route.

### Deliberately excluded
- compact topology;
- GPU culling;
- indirect draws;
- new LOD policy;
- instance conversion;
- HLOD conversion.

### Acceptance
Reference captures agree for:
- static camera;
- clipmap movement;
- level snaps;
- world-origin extremes used by the project;
- near/middle/far boundaries;
- holes;
- render scale and MSAA configurations.

---

## Part 11 - Stable update/lifetime pass

### Work
Ensure steady state converges after warmup.

Normal frame operations:
- one frame/view buffer update;
- dirty patch-range updates when required;
- zero topology uploads;
- zero VB/IB creation;
- zero pipeline creation;
- zero descriptor/uniform-set creation.

### Acceptance
Extended traversal shows constant topology RID count and no resource-count drift.

---

## Part 12 - Compact clipmap topology

### New representation
```cpp
typedef struct t3dRdClipmapVertex_s {
    int16_t m_nGridX;
    int16_t m_nGridZ;
    uint16_t m_nPartCode;
    uint16_t m_nFlags;
} t3dRdClipmapVertex_t;
```

Move LOD entirely out of per-vertex data and into patch/draw state.

### Optional later variant
Storage-buffer vertex pulling via `gl_VertexIndex` only after the fixed 8-byte vertex path is measured.

### Acceptance
- byte accounting reflects expected topology reduction;
- output remains visually identical;
- no additional CPU work appears on camera movement.

---

## Part 13 - `Terrain3DRdCulling`

### New files
- `src/rendering/rd/terrain_3d_rd_culling.h`
- `src/rendering/rd/terrain_3d_rd_culling.cpp`

### Persistent buffers
Terrain:
- patch descriptors;
- visible patch indices;
- tier counters;
- indirect indexed commands;
- scratch.

Instances later reuse the same culling service with separate tables.

### Terrain compute pass
- frustum test patch bounds;
- classify tier/LOD if policy moves to GPU;
- append visible patches per tier;
- emit bounded indirect commands.

### Debug path
Keep direct CPU-issued draw submission selectable for validation.

### Acceptance
- CPU validation mode and GPU visible sets agree;
- every indirect range remains inside topology buffers;
- counter reset/order barriers are correct.

---

## Part 14 - `Terrain3DRdMeshCache`

### New files
- `src/rendering/rd/terrain_3d_rd_mesh_cache.h`
- `src/rendering/rd/terrain_3d_rd_mesh_cache.cpp`

### Responsibilities
- import/read each registered mesh surface once;
- pack vertex data into Terrain3D runtime format;
- select 16/32-bit index width;
- append data into large persistent arenas;
- store mesh/surface/material/LOD range records;
- expose generation-safe mesh handles;
- account capacity/used bytes separately.

### Arena classes
At minimum:
- static instance vertex arena;
- static instance index arena.

Optionally separate HLOD arenas if lifetime/churn measurements justify it.

### Acceptance
- no GPU allocation per tiny asset surface;
- geometry ranges remain stable until explicit cache rebuild;
- mesh handles reject stale generations.

---

## Part 15 - `Terrain3DRdInstanceRenderer`

### New files
- `src/rendering/rd/terrain_3d_rd_instance_renderer.h`
- `src/rendering/rd/terrain_3d_rd_instance_renderer.cpp`

### Existing `Terrain3DInstancer`
Keep:
- logical cells;
- region lists;
- dirty-region tracking;
- transition buckets;
- streaming radius/cell policy;
- placement state;
- mesh settings revisions.

Replace only GPU MultiMesh ownership/submission.

### RD path
```text
Terrain3DInstancer logical state
    -> packed instance descriptors
        -> GPU instance table
            -> region/cell culling
                -> per-instance culling where needed
                    -> mesh/LOD/material buckets
                        -> indexed indirect draws
```

### Instance record target
16-32 bytes for production static instances, plus an uncompressed parity/debug mode.

### Acceptance
- transform/material/LOD parity with MultiMesh path;
- logical instance counts and rendered counts agree;
- instance residency does not create one RD resource per cell.

---

## Part 16 - HLOD RD conversion

### Existing `Terrain3DHLODStreamer`
Keep:
- manifest compilation;
- hierarchy;
- selection;
- residency budgets;
- promotion/retirement behavior;
- package loading.

### Replace
- RenderingServer instance RID creation;
- direct Godot `Mesh` submission.

### New path
```text
HLOD package
    -> Terrain3DRdMeshCache mesh handle
        -> HLOD/static instance descriptor
            -> Terrain3DRdInstanceRenderer
```

### Acceptance
- HLOD transition semantics unchanged;
- no duplicate HLOD-specific GPU uploader exists;
- geometry memory is accounted through the mesh cache.

---

## Part 17 - `Terrain3DRdDiagnostics`

### New files
- `src/rendering/rd/terrain_3d_rd_diagnostics.h`
- `src/rendering/rd/terrain_3d_rd_diagnostics.cpp`

### Counters
Track at least:
- GPU frame ID;
- topology VB/IB bytes;
- mesh arena capacity/used;
- patch bytes;
- instance bytes;
- uniform bytes;
- indirect bytes;
- culling scratch bytes;
- bytes uploaded/frame;
- update call count;
- VB/IB create/free counts;
- pipeline create/rebuild count;
- uniform-set create/rebuild count;
- visible/culled patches;
- visible/culled instances;
- indirect command count;
- compute dispatch count;
- last successful upload generation;
- last successful render pass.

Name every major RD resource with `RenderingDevice::set_resource_name()`.

### Integration
Feed these values into the existing Terrain3D diagnostics/runtime-profiler system rather than creating a parallel profiler UI.

---

## Part 18 - GPU memory budget integration

### Existing system
Extend `Terrain3DMemoryBudgetCoordinator`; do not replace the existing CPU arena/zone/pool systems.

### Physical RD categories
- topology vertex;
- topology index;
- mesh vertex arena;
- mesh index arena;
- patch storage;
- instance storage;
- indirect storage;
- uniform storage;
- culling scratch;
- Terrain3D-owned RD textures.

Keep physical allocation accounting distinct from logical feature budgets so borrowed RenderingServer textures are not counted twice.

### Acceptance
- total live owned RD bytes equal the sum of physical categories;
- borrowed texture bytes are reported separately or excluded from owned total;
- arena capacity and used bytes are distinct.

---

## Part 19 - Optional upload ring

### Add only when required
- `src/rendering/rd/terrain_3d_rd_upload_ring.h`
- `src/rendering/rd/terrain_3d_rd_upload_ring.cpp`

First implementation uses `RenderingDevice::buffer_update()` for small dirty ranges.

If the custom persistent upload bridge becomes the standard path, centralize it into one upload ring with:
- mapped staging allocation;
- frame segments;
- offset/alignment handling;
- generation/fence tracking;
- byte/update telemetry.

No subsystem gets a private persistent-upload mechanism.

---

## Part 20 - Lighting/shadow route

Choose one route after terrain raster parity.

### Route A - GDExtension-owned terrain lighting
Add:
- `Terrain3DRdLightingState`;
- `Terrain3DRdShadowRenderer` if dynamic terrain sun shadows are required.

Inputs include:
- sun direction/color/intensity;
- terrain baked/precomputed lighting;
- AO/cavity;
- fog/environment state;
- terrain-owned shadow state.

### Route B - engine-integrated Forward+ submission
Add a small Godot-fork bridge accepting immutable Terrain3D render packets before Forward+ opaque/depth/shadow list construction.

Terrain3D keeps ownership of all RD buffers; the engine only consumes current-frame packet references.

Packet fields include:
- vertex/index buffers or arena ranges;
- vertex/index formats;
- pipeline/material key;
- transform or patch descriptor index;
- AABB;
- render mask;
- depth/shadow flags;
- previous transform/state for velocity.

---

## Part 21 - Validation matrix

### Static/build
- regenerate MSVC project files;
- compile x64 debug and release;
- compile D3D12 Windows target;
- compile Vulkan-capable target;
- run configured formatting/analyzer checks;
- rescan stale RS-only symbols after each migration stage.

### Visual
- all stitch states;
- clipmap snaps;
- near/middle/far transitions;
- height/displacement;
- holes;
- camera-relative movement;
- origin extremes;
- MSAA on/off;
- render scaling;
- editor/runtime cameras;
- selected shadow profile.

### Lifetime
After initialization and long traversal capture:
- created/live/freed VB count;
- created/live/freed IB count;
- live topology bytes;
- mesh arena capacity/used;
- patch/instance/uniform/indirect/scratch bytes;
- pipeline and uniform-set rebuild counts;
- grow/free events.

### Performance
Measure independently:
- CPU frame preparation;
- render-thread CPU;
- upload bytes/calls;
- draw submission count;
- compute dispatch count;
- terrain GPU time;
- culling GPU time;
- VRAM delta;
- clipmap snap spikes;
- region-stream spikes.

---

## Recommended implementation order

1. Compile-clean baseline.
2. Backend enum + render coordinator.
3. RD context + resource owner + diagnostics foundation.
4. Frame snapshot boundary.
5. Canonical clipmap topology upload.
6. Shader library.
7. Material state.
8. Clipmap renderer.
9. Compositor adapter/framebuffer cache.
10. Raster/material parity.
11. Stable lifetime/update pass.
12. Compact topology.
13. GPU terrain culling + indirect draws.
14. Mesh cache/arenas.
15. Instance renderer.
16. HLOD conversion.
17. Full memory-budget integration.
18. Optional persistent upload ring.
19. Lighting/shadow route.
20. Extended D3D12/Vulkan validation.
21. Remove RS path only after it is no longer required as a reference/fallback.

## Final target ownership model

```text
Terrain3D
|
+-- existing CPU/data/streaming/authoring/collision systems
|
+-- Terrain3DRenderCoordinator
    |
    +-- RenderingServer fallback
    |
    +-- RenderingDevice backend
        |
        +-- Terrain3DRdContext
        +-- Terrain3DRdResourceOwner
        +-- Terrain3DRdDiagnostics
        +-- Terrain3DRdShaderLibrary
        +-- Terrain3DRdMaterialState
        +-- Terrain3DRdTopology
        +-- Terrain3DRdClipmapRenderer
        +-- Terrain3DRdCulling
        +-- Terrain3DRdMeshCache
        +-- Terrain3DRdInstanceRenderer
        +-- Terrain3DRdCompositorEffect
        +-- optional lighting/shadow/upload systems
```
