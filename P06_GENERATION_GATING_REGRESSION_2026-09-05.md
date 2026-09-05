# Terrain3D P06 Generation-Gating Regression

**Date:** 2026-09-05  
**Affected cumulative stage:** P01 -> P06, including the owner-controlled allocation / GPU-cache / runtime-page fixes  
**Regression source:** P06 generation-gating conversion in `Terrain3DRenderCoordinator` and its producer generations  
**Primary symptoms:**

- Control map appears loaded but does not select the intended material textures.
- Albedo can collapse to material-color/fallback output.
- Initial load can render the Near footprint with stale Middle/Far material feature state.
- Editing a region can suddenly make Near/control rendering correct.
- Some tier-quality, regional-lighting replacement, and mesh-preview changes can remain stale indefinitely.

## Root cause A: raw runtime layer map changed semantics

Before P06, `Terrain3DRenderCoordinator::buildMaterialState()` consumed `Terrain3DAssets::getTextureLayerMap()`. That function has two different contracts:

```text
runtime material cache active   -> logical material -> physical streamed layer
runtime material cache inactive -> logical material -> identity layer
```

P06 replaced that accessor with the raw `m_runtimeMaterialLogicalToPhysical` array to avoid allocating a `PackedInt32Array` in the publication path. The raw array is not semantically equivalent. It contains `-1` for nonresident/uninitialized entries even when the runtime material cache is inactive.

The coordinator then published those entries as nonresident and clamped their physical layer to zero. The control map still selected the correct logical material ID, but the material table said that logical material had no resident texture layer. This produced missing albedo / fallback coloration and made the control map look ineffective.

### Fix

P06 now uses an allocation-free semantic accessor:

```cpp
getTextureLayerPhysicalIndexInternal( logical )
```

It preserves the original contract exactly:

```text
cache active   -> streamed logical-to-physical mapping
cache inactive -> identity mapping
```

The unsafe raw pointer accessor was removed so this semantic distinction cannot be bypassed accidentally.

## Root cause B: material-state generation key did not cover runtime publication state

P06 changed material-state publication from rebuilding every snapshot to caching by a five-value generation key:

```text
material RD generation
texture publication generation
texture layer-map generation
active storage generation
compute displacement enabled
```

`buildMaterialState()` depends on substantially more state than that key represented. In particular:

- implicit control-map state;
- implicit color-map state;
- derived-map RID;
- Middle composite RID;
- Middle height + region-map RIDs;
- Far height + color atlas RIDs;
- Far albedo RID;
- compute displacement RID;
- world-noise cache RID / mip state;
- macro-noise RID;
- generated material-array layer counts.

During startup, those resources and flags can become valid after the first material snapshot is built. P03-P05 startup bulk loading also suppresses some ordinary `maps_changed` notifications. P06 therefore had a stable generation key while the actual material feature state changed underneath it.

That created the characteristic lifecycle failure:

```text
initial P06 material snapshot built too early
        -> stale control / Middle / Far / derived feature flags cached
        -> startup GPU pages/resources become valid
        -> generation key does not change
        -> stale snapshot reused every frame
        -> region edit triggers another producer generation
        -> material snapshot rebuilds
        -> Near/control rendering suddenly becomes correct
```

### Fix

The P06 material source key now includes the runtime state it actually consumes:

- implicit control/color flags;
- all terrain RIDs whose validity changes material feature flags;
- Far albedo, displacement, world-noise and macro-noise RIDs;
- world-noise mip state;
- generated array layer counts;
- existing material/texture/mapping generations.

This retains P06's generation-gating optimization without assuming an incomplete generation domain.

## Root cause C: dynamic tier values did not advance the RD material generation

Some Near/Middle/Far properties call:

```cpp
tierQualityChanged( tier, false )
```

These values update uniforms without recompiling the shader. P06's cached material image also reads them directly, but the non-recompile path did not advance `m_nRdPublicationGeneration`.

Affected categories include authored-color contribution, wetness contribution, and material LOD bias.

### Fix

The non-recompile tier path now advances `m_nRdPublicationGeneration` before publishing tier uniforms.

## Root cause D: replacing a regional-lighting Resource did not invalidate the global lighting generation

P06 added a global `Terrain3DRegionLighting` publication generation and increments it when fields inside an existing lighting Resource change. `Terrain3DRegion::setLighting()` can replace the Resource object itself, but that operation did not advance the global generation.

### Fix

`setLighting()` now calls `Terrain3DRegionLighting::markGlobalPublicationDirtyInternal()` whenever the region's lighting Resource changes.

## Root cause E: nonpersistent mesh/preview changes did not advance mesh source generation

P06 made RD mesh-source rebuilding depend on `Terrain3DAssets::m_nMeshSourceGeneration`. `updateMeshInternal()` incremented that generation only for persistent changes. The `preview_changed` path intentionally calls it with `bPersistentChange == false`.

### Fix

`updateMeshInternal()` now advances mesh source generation for every actual mesh update; the persistence flag controls dirty serialization state only.

## Preserved P06 optimization

This patch does **not** revert P06 wholesale. The following remain:

- generation-gated material publication;
- generation-gated regional-lighting scans;
- mesh/HLOD source generations instead of hot-path source fingerprints;
- cached ocean static state;
- P06 producer telemetry.

The correction is to make the generation contracts complete and semantically equivalent to the pre-P06 behavior.

## Regression invariants

The updated `tools/verify_p06_generation_gates.py` enforces:

1. no allocating `getTextureLayerMap()` call in `publishSnapshot()`;
2. no raw logical-to-physical pointer accessor exposed to the coordinator;
3. allocation-free semantic layer-map lookup is used;
4. runtime material source gating includes implicit-map state and all relevant material-resource RID transitions;
5. non-recompile tier changes advance the RD material generation;
6. preview/nonpersistent mesh changes advance the mesh-source generation;
7. replacing a regional-lighting Resource advances the global lighting generation;
8. P07/P08-only functionality remains absent from this isolated P01-P06 package.

## Validation performed

- P06 generation-gate regression verifier: PASS.
- P03/P04/P05 contract verifier: PASS.
- Owner-controlled allocation A/B/C, D/E/F and G gates: PASS.
- READY/cold-phase boundary verifier: PASS.
- Initialization/residency ownership verifier: PASS.
- RD startup/capacity/shader-bound verifier: PASS.
- Systems-C ownership/style gates: PASS.
- Direct syntax compilation of all modified production translation units: PASS against the supplied generated Godot 4.7.x headers.
- Full 113-TU syntax sweep was attempted; the environment timed out after 32 translation units, with no compilation failures recorded before timeout. This is not reported as a completed full build.

## Runtime validation target

The decisive editor smoke test is:

```text
load existing Terrain3D data
-> do not edit terrain
-> first visible frame already uses Near material path where configured
-> control map selects the correct material textures
-> albedo is present
-> wait for pending page/material streaming
-> Near/Middle/Far transitions converge without an edit
-> modify a region and verify appearance does not "wake up" from a previously stale state
```
