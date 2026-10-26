# Terrain3D P00-P05 — fragment capture and acceptance

This directory contains the offline compiler, exact-output comparator, GPU-time
comparator, and deterministic contract tests for the P00-P05 fragment patches.
A compiler result is **not** a GPU performance result or proof of pixel parity.

## Compiling shipping, editor and compatibility versions

Obtain `glslangValidator` v16.6.0 or later and run from a source root:

```sh
python src/diagnostics/tests/shaders/compile_fragment_variants.py \
    --root . --glslang /path/to/glslangValidator --full --jobs 4 \
    --out /tmp/terrain3d_fragment_spirv
```

To compare to the untouched source tree, add `--baseline /path/to/original`.
Every variant is flattened from the RDShaderFile's **fragment** root. The tool
reproduces Godot's single-pass include replacement and version macros, compiles
for Vulkan 1.2, invokes `--spirv-val`, and enumerates static SPIR-V opcodes.
Static instruction counts are **not executed instruction counts**. Run the
compilation with the exact GLSL version and shader macros the engine selects.

Known reference results (glslang 16.6.0): **365/365** variants compile in each
of the supplied baseline and P00-P05 patched trees. Both trees have **144**
SPIRV-Tools failures with `VUID-StandaloneSpirv-None-10684` due to explicit
`ColMajor` layout decorations on function-local `T3DNativeLightData` types.
There are **zero newly failing validation variants**. This pre-existing validator
issue should be repaired separately from a performance patch.

## Capturing GPU times

Capture the same frames of the same scene/camera/settings on the same GPU, driver,
engine revision and viewport resolution. Distinguish cold-start vs warm and full
vs half resolution. Capture pass GPU timestamps from actual Vulkan traces (e.g.
RenderDoc, Intel GPA), *not CPU submission times*. `getGpuInformationStats()`
currently uses `-1` for missing isolated GPU time and must stay that way until
real GPU data is available.

Export the existing CS00 CSV schema, one measured frame per row, using columns
`case,resolution,warm_state,shader_path,frame_gpu_usec,terrain_depth_usec,terrain_dnr_usec,terrain_color_usec,visibility_usec,transfer_usec,upload_bytes,vt_misses`.
Run:

```sh
python src/diagnostics/tests/shaders/compare_fragment_gpu_times.py \
    before.csv after.csv --max-regression-percent 1.0 --out gpu_diff.json
```

The maximum tolerated regression must be chosen from baseline variation. The
comparator needs at least five measured samples **per cohort and metric** and
reports p50/p95/p99. Empty pass metrics remain unknown. The CSV itself lacks
GPU/driver identity; record those details in the capture session and keep the
same setup for both captures. A pass/no-regression result is not by itself a
proof of speedup.

## Capturing fragment attachments

Export uncompressed, full-size shader outputs. Do not substitute tone-mapped
screenshots for HDR, do not rescale comparison images, and do not transform
encoded UNORM terrain albedo into the instance's linear-filtering convention.
Create `capture.json` in each capture directory:

```json
{
  "schema": 1,
  "identity": {
    "renderer": "Vulkan", "device": "GPU model", "driver": "driver version",
    "godot_build": "Godot executable identity", "scene_sha256": "scene hash",
    "assets_sha256": "asset/cooked-VT content hash",
    "camera_state_sha256": "camera state hash",
    "material_state_sha256": "published shader/material configuration hash",
    "vt_state_sha256": "resident page-table/generation hash",
    "variant": "near_color_native", "capture_case": "material_four_corner",
    "state": "warm", "frame_number": 123, "width": 1920, "height": 1080
  },
  "attachments": {
    "hdr_color": {"file": "hdr_color.f32le", "type": "f32le", "components": 4},
    "dnr": {"file": "dnr.f32le", "type": "f32le", "components": 4},
    "depth": {"file": "depth.f32le", "type": "f32le", "components": 1},
    "coverage": {"file": "coverage.u8", "type": "u8", "components": 1},
    "vt_page_ids": {"file": "vt_page_ids.u32le", "type": "u32le", "components": 1}
  }
}
```

All files are raw row-major interleaved channels, no headers: `f32le` IEEE-754
little-endian float, `u32le` uint32, `u8` uint8. `coverage` is the exact
integer attachment or derived mask, and `vt_page_ids` represents the exact
captured page identifier for each pixel/cell. A capture harness may attach
unchanged and candidate variants to independent manifests; identities must be
equal. **Use the same post-stream residency snapshot** for strict image parity,
and capture **temporal streaming** separately to detect residency and fallback
artifacts that a fixed snapshot would conceal.

Choose continuous-output tolerances *after* capturing repeated unchanged
baseline frames. Write explicit threshold JSON:

```json
{
  "hdr_color": {"max_abs": 0, "mae": 0},
  "dnr": {"max_abs": 0, "mae": 0},
  "depth": {"max_abs": 0, "mae": 0}
}
```

These values are **strict examples, not promised real-GPU tolerances**. Coverage
and page-ID attachments always require exact identity (no tolerance). Run:

```sh
python src/diagnostics/tests/shaders/compare_fragment_captures.py \
    before/capture.json after/capture.json --limits limits.json --out image_diff.json
```

Mismatch of scene/camera/driver/material/residency identity, absent data,
nonfinite floats, attachment dimensions/types, pixel coverage, or page IDs
fails the comparator. Regression tests use **synthetic samples only**:

```sh
python src/diagnostics/tests/shaders/test_fragment_shader_contracts.py
```

## External acceptance pending

`T3D_FRAGMENT_SOURCE_REUSE=0` disables P02 for a compiler A/B bisect. P02 is
skipped automatically in fragment-feedback variants, preserving page-request
frequency across different material pairs. The P05 suppression is intentionally
**off by default** and can be tested by adding
`T3D_VT_FEEDBACK_DEDUP_EXPERIMENT=1` to a feedback shader. Coarse feedback is
lossy, so omitting even an identical repeated store can affect the final page
chosen under inter-fragment contention. Enable P05 in production only after
cold/moving/crowded-VT residency parity testing on the target GPU.

P00 measured GPU captures (including editor brush/undo and cold streaming), GPU
image parity, and the C++ Godot build all require the actual renderer/device and
project build environment. They are not certified by offline GLSL compilation.
For P02 material-source reuse inspect heterogeneous transitions versus
homogeneous areas; for P04 measure foliage and alpha-mask overdraw; for P05
compare `vt_feedback_grid_valid_samples`,
`vt_feedback_grid_direct_unique_pages`,
`vt_feedback_grid_selected_candidates`, resident rescues and missing-page
latency during turns/teleports. These statistics cannot directly measure
attempted shader writes; the latter requires a separate GPU instrumentation run.
