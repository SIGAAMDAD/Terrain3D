# Terrain3D RenderingDevice Conversion Patch Specification

## Scope

This document specifies how to convert the live Terrain3D rendering path from Godot `RenderingServer` mesh/MultiMesh ownership to an explicit `RenderingDevice` backend while preserving the current CPU terrain, streaming, residency, authoring, collision, and bake architecture.

The conversion is deliberately staged. The first milestone is rendering parity with explicit GPU ownership. The later milestones exploit the lower-level backend for shared topology, compact GPU data, compute culling, indirect draws, and explicit memory budgets.

Target engine: Godot 4.7.1, Forward+ first. Mobile may use the same RenderingDevice backend after Forward+ parity. Compatibility remains on the existing RenderingServer path because RenderingDevice is unavailable there.

The existing RenderingServer backend should remain selectable until the direct backend passes visual, memory, device-loss, and performance validation.

---

## 1. Current rendering architecture

### 1.1 Terrain clipmap

The live terrain clipmap is currently owned by `Terrain3DMesher` in:

- `src/rendering/terrain_3d_mesher.h`
- `src/rendering/terrain_3d_mesher.cpp`

The CPU side already has a useful separation between topology generation and scene submission:

1. `generateCombinedGeometry()` builds two canonical CPU geometries:
   - `LOD0_MESH`
   - `OUTER_RING_MESH`
2. `t3dWeldClipmapGeometry()` deduplicates vertices and remaps indices.
3. `instantiateMesh()` converts the CPU arrays into a RenderingServer mesh.
4. State batching may duplicate the canonical topology across several LODs in `instantiateStateMesh()`.
5. `createBatch()` / `createStateBatch()` create RenderingServer MultiMesh/instance RIDs.
6. Camera movement updates transforms, LOD-state data, material parameters, and AABBs rather than rebuilding geometry every frame.

The current surface payload contains:

- `Vector3` position;
- `Vector2` clipmap metadata (`part code`, `LOD`);
- index data;
- optional normal/tangent compatibility attributes for the fallback path.

The built-in state-batched path already avoids static normal/tangent storage.

### 1.2 Terrain material

`Terrain3DMaterial` currently owns Godot `Shader` / `ShaderMaterial` variants and builds spatial shader source from the shader include tree under:

- `project/addons/terrain_3d/shaders/`

The vertex shader expects the current clipmap metadata and LOD state. The near/middle/far tiers already provide a natural basis for three direct RenderingDevice pipelines.

### 1.3 Instancing

`Terrain3DInstancer` currently uses RenderingServer MultiMesh resources. Mesh geometry belongs to imported Godot `Mesh` resources; Terrain3D owns the instance population and uploads transforms/custom data through MultiMesh.

### 1.4 HLOD

`Terrain3DHLODStreamer` currently streams Godot mesh resources/packages and creates RenderingServer instances around them.

### 1.5 Existing direct RenderingDevice code

`Terrain3DCompute` already uses both:

- a local RenderingDevice for readback-oriented authoring work;
- the global renderer RenderingDevice for displacement work that must interoperate with visible scene resources.

This means the repository already has working conventions for:

- RD resource ownership;
- storage buffers;
- uniform sets;
- render-thread execution;
- RD/RenderingServer texture bridging;
- explicit barriers.

The renderer conversion should reuse these conventions rather than create a second unrelated RD utility layer.

---

## 2. Architectural constraint: GDExtension-only versus native Forward+ integration

This must be decided before implementation because the two targets are materially different.

### 2.1 GDExtension-only direct RD backend

A GDExtension can obtain Godot's global RenderingDevice through `RenderingServer::get_singleton()->get_rendering_device()` and can execute custom rendering from a `CompositorEffect` callback.

Godot exposes 3D compositor callbacks at:

- pre-opaque;
- post-opaque;
- post-sky;
- pre-transparent;
- post-transparent.

The pre-opaque callback occurs after Godot's depth prepass. Therefore a terrain rasterized from a GDExtension compositor callback is not a normal participant in the earlier Forward+ depth-prepass and shadow-list construction.

A pure GDExtension RD backend can still:

