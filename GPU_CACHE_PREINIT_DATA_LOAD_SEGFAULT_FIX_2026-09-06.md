# Terrain3D GPU Cache Pre-Initialization Data-Load Segfault Fix

**Date:** 2026-09-06

## Failure

The adjustable GPU page-limit patch introduced a null dereference in `Terrain3DGpuRegionCache::calculateGpuRegionPhysicalLayers()`. The runtime/editor branch first computed `bAuthoring` with a null-safe check, then dereferenced `m_pTerrain` unconditionally in the non-authoring branch. During resource/property deserialization, `Terrain3D` can own a `Terrain3DData` object before that data object has been activated against its owning terrain, so GPU cache configuration can transiently observe `m_pTerrain == NULL`.

The new serialized `gpu_region_cache_page_limit` setter also attempted immediate cache reconfiguration whenever `m_pData` existed, even before `Terrain3D::initializeInternal()` had completed. That made the pre-activation null state reachable during scene/data load.

## Fix

- `calculateGpuRegionPhysicalLayers()` now uses the compile-time runtime ceiling when no terrain owner is attached.
- `setGpuRegionCachePageLimit()` stores the authored value during deserialization but only reconfigures live cache storage after `m_bInitialized` is true.
- The adjustable budget/page-limit API and larger budgets remain unchanged.
- Native build provenance updated to `p07-budget-residency-height-minimum-loadfix-2026-09-06`.

## Validation

- Clang static analyzer on `terrain_3d_gpu_region_cache.cpp`: no diagnostics after the fix; before the fix it reported `Called C++ object pointer is null` on the page-limit getter.
- Direct C++17 syntax compilation of the modified streaming integration TU: PASS.
- Dedicated `verify_gpu_cache_preinit_load_safety.py`: PASS.
