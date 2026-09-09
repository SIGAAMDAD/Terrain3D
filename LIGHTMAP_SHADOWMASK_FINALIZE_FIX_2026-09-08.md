# Terrain3D regional lightmap shadowmask finalize fix

Date: 2026-09-08

## Observed failure

Godot completed the LightmapGI bake, then Terrain3D failed during `finalize_prepared_bake()` with:

- `Cannot blit_rect in compressed image formats.`
- `_extract_terrain_shadow_mask()` at the atlas crop
- a later missing `Region_[0_-8].terrain_shadow.res`
- runtime-page cooking failure for neighbor `(0, -8)` while the newly baked region was `(-1, -8)`

## Root cause 1: compressed shadowmask atlas crop

`TextureLayered.get_layer_data()` can return the baked shadowmask slice in a compressed image format. Terrain3D duplicated the image and passed it directly to `Image.get_region()`. Godot implements the region copy through a blit operation, which rejects compressed images.

The shadowmask slice helper now duplicates and decompresses the image before returning it to the extraction/cropping path. The baked LightmapGI texture resource itself is not modified.

## Root cause 2: stale neighboring optional-lighting sidecar aborted runtime cooking

Saving one region intentionally recooks the surrounding 3x3 runtime-page footprint. The logged runtime-page failure was for `(0, -8)`, not the newly baked `(-1, -8)` region. That neighbor carried a lighting package whose terrain-shadow sidecar path no longer existed.

Regional lighting is optional to Terrain3D terrain functionality. Runtime-page cooking therefore no longer directly loads the sidecar and aborts when it is absent. It routes through the canonical terrain-shadow image builder, which:

1. checks path existence before calling `ResourceLoader`, avoiding noisy missing-file errors;
2. normalizes valid masks to a mipmapped region-sized `L8` image;
3. substitutes a fully-lit `L8` mask when the sidecar is absent/unreadable.

The same missing-file guard is applied to the live GPU-region-cache shadowmask path.

This keeps the authoritative lighting package intact for diagnosis/rebake, but prevents stale optional lighting from invalidating heightmap/runtime-page cooking.
