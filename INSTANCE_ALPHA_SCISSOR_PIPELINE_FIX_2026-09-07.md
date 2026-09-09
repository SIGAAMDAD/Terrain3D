# Terrain3D instance alpha-scissor pipeline investigation/fix — 2026-09-07

## Symptom

Instance placement succeeds and the logical count increases, but no instance geometry appears. Mesh thumbnails also report `RenderingDevice mesh preview failed`. The editor additionally reports a RenderingDevice texture failure with an impossible mip count of `4294967295`.

## Root causes found

### 1. A failed material texture upload killed the entire RD mesh cache publication

`Terrain3DRdMeshCache::syncTextures()` treated failure to create any imported material texture as fatal and returned `false`. `Terrain3DRdMeshCache::upload()` then failed before publishing the mesh arenas. The instance renderer could therefore have valid logical cells/instance counts but no drawable mesh cache.

This is directly relevant to alpha-scissor vegetation/cards because their albedo texture is required for the cutout shape, and the editor thumbnail path uses the same mesh/material upload path.

The failure is now non-fatal to geometry. If an optional material texture cannot be created, the mesh remains resident and the material binding resolves that texture to Terrain3D's existing fallback texture. Geometry therefore remains visible and diagnostics report the failed source.

### 2. Invalid signed Image mip counts could wrap to UINT32_MAX

`Terrain3DRdTextureManager::getImageMipmapCount()` previously performed:

```cpp
(uint32_t)(image->get_mipmap_count() + 1)
```

without validating the signed `int32_t` result or the maximum mip chain allowed by the texture dimensions. An invalid negative result could therefore become a huge unsigned value and be forwarded to `RDTextureFormat::set_mipmaps()`. The observed `4294967295` error is exactly the kind of value this unchecked conversion permits.

The manager now:

- rejects negative Image mip counts;
- computes the maximum legal mip count from width/height;
- rejects mip counts beyond that bound before RenderingDevice creation;
- strips a broken imported mip chain and retries the base level before decompression/conversion fallback.

### 3. DXT1/BC1 cutout textures were mapped as RGB instead of RGBA

`Image::FORMAT_DXT1` was mapped to `DATA_FORMAT_BC1_RGB_*`. Alpha-scissor cards can depend on BC1's 1-bit transparent selector. The mapping is now `DATA_FORMAT_BC1_RGBA_*`, preserving cutout alpha while remaining valid for fully opaque BC1 content.

### 4. Imported material textures requested unnecessary RD usage capabilities

Imported material textures are immutable after publication but were requesting sampling, update, copy-from and copy-to usage simultaneously. Compressed material formats do not need those capabilities and backend support can be narrower than sampling support.

Imported instance material textures now request `TEXTURE_USAGE_SAMPLING_BIT` only.

### 5. Planar mesh AABBs could be discarded before P11 GPU culling

`Terrain3DInstancer::ensureCellAabb()` only accepted `source_aabb.has_volume()`. A one-face texture card or other planar imported mesh has surface area but zero thickness, so its source AABB can fail `has_volume()`. The cached cell bound then became an empty AABB at the region origin, and CPU/P11 GPU culling could reject the whole cell regardless of the actual placed transforms.

The cell AABB builder now accepts `has_surface()`, transforms the planar AABB for every instance, and merges those transformed bounds normally.

## Preview diagnostics

The coordinator now prints the actual `Terrain3DRdInstanceRenderer::getLastError()` string when an RD mesh preview fails, instead of only retaining it in diagnostics while the asset dock prints the generic preview failure.

No GDScript API or `.gd` source was changed.

## Validation

- `tools/verify_alpha_scissor_instance_pipeline.py`: PASS
- all 33 `tools/verify_*.py` gates: PASS
- C++ front-end (`clang++ -std=c++17 -DGDEXTENSION -fsyntax-only`) passes for:
  - `src/instances/terrain_3d_instancer.cpp`
  - `src/rendering/rd/terrain_3d_rd_texture_manager.cpp`
  - `src/rendering/rd/terrain_3d_rd_mesh_cache.cpp`
  - `src/rendering/rd/terrain_3d_render_coordinator.cpp`
- the six current 128m/P01-P02 shell gates pass. Fifteen old refactor/container shell gates still fail identically to the supplied baseline because they encode obsolete pre-current container contracts; this patch does not touch those areas.

A full Godot 4.7.2 runtime/editor smoke test and final MSVC/SCons DLL link are not available in this environment.

## Debug-build logger compile repair — 2026-09-07

The material-texture fallback warning added to `Terrain3DRdMeshCache::syncTextures()` uses the project `LOG()` macro. In `DEBUG_ENABLED` builds that macro expands to a Godot `UtilityFunctions` call prefixed by `__class__`. `Terrain3DRdMeshCache` did not previously declare the class logger token, so MSVC correctly emitted C2065 for `__class__`.

`Terrain3DRdMeshCache` now declares `CLASS_NAME_STATIC( "Terrain3DRdMeshCache" );` in the class body, matching other non-Object native subsystems. The affected translation unit was syntax-checked with both `DEBUG_ENABLED` and `GDEXTENSION` defined so the logging macro is actually compiled.
