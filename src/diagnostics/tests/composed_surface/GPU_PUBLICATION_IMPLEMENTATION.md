# CS05–CS06 (CS05-CS06) - composed GPU publication and opt-in raster sampling

## Source changes

- `rendering/rd/vt/core/terrain_3d_composed_surface_gpu_table.h`: independent std430 page lookup, 16-probe bounded hash, signed world-page coordinates, tombstone eviction, revision/device generation validation. The table never aliases Sparse VT V3 entries.
- `rendering/rd/vt/gpu/terrain_3d_composed_surface_gpu.{h,cpp}`: cold-only BC1/BC5/BC7 arrays and a tracked GPU lookup buffer. Fixed staging buffers, all-channel upload, external GPU-completion receipt before render-owner publication, positive GPU-use retirement confirmation before slot reuse. The main VT transfer-budget owner must grant a token before calling `submitPageSetHot`. Failed/stale jobs must not write physical texture slots.
- `rendering/rd/vt/runtime/terrain_3d_composed_surface_cache.{h,cpp}`: read-only state assertions for ready/uploading jobs, including the corresponding sidecar page revision and region key.
- `memory/policy/terrain_3d_composed_surface_budget.h` plus RD memory domain/category: budget 4x48B lookup entries *per resident page* in GPU and CPU, plus bounded CPU pending and staging. Physical RD objects charge `COMPOSED_SURFACE` directly; do not also call `t3d_comp_surf_reserve_budget()` for the same allocations.
- `project/addons/terrain_3d/rd/terrain_composed_surface_sampling.glslinc`: mip0 cache lookup, checked physical layer, BC1 sRGB decoding, BC5 tangent normal and BC7 ORM reconstruction, explicit-gradient sampling.
- Fragment shader integration: optional `T3D_COMP_SURF_ACTIVE` gate in standalone/embedded interfaces and the color/DNR material paths. Misses use **detailed material sampling**, not the legacy flat-normal RGB-only middle composite. Control-map coverage remains authoritative for the depth path; lighting, macro, authored tint and weather stay live.

## ABI and descriptor ranges

The existing clipmap binding 38 is occupied by `STRIP_PARTS` and must not be overwritten. The optional composed namespace uses set 0 bindings **39-42** for standalone clipmap, and set 1 bindings **13-16** for embedded world raster. In both cases, the order is `storage buffer lookup`, `combined sampler + BC1 array`, `combined sampler + BC5 array`, `combined sampler + BC7 array`.

These bindings belong in the *existing global descriptor authority* when cold-enabled; they are not constructed by per-fragment or per-page code. `acquireBindingsHot` returns the physical RID snapshot for binding creation. Any descriptor/pipeline selection must be coherent across Forward+ color, depth-normal-roughness, depth and world-raster variants.

## Preconditions and remaining work

The feature is **not activated by any version** in this patch. Composed material parity (CS02) is outstanding: CS00–CS02's compute shader consumes already-resolved samples, so no authoritative full-material `.t3sc` source exists yet. The supplied source snapshot also has no consuming `build/source_sets.py` / root SCons build and no local Vulkan/Godot/shader compiler tools.

To activate, a future integration must:

1. Finish the GPU composer / reference parity producer, including height blends, authored controls, normals, gutters and color-space equivalence. Feed exactly matched sidecar page IDs plus region keys and world-page coordinates to `submitPageSetHot`. Currently only mip0 is accepted; higher mips require a versioned lookup/filter extension.
2. Attach the composed cache and GPU resource owner to `Terrain3DSharedVTRuntime`, supply *accounted* cold scratch/lookup/pending arrays, pass the **existing shared VT transfer-budget grant**, and connect real GPU submission completion/fence epochs. No immediate `true` completion on texture upload is sufficient.
3. Extend standalone and world-raster descriptor recipes and cold shader variants together; validate shader reflection, active pass consistency, device-loss descriptor retirement, shader compilation and memory plateau.
4. Wire edit/material-source invalidation before any replacement can become visible. Retire a stale mapping at a safe render boundary and wait for prior consumers to drain before allowing physical slot reuse.

### Available validation

- `g++ -std=c++17 -Isrc -Wall -Wextra -Werror src/diagnostics/tests/composed_surface/test_gpu_table.cpp -o /tmp/cs05_table && /tmp/cs05_table`
- `python src/diagnostics/tests/composed_surface/test_shader_contract.py`
- `python src/diagnostics/tests/composed_surface/composed_surface_shader_checks.py`
- CS01 / CS03-CS04 standalone tests.

These tests exercise contracts and topology. They do **not** establish Vulkan correctness, an enabled shader's SPIR-V validity, complete engine compilation, or a measured GPU uplift.
