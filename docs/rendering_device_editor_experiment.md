# Terrain3D RenderingDevice Editor Experiment

## Purpose

This branch adds an experimental explicit RenderingDevice terrain path without removing the existing RenderingServer path. The experiment is intended for editor/runtime A/B testing of explicit GPU ownership and direct clipmap submission while keeping the rest of Terrain3D usable.

Branch: `experiment/rendering-device-editor`

## Editor controls

`Terrain3D > Terrain Rendering > RenderingDevice Experiment`

### Render Backend

- `RenderingServer`: existing Terrain3D renderer. This is the baseline.
- `RenderingDevice Direct`: the visible terrain clipmap is rasterized directly through the global RenderingDevice from a `CompositorEffect` PRE_OPAQUE callback.
- `RenderingDevice Hybrid`: keeps the native RenderingServer terrain visible while also executing the RD experiment. Use this when testing RD ownership/submission while retaining Godot's complete native 3D feature path.

### RD Native Proxy Mode

Used by `RenderingDevice Direct`:

- `Disabled`: no native Terrain3D geometry proxy is presented to Godot's normal renderer.
- `Shadows Only`: default. The native clipmap remains registered as `SHADOWS_ONLY`, so Terrain3D can continue casting through Godot's normal directional/local shadow passes while the visible terrain comes from RD.
- `Full Native`: keeps the complete native Terrain3D surface active in parallel with the RD pass. This provides the same native lighting/GI/decal/reflection/lightmap participation as the normal renderer while the explicit RD pass also runs.

`RenderingDevice Hybrid` always keeps the full native surface active.

## What the direct RD path currently renders

The direct renderer owns and submits:

- canonical LOD0 and outer-ring vertex/index buffers;
- explicit vertex/index arrays;
- a persistent frame UBO;
- a persistent region-page-table storage buffer;
- persistent uniform sets while dependent RIDs remain unchanged;
- a scene color/depth framebuffer matching the active viewport and MSAA sample count;
- the existing Terrain3D height, control, color, and generated albedo texture arrays through `RenderingServer.texture_get_rd_texture()`;
- the existing clipmap snap positions, LOD scales, tier IDs, and stitch states;
- Terrain3D's current edge-stitch mapping;
- clipmap geomorphing between LOD levels;
- fragment-accurate terrain holes from the control map;
- missing-region fragment rejection instead of a fabricated minimum-height floor;
- bilinear height sampling when clipmap scale is below the authored vertex spacing;
- height-derived normals;
- base/overlay texture IDs, canonical control rotation/scale decode, and blend from the control map;
- fragment-sampled painted terrain color modulation;
- separate clamp terrain sampling and repeating material sampling;
- the managed Terrain3D directional light color/direction;
- direct scene depth writes so later opaque geometry can depth-test against the terrain.

No topology buffers are rebuilt during ordinary camera movement. Patch state is published from the existing `Terrain3DMesher` clipmap state.

## Standard Godot 3D features

### RenderingDevice Direct + Shadows Only

Usable for inspecting the direct visible terrain while keeping native terrain shadow casting. Standard Godot scene geometry rendered after PRE_OPAQUE depth-tests against the terrain depth written by the RD pass.

The direct surface itself does not enter Godot's native Forward+ render lists, so native clustered-light evaluation, native shadow receiving, decals, reflection-probe registration, motion-vector generation, and GI registration are not automatically applied to the RD surface.

### RenderingDevice Hybrid / Full Native proxy

Use this mode when the scene needs normal Godot 3D behavior during the experiment. The native Terrain3D surface remains registered with RenderingServer, so existing:

- shadow casting/receiving;
- Forward+ lights;
- GI modes;
- decals and reflection probes;
- normal render ordering;
- regional LightmapGI receiver workflow;

continue through the native path while the RD path is also active for comparison and profiling.

## Lightmapping

The existing regional lightmap baker is unchanged. It already creates a temporary static `MeshInstance3D` terrain proxy through `bake_region_lightmap_mesh()`, so selecting an RD backend does not prevent LightmapGI baking.

Existing `Terrain3DLightmapStreamer` behavior is also unchanged. Baked LightmapGI packages for scene receivers continue to stream normally.

For native Terrain3D GI/lightmap participation during the RD experiment, use `RenderingDevice Hybrid` or `RD Native Proxy Mode = Full Native`.

## Editor/GDScript integration

The addon scripts on this branch are aligned to the current native experiment contract (`2026082203`, build `a2.9.2-shader-preprocessor-repair`). The inspector metadata exposes the RD backend and native-proxy controls, the runtime diagnostics window reports RD state, and isolated Region Workbench scenes preserve the selected RD experiment settings.

The same modernization pass removes stale native/editor calls left by older revisions: Workbench docking uses `EditorDock`, runtime-page context cooking is implemented around the current native cook API, old asset-limit/region-type constants are replaced by the current public names, and HLOD mesh primitive inspection uses APIs present in the current Godot renderer interface. See `docs/gdscript_modernization_audit.md` for the audit details.

## Failure behavior

The RD path does not leave the terrain invisible indefinitely if initialization/rendering fails:

- failed global RenderingDevice/shader/pipeline initialization marks the experiment failed;
- eight consecutive render failures activate native fallback;
- fallback restores the full RenderingServer terrain surface;
- diagnostics preserve the initialization/last-render failure.

Compatibility renderer therefore falls back to RenderingServer rather than attempting to use an unavailable global RenderingDevice.

## Diagnostics

From GDScript:

```gdscript
var diagnostics: Dictionary = terrain.get_rd_renderer_diagnostics()
print(diagnostics)
```

The dictionary currently reports:

- backend and proxy mode;
- render/fallback state;
- renderer initialization state;
- compositor callback count;
- consecutive render failures;
- rendered frame count;
- draw count;
- explicit topology bytes;
- initialization and last-render errors;
- current single-view limitation;
- native depth-prepass participation status.

## Current experiment boundary

The direct alpha material path still assumes the default `Terrain3DTextureAsset` UV scale (`0.1`). Per-asset UV scale/tint, runtime logical-to-physical material layer remapping, packed PBR channels, and middle/far nonresident fallback are pending RD material/streaming work.

This branch is a GDExtension compositor experiment. `PRE_OPAQUE` executes after Godot's native depth prepass. The direct surface can write depth for later opaque rendering, but it cannot retroactively join the already-built native depth/shadow/opaque render lists.

A future engine submission bridge is still required if explicit Terrain3D RD buffers must participate as ordinary native Forward+ geometry without retaining a RenderingServer proxy.

## Validation target

The repository target remains Godot 4.7.1. This checkout does not contain its `godot-cpp` dependency or a Godot 4.7.1 editor executable, so the API/shader repair pass is statically validated here but is not a fresh native/editor build. Earlier branch history contains a compile pass against older 4.6-generated bindings; that historical result does not validate the current breaking class rename. The next target-machine pass must build against 4.7.1-compatible bindings and run the editor/import/shader smoke tests.