- render terrain color;
- write terrain depth;
- implement its own terrain normals/PBR or simplified lighting;
- sample Godot scene textures where exposed;
- use custom terrain shadowing;
- participate correctly in later depth testing if drawn into the active depth attachment;
- use compute culling and indirect drawing;
- own all terrain VB/IB/instance buffers directly.

It does **not** automatically inherit normal RenderingServer geometry behavior for:

- Forward+ clustered light application;
- native Godot shadow caster lists;
- native depth prepass contribution;
- native motion-vector generation;
- GI registration;
- decals;
- reflection-probe geometry registration.

Those features must either be reproduced by the Terrain3D renderer or deliberately remain on the RenderingServer backend.

### 2.2 Engine-integrated direct RD backend

If the terrain must participate exactly like native Forward+ geometry, add a small engine-side submission hook to the custom Godot build.

That hook should accept Terrain3D draw packets before Forward+ builds its opaque/depth/shadow render lists. Terrain3D still owns explicit RenderingDevice buffers; the engine hook only inserts their draw data into the relevant renderer passes.

This is the only target that combines all of the following at once:

- explicit Terrain3D vertex/index/storage buffer ownership;
- native Forward+ depth prepass;
- native directional/local shadow passes;
- native clustered lighting;
- native motion vectors;
- native scene render ordering.

### 2.3 Recommended repository target

Implement the repository in two layers:

1. **Terrain3D RD core** — buffer ownership, pipelines, shader data, culling, draw packets. This is renderer-agnostic within Godot's RenderingDevice.
2. **Presentation adapter** — initially a GDExtension `CompositorEffect`; optionally an engine submission bridge later.

Do not couple buffer ownership to the compositor callback itself. That would make an eventual engine hook unnecessarily expensive to adopt.

---

## 3. Target source layout

Add the following directory:

```text
src/rendering/rd/
    terrain_3d_rd_types.h
    terrain_3d_rd_context.h
    terrain_3d_rd_context.cpp
    terrain_3d_rd_resource_owner.h
    terrain_3d_rd_resource_owner.cpp
    terrain_3d_rd_topology.h
    terrain_3d_rd_topology.cpp
    terrain_3d_rd_shader_library.h
    terrain_3d_rd_shader_library.cpp
    terrain_3d_rd_material_state.h
    terrain_3d_rd_material_state.cpp
    terrain_3d_rd_clipmap_renderer.h
    terrain_3d_rd_clipmap_renderer.cpp
    terrain_3d_rd_culling.h
    terrain_3d_rd_culling.cpp
    terrain_3d_rd_instance_renderer.h
    terrain_3d_rd_instance_renderer.cpp
    terrain_3d_rd_mesh_cache.h
    terrain_3d_rd_mesh_cache.cpp
    terrain_3d_rd_compositor_effect.h
    terrain_3d_rd_compositor_effect.cpp
    terrain_3d_rd_diagnostics.h
    terrain_3d_rd_diagnostics.cpp
```

Add raw RD shaders under:

```text
project/addons/terrain_3d/rd/
    terrain_clipmap_near.glsl
    terrain_clipmap_middle.glsl
    terrain_clipmap_far.glsl
    terrain_depth.glsl
    terrain_shadow.glsl
    terrain_cull.glsl
    instance_cull.glsl
    instance_surface.glsl
```

Do not move the existing Godot spatial shader tree during the first conversion. It remains the reference renderer and fallback backend.

---

## 4. Backend selection and ownership

### 4.1 Add an explicit backend enum

Add a file-scope enum in the normal Terrain3D type style:

```cpp
typedef enum t3dRenderBackend_e {
    T3D_RENDER_BACKEND_RENDERING_SERVER = 0,
    T3D_RENDER_BACKEND_RENDERING_DEVICE = 1,
} t3dRenderBackend_t;
```

Expose the backend on `Terrain3D` as an editor/project setting and keep RenderingServer as the fallback for:

- Compatibility renderer;
- headless use;
- failed RD initialization;
- explicit debugging/parity comparisons.

### 4.2 One RD owner

Create one `Terrain3DRdContext` owned by `Terrain3D` or by a single renderer owner beneath it.

