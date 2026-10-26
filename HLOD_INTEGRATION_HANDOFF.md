# Terrain3D HLOD — canonical V5 full-source handoff (2026-10-26)

## Integration

This is one **full source snapshot**. It includes the October 25 W1–W7 + U00–U05 integrated source and the three subsequent spatial overlays in order:

1. `Terrain3D_HLOD_RegionLeaf_And_64mCell_Changed_Files_2026-10-25.zip` — independently culled 128 m HLOD leaves and cold 64 m instance cell buckets.
2. `Terrain3D_HLOD_64m_Streamable_Subcells_Changed_Files_2026-10-25.zip` — 64 m independently streamed HLOD packages / wire V4.
3. `Terrain3D_HLOD_64m_Independent_Source_Ownership_Changed_Files_2026-10-26.zip` — V5 manifest, quadrant-specific mesh/node ownership and GPU-gated detailed-source suppression.

**Do not apply any of these overlays again on this snapshot.** The source can be merged as a whole against other workgroups' canonical trees. Retain the canonical manifest and ownership code together; merging only the editor baker or only the runtime streamer breaks the V5 contract.

## Key contracts

- `src/hlod/format/terrain_3d_hlod_runtime_codec.*`: V5 HLOD manifest with earlier V2–V4 readers.
- `src/hlod/runtime/terrain_3d_hlod_streamer.*`: independent leaf residency, quadrant readiness and GPU-publication gating.
- `src/instances/storage/terrain_3d_instance_publication_builder.*`: detailed instancer visibility per owned quadrant.
- `src/streaming/runtime/terrain_3d_node_streamer.*`: detailed node visibility per owned quadrant.
- `project/addons/terrain_3d/tools/hlod_baker.gd`: deterministic source ownership, contained-geometry cook and package publication.
- `thirdparty/meshoptimizer`: vendored 1.3, editor-native cooking only.

The W1–W7 geometry-cache physical span model, shader ABI and indirect draw path are preserved according to previous targeted checks; this handoff does not include graphics runtime proof.

## Handoff / acceptance state

The **source integration is consolidated**; Godot editor/runtime scene testing, Windows MSVC linking and GPU publication/camera-reversal benchmarks have **not** been executed by this consolidation. These remain acceptance tasks. Do not claim validated shipping/runtime behavior on this evidence alone.

Run both `T3D_EDITOR_NATIVE` and `T3D_RUNTIME_NATIVE` builds against custom Godot 4.7.2 and the matching godot-cpp/EASTL dependencies. Exercise the bundled HLOD GDScript tests, create mixed authored meshes/instancer 64 m quadrant fixtures, bake, reload V5 package, and test camera transitions with delayed GPU upload, memory budget eviction and sparse 64 m package sets. Compare V2/V3/V4 legacy manifest behavior. Measure Intel Arc 140V and RTX 3060 Ti streaming and frame times.

## Verification of consolidation

- All original integrated-source SHA-256 entries verified before overlay application.
- Applied overlay files in exact documented dependency order.
- Verified each changed-file SHA-256 and V5 preimage hashes before overwrite.
- Compared merged production-source files to previous V5 working tree; no mismatches.
- `SOURCE_SHA256.json` covers the resulting single full-source snapshot.
- See `CONSOLIDATION_AUDIT.json` for source lineage and changed file hashes.

## Post-merge validation performed (2026-10-26)

- 10/10 Python structural regression suites passed against the consolidated source.
- 12/12 Clang C++17 `-fsyntax-only` checks passed across the editor/runtime native configurations for package format, codec, streamer, instance publication, node streamer, and streaming integration.
- Standalone `hlod_subcell_ownership_native_test.cpp` and `hlod_spatial_bucket_test.cpp` compiled with `-O2 -Wall -Wextra -Werror` and passed.
- Updated two **test-only** stale builder-version assertions from 12 to 13 to match the current `hlod_baker.gd` signature version. No production code was changed during this consolidation.
- Original baseline: 1,233 SHA-256-verified files; three overlays: 14 + 19 + 22 checksummed source/test entries, 33 distinct paths affected.
- Consolidated source matched the previously delivered latest V5 source tree across 1,204 comparable files, before the two test-only version changes.
- No custom Godot executable or on-target GPUs were run during this packaging operation. Acceptance work is still required on those targets.
