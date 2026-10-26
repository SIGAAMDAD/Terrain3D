# SuperTerrain ST06 + HLOD V5 — merged source handoff (2026-10-26)

**Canonical baseline:** this archive is a standalone merged source snapshot. It combines SuperTerrain ST00-ST06, GlobalMesh/HLOD W1-W7, the Godot 4.7 binding-collision fix, and the complete HLOD V5 delivery (U00-U05 + spatial 64 m / V5 subcell overlays).

## Ancestry and merge rules

- Target ancestor: `Terrain3D_SuperTerrain_ST06_GlobalMesh_HLOD_W7_Integrated_Full_Source.zip`, with `Terrain3D_Godot_4_7_Resource_Binding_Collision_Fix_Changed_Files.zip` applied.
- Incoming branch: `Terrain3D_HLOD_V5_Integrated_Full_Source_2026-10-26.zip`.
- Shared W1-W7 ancestor: `Terrain3D_GlobalMesh_HLOD_W1-W7_Integrated_Full_Source.zip`.
- Three shared modified files were three-way merged without text conflicts: `src/Terrain3DTypes.h`, `src/assets/resources/terrain_3d_mesh_asset.cpp`, `src/rendering/rd/core/terrain_3d_render_publication_builder.cpp`.
- V5 manifest and quadrant/source ownership, 64 m independent HLOD packages, meshoptimizer editor cooking, HLOD residency and detailed-source suppression are preserved together.
- ST06 COMPOSED VT, VHM and cutout publication; 64-byte GPU region ABI; global geometry cache and render publication; and all four Godot 4.7 resource-binding fixes are retained.
- The three retired FAR cooker/height shader sources remain deleted. Do not apply the earlier W1-W7/HLOD V5 overlays again.

## Validation performed

- 43/43 focused Clang C++17 `-fsyntax-only` jobs passed for affected editor/runtime production translation units (including all native V5 C++ units and shared WORLD renderer).
- 12/12 HLOD V5 + ST06 structural Python contract suites passed.
- 4/4 standalone HLOD/meshoptimizer C++ regression test files compiled with `-Wall -Wextra -Werror -fsyntax-only`.
- SHD-05 shader retirement and FAR intermediate retirement static validators passed.
- Input-preservation and combined source SHA-256 manifests were checked; see `CONSOLIDATION_AUDIT.json` and `SOURCE_SHA256.json`.

**Not tested:** custom Godot 4.7.2 MSVC editor/runtime DLL linking, live GDExtension binding registration, runtime HLOD/VHM Vulkan visuals, Arc/RTX GPU memory/transition telemetry, and canonical material-parity on hardware.

## Build / apply

Use the **full-source ZIP** for a new canonical baseline. Supply `godot-cpp/` and EASTL/EABase through `terrain3d_dependencies.zip` as before; they are external to the source archive. If using the incremental overlay, apply it to the _binding-fixed_ ST06+W7 baseline, not to the unpatched W7 source.

The upstream V5-only handoff remains `HLOD_INTEGRATION_HANDOFF.md`, and the original upstream consolidation report is archived at `docs/integration/HLOD_V5_Upstream_CONSOLIDATION_AUDIT.json`. This top-level `MERGE_HANDOFF.md` governs the composite tree.