`Terrain3DRdContext` owns only non-Godot-resource GPU state:

- global RenderingDevice pointer (non-owning);
- shader RIDs;
- pipeline RIDs;
- sampler RIDs;
- uniform/storage buffers;
- vertex/index buffers;
- vertex/index array RIDs when used;
- uniform-set RIDs;
- indirect buffers;
- culling scratch buffers;
- render diagnostics counters.

Do not let individual clipmap batches independently create/free RD resources.

### 4.3 Render-thread rule

All operations touching the global RenderingDevice must occur on the render thread.

Use one of these two paths:

- execute draw/update work directly in the compositor render callback;
- queue create/destroy/rebuild work using `RenderingServer::call_on_render_thread()`.

CPU topology generation, streaming decisions, and visibility preparation may occur off the render thread as long as they only write CPU snapshots.

### 4.4 Snapshot boundary

The main/game thread should publish immutable render snapshots rather than mutate GPU objects directly.

Use a double- or triple-buffered frame snapshot:

```text
main/streaming thread
    builds snapshot N+1
        |
        v
atomic publication / generation number
        |
        v
render thread
    consumes latest complete snapshot
```

The snapshot contains handles/indices and POD data only. No `Ref<>` mutation is allowed while the render thread consumes it.

---

## 5. Phase 1: preserve current topology, replace only GPU submission

The first RD milestone must **not** combine the API conversion with procedural topology, GPU culling, or new LOD algorithms.

### 5.1 Keep CPU generation unchanged

Keep:

- `generateOffsetData()`;
- `generateLodCache()`;
- `generateCombinedGeometry()`;
- `appendGrid()`;
- `t3dWeldClipmapGeometry()`;
- runtime geometry pack loading/writing.

Change their output destination only.

### 5.2 Introduce `Terrain3DRdTopology`

`Terrain3DRdTopology` receives `t3dClipmapGeometry_t` and converts it to explicit byte buffers.

Initial parity format:

```text
position stream: 3 x float32
metadata stream: 2 x float32
index stream:    uint16 or uint32
```

This intentionally matches current semantics before compression.

### 5.3 Explicit index selection

At upload time:

```text
if maximum welded vertex index <= 65535:
    convert indices to uint16
    create INDEX_BUFFER_FORMAT_UINT16
else:
    retain uint32
    create INDEX_BUFFER_FORMAT_UINT32
```

The CPU geometry may continue using `PackedInt32Array`; only the upload representation changes.

Store:

```cpp
typedef struct t3dRdIndexRange_s {
    uint32_t m_nFirstIndex;
    uint32_t m_nIndexCount;
    int32_t m_nVertexOffset;
} t3dRdIndexRange_t;
```

### 5.4 No per-batch vertex/index allocation

Upload the canonical `LOD0_MESH` and `OUTER_RING_MESH` topology once.

Do **not** reproduce `instantiateStateMesh()` by concatenating duplicate geometry into one new GPU surface for each near/middle/far range.

The direct backend should instead issue repeated indexed draws against the same topology and vary LOD/tier state through instance or push-constant data.

That removes one of the largest avoidable differences between the existing state-batched representation and an explicit GPU representation.

### 5.5 Initial resource set

The first parity implementation should own approximately:

```text
1 LOD0 vertex buffer
1 LOD0 metadata buffer
1 LOD0 index buffer
1 outer-ring vertex buffer
1 outer-ring metadata buffer
1 outer-ring index buffer
1 clipmap LOD-state buffer
1 patch/instance state buffer
3 visible terrain pipelines (near/middle/far)
optional shadow/depth pipelines
persistent uniform sets
```

No vertex/index buffer is created during ordinary camera movement.

---

## 6. Phase 2: convert material/shader state

### 6.1 Spatial shader source cannot be passed through unchanged

The current Terrain3D shader tree is Godot spatial-shader source. RenderingDevice raster pipelines require raw RD shader stages.

Create a parallel raw GLSL implementation instead of trying to mechanically transform the current source at runtime.

The raw shaders should share generated constants/data definitions where practical, but they should be treated as a separate backend.

### 6.2 Map Godot built-ins explicitly

