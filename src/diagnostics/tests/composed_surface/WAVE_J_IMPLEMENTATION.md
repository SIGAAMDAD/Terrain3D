# Wave J / CS14 - Editor journal commit and material-parity capture gate

## Source changes

- `authoring/vt/integration/terrain_3d_composed_surface_editor_cook.{h,cpp}`: synchronous editor-owner cold job, using the existing CS07 journal + recook flags, CS13 real V3 pack invocation, CS03 validated atomic sidecar writer.
- `rendering/rd/core/terrain_3d_render_coordinator.{h,cpp}`: `T3D_EDITOR_NATIVE`-only `cookComposedSurfaceSidecarEditorCold()` entrypoint, preserving journal ownership in the existing coordinator; runtime DLL does not gain the editor method.
- `diagnostics/tests/composed_surface/test_editor_cook.cpp` and `run_editor_cook.sh`: CS14 claim/commit/stale/denied-parity tests. These use the existing deterministic CS13 pack fixture, not a production .t3sp.
- `diagnostics/tests/composed_surface/compare_material_parity.py` and `test_parity.py`: require matched region/mip/fingerprint/revision for albedo, normal and ORM linear-space exports; reject NaN, truncation, wrong identity, and report MAE/RMSE/max/failures.

## Lifecycle and serialization contract

1. Editor owner prepares immutable V3 source-pack bytes, sorted complete region-page list, actual control/page callbacks, authored material layers, valid source/material/topology revisions, fixed scratch/cache/flags/output buffers, and `m_bCanonicalMaterialParityAccepted` from independent real shader-to-producer validation.
2. Editor owner supplies a commit lock pair that serializes map/material revisions, sidecar publication and journal acceptance against all edits and source-pack swaps. These callbacks **must** be implemented by the owning editor service. `m_pBeginCommit` is permitted to reject acquisition; `m_pEndCommit` is called exactly once for a successful acquisition.
3. `cookComposedSurfaceSidecarEditorCold` claims the dirty region, verifies the ticket, derives mip-closure flags and calls CS13 in **memory-only** mode. CS13 performs both pre/post source revision checks. Any failure cancels the journal claim, retaining dirty bits for retry.
4. On completed composition, CS14 acquires the caller's edit serialization, rechecks both generations and the exact journal ticket, atomically commits the complete CRC-validated sidecar, acknowledges the ticket, then releases the lock. No GPU publication, shader activation, or legacy atlas retirement is performed.

If the edit owner permits revisions to change while the commit lock is held, transaction safety is no longer guaranteed; the interface makes that synchronization explicit. The method is **not** invoked from frame services or edit callbacks and has no producer warmup or internal heap allocator. The caller supplies the bounded buffers.

## Capture parity format

Reference and candidate each provide a JSON manifest with identical `width`, `height`, `region_key`, `mip`, `source_fingerprint` and `composition_revision`. Each manifest defines three row-major float32 little-endian channels in linear space: `albedo` RGB, `normal` XYZ, and `orm` RGBA. The capture files should be exported from the **same raster footprint** before compression. The comparator is not an automatic Vulkan readback service and is not a substitute for hardware image validation.

Example:

```sh
python3 src/diagnostics/tests/composed_surface/compare_material_parity.py reference/capture.json candidate/capture.json --max-abs 0.02 --max-mae 0.005 --output parity.json
```

The tolerances are explicit experimental inputs, not known visual-equivalence thresholds. Nonzero exit code indicates a failed channel. Compare final compressed results separately because BC encoding adds error.

## Validation and incomplete gates

`run_editor_cook.sh` runs Clang C++17 `-Wall -Wextra -Werror` with ASan and UBSan on the actual CS14/CS13/CS12/CS07/CS03 source units using a deterministic synthetic pack opener/decoder. It verifies no dirty job, parity denial, mid-cook stale rev, refused commit, superseding edit at commit and successful atomic sidecar commit followed by journal acknowledgment. `test_parity.py` verifies acceptance/failure/identity mismatch behavior; the earlier CS12/CS13 and shader contract checks were also re-run.

**Not yet validated:** Full engine/editor build, native `build/source_sets.py` registration, Godot Image BC decoder against a real .t3sp, actual raster-vs-composer comparison, matching enabled SPIR-V/Forward+ variants, frame capture or GPU performance. The provided native source snapshot lacks the build root: add `src/authoring/vt/integration/terrain_3d_composed_surface_editor_cook.cpp` to the **editor-only** source set in the consuming repository. The code remains opt-in, and `m_bCanonicalMaterialParityAccepted` stays closed in production until independent parity evidence exists.
