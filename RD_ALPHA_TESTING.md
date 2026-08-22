# Terrain3D RenderingDevice Alpha

Implementation branch: `experiment/rendering-device`
Frozen pre-clean-break reference: `rd-alpha-pre-cleanbreak-reference` (`5d9e8b1`)
Earlier reconstruction baseline: `rd-alpha-baseline` (`9bc792a`)
Target: Godot 4.7.1, Forward+ first

This branch contains the alpha RenderingDevice terrain backend while preserving the original RenderingServer path and the fork's existing streaming, authoring, collision, instancing, HLOD, baking, and editor systems.

## Backend modes

### RenderingServer

The original native Terrain3D rendering path. It remains the fallback and reference renderer.

### RenderingDevice Direct

Terrain clipmap color and depth are submitted from a `PRE_OPAQUE` compositor callback using Terrain3D-owned RenderingDevice resources:

- canonical LOD0 and outer-ring topology uploaded once;
- 8-byte packed clipmap vertices;
- 16-bit indices when the welded topology permits them, otherwise 32-bit;
- persistent frame, material, patch, visible-index, region-map, and indirect buffers;
- separate near, middle, and far raster pipelines;
- direct resident Terrain3D texture RID bridging without duplicating texture residency;
- displacement-buffer sampling;
- terrain color/control/material/PBR sampling;
- directional-light, ambient-light, and environment-fog inputs;
- compute frustum culling with CPU indirect-command fallback;
- indexed indirect draws;
- per-view framebuffer/pipeline state for multiview;
- explicit buffer/texture/resource/upload diagnostics;
- optional native shadow-only proxy so Godot can continue to populate its directional shadow passes.

### RenderingDevice Hybrid

The native Terrain3D surface remains active for full Godot material/lighting/GI/decal/depth-prepass behavior. The RenderingDevice backend remains active for packed topology, explicit buffer ownership, compute culling, indirect submission, and matching terrain depth, but its color write mask is disabled. This avoids the old alpha's double-color rendering while providing a parity/test mode for the direct backend.

Selecting `Full Native` as the native proxy while using Direct mode has the same depth-only RD behavior to avoid two color surfaces.

## Existing fork systems retained

The alpha does not disable or replace the fork's non-clipmap systems. Regular Terrain3D instancing and HLOD continue through their existing RenderingServer integration in both RD modes, while the terrain clipmap is the explicitly owned RD path. Collision, runtime page streaming, caches, lightmap/bake tooling, compute displacement generation, editor tooling, and GDScript authoring tools remain the existing implementation.

## GDExtension rendering boundary

A compositor callback is not an engine render-list insertion hook. Direct mode therefore cannot retroactively enter the Forward+ depth prepass that already ran before `PRE_OPAQUE`, nor can it register itself in Godot's private clustered-light, GI, decal, reflection-probe, or motion-vector geometry lists. Hybrid mode exists when those native features are required without giving up RD topology/culling/depth validation.

Native shadow casting in Direct mode is provided by the shadow-only RenderingServer proxy. Exact native Forward+ participation for a fully direct terrain surface requires the engine-side submission bridge described in `docs/rendering_device_conversion_patch.md`; it cannot be implemented purely inside a GDExtension without modifying Godot itself.

## Build

The repository vendors `godot-cpp 10.0.0-rc1` and includes `thirdparty/godot_api/extension_api-4.7.1.json`. `SConstruct` forces that API by default because this godot-cpp release predates 4.7.

From the repository root:

```sh
scons platform=linux target=editor arch=x86_64 -j4
```

The expected Linux editor output is:

```text
project/addons/terrain_3d/bin/libterrain.linux.debug.x86_64.so
```

## Validation performed for this snapshot

- all 75 Terrain3D C++ translation units compiled against bindings generated from the supplied Godot 4.7.1 stable API;
- the Linux x86-64 debug GDExtension linked successfully;
- all 74 GDScript files pass Godot 4.7.1 `--check-only` parsing;
- Godot 4.7.1 imports the near/middle/far RD raster shaders and compute-cull shader without GLSL/SPIR-V errors;
- a cacheless headless Godot 4.7.1 editor startup reimports the project and all four RD shaders with zero hard load/parse/shader errors;
- `git diff --check` passes.

The validation environment has no Vulkan ICD / presentable Vulkan device, so an actual Forward+ GPU frame cannot be executed here. GPU-frame testing should therefore start with RenderingDevice Hybrid, then Direct, on D3D12/Vulkan hardware.
