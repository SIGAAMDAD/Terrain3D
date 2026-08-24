# Batch H — Final clean-break convergence and validation

Batch H closes the RenderingDevice branch after Batches A–G. It is a final regression, ownership, diagnostics, non-rendering parity, validation, and packaging pass; it does not reintroduce a compatibility renderer to make validation easier.

## Final implementation convergence

- `Terrain3DRdContext` is the sole direct engine rendering gateway. Global/local `RenderingDevice` acquisition, render-thread queueing, and lifecycle synchronization are centralized there.
- The legacy clipmap mesher is CPU topology/state generation only. Its RenderingServer mesh/MultiMesh submission path has been removed.
- The public ocean feature is direct RD: immutable clipmap topology, RD buffers/pipelines, `terrain_ocean.glsl`, Forward+ transparent submission, and optional native shadow submission.
- The Forward+ integration ABI is 8 and includes transparent submission in addition to depth, opaque, shadow, and motion.
- `Terrain3DMaterial` no longer owns/publishes a native material RID. Shader source/reflection remains for authoring while render-time material state is published through RD snapshots/tables.
- Displacement uses the RD compute path; the raster/native-material fallback is gone.
- Remaining `MultiMesh` references are import/authoring compatibility only and convert to Terrain3D packed instance storage.

## Validation performed in this environment

- All Batch C–G/F static regression contracts pass after the H clean-break changes.
- Final legacy audit is allowlist-clean: direct `RenderingServer` is confined to `terrain_3d_rd_context.cpp`; runtime mesh/instance/material creation legacy symbols are zero.
- 81/81 Terrain3D C++ translation units compile in both C++17 debug/tools and release GDExtension configurations against the retained Godot 4.7.1 extension API.
- Both Linux debug and release GDExtensions link with `--no-undefined` against the bundled/generated Godot 4.7.1 bindings.
- Godot 4.7.1 cacheless import/editor parsing succeeds with the linked extension; the native RD CPU suite reports 85 passed, 0 failed.
- The supplied 176-region fixture passes its 354-file SHA-256 manifest. Godot loads all 176 `Terrain3DRegion` resources as 128×128 regions with valid height maps and verifies 176 paired runtime pages plus the far atlas.
- The engine patch remains syntactically parseable against its exact 4.7.1-stable target metadata.
- `git diff --check` and `git fsck --full --strict` are final gates before packaging.

## Environmental validation limits

The available Godot binary is the stock 4.7.1 build and therefore intentionally lacks `Terrain3DForwardPlusBridge`. Native Forward+ pass execution, GPU visual traversal, runtime MSAA/multiview/temporal checks, and GPU memory/performance plateau measurements cannot be truthfully executed with that binary. The extension fails visibly when the bridge is absent, as required.

SCons and dotnet/MSBuild/Mono are not installed in this environment. Native compilation/linking is performed directly with the retained Godot 4.7.1 bindings; managed C# validation is limited to exact reference-file parity, generated binding/static API checks, and project metadata. These limitations are validation availability limits, not missing Batch H implementation paths.

The final parity matrix is `docs/rd_work/final_parity_matrix.tsv`; the justified final-symbol set is `docs/rd_work/final_symbol_allowlist.tsv`.
