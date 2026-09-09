# Terrain3D Instancer Fault Reporting / Texture Fallback Fix — 2026-09-09

## Build contract

Expected native/script build ID:

```text
instancer-fault-reporting-texture-fallback-fix-2026-09-09
```

This patch keeps `T3D_INSTANCER_MINIMAL_RENDER_PATH=1` and preserves the minimal bring-up route. It does not re-enable instance streaming, HLOD, GPU culling, depth/shadow/motion instance passes, or LOD transitions.

## Patched issue 1 — non-empty instance scenes can no longer silently prepare as empty

`Terrain3DRdInstanceRenderer::prepareForwardPass()` previously returned success with no error whenever any of the following were empty:

- instance records;
- cells;
- mesh vertex bytes;
- mesh LODs;
- mesh surfaces.

That allowed a real instance publication failure to appear as a successful Forward+ prepare with zero draws.

The path now distinguishes:

1. **Truly empty scene** — zero records and zero cells. This remains a valid no-op.
2. **Structurally incomplete scene** — records without cells or cells without records. This is a hard prepare failure with counts.
3. **Non-empty scene without renderable mesh publication** — hard prepare failure with records, cells, vertex bytes, LOD count and surface count.
4. **Published cells whose mesh source keys all fail resolution** — a hard failure in the minimal bring-up path with input/resolved/unresolved counts.
5. **Successful prepare with zero packets/draws** — the coordinator now emits a rate-limited stage report rather than silently incrementing a counter.

The older non-Forward `render()` path receives the same empty/incomplete/renderable-mesh distinction so the semantics no longer diverge.

## Patched issue 2 — optional instance material textures can no longer suppress mesh geometry

The mesh cache was intended to keep mesh geometry resident when an imported material texture could not be uploaded, using the existing fallback albedo/normal/ORM textures instead.

Two implementation details violated that contract:

- absence of `Terrain3DRdTextureManager` caused `syncTextures()` to fail whenever the mesh snapshot contained textures;
- `Terrain3DRdTextureManager::renderCreateTexture()` marked the **shared** RD context failed for unsupported/failed optional instance material textures, so the subsequent vertex/index arena upload lost `getDevice()`.

The texture creation API now has an explicit fatal/non-fatal policy:

```cpp
bool bFailureFatal = true
```

Existing callers retain fatal behavior by default. Imported instance material textures call it with `false`.

For instance material textures:

- unsupported texture format/usage does not poison the shared RD context;
- texture creation failure does not poison the shared RD context;
- missing texture manager does not abort geometry upload;
- missing/failed textures increment a fallback counter and material binding resolves the source ID to fallback textures;
- texture resource ownership is inferred from the resource name, so `T3D.Instance.*` textures are accounted to the instance owner instead of terrain.

Registration/accounting failures remain fatal because they indicate a renderer ownership/resource-registry defect rather than optional asset data.

## Material generation false-cache-hit fix

`uploadMaterials()` previously committed:

```cpp
m_nUploadedMeshGeneration = m_meshCache.getSourceGeneration();
```

before `rebuildMaterialBindings()` had succeeded.

A transient sampler/fallback/uniform-set failure could therefore become a false cache hit on the next call.

The generation is now committed only after:

- material table upload succeeds;
- samplers/fallback textures are valid;
- all material pair bindings are valid.

The fast-path additionally requires the material binding generation to match and the binding table to be non-empty.

## Mesh-cache error stages

`Terrain3DRdMeshCache` now retains:

- upload attempts;
- upload failures;
- optional texture fallback count;
- last stage error.

Failures are differentiated across:

- null snapshot;
- shared RD context initialization;
- device validation;
- vertex format creation/registration;
- optional texture synchronization backing;
- arena capacity calculation;
- vertex/index arena creation;
- vertex/index arena registration;
- vertex/index array creation;
- vertex/index array registration;
- dirty-upload scratch;
- vertex buffer update;
- uint16 index buffer update;
- uint32 index buffer update;
- retained mesh metadata copy.

The top-level instance renderer preserves these lower-level errors instead of overwriting them with a generic `mesh/material data` message.

## Preview error stages

The old generic error:

```text
Failed to upload RD editor preview mesh/material data
```

is removed from the renderer.

Preview failures now distinguish at least:

```text
RD editor preview received a null mesh publication
RD editor preview could not acquire the shared RenderingDevice; ...
RD editor preview color/depth target creation/registration failed
RD editor preview mesh-cache upload failed: <exact mesh-cache stage>
RD editor preview material upload failed: <exact material stage>
RD editor preview mesh asset/LOD could not be resolved
RD editor preview scene/visibility/draw/readback failures
```

## Resource-registration retry safety

