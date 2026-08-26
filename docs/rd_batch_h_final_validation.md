# Batch H — Final clean-break convergence and validation

Batch H closes the RenderingDevice branch after Batches A–G. It is a final regression, ownership, diagnostics, non-rendering parity, validation, and packaging pass; it does not reintroduce a compatibility renderer to make validation easier.

## Final implementation convergence

- `Terrain3DRdContext` is the sole direct engine rendering gateway. Global/local `RenderingDevice` acquisition, render-thread queueing, and lifecycle synchronization are centralized there.
- The legacy clipmap mesher is CPU topology/state generation only. Its RenderingServer mesh/MultiMesh submission path has been removed.
- The public ocean feature is direct RD: immutable clipmap topology, RD buffers/pipelines, `terrain_ocean.glsl`, Forward+ transparent submission, and optional native shadow submission.
- Forward+ integration uses ABI 10 with exact 472/120/48/200-byte packets, separate main/shadow contexts, generation-gated borrowed state, serial-validated submit, and all required capability bits.
- The Godot 4.7.2 producer is maintained in the external engine source tree; this repository contains the consumer and exact dependency/failure contract, not a stale embedded patch.
- `Terrain3DMaterial` no longer owns or publishes a native material RID. Shader source/reflection remains for authoring while render-time material state is published through RD snapshots/tables.
- Displacement uses the RD compute path; the raster/native-material fallback is gone.
- Remaining `MultiMesh` references are import/authoring compatibility only and convert to Terrain3D packed instance storage.

## Validation target

- Godot source/editor: `D:\godot-4.7.2-stable`, Windows x86-64, Forward+ ABI 10.
- Terrain3D bindings: `thirdparty/godot_api/extension_api-4.7.2.json` and Godot.NET.Sdk/4.7.2.
- Native gates: debug/tools and release Terrain3D SCons builds, Godot editor build with focused bridge tests, cacheless import/editor smoke, and Vulkan/D3D12 GPU smoke when the host driver permits them.
- Static gates: Batch C–G/F contracts, final legacy audit, `git diff --check`, and repository integrity checks.

Runtime and GPU results are evidence, not inferred from successful compilation. Any host- or driver-specific limitation must be reported alongside the exact command and failure rather than described as completed validation.

The final parity matrix is `docs/rd_work/final_parity_matrix.tsv`; the justified final-symbol set is `docs/rd_work/final_symbol_allowlist.tsv`.
