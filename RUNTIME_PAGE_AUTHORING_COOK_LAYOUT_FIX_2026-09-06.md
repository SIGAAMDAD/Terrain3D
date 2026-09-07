# Runtime Page Authoring Cook Layout Fix — 2026-09-06

## Failure

Editor Authoring could discover every authored region and still report every runtime page cook as failed with:

`middle-height page has an invalid size/format`

The failure was deterministic when `gpu_middle_height_resolution_divisor` was greater than 1 (the Xbox 360 profile uses 4).

## Root cause

`Terrain3DData::writeGpuRuntimePage()` generated the cooked Middle-height channel through:

`m_middleHeightRegionCache.makeHeightImage(location, true)`

The live Middle-height cache intentionally forces full resolution while Editor Authoring is active. Its live cache config therefore carries divisor 1. The runtime-page header/config remains authoritative for the shipping artifact and carries the configured runtime divisor (normally 4).

The cook therefore generated a 128x128 R16 Middle-height image while `Terrain3DRuntimePageStore::writePage()` correctly required a 32x32 R16 image for divisor 4. Every page failed before publication.

This was a source/layout contract mismatch, not a legacy-page validation problem.

## Fix

- Added `Terrain3DMiddleHeightRegionCache::makeHeightImageForDivisor()`.
- Runtime page cooking freezes `getRuntimePageConfig()` once per region and generates Middle height explicitly from `runtime_page_config.m_nMiddleHeightDivisor`.
- The exact same frozen config is passed into `writePage()` so generation and validation cannot disagree.
- The live Editor Authoring Middle-height cache remains full resolution; cooking no longer mutates or borrows that presentation policy.
- Middle-height validation errors now report expected size/divisor and actual size/format.

## Edge collision cook follow-up

The 129x129 canonical collision tile includes a +X/+Z border. At the authored-world positive edge those border samples may legitimately have no source region. `copyHeightRect()` previously marked any absent region incomplete and `Terrain3DCollisionSurfaceBuilder::build()` rejected the entire tile.

The sampler now has an explicit partial-coverage mode used only by the canonical collision surface. Genuinely absent regions remain NaN HeightMapShape hole samples. If an authored region exists but its sampling view is unavailable, the copy still fails. Strict callers retain the old default behavior.

This prevents the next wave of edge-region cook failures after the Middle-height mismatch is removed.

## Contract

- Editor live Middle cache resolution and runtime cooked Middle resolution are independent policies.
- Runtime page generation must use runtime-page numeric configuration directly.
- Heightmap-only authored regions remain cookable; optional control/color channels continue to synthesize their canonical defaults.
- Missing world-edge neighbors do not invalidate an otherwise valid collision page.