A failed `registerResource()` transitions the shared RD context to a failed state. Several instance paths previously left the just-created but never-registered RID in a member and later passed it to `Terrain3DRdContext::freeResource()`. That is an untracked free and can turn the original error into a second ownership failure.

The active instance bring-up path now directly frees and clears never-registered RIDs using the already-borrowed `RenderingDevice*` for:

- instance mesh vertex/index arenas;
- mesh vertex/index array views;
- base instance shader;
- Forward+ instance pipelines;
- instance frame UBO;
- general instance buffers;
- base set-0 uniform set;
- Forward+ set-0 uniform sets;
- material samplers;
- fallback material textures;
- base material pair set-1;
- Forward+ material pair set-1;
- preview color/depth targets.

Registered resources still use normal `Terrain3DRdContext::freeResource()` accounting.

## Packet/visibility fault lines

Silent false returns in the CPU/reference packet path now identify:

- cell-span backing shortage;
- packet slot capacity overflow;
- packet entry capacity overflow;
- sealed vector/hash overflow;
- lost surface handles during packet finalization;
- draw/depth/alpha/shadow packet output overflow;
- instance/HLOD/shadow descriptor output overflow;
- invalid view/projection state;
- missing RD device during visibility upload;
- visible-index buffer update failure;
- indirect-command buffer update failure.

In minimal bring-up mode, if published cells exist but **zero mesh keys resolve**, prepare now fails with an explicit source-key-resolution error rather than continuing toward a zero-draw frame.

## Coordinator stage diagnostics

A failed instance Forward+ prepare now logs one fault line containing:

```text
pass
scene_pass
records
cells
published_vertex_bytes
published_lods
published_surfaces
packet_input_cells
resolved_cells
unresolved_cells
draw_packets
mesh_upload_failures
texture_fallbacks
error
```

A nominally successful prepare that produces zero draws for a non-empty expected instance lane emits the same stage ladder plus GPU mesh-cache LOD/surface counts.

This should make the next runtime log identify the first broken boundary instead of requiring another broad pipeline audit.

## Diagnostics additions

`Terrain3DRdInstanceRenderer::getDiagnostics()` now exposes:

```text
mesh_upload_attempts
mesh_upload_failures
mesh_texture_fallbacks
mesh_last_error
```

Low-overhead getters expose packet/resolution/upload counters to the coordinator without constructing a diagnostic `Dictionary` in the render hot path.

## Verification

### C++17 syntax

Debug/tools-style front-end syntax checks pass for:

- `terrain_3d_rd_texture_manager.cpp`
- `terrain_3d_rd_mesh_cache.cpp`
- `terrain_3d_rd_instance_renderer.cpp`
- `terrain_3d_render_coordinator.cpp`

Release-style syntax checks pass for the same four files.

Dependent debug/tools checks also pass for:

- `assets/generated_texture.cpp`
- `core/terrain_3d_runtime_diagnostics.cpp`
- `register_types.cpp`

The only emitted compiler warning is the pre-existing generated godot-cpp `INT64_MIN` literal warning.

### Focused cumulative regression gates

17 focused verifiers pass:

- `verify_instance_fault_reporting_texture_fallback.py`
- `verify_instancer_minimal_render_path.py`
- `verify_instance_indirect_buffer_usage.py`
- `verify_instance_forward_submission.py`
- `verify_instance_first_population_render.py`
- `verify_instance_pipeline_recovery.py`
- `verify_instance_preview_and_lazy_forward_fix.py`
- `verify_p0_forward_viewport_lifecycle.py`
- `verify_p1_forward_lane_diagnostics.py`
- `verify_p2_forward_pass_resource_prerequisites.py`
- `verify_p09_gpu_indirect_visibility.py`
- `verify_p11_instance_gpu_culling.py`
- `verify_script_native_contract.py`
- `verify_gpu_page_tier_nondestructive.py`
- `verify_p10_vt_streaming.py`
- `verify_systems_c_style.py`
- `verify_systems_c_ownership.py`

## Runtime validation limitation

This environment has no Godot executable, SCons, or `glslangValidator`, so it cannot perform a linked GDExtension build or a live Godot 4.7.2 Forward+ viewport smoke test.

The next runtime test should first confirm:

```text
native_build_id=instancer-fault-reporting-texture-fallback-fix-2026-09-09
```

If instances remain invisible, the new log should now identify whether the first failure is mesh publication, RD mesh arena upload, material upload/binding, mesh source-key resolution, packet construction, or final submission.

## Separate Jolt editor raycast issue

The editor `PhysicsDirectSpaceState3D`/Jolt iteration-window error is a separate editor-query timing problem. This patch does not alter physics query scheduling because it is independent of the two instance rendering faults patched here.
