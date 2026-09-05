# Terrain3D VT Shader + Packed Surface Array Fix — 2026-09-04

## Fixed

1. `render_world_vt_predict.glsl` no longer reuses `Counters` as an SSBO block name after `render_world_common_inc.glsl` defines `struct Counters`.
   - VT SSBO blocks now use `VtCounterBuffer`, `VtHashBuffer`, and `VtRequestBuffer` names consistently.
   - The reported compiler line lands on the counter block after include expansion.

2. Packed surface array validation no longer warns once per layer for compatible RGBA8 / BC3 / BC7 source-format differences.
   - `Image::FORMAT_RGBA8` is enum value `5`.
   - Mixed native packed-surface formats now select one canonical native array format.
   - BC7 is preferred when present, then BC3, then RGBA8.
   - Size/mipmap normalization no longer unconditionally expands a BC3/BC7 packed surface array to RGBA8.

## Performance effect

BC3 and BC7 are 8 bits/pixel while RGBA8 is 32 bits/pixel. Preserving compressed packed-surface arrays therefore cuts the steady GPU payload for those arrays to one quarter of the RGBA8 fallback, before considering mip chains.

## Verification

- All `tools/verify_*.py` gates pass.
- `src/assets/terrain_3d_assets.cpp` passes `clang++ -std=c++17 -DGDEXTENSION -fsyntax-only` against the bundled godot-cpp headers.
- The compile emits only the pre-existing generated godot-cpp `INT64_MIN` literal warning.
- The environment does not contain Godot or glslang, so actual GLSL-to-SPIR-V import was not executable here.
