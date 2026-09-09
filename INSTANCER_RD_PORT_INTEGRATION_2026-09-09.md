# Terrain3D Full Project - Instancer RenderingDevice Port Integration

Date: 2026-09-09
Build ID: `instancer-rd-port-integrated-2026-09-09`

## Scope

This patch integrates the upstream-derived RenderingDevice instancer presentation model into the latest full Terrain3D fork without replacing the fork's authoritative instancer data model or unrelated renderer systems.

Preserved from the full fork:

- `Terrain3DInstanceStore` authoritative instance ownership
- region/cell streaming and residency
- painting/editing/serialization APIs
- authored transforms and colors
- LOD selection and render-cell publication
- material metadata and imported material textures
- Forward+ depth/opaque/shadow/motion/transparent pass integration
- motion-history support
- existing HLOD publication; the direct CPU path performs HLOD frustum visibility instead of requiring indirect-command mutation

Not ported or replaced:

- terrain clipmap renderer
- terrain Near/Middle/Far page system
- ocean renderer
- collision/navigation
- HLOD streamer implementation
- RenderWorld
- terrain material authoring system

## Active instance rendering path

```text
Terrain3DInstanceStore / render cells
        |
        v
immutable RD instance publication
        |
        +--> CPU packet + visibility construction
        |       - normal LOD/range filtering
        |       - frustum filtering
        |       - visibility layers
        |       - alpha sorting/routing
        |       - shadow visibility stream
        |
        +--> packed instance record SSBO
        +--> visible-index SSBO
        +--> shadow-visible-index SSBO
        +--> material table / material texture sets
        |
        v
Terrain3DRdMeshCache
        |
        +--> exact per-surface vertex buffer
        +--> exact per-surface index buffer
        +--> exact per-surface vertex array
        +--> exact per-surface index array
        |
        v
Godot native Forward+ draw list
        |
        +--> ordinary indexed instanced draw
        +--> production material bindings
        +--> native lighting set
        +--> reverse-Z depth state
```

The active path deliberately does not depend on the experimental shared mesh arena, GPU instance-cull topology, or indexed-indirect instance command buffers.

## Main changes

### Instancer mode

`src/instances/terrain_3d_instancer_bringup.h`

- Added `T3D_INSTANCER_RD_PORT=1`.
- Disabled the old `T3D_INSTANCER_MINIMAL_RENDER_PATH` magenta/LOD0 diagnostic mode.
- Normal full-fork instance streaming, LOD selection, materials, and pass participation are active again.

### RD mesh cache

`src/rendering/rd/terrain_3d_rd_mesh_cache.cpp/.h`

- Promoted standalone per-surface GPU geometry from the old bring-up-only path into the production instancer RD port.
- Preserves production material metadata.
- Preserves imported material texture synchronization/fallback behavior.
- Does not allocate the experimental shared instance mesh arenas while the RD port is active.
- Direct-surface resources remain instance-owned and use child-before-parent teardown.

### RD instance renderer

`src/rendering/rd/terrain_3d_rd_instance_renderer.cpp`

- Active visibility construction is CPU/reference packet construction rather than the experimental GPU-cull topology.
- Main and shadow visibility upload only their visible-index streams.
- Main/shadow indirect command buffers are not required by the active port.
- Production material and previous-instance buffers remain present.
- Forward+ submission binds the exact surface vertex/index arrays and issues ordinary indexed instanced draws.
- `visible_base` is the packet's first visible index because ordinary draws begin with `gl_InstanceIndex == 0`.
- Production material set 1 and native lighting set 2 remain bound.
- Depth/opaque/shadow/motion/transparent pass routing remains active.
- Existing HLOD publications are retained; single-view HLOD visibility is performed with the CPU frustum path instead of an indirect-command cull mutation.
- Implemented the previously declared-but-missing `uploadShadowVisibility()` function.
- Diagnostics identify the active backend as `rd_direct_surface` and report direct rather than indirect draw counts.

### Build contract

Native and GDScript build IDs now both require:

`instancer-rd-port-integrated-2026-09-09`

## Verification performed

### C++17 source verification

- Debug/tools syntax check: all 113 native `src/**/*.cpp` translation units passed against the project's source-matched Godot 4.7.2 generated bindings.
- Release-style syntax check: all integration-critical instancer/RD translation units passed.
- Only existing generated godot-cpp warning: `INT64_MIN` literal in `global_constants.hpp`.

### Regression gates

Passed:

- integrated instancer RD-port contract
- native/GDScript contract
- instance fault reporting / texture fallback
- instance pipeline recovery
- first-population publication
- preview / lazy Forward resources
- Forward+ submission / transparency
- P0 Forward viewport lifecycle
- P1 Forward lane diagnostics
- P2 Forward resource prerequisites
- indirect-buffer legacy contract presence
- P09 cumulative visibility contract
- P11 cumulative instance publication contract
- GPU page tier non-destructive contract
- P10 VT streaming contract
- systems-C style
- systems-C ownership
- historical minimal/direct-surface gates now explicitly recognize that the integrated RD port supersedes those temporary modes

### Environment limitation

`scons` is not installed in the execution environment and no Godot 4.7.2 editor binary is available here, so a final shared-library link and viewport smoke test could not be performed in this environment. The complete native source tree was nevertheless compiler-front-end validated.
