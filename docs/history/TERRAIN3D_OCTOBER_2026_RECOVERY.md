# Terrain3D recovery — October 24–26, 2026

This is the **source-backed archive reconstruction** branch. Every original Git
commit prior to `4c0632f9e8f` remains unchanged. Additional commits replay the
18 source archives/patch packages listed below using an explicitly synthetic
reconstruction identity and timestamp. They are **not** the original author dates.

## Automation source baseline

The active `src/` and `project/` source is the same byte-for-byte as the
2026-10-26 ST06 + HLOD V5 consolidated handoff and the separately uploaded
`src(1).zip`, excluding four generated `.dll`, `.exp`, `.lib`, `.pdb` files.
Those four original binary artifacts remain recoverable from the preserved
`recovered/source-backed-2026-10-26` checkpoint. All build and HLOD
integration files from the 2026-10-26 handoff are present.

The current branch should be the starting point for development automation.
The source has **not** passed a live Godot editor/Windows MSVC/GPU integration
test in this recovery operation. Source-handoff documents contain separate
static checks; do not equate those with successful builds on your toolchain.

## Reconstructed milestone order

The order is inferred from patch metadata, source-package lineage and merge
handoff documents; it is not evidence of the original Git parent graph.

1. `env-p01-p07` — `Terrain3D_ENV_P01-P07_Consolidated_Source_2026-10-24.zip`; +1407, ~0, -0 file paths.
2. `shd04-w7` — `Terrain3D_W7_SHD04_Handoff_2026-10-24.zip`; +2, ~6, -0 file paths.
3. `w1-w9` — `Terrain3D_W1-W9_Cumulative_Changed_Files_2026-10-24.zip`; +14, ~53, -7 file paths.
4. `w10-brush` — `Terrain3D_W10_Brush_GPU_Changed_Files_2026-10-24.zip`; +0, ~5, -0 file paths.
5. `avt08-brp-w1-w2` — `Terrain3D_AVT08_BRP_Waves1-2_Integrated_Full_Source_2026-10-25.zip`; +37, ~193, -16 file paths.
6. `avt08-brp-ucmp-w3` — `Terrain3D_AVT08_BRP_UCMP_Wave3_Merged_Full_Source_2026-10-25.zip`; +12, ~60, -3 file paths.
7. `ucmp-legacy-retire` — `Terrain3D_UCMP_Legacy_Shader_Retirement_Integrated_Source_2026-10-25.zip`; +2, ~29, -288 file paths.
8. `ucmp-legacy-recheck` — `Terrain3D_UCMP_Legacy_Shader_Retirement_Rechecked_2026-10-25_Integrated_Source.zip`; +0, ~15, -0 file paths.
9. `ucmp-editor-startup` — `Terrain3D_UCMP_Editor_Shader_Startup_Hotfix_Changed_Files_2026-10-25.zip`; +1, ~5, -0 file paths.
10. `ucmp-brush-ansi` — `Terrain3D_UCMP_Brush_Shader_Import_ANSI_C_2026-10-25_Changed_Files.zip`; +2, ~41, -0 file paths.
11. `ucmp-catalog` — `Terrain3D_UCMP_Shader_Source_Catalog_Closure_Fix_2026-10-25_Changed_Files.zip`; +1, ~2, -0 file paths.
12. `ucmp-snapshot` — `Terrain3D_UCMP_Snapshot_Activation_Fix_2026-10-25_Changed_Files.zip`; +1, ~5, -0 file paths.
13. `bld01-bld02` — `Terrain3D_BLD01_BLD02_PCH_VT_Header_Network_2026-10-25_Changed_Files.zip`; +6, ~26, -0 file paths.
14. `bld01-sconstruct` — `Terrain3D_BLD01_PCH_SConstruct_Integration_2026-10-25_Changed_Files.zip`; +2, ~1, -0 file paths.
15. `pch-lnk2011` — `Terrain3D_PCH_LNK2011_Companion_Object_Fix_2026-10-25_Changed_Files.zip`; +0, ~4, -0 file paths.
16. `st06-hlod-w7` — `Terrain3D_SuperTerrain_ST06_GlobalMesh_HLOD_W7_Integrated_Full_Source.zip`; +86, ~80, -5 file paths.
17. `godot-binding-fix` — `Terrain3D_Godot_4_7_Resource_Binding_Collision_Fix_Changed_Files.zip`; +0, ~6, -0 file paths.
18. `st06-hlod-v5` — `Terrain3D_SuperTerrain_ST06_HLOD_V5_Integrated_Full_Source_2026-10-26.zip`; +67, ~28, -0 file paths.

## Source provenance

The old source-backed checkpoint and forensic object branches are preserved.
No original Git author, timestamp, commit object, or source content was
rewritten. This branch supersedes the previous synthetic final-snapshot
reconstruction for ongoing source development.