Replace spatial shader built-ins with explicit RD inputs.

At minimum provide:

```text
camera/view projection
camera transform / world camera position
current and previous view-projection for motion vectors if supported
terrain target position
vertex spacing / density
mesh size / tessellation level
region size / region map dimensions
near/middle/far tier state
clipmap LOD state
sun direction/color/intensity
fog/environment parameters used by the terrain shader
```

The current `VIEW_MATRIX`, projection state, and camera transform can be obtained from frame render data in the compositor adapter.

### 6.3 Material parameter block

Replace many individual RenderingServer `material_set_param()` calls with one stable GPU parameter block.

Use a fixed std140/std430-compatible structure. A reasonable split is:

- small per-frame/per-view UBO;
- terrain-global UBO;
- material layer storage buffer;
- clipmap state storage buffer;
- patch/instance storage buffer.

Keep scalar layout deterministic and `static_assert` CPU sizes/offsets where possible.

### 6.4 Texture access

Existing Godot texture RIDs can be bridged into the global RenderingDevice using `RenderingServer::texture_get_rd_texture()`.

Use this for the first parity implementation so the RD backend and current streaming/resource system share the same resident textures.

Do not duplicate all terrain texture memory merely to switch drawing APIs.

Later, caches that are already created directly on RD may remain direct RD textures and only receive RenderingServer wrappers when another Godot subsystem needs to sample them.

### 6.5 Uniform-set lifetime

Uniform sets should be recreated only when one of their dependent texture/buffer RIDs changes.

Normal per-frame camera movement must update data inside existing buffers, not rebuild descriptor/uniform sets.

---

## 7. Phase 3: raster integration

### 7.1 Add `Terrain3DRdCompositorEffect`

This object is the GDExtension presentation adapter.

Responsibilities:

- receive the current render callback;
- fetch `RenderSceneBuffersRD` and `RenderSceneData`;
- obtain color/depth texture RIDs;
- publish camera matrices into the Terrain3D frame UBO;
- ask `Terrain3DRdClipmapRenderer` to record terrain draw commands;
- perform no streaming or CPU topology work.

### 7.2 Framebuffer cache

The viewport color/depth buffers may be recreated on resize, MSAA changes, resolution scaling changes, or renderer reconfiguration.

Cache framebuffers by a key containing at least:

```text
color RID
depth RID
sample count
view count
framebuffer format
```

Invalidate on any key change.

Never retain `RenderData` itself outside the callback; it is frame-lifetime state.

### 7.3 Depth

For the GDExtension path, terrain should draw during `PRE_OPAQUE` into the active color/depth targets with depth test/write enabled.

Godot's depth prepass has already happened at this point. Therefore:

- the terrain can establish depth for later opaque geometry;
- it cannot retroactively participate in the already-completed depth prepass;
- any effect that specifically consumes the prepass before this callback will not contain terrain depth.

If this is unacceptable, use the engine-integrated submission path described later.

### 7.4 MSAA and multiview

Create pipeline/framebuffer variants matching:

- viewport sample count;
- view count;
- render-target format.

Do not assume one framebuffer/pipeline works for every viewport configuration.

---

## 8. Phase 4: eliminate duplicated clipmap vertex payload

After parity is stable, replace the 20-byte logical current position+metadata vertex payload with a compact topology record.

### 8.1 Move LOD out of the vertex stream

`instantiateStateMesh()` currently writes the LOD into metadata `.y` for every copied vertex.

With explicit instancing, LOD is draw/instance state and does not belong in every vertex.

Remove it from the vertex record.

### 8.2 Pack grid coordinates

The clipmap X/Z coordinates are small integer grid coordinates before scale/translation.

Use a packed vertex such as:

```cpp
typedef struct t3dRdClipmapVertex_s {
    int16_t m_nGridX;
    int16_t m_nGridZ;
    uint16_t m_nPartCode;
    uint16_t m_nFlags;
} t3dRdClipmapVertex_t;
```

Eight bytes replaces the current logical 20-byte position+metadata stream.

`Y` remains implicit because the terrain shader samples or derives height.

### 8.3 Optional storage-buffer vertex pulling

