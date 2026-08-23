# Batch F6 — GI, lightmaps, decals, and reflection probes

## Status

F6 completes the scene-feature interactions required by the fork's actual regional-lighting workflow and Forward+ parity surface. The bridge is ABI 6.

## Reference-fork audit

The fork does not treat terrain clipmap geometry as a persistent native `LightmapGI` mesh receiver. `region_lightmap_baker.gd` bakes a temporary `Terrain3DBakeProxy`, extracts the directional shadowmask slice into a per-region mipmapped L8 resource, removes temporary Terrain3D bake users from the final `LightmapGIData`, and publishes the remaining baked users for streamed scene-node receivers.

The runtime `Terrain3DLightmapStreamer` continues to stream those external `LightmapGIData` packages for the scene-node receivers recorded by the regional package. The RD regular-instance/HLOD record path is not authored as a LightmapGI receiver by this workflow, so F6 does not invent UV2/lightmap indices for it.

No Terrain3D project/fork runtime path was found that requires custom clipmap/instance/HLOD contribution to VoxelGI or SDFGI. The existing normal/roughness/VoxelGI depth attachment variant remains compatible with Godot's native prepass, but F6 does not claim custom VoxelGI/SDFGI injection.

## Terrain baked-lighting path

The extracted regional terrain shadowmask now participates in both residency modes:

- full residency owns a direct RD `Texture2DArray` aligned with repository slots;
- bounded residency owns the shadowmask as another physical cache channel aligned with the GPU page table;
- the packed region-map word stores shadowmask mode alongside physical layer identity;
- `None`, `Replace`, and `Overlay` follow the regional package mode;
- a lighting-only package/signature change updates shadowmask layers in place without rebuilding height/control/color textures or allocating a new Forward+ descriptor set;
- newly streamed bounded pages load the current regional shadowmask, while already resident pages are explicitly refreshed.

The primary directional light uses the baked visibility only for the terrain region carrying that mask. `Replace` substitutes baked visibility; `Overlay` multiplies native dynamic shadow visibility by baked visibility.

## Native decals

ABI 6 consumes Godot 4.7.1's native decal storage buffer and linear/SRGB atlas textures. Terrain, regular instances, and HLOD use cluster element type 3 and apply native projection bounds, layer masks, upper/lower fade, normal fade, albedo blending, normal mapping, ORM blending, and emission. The RD path does not enumerate `Decal` nodes.

## Native reflection probes

ABI 6 consumes Godot 4.7.1's native reflection-probe buffer and reflection atlas. Terrain, regular instances, and HLOD use cluster element type 4, layer masks, box bounds, blend distance, box projection, exact native reflection-atlas border padding, octahedral atlas sampling, exposure normalization, intensity, and roughness-dependent mip selection. Outside local probe influence the pre-existing Terrain3D environment/ambient path remains the fallback rather than forcing a local probe contribution.

## Descriptor/lifetime behavior

F6 extends the F4 set-2 adapter only with resources the custom shaders actually consume: reflection-probe storage/atlas, decal storage/atlases, and the shared sampler. Native lightmap/radiance RIDs remain available in the bridge packet for Godot-owned/native users but are deliberately not inserted into Terrain3D's shader descriptor key because the reference audit found no custom Terrain3D receiver for them. Native RIDs are never registered as Terrain3D-owned resources. Descriptor sets are recreated only when a consumed borrowed RID changes. PREPARE may update Terrain3D's small metadata UBO or regional shadowmask content; SUBMIT performs only binds and draws.

## Validation contract

`tools/rd.py` contains `rd_batch_f6_scene_integration_check()`. It guards ABI 6, the reference-fork lightmap decision, shadowmask residency/invalidation, native decal/probe layouts and cluster types, terrain/instance/HLOD use, descriptor caching, absence of node-enumeration compatibility shims, and engine-patch syntax.

The affected forward shaders are compiled through the supplied Godot 4.7.1 `RDShaderFile` importer. A stock Godot binary cannot execute the patched bridge; scene-level patched-engine GPU validation therefore remains external/environment-dependent.

## Validation performed for the F6 handoff

- affected C++17 translation units: 6/6 compiled against the Godot 4.7.1 bindings;
- F6 static scene-integration contract: 22/22;
- retained contracts: C 70/70, D 61/61, E 46/46, F1 41/41, F2 27/27, F3 19/19, F4 25/25, F5 24/24;
- forward shader matrix: 4 shader files x 8 named variants x vertex/fragment = 64/64 stage checks through the supplied Godot 4.7.1 `RDShaderFile` importer;
- engine patch: `git apply --stat` parses;
- `git diff --check` passes.

A stock Godot 4.7.1 binary cannot execute the packaged Forward+ engine patch, so patched-engine scene/GPU validation remains an external validation item rather than an unimplemented F6 code path.
