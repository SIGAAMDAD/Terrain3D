# Terrain3D RenderingDevice Alpha

Implementation branch: `experiment/rendering-device`
Frozen pre-clean-break reference: `rd-alpha-pre-cleanbreak-reference` (`5d9e8b1`)
Earlier reconstruction baseline: `rd-alpha-baseline` (`9bc792a`)
Target: Godot 4.7.1, Forward+ first

This branch is the clean-break RenderingDevice implementation in progress. Batches A-E have removed Terrain3D-owned RenderingServer rendering for terrain-generated GPU textures, the regular instancer, and runtime HLOD submission. The remaining rendering integration work is the Godot 4.7.1 Forward+ bridge in Batch F, followed by editor/API cleanup and the final regression/validation batch.

## Implemented RenderingDevice path through Batch E

The current direct path owns and publishes:

- canonical near/middle/far terrain topology, patch state, fixed material ABI, generated terrain textures/caches, compute culling and indexed indirect terrain submission;
- direct RD material sampling for terrain and imported instance/HLOD materials;
- shared static mesh vertex/index arenas with generation-safe handles;
- packed persistent regular-instance storage, visibility/LOD/fade processing, grouped indirect draws, depth/shadow packet publication, alpha/scissor/depth-prepass variants and imported texture/sampler ownership;
- runtime HLOD package publication into the same mesh/material cache, CPU residency/selection, RD descriptor/state buffers, GPU AABB/layer visibility culling, bounded indirect commands, shadow participation, and deterministic unload/reload generation invalidation.

Runtime regular instances no longer allocate MultiMesh renderers, and the HLOD streamer no longer allocates native RenderingServer render instances. Collision, navigation, runtime page streaming, persistence, baking/lightmap workflows and authoring data remain CPU/non-rendering systems and are preserved for the final regression batch.

## Forward+ boundary still pending Batch F

A GDExtension compositor callback is not an engine render-list insertion hook. Exact participation in Godot's private Forward+ depth-prepass, clustered/local-light, selected shadow, velocity, GI, decal and reflection-probe lists requires the minimal Godot 4.7.1 engine-side submission bridge specified in `docs/rendering_device_conversion_patch.md`. Batch F owns that bridge and removes the remaining native shadow/fallback assumptions.

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

## Validation performed for the Batch E snapshot

- Batch C static material contract: 70/70 pass;
- Batch D static regular-instance contract: 61/61 pass;
- Batch E static HLOD contract: 46/46 pass;
- all 78 Terrain3D C++ translation units pass GCC 14.2 C++17 syntax compilation against bindings generated from the supplied Godot 4.7.1 API;
- all 78 Terrain3D C++ translation units also produce native object files with the same include/define set;
- `git diff --check` and `git fsck --full --strict` pass.

The current execution environment does not contain SCons, a runnable Godot 4.7.1 editor, or .NET, so the Godot-hosted/runtime lanes of `tools/rd.py full` cannot execute here. Vulkan/D3D12 runtime validation remains at the explicit GPU gates in Batches F/H rather than being inferred from static compilation.
