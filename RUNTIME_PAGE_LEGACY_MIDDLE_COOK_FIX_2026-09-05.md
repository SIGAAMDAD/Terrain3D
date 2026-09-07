# Runtime-page legacy / Middle-page cook repair

**Date:** 2026-09-05

## Symptom

Older runtime pages, including pages carrying an older Middle-page layout, could be reported as invalid and could cause the final world-manifest stage of `cook_gpu_region_pages()` to fail even though the authored `terrain3d*.res` sources were available to regenerate them.

## Root cause

Two cold-path authority errors combined:

1. `inspectGpuRegionPages()` attempted the current-format source-signature read before probing the discovery prefix. A recognized legacy page therefore fell through as `invalid` rather than `legacy`.
2. `cookGpuRegionPages()` allowed a loaded `Terrain3DCookDatabase` entry to mark an on-disk page `current` without physically validating the artifact. An older or current-version-but-incompatible Middle payload could retain the same authored source signature and be skipped as unchanged. World-manifest publication then parsed that stale artifact using the current page contract and rejected the dataset.

The cook database is an acceleration index. It is not authoritative over the bytes on disk.

## Fix

- Inspection probes `magic/version/header_bytes` before current-format parsing.
- Page states are reported separately as `legacy`, `future`, `stale`, `incompatible`, and `invalid`.
- Legacy, stale, incompatible, invalid, and missing pages are normal recook candidates.
- A page may be counted `unchanged` only after:
  - current-version probe;
  - matching authored source signature;
  - full current-contract `validatePageForRegion()` validation.
- A stale cook-database entry is cleared when the physical artifact fails validation.
- Future-version pages are never overwritten by an older build.
- World-manifest publication is withheld after an actual page-cook failure instead of adding a misleading secondary missing/invalid-page diagnosis.
- Editor cook reporting exposes how many legacy/stale/incompatible/invalid artifacts were replaced.

## Invariant

```text
authored source available + old page artifact
        -> classify old artifact
        -> regenerate current page atomically
        -> reload validation succeeds
        -> manifest consumes current artifact

cook database hit
        -> does NOT bypass physical current-contract validation
```
