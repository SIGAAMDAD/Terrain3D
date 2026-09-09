# Terrain3D Instancer Authoritative-Store RD Publication Fix — 2026-09-09

## Purpose

This patch isolates the remaining instance-rendering failure above the integrated RenderingDevice renderer by removing `m_renderCells` as the source of the production RD instance scene.

The production RD renderer remains active. This is **not** the old magenta/minimal renderer.

## Runtime path

```text
Terrain3DRegion resident instance store
    -> mesh/cell iteration
    -> LOD0 production mesh source key
    -> packed world-space instance records
    -> exact bounds recomputed from source mesh AABB + actual transforms
    -> production RD mesh/material upload
    -> CPU packet construction
    -> visible-index upload
    -> ordinary indexed instanced Forward+ draws
```

## Bypassed fault lines

For the authoritative RD scene publication only, this patch bypasses:

- `m_renderCells` materialization
- instance-cell streaming residency rejection
- HLOD suppression of ordinary render cells
- stale `m_cachedAabb` render-cell bounds
- CPU LOD transition state
- camera visibility-layer rejection
- CPU instance frustum rejection

It does **not** bypass:

- production RD mesh cache
- production material upload/bindings
- instance shader
- reverse-Z depth state
- Forward+ pass registration/submission
- normal opaque/transparent/shadow routing

## Publication invalidation

`Terrain3DInstancer::updateMmis()` now advances the RD publication generation immediately when authoritative-store publication is enabled. This prevents a valid store mutation from remaining invisible merely because render-cell materialization was deferred or rejected.

## Diagnostics

The instancer stats now expose:

- `rd_authoritative_store_snapshot`
- `rd_authoritative_records`
- `rd_authoritative_cells`

Snapshot publication reports an explicit error when resident authoritative instance data exists but zero renderable records can be published, including the count skipped for missing/disabled/unavailable mesh assets.

## Build ID

```text
instancer-authoritative-store-publication-2026-09-09
```

The native and editor-script build IDs were updated together.

## Verification

Passed:

- debug/tools C++17 syntax for the changed instancer, instance renderer, and coordinator
- release-style C++17 syntax for the same changed units
- 19 focused cumulative regression/verifier gates, including:
  - authoritative-store RD publication
  - integrated RD-port contract
  - fault reporting / texture fallback
  - first-population render
  - Forward+ instance submission
  - instance pipeline recovery
  - preview/lazy Forward+
  - alpha/scissor instance contract
  - P0/P1/P2 Forward+ diagnostics/prerequisites
  - P09/P10/P11 cumulative GPU/streaming contracts
  - script/native API contract
  - systems-C style and ownership
  - render-startup recovery

A full native-tree syntax sweep was also attempted, but the environment command timeout occurred before completion. No compile error was emitted before the timeout. The integration-critical changed units are compiler-clean in both configurations.

No Godot viewport executable is available in the execution environment, so viewport smoke testing is not claimed.