A later variant may place the packed topology table in a storage buffer and use `gl_VertexIndex` to pull vertex descriptors manually.

That removes the conventional vertex-array dependency and makes topology representation easier to change, at the cost of explicit buffer reads in the vertex shader.

Do this only after the 8-byte fixed vertex path is benchmarked.

---

## 9. Phase 5: replace clipmap MultiMesh state with patch descriptors

The clipmap does not need generic MultiMesh transforms once Terrain3D owns submission.

Use a compact patch descriptor:

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

The exact packing may be reduced later. Keep the first implementation naturally aligned and easy to inspect.

Upload only dirty descriptors when a clipmap level snaps.

Normal frame cost should be approximately:

```text
camera UBO update
possibly a few dirty patch records
no topology upload
no VB/IB creation
no descriptor recreation
```

---

## 10. Phase 6: GPU culling and indirect draws

Do not implement this until direct raster parity is stable.

### 10.1 Inputs

Create persistent storage buffers for:

- all terrain patch descriptors;
- visible patch indices;
- draw counters;
- indirect indexed-draw commands.

### 10.2 Compute pass

`terrain_cull.glsl` should:

1. read patch bounds;
2. test against camera frustum;
3. determine current LOD/tier if this policy is moved to GPU;
4. optionally test a Hi-Z/occlusion source when a reliable source is available;
5. append visible patches by tier;
6. emit indirect indexed-draw records.

### 10.3 Raster pass

Issue one indirect sequence per terrain tier/topology combination instead of one CPU call per visible patch.

Use `draw_list_draw_indirect()` after binding persistent index/topology state.

Keep a non-indirect debug path behind a build/runtime switch. It is essential for diagnosing indirect-buffer corruption.

---

## 11. Phase 7: convert the regular instancer

The terrain clipmap and asset instancer should not be converted in the same first patch. Their data problems are different.

After terrain parity:

### 11.1 Add `Terrain3DRdMeshCache`

At asset registration/load time:

1. read each imported mesh surface once;
2. convert it into a Terrain3D-owned packed vertex representation;
3. convert indices to 16-bit when possible;
4. append geometry into large vertex/index arenas;
5. store a small range record per mesh/LOD/surface/material.

Do not keep one D3D12/Vulkan allocation per tiny asset surface.

### 11.2 Arena layout

Prefer a small number of large persistent buffers:

```text
static vertex arena
static index arena
mesh surface table
material table
instance table
visible instance table
indirect command table
```

Use offsets/handles instead of GPU-resource-per-mesh ownership.

### 11.3 Packed instance format

The current MultiMesh transform format is generic. Terrain instances can use a format optimized for world/static placement.

Candidate record:

```text
quantized/local position
packed quaternion or yaw-only rotation where asset policy permits
packed scale
mesh handle
material/variant handle
flags
```

Target 16-32 bytes per static instance depending on required transform precision.

Keep an uncompressed 48-byte transform debug mode for parity testing.

### 11.4 Compute culling

Cull instances by region/cell first, then per-instance only where needed. Group visible output by mesh LOD/material and emit indexed indirect draws.

This replaces MultiMesh upload/submission with Terrain3D-owned SoA/AoSoA GPU data.

---

## 12. Phase 8: HLOD conversion

Reuse `Terrain3DRdMeshCache`; do not create a second HLOD-only mesh uploader.

The HLOD streamer should change from:

```text
load Godot Mesh -> create RenderingServer instance -> set base
```

to:

```text
load HLOD package -> resolve/upload mesh-cache handle -> publish HLOD instance descriptor
```

The HLOD residency system remains responsible for deciding what is resident. The RD mesh cache is responsible only for GPU geometry residency and ranges.

Large HLOD geometry may use a separate arena class from small instanced clutter if their churn/lifetime characteristics differ.

---

## 13. Lighting and shadow paths

### 13.1 GDExtension-only renderer

The first direct terrain shader should use explicit terrain lighting inputs rather than attempting to consume undocumented Forward+ internal clustered-light buffers.

Recommended first target:

