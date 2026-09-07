# Terrain3D deferred mesh commit publication regression

**Date:** 2026-09-06

## Symptom

On scene load, instance meshes can render with the pre-commit/stale mesh image. A forced mesh update immediately repairs the visible instance geometry/LOD state. Terrain control-map/material selection remains correct.

## Root cause

`Terrain3DMeshAsset::setSceneFile()` deliberately leaves replacement geometry in `m_pendingMeshes` when an existing committed mesh is already active. `Terrain3DInstancer::processUpdates()` later calls `Terrain3DAssets::loadPendingMeshes()` after its current render-cell teardown/reconciliation pass.

The delayed `commitMeshes()` swapped `m_pendingMeshes` into `m_meshes` but did **not** advance `Terrain3DAssets::m_nMeshSourceGeneration`. P06 generation gating therefore considered the already-cached RD mesh-source snapshot current. In addition, render-cell LOD/settings state had been generated before the delayed commit and was not requeued.

A later forced asset update entered `updateMeshInternal()`, which both advanced the mesh-source generation and called `updateMmis()`. That is why the forced update repaired the scene.

## Fix

`Terrain3DAssets::loadPendingMeshes()` now treats a delayed commit as a real source transition:

1. commit each pending mesh;
2. requeue that mesh ID through `Terrain3DInstancer::updateMmis()` so render-cell LOD/settings state is rebuilt from the committed mesh set;
3. advance the mesh-source generation once for the commit batch so the RD coordinator rebuilds its immutable mesh snapshot on the following publication.

This does not dirty persistence and does not touch the P06 material/control-map generation domain.

## Required runtime check

```text
load scene with existing instance assets
-> do not force/update any mesh asset
-> allow initial instancer update queue to converge
-> delayed scene-file mesh commits occur
-> next RD publication observes new mesh-source generation
-> affected instance render cells rebuild
-> visible result already matches the post-forced-update result
```
