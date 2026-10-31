# Terrain3D P06-P09 - fragment optimization acceptance

Baseline: completed P00-P05 source. Delta: P06-P09 only. These changes are
source and offline regression candidates, **not an approved faster renderer**.

## P06: forward lighting and decals

`project/addons/terrain_3d/rd/terrain_forward_lighting_inc.glslinc`

- Skip decals with **exact** zero falloff, before fetching decal textures.
- Skip normal/ORM samples for an exactly transparent decal-albedo evaluation.
- Skip emission texture reads when declared emission energy is exactly zero.
- Skip expensive light shadow sampling only when physically computed point,
  spot or area attenuation is exactly zero.
- Preserve each shadow-map filter/PCF, area-light dimensions, light counts,
  reflection and BRDF response. No distance limit, mip or quality reduction.

These branches require measured shader timings in light-dense/decal-dense and
zero-contribution scenes; a branch can cost more than the work it saves.

## P07: VT transfer preflight and staging

`src/rendering/rd/vt/gpu/terrain_3d_vt_gpu.cpp` and related diagnostics:

- Reject duplicate virtual pages, physical slot reuse, repeated live victim,
  and full reservation batches **before** updating physical textures.
- Preflight all channel lengths, valid family mappings, backing staging sizes
  and pointers before any of the batch's channels are uploaded. This removes
  partial uploads caused by detecting a malformed *later* channel mid-loop.
- Use the existing immutable neutral `PackedByteArray` directly as the
  input to the same Godot `RenderingDevice::texture_update` operation. No
  second CPU memcpy into mutable staging for a neutral channel.
- Keep logical page-table publication after texture layers have been staged,
  and preserve the RenderingDevice's own resource-ordering contract.
- Counters: `vt_upload_preflight_rejections` and
  `vt_neutral_staging_copy_bytes_saved`.

The patch does **not** alter low-level Vulkan barriers: they are managed by
Godot's RenderingDevice. The physical-upload preflight protects validity but
does not establish reduced GPU latency without backend captures.

## P08: integrated release gate

From the repository root containing `src/` and `project/`:

```
python src/diagnostics/tests/shaders/run_fragment_release_gate.py \
  --root . --baseline /path/to/p00-p05-source \
  --glslang /path/to/glslangValidator --jobs 8 \
  --out /tmp/terrain3d
```

Runs all 365 RD fragment variants, baseline comparison, 20 CS19 shader stages,
Python shader/composed contracts, and standalone C++17 source/reducer tests.
The reference may report **existing** SPIR-V validator failures; new failures
are prohibited. Use `--quick` for a 19-variant smoke run, never release proof.

To request actual release approval, append:

```
  --require-live \
  --capture-index /captures/index.json \
  --capture-limits /captures/limits.json \
  --gpu-baseline /captures/base.csv \
  --gpu-patched /captures/patched.csv \
  --max-regression-percent <approved_stability_limit> \
  --min-frame-improvement-percent <positive_measured_threshold> \
  --smoke-evidence /captures/build_editor_evidence.json
```

Capture index JSON: `{"pairs":[{"case":"looking_down","resolution":"full",
"warm_state":"warm_steady_state","shader_path":"canonical_vt_v3",
"reference":"before/capture.json","candidate":"after/capture.json"},...]}`.
Each image is the documented P00 manifest with HDR/DNR/depth/coverage/page IDs;
the index must cover the full Cartesian product of the camera cases,
resolutions, streaming states and shader paths in `capture_matrix.json`.

`--capture-limits` is the existing P00 map with `hdr_color`, `dnr`, and `depth`
each containing `max_abs` and `mae`. Derive numerical tolerances from
baseline image-repeatability captures, not from a chosen target speedup.

`build_editor_evidence.json`: `{"checks": {"godot_cpp_build":
{"passed":true,"log":"build.log"}, ...}}`. Required check keys are defined
in `run_fragment_release_gate.py` and include Vulkan validation, runtime,
editor inspector/toolbar, brush+undo/redo, save/reload and shutdown. The
corresponding log files must exist and be nonempty. Actual GPU timing CSVs
require >=5 real frames per cohort; an improvement threshold must be positive.

This runner **does not** itself launch the Godot editor/GPU capture tools.
Those artifacts must come from the target hardware. It will not approve
shipping without that evidence.

## P09: independent compressed composed-surface R&D

`src/authoring/vt/build/terrain_3d_composed_surface_static_extended.cpp`
provides `t3d_comp_surf_static_extended_evaluate_linear` as a pre-compression
reference, using the exact same **existing eligible static-V3 subset** source
material evaluator as the sidecar cooker. It returns a full
`T3D_COMP_SURF_PHYSICAL_TILE^2 * 4` float RGBA channel buffer, before sRGB
encoding/quantization/BC. Channel meanings:

- ALBEDO: linear RGB, unused A=1.
- NORMAL: normalized tangent XYZ (up in Y), unused A=1.
- SURFACE: AO, roughness, metallic, AO influence.

Export each uncompressed RGBA32F buffer using:

```
python src/diagnostics/tests/composed_surface/export_reference.py \
  --albedo <albedo_rgba.f32le> --normal <normal_rgba.f32le> \
  --orm <surface_rgba.f32le> --region-key <region> --mip <mip> \
  --source-fingerprint <hash> --composition-revision <revision> \
  --output /captures/baker
```

Record corresponding full raster-source (`capture_stage: raster_source`)
and compressed-then-decoded (`capture_stage: decoded_bc`) channel captures
with **identical** page identity metadata and float channel conventions.
Then compare:

```
python src/diagnostics/tests/composed_surface/compare_cache_stages.py \
  /captures/raster/capture.json /captures/baker/capture.json \
  /captures/decoded/capture.json --limits /captures/cs21_limits.json \
  --out /captures/cs21_result.json
```

Limits JSON must have `source_to_baker` and `baker_to_decoded_bc`, each with
`albedo`, `normal`, `orm`: every channel requires `max_abs`, `mae`; normal
also requires `max_degrees` and `mean_degrees`. This separates source-math
error from BC compression. Missing captures, provenance mismatch, non-finite
samples or unapproved differences are errors.

**No composer is enabled by these tests**. The existing eligibility and cold
shader opt-in remain. Full V3 resident-page/trilinear/filtering equivalence,
authoring invalidation, tangent-space normal fidelity, BC7/BC5/BC1 error,
editor/RD GPU timings and runtime visual approval remain required before the
composed texture cache can replace source rendering.
