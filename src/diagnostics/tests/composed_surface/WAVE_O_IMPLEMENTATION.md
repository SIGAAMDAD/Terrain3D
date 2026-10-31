# Wave O / CS19 - Experimental Forward+ shader startup isolation and shader verification

## Source changes

- The ten `cs_terrain_strip_*` catalog entries are no longer mandatory CPU shader-load entries on the normal startup path. Catalog IDs and existing shipping variant IDs remain unchanged.
- The shader library samples `TERRAIN3D_COMPOSED_SHADER_EXPERIMENT=1` once during process-wide shared-state cold initialization. Only then does it request the ten experimental SPIR-V versions. A failed experimental version cannot fail ordinary default startup through the catalog load loop.
- `Terrain3DWorldRaster::configureComposedExperimentalCold(true)` rejects enablement if the process-wide shader catalog did not opt in. Callers must still explicitly configure the world runtime **before** world raster initialization and prepare a valid composed GPU pool. The environment switch alone does not activate the raster path.
- `test_shader_variants.py` expands the real world raster includes for all ten variants (20 vertex/fragment stages) and verifies the world descriptor bindings 13-16 and legacy binding separation. If `glslangValidator` is installed it compiles the expanded sources using Vulkan 1.2; `--require-compiler` makes its absence a hard error.

## Tests executed in this environment

- `run_pack_sampler.sh`, `run_cold_pipeline.sh`, `run_editor_cook.sh`, `run_bc7_mode6.sh`, `run_publication.sh`: PASS.
- `run_validation.sh` (including CS18): PASS for source-variant expansion, descriptor checks, and default-off opt-in policy.
- `test_shader_variants.py --require-compiler`: BLOCKED because glslangValidator is absent. **No SPIR-V validation was performed**.
- Native shader-library `clang++ -fsyntax-only` with Godot 4.7 headers: BLOCKED by missing EASTL headers in the source snapshot (`EASTL/algorithm.h`).
- Standalone CS18 catalog executable: BLOCKED at link because a built godot-cpp library is absent. A compiler-only catalog check is possible with Godot headers, but the linked fixture is not runnable in this environment.

## Remaining blockers before production activation

1. Compile all composed Forward+ vertex/fragment variants in Godot's actual shader importer and run SPIR-V validation/reflection, including all shader descriptor sets and buffer layouts.
2. Provide a full build root (SCons `build/source_sets.py` and EASTL/godot-cpp build artifacts) and compile editor, template_debug, and template_release.
3. Wire and exercise the real editor-side source package snapshot/cooker and production `.t3sc` population lifecycle; the cold cooker and CPU cache are not yet a complete autonomous editor/runtime workflow.
4. Run matched source-versus-composed raster image parity, holes, normal/DNR pass coherence, real compressed package decoding, and Vulkan GPU completion/retirement checks.
5. Measure GPU p50/p95/p99 and upload cost at moving and stationary horizon before removing the legacy middle composite.

The `T3D_COMP_SURF_ACTIVE` shader path remains an experiment; Source VT V3 and `.t3sc` disk ABIs are untouched. Do not enable composed shading for production on the strength of static source tests.