- directional sun;
- existing baked/precomputed terrain lighting data;
- terrain AO/cavity;
- current material normal/roughness/metallic where desired;
- fog approximation matching the project environment;
- custom terrain self-shadow or heightfield shadow if required.

This maps cleanly onto the existing simplified terrain shader direction and avoids coupling to private renderer internals.

### 13.2 Custom sun shadow map

If direct terrain requires dynamic sun shadowing, add a Terrain3D-owned shadow pass:

```text
terrain topology
    -> sun-view depth texture
    -> sampled by terrain fragment shader
```

Because terrain is a heightfield, a specialized heightfield/horizon shadow implementation may be cheaper than rasterizing the entire clipmap into a conventional shadow map.

### 13.3 Native Forward+ shadows

If terrain must cast through Godot's native directional shadow pipeline and receive the native clustered-light implementation, implement the engine submission bridge rather than duplicating private Forward+ buffers in the GDExtension.

---

## 14. Engine submission bridge (optional but required for full Forward+ parity)

Keep this outside the GDExtension repository if possible.

Add a small custom engine module/API that accepts immutable Terrain3D render packets on the render thread.

A packet should reference:

```text
RD vertex/index buffers or buffer ranges
vertex format
index format/range
pipeline/material key
world transform or patch descriptor index
AABB
render layer mask
shadow/depth flags
previous transform/state for velocity
```

The engine renderer then inserts these packets into the same native passes used for opaque mesh geometry.

The bridge must not take ownership of Terrain3D buffers. Terrain3D remains the allocator/owner; the engine uses them only for the current frame.

This bridge is preferable to exposing large portions of Forward+ internal state to GDExtension.

---

## 15. Memory model

### 15.1 Separate physical ownership from logical accounting

Add explicit GPU memory categories to Terrain3D's existing memory budget reporting:

```text
RD topology vertex bytes
RD topology index bytes
RD mesh arena bytes
RD instance bytes
RD patch bytes
RD indirect bytes
RD uniform bytes
RD culling scratch bytes
RD terrain-owned texture bytes
```

Do not double-count RenderingServer-owned textures that are merely borrowed through `texture_get_rd_texture()`.

### 15.2 Fixed/high-water allocation policy

For stable terrain runtime, resource creation should converge after warmup.

Expected steady state:

- no per-frame vertex-buffer creation;
- no per-frame index-buffer creation;
- no per-frame pipeline creation;
- no per-frame uniform-set creation;
- no per-frame mesh resource creation;
- buffer updates limited to camera/frame data and dirty patch/instance ranges.

For instance/HLOD arenas, use explicit capacity classes and record grow events. Growth should happen by policy, not accidentally through many tiny RIDs.

### 15.3 Upload strategy

Use `buffer_update()` for small dirty ranges in the first implementation.

If the custom Godot build's persistent-upload bridge is retained, route high-frequency instance/patch updates through one explicit upload-ring abstraction rather than allowing each subsystem to invent its own mapped-buffer path.

The renderer should expose per-frame counters:

```text
bytes uploaded
buffer update call count
VB/IB create count
VB/IB free count
uniform-set rebuild count
pipeline rebuild count
indirect command count
visible patch count
visible instance count
```

---

## 16. D3D12/device-loss diagnostics built into the conversion

The direct backend should make GPU failures easier to diagnose than the current opaque mesh path.

### Required instrumentation

1. Call `RenderingDevice::set_resource_name()` for every major buffer, shader, and pipeline.
2. Assign stable numeric breadcrumbs to terrain draw/compute passes where the API accepts them.
3. Record resource creation generation and size in Terrain3D diagnostics.
4. Maintain a monotonically increasing GPU-frame ID in snapshots.
5. On any failed buffer/pipeline/uniform creation, stop issuing dependent terrain draws for that frame rather than cascading invalid RIDs.
6. Expose the last successful terrain pass/frame/upload generation to diagnostics.

Example resource names:

```text
T3D.Clipmap.Lod0.VB
T3D.Clipmap.Lod0.IB16
T3D.Clipmap.Outer.VB
T3D.Clipmap.Outer.IB16
T3D.Clipmap.Patches
T3D.Instances.StaticArena
T3D.Instances.Visible
T3D.Indirect.Near
T3D.Pipeline.Near
T3D.Pipeline.Middle
T3D.Pipeline.Far
```

