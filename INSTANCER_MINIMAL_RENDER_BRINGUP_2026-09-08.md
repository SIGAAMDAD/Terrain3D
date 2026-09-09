# Terrain3D Instancer Minimal Render Bring-Up

**Date:** 2026-09-08  
**Purpose:** reduce the live instancer to the smallest existing direct-RD path that can prove a real instanced mesh reaches Godot Forward+.

## Bring-up switch

`src/instances/terrain_3d_instancer_bringup.h`

```cpp
#define T3D_INSTANCER_MINIMAL_RENDER_PATH 1
```

Set this to `0` to restore the preserved optimized path.

## Active path

```text
resident authoritative Terrain3DInstanceStore
        -> LOD0 source mesh only per published cell
        -> packed 32-byte RD instance records
        -> immutable RD scene publication
        -> existing Terrain3DRdMeshCache
        -> CPU reference packet/visible-index construction
        -> correctly addressed indexed-indirect draw
        -> Forward+ opaque / transparent pass
```

The indexed-indirect submit remains intentional. The mesh cache stores surfaces inside shared vertex/index arenas, so raw direct indexed draw calls cannot represent the required first-index/base-vertex addressing without building offset array views first.

## Deliberately bypassed

- render-cell materialization (`m_renderCells` is expected to remain empty);
- pending cell-update processing;
- cell streaming;
- instance-region streaming cache;
- instance LOD selection and transitions;
- fade/range rejection;
- camera visibility-layer rejection;
- CPU frustum rejection;
- production GPU instance culling;
- HLOD instance combination and HLOD visibility dispatch;
- HLOD detail suppression;
- instance depth-prepass dependency;
- instance depth, shadow, and motion passes;
- temporal/motion-history preparation for the visible bring-up lane.

The source/authoring store, packed instance ABI, mesh cache, material conversion/binding, Forward+ shader/pipeline, shared GPU arenas, visible-index buffer, indirect command buffer, and final Forward+ submission are still real production components.

## Publication policy

`Terrain3DInstancer::buildRdInstanceSnapshot()` walks the authoritative resident region stores directly. For every non-empty logical cell of an enabled mesh asset it publishes:

- mesh LOD `0`;
- all authored transforms;
- authored colors or white fallback;
- unrestricted visibility layers;
- no LOD begin/end range;
- no fade;
- no shadow/HLOD/highlight flags;
- world-space bounds derived from the LOD0 source mesh.

Any authoritative instance mutation marks the complete RD instance snapshot dirty directly. No render-cell state has to converge before the renderer can see the mutation.

## Expected diagnostics

The instancer statistics expose:

```text
minimal_render_path = true
render_cells = 0
```

`render_cells = 0` is correct for this branch and must not be used as a failure signal.

Use the renderer/coordinator counters in this order:

```text
logical_instances
    -> published_instance_records / published_instance_cells
    -> published_mesh_surfaces / mesh arena bytes
    -> instance_packet_build_input_cells
    -> instance_resolved_mesh_cells / instance_unresolved_mesh_cells
    -> instance_draw_packets
    -> instance_prepare_succeeded / instance_prepared_has_draws
    -> instance_submit_succeeded / instance_submit_draws
```

Interpretation:

- `logical_instances > 0`, published records `= 0`: authoritative store residency/publication is still wrong.
- published records `> 0`, resolved mesh cells `= 0`: source-key / mesh-source / mesh-cache failure.
- resolved cells `> 0`, draw packets `= 0`: surface/material/pass routing failure.
- draw packets and submitted draws `> 0`, but no pixels: the remaining fault is below the instancer -- transform unpack, vertex/index arena addressing, material/alpha behavior, Forward+ pipeline state, or raster submission.

Because CPU reference visibility directly populates the visible-index and indirect-command instance counts, a zero-instance GPU-cull command can no longer explain invisibility in this mode.

## Reintroduction order after first visible mesh

Re-enable one boundary at a time:

1. camera visibility layers and CPU frustum rejection;
2. GPU instance culling;
3. render-cell materialization;
4. cell/region streaming;
5. LOD selection, transitions, and fade;
6. depth/shadow/motion passes and temporal history;
7. HLOD combination/suppression;
8. category-specific clutter / ground-cover / occluder policy.

Do not re-enable multiple boundaries in one patch until the first visible mesh has been proven.

## Verification performed

- C++17 `clang++ -fsyntax-only` passes for all four modified translation units in debug/tools and release-style configurations.
- All repository verification scripts pass, including the new `verify_instancer_minimal_render_path.py` gate: **45 / 45**.
- `verify_systems_c_style.py` and `verify_systems_c_ownership.py` pass.
- Repository-wide `format_cpp.py --check` still reports the pre-existing formatting drift already present in the supplied tree; no whitespace errors were reported by a no-index diff check of the modified source tree.
- A Godot runtime smoke test was not possible in this environment because no Godot executable is installed.
