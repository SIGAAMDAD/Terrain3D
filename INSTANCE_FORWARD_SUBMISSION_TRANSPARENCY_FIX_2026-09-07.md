# Terrain3D instance Forward+ submission repair — 2026-09-07

## Symptom

Instance placement remained functional and the logical instance count increased, but no instance geometry appeared in the editor/runtime viewport.

A separate editor diagnostic also repeatedly emitted:

`Dictionary::operator[] used when there was no value for the given key ""`

## Rendering root defect

The direct-RD instance renderer created set-0 instance descriptors and set-1 material descriptors against the legacy `m_hShader` RID, then bound those descriptor sets while drawing pipelines created from separate Forward+ shader-variant RIDs (`m_hszForwardShaders[variant]`).

That violates the RenderingDevice shader/uniform-set ownership contract. Terrain clipmap rendering already creates its raster uniform sets against the exact shader RID used by each pipeline; the instance path did not.

The repair keeps the preview/legacy set on `m_hShader`, but gives native Forward+ rendering its own shader-variant descriptor banks:

- one main set-0 uniform set per Forward+ shader variant;
- one shadow set-0 uniform set per Forward+ shader variant;
- one set-1 material-pair uniform set per material pair and shader variant, lazily created at PREPARE/cold boundaries;
- Forward+ draw submission now binds only descriptor sets created from the exact shader variant backing the selected pipeline.

No ordinary SUBMIT path creates descriptor resources.

## Transparent instance repair

True alpha-blended instance surfaces were flagged by the mesh cache but had no native color submission path. Opaque submission skipped them and the coordinator skipped the instance renderer for the transparent pass.

The repair adds:

- instance preparation/submission in `T3D_RD_FORWARD_PASS_TRANSPARENT`;
- transparent color shader-variant selection;
- one color attachment with standard source-alpha blending;
- depth test enabled with depth writes disabled for transparent instance color;
- transparent draw filtering that submits alpha-blended packets and excludes opaque packets;
- native Forward+ lighting set preparation/binding for the transparent instance pass.

Alpha-scissor remains in the opaque/depth path as before.

## Failure visibility repair

The coordinator previously discarded native instance SUBMIT results and allowed instance PREPARE failure to disappear whenever terrain itself prepared successfully.

Terrain remains the authoritative functional lane, but companion instance failures are now visible through:

- rate-limited `[RD instances] Forward+ PREPARE failed` errors;
- rate-limited `[RD instances] Forward+ SUBMIT failed` errors;
- `instance_forward_prepare_failures`;
- `instance_forward_submit_failures`;
- `instance_forward_last_error`.

The instance renderer also exposes packet/material/mesh and prepared-pipeline diagnostics useful for separating publication, material, visibility, and submission failures.

## Dictionary diagnostic repair

`Terrain3DUtil::printDict()` enumerated a `Variant` key, coerced it to `StringName`, then used the coerced value for a second dictionary lookup. A key that stringified/coerced to an empty name therefore generated Godot's missing-key `Dictionary::operator[]` error.

The lookup now uses the original `Variant` key via `Dictionary::get()`. String conversion is display-only. This does not change the GDScript API.

## Files changed

- `src/core/terrain_3d_util.cpp`
- `src/rendering/rd/terrain_3d_rd_instance_renderer.h`
- `src/rendering/rd/terrain_3d_rd_instance_renderer.cpp`
- `src/rendering/rd/terrain_3d_render_coordinator.h`
- `src/rendering/rd/terrain_3d_render_coordinator.cpp`
- `tools/verify_instance_forward_submission.py`
- `tools/verify_terrain_primary_render_lane.py`

No `.gd` file was modified.

## Verification performed

- 32/32 `tools/verify_*.py` gates pass.
- `clang++ -std=c++17 -fsyntax-only` passes for:
  - `src/core/terrain_3d_util.cpp`
  - `src/rendering/rd/terrain_3d_rd_instance_renderer.cpp`
  - `src/rendering/rd/terrain_3d_render_coordinator.cpp`
  - `src/instances/terrain_3d_instancer.cpp`
  - `src/hlod/terrain_3d_hlod_streamer.cpp`
- `git diff --check` passes.
- No SCons, Godot executable, or `glslangValidator` is available in the patch environment, so final DLL linking and an actual Forward+ viewport smoke test remain machine-side validation.

## Runtime diagnostic expectation

After rebuilding, if instance rendering still fails, the failure is no longer silent. The coordinator should emit the failing Forward+ pass and `Terrain3DRdInstanceRenderer::getLastError()` text. The RD diagnostics also expose whether scene records, mesh surfaces, draw packets, alpha packets, pipeline preparation, and GPU-cull descriptors exist.