This will make RenderDoc/DRED/backend diagnostics materially more useful.

---

## 17. Exact current-method migration map

| Current symbol | First RD action | Final state |
| --- | --- | --- |
| `Terrain3DMesher::generateCombinedGeometry()` | Keep unchanged | Keep as CPU fallback/cook generator |
| `t3dWeldClipmapGeometry()` | Keep unchanged | Keep or move to topology builder |
| `Terrain3DMesher::instantiateMesh()` | Replace with `Terrain3DRdTopology::upload()` | Delete RS implementation when fallback removed |
| `Terrain3DMesher::instantiateStateMesh()` | Do not reproduce GPU duplication | Remove from RD backend |
| `Terrain3DMesher::createBatch()` | Produce patch descriptors | RS-only fallback |
| `Terrain3DMesher::createStateBatch()` | Produce range/tier draw state | RS-only fallback |
| `Terrain3DMesher::freeBatch()` | Clear CPU handle; GPU topology remains shared | RS-only fallback |
| `Terrain3DMesher::setBatchInstance()` | Write patch descriptor | RD buffer dirty range |
| `Terrain3DMesher::flushDirtyBatches()` | Upload dirty patch ranges | RD buffer update |
| `Terrain3DMesher::updateBatchCustomAabb()` | Compute CPU/GPU patch bounds | Culling input |
| `Terrain3DMesher::configureStateMaterials()` | Build pipeline/material-state keys | RD shader/material state |
| `Terrain3DMesher::rebuildRenderBatches()` | Rebuild CPU draw ranges only | No VB/IB recreation |
| `Terrain3DMaterial::submitShaderSource()` | Keep for RS fallback | Add separate RD shader compiler/library |
| `Terrain3DCompute` global RD ownership | Reuse convention | Share context/device utilities |
| `Terrain3DInstancer` MultiMesh creation | Leave for terrain-only milestone | Replace with mesh arena + instance buffer later |
| `Terrain3DHLODStreamer` RS instance creation | Leave initially | Replace with mesh-cache handles later |

---

## 18. Implementation phases and commit boundaries

### Patch 1 - backend skeleton

- add backend enum/property;
- add `Terrain3DRdContext` and diagnostics;
- obtain global RD safely;
- no visual change.

**Acceptance:** RS backend renders identically; RD backend can initialize/destroy with zero leaked RIDs.

### Patch 2 - direct clipmap topology upload

- upload canonical LOD0/outer geometry to explicit VB/IB;
- no drawing yet;
- expose memory telemetry.

**Acceptance:** uploaded vertex/index counts and checksums match current CPU geometry; 16-bit fallback logic tested.

### Patch 3 - compositor raster parity

- add compositor effect;
- add raw near/middle/far shaders;
- draw terrain through RD;
- keep RS terrain switchable.

**Acceptance:** geometry alignment, height, seams, stitch state, near/middle/far boundaries match reference screenshots.

### Patch 4 - material parity

- bridge resident textures;
- port current surface material logic required by production profile;
- editor/debug overlays may remain RS temporarily.

**Acceptance:** material/normal/roughness/terrain texture transitions match chosen reference configuration.

### Patch 5 - stable lifetime/update model

- persistent uniform sets;
- dirty-range patch uploads;
- no normal-frame VB/IB/pipeline/descriptor creation.

**Acceptance:** telemetry reports zero topology resource churn during extended camera traversal.

### Patch 6 - compact clipmap topology

- 8-byte packed vertex record;
- remove per-vertex LOD;
- 16-bit indices where possible.

**Acceptance:** byte-exact memory counters and no visual difference.

### Patch 7 - GPU culling and indirect terrain draws

- culling compute;
- visible lists;
- indirect draws;
- debug direct-draw fallback.

**Acceptance:** visible set agrees with CPU validation mode and indirect commands remain in bounds.

### Patch 8 - instance renderer

- mesh geometry arenas;
- packed instance buffer;
- compute culling;
- indirect grouped draws.

**Acceptance:** transform/material/LOD parity with existing MultiMesh path.

