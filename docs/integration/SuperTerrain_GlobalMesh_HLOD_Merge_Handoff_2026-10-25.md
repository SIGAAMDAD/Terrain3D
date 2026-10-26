# Terrain3D SuperTerrain ST00-ST06 + Global Mesh/HLOD W1-W7 source merge

Consolidated from the common `Terrain3D_FAR_Regional_V1_Contract_Cleanup_2026-10-25_Full_Source.zip` baseline. This is the **integrated source tree**, without vendored `godot-cpp` or generated build products.

## Integrated systems

- SuperTerrain ST00-ST06: shared COMPOSED BC1/BC5/BC7 VT, 64-byte region publication contract, grouped VHM/cutout sidecars, coherent GPU candidate publication, unified shader sampling, and source-level FAR allocation/cooker retirement.
- GlobalMesh/HLOD W1-W7: `Terrain3DMesh`, `Terrain3DHLODMesh`, authored LOD source indexing, global mesh span cache, immutable WORLD geometry publication leases, deferred GPU range retirement, and editor import/conversion support.

## Merge-resolution ownership

Both sides changed exactly six common files:

- `src/rendering/rd/core/terrain_3d_render_coordinator.{h,cpp}`
- `src/rendering/rd/core/terrain_3d_render_publication_builder.{h,cpp}`
- `src/rendering/rd/world/runtime/terrain_3d_world_runtime.{h,cpp}`

The coordinator retains **both** SuperTerrain activation/reset state and mesh/HLOD GPU-safe publication and retirement state, including lifecycle resets. The publication builder retains committed SuperTerrain generation validation, HLOD mesh source indexing, the global geometry-cache copy/lease path, and WORLD publication identity. The WORLD runtime retains the SuperTerrain resource lifecycle and mesh-cache WORLD upload changes.

ST06's three retired FAR source/shader files have been physically removed. Five historical FAR-dependent validation scripts now assert the retired implementation's absence or the unified VHM contract instead of referencing deleted files.

## Acceptance performed during merge

- Clang C++17 `-fsyntax-only` using Godot 4.7 `godot-cpp` generated headers, in both `T3D_EDITOR_NATIVE` and `T3D_RUNTIME_NATIVE`, 48/48 focused translation unit checks passed.
- ST01, ST02, ST03/ST04, ST05, ST06 package regressions and shader ABI/retirement/startup gates passed.
- PCH integration and five repaired historical validators passed.
- HLOD W1-W7 source overlay preserved byte-for-byte except six knowingly merged shared files; three-way merge from the common baseline had two direct text conflicts, both resolved with both owner state machines retained.

## Verification remaining

- No Windows MSVC editor/runtime DLL link, Godot editor startup, live Vulkan pipeline/SPIR-V compilation, GPU fence/retirement behavior, HLOD transition screenshot or performance benchmarks executed in this environment.
- No canonical COMPOSED material GPU image-parity sign-off was established. Full visual and device-residency correctness remain to be verified on the target hardware.
- The supplied Unreal-style HLOD + meshoptimizer plan is reference material. Its planned follow-on policies and meshoptimizer integration were **not** implemented by this merge.

Apply the full-source archive as the single canonical baseline for future patches, or apply the incremental overlay only to the exact original GlobalMesh/HLOD W1-W7 full-source snapshot and run the included deletion helper.