### Patch 9 - HLOD renderer

- reuse mesh arena/cache;
- direct HLOD draw descriptors;
- remove HLOD RS instances when stable.

### Patch 10 - lighting/shadow decision

Choose either:

- GDExtension-owned terrain lighting/shadow implementation; or
- engine-native Forward+ submission bridge.

Do not leave the renderer half-dependent on undocumented internal Forward+ buffers.

### Patch 11 - remove obsolete RS terrain path

Only after the direct backend is the shipping default and extended validation passes.

---

## 19. Validation matrix

Every major phase should be tested on at least:

```text
D3D12 - Intel Arc/Battlemage
D3D12 - NVIDIA
D3D12 - AMD
Vulkan - same Intel machine where possible
Vulkan - discrete GPU
```

### Visual validation

- identical clipmap origin/snap behavior;
- no cracks during all stitch states;
- near/middle/far transitions;
- height and displacement parity;
- holes/background behavior;
- camera-relative movement;
- world origin extremes used by the project;
- MSAA on/off;
- internal render scaling;
- editor camera and runtime camera;
- shadow-enabled/disabled profiles.

### GPU correctness

- validation/debug build clean where supported;
- no out-of-range indirect draws;
- no invalid RID use;
- no use-after-free on viewport resize;
- no uniform-set use after dependent texture replacement;
- no resource creation after owner teardown;
- no render-thread violations.

### Memory/lifetime

Capture after initialization and after a long traversal:

```text
created VB count
created IB count
live VB bytes
live IB bytes
mesh-arena capacity/used bytes
instance-buffer capacity/used bytes
uniform/storage bytes
scratch bytes
resource grow count
resource free count
```

Steady-state topology counts must remain constant.

### Performance

Measure separately:

- CPU frame setup;
- terrain render-thread CPU;
- upload bytes/calls;
- draw calls;
- compute dispatches;
- GPU terrain pass time;
- GPU culling time;
- total VRAM delta;
- camera snap spikes;
- region-stream spikes.

Do not treat an API conversion alone as an optimization. Each performance claim must name the removed work: duplicated topology, resource allocation, upload bandwidth, CPU culling, draw submission, or memory bandwidth.

---

## 20. Completion criteria

The conversion is complete only when all of the following are true:

- Terrain3D owns explicit terrain VB/IB state on RenderingDevice.
- No terrain VB/IB is created during ordinary runtime traversal.
- Canonical topology is shared instead of duplicated per state batch.
- RD resource ownership and teardown are deterministic.
- The direct backend has explicit byte accounting.
- Near/middle/far production shader behavior is ported.
- Texture residency is shared or explicitly owned without double allocation.
- Viewport resize/MSAA/render-scale changes rebuild only framebuffer/pipeline state that truly depends on them.
- D3D12 and Vulkan both survive extended traversal without resource-count growth.
- The RS fallback is removed only after direct-backend parity is demonstrated.
- If native Forward+ shadows/lighting/depth-prepass participation is required, the engine submission bridge is implemented rather than approximated through undocumented buffers.

---

## 21. Relevant Godot APIs

Godot 4.7 documentation/reference points used by this plan:

- RenderingDevice: https://docs.godotengine.org/en/4.7/classes/class_renderingdevice.html
- Compositor: https://docs.godotengine.org/en/4.7/tutorials/rendering/compositor.html
- CompositorEffect: https://docs.godotengine.org/en/4.7/classes/class_compositoreffect.html
- RenderingServer render-thread dispatch: https://docs.godotengine.org/en/4.7/classes/class_renderingserver.html
- RenderData: https://docs.godotengine.org/en/4.7/classes/class_renderdata.html
- RenderSceneData: https://docs.godotengine.org/en/4.7/classes/class_renderscenedata.html
- RenderSceneBuffersRD: https://docs.godotengine.org/en/4.7/classes/class_renderscenebuffersrd.html

Key API capabilities used by the design include explicit vertex/index buffers, 16/32-bit index formats, persistent storage/uniform buffers, direct vertex-buffer binding, indirect draws, RD texture bridging, render-scene color/depth access, and render-thread callbacks.
