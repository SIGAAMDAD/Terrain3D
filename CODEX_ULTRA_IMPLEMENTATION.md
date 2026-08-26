# Terrain3D RenderingDevice — Codex Ultra implementation specification

This is the compact execution specification. Exact checklist language remains in `docs/rendering_device_completion_contract.md`; load only the active sections with `tools\rd.cmd contract <batch>`.

## 1. Target

Produce a completed `experiment/rendering-device` branch that preserves the original fork's required public feature set while replacing Terrain3D-owned rendering with explicit RenderingDevice ownership. Final output must be a clean, reproducible Windows-capable repository and independently verified ZIP.

Reference implementation: `rd-alpha-pre-cleanbreak-reference` (`5d9e8b1c264867e147d5b9d1992acb883f82adb5`).
Target Godot: 4.7.2 stable, Forward+ with the ABI-10 native bridge.
Local Godot source root: `D:\godot-4.7.2-stable` (or `RD_GODOT_SOURCE`).
Validation hardware: Core Ultra 7 258V / Arc 140V, Windows x86-64.

## 2. Existing useful implementation

Retain and harden rather than restart:
- packed canonical clipmap topology;
- 8-byte terrain clipmap vertex records;
- U16/U32 topology index selection;
- near/middle/far RD shaders;
- frame/material/patch RD buffers;
- compute frustum culling;
- indexed indirect terrain submission;
- compositor/framebuffer scaffolding;
- partial diagnostics/accounting.

Batches A-E are complete. The clean-break integration targets the external Godot 4.7.2 ABI-10 Forward+ bridge, followed by editor/preview cleanup and complete cross-system/runtime/performance validation.

## 3. Non-negotiable end-state

1. `Terrain3DRdContext` owns/co-ordinates all Terrain3D GPU resources and lifetime.
2. Immutable triple-buffer (or equivalent >=3-slot) snapshot publication; generation IDs; no steady-state renderer mutex.
3. Terrain topology uploaded once; patch data updates dirty ranges only; validated culling and persistent indirect commands.
4. Terrain-generated textures/caches are direct RD resources; no Terrain3D RS duplicate or `texture_get_rd_texture()` bridge.
5. Fixed C++/GLSL terrain material ABI with stable material-table indices and persistent descriptors.
6. Near/middle/far retain reference material behavior, editing/debug behavior, holes, displacement, caches, Blinn-Phong path, fog/environment inputs, and required production lighting state.
7. `Terrain3DRdMeshCache` ingests imported mesh surfaces once into static vertex/index arenas with stable handles, U16 where valid and U32 fallback.
8. Regular Terrain3D instances use packed RD instance records, culling/LOD/grouping and indexed indirect submission. No runtime MultiMesh.
9. HLOD reuses RD mesh/material data and has RD descriptors, residency, culling, distance/LOD rules, indirect draws and shadows. No native render instance.
10. Instance/HLOD materials cover the subset actually used by the reference assets: albedo, normals, ORM/PBR, alpha scissor, culling/double-sided, colors, roughness, metallic, AO, emissive if present, filtering/samplers and shadow alpha.
11. Remove native terrain/instance/HLOD shadow proxies. Terrain, instances and HLOD participate in required directional shadows and receive required lighting.
12. Restore native-equivalent Forward+ participation through the minimal Godot 4.7.2 ABI-10 engine-side submission bridge. Terrain3D continues to own its buffers/resources; the bridge only inserts externally owned draw data into the proper renderer lists.
13. Correct depth, reversed-Z, MSAA, render scaling, multiview, multiple cameras, viewport recreation and motion vectors/previous state.
14. Editor painting/brush/region/debug/selection rendering and asset previews work without a hidden native terrain renderer.
15. Collision, navigation, sampling, editing, save/load, import/export, residency, baking/lightmap workflows and other non-rendering systems retain reference behavior.
16. C++, GDScript and C# are consistent with the clean-break API. Delete obsolete backend/Hybrid/proxy controls rather than shimming them.
17. Steady-state resource counts and memory plateau after warmup/traversal; no per-frame GPU object churn.
18. Final source audit has no unexplained legacy renderer symbols, placeholders or stale APIs.
19. Final Windows editor/debug and release builds, scripts, shaders, managed projects, runtime visual/streaming/memory/performance checks and archive revalidation pass.

## 4. Execution batches

The machine-readable source/contract mapping is in `docs/rd_codex_batches.tsv`, `docs/rd_contract_ledger.tsv`, and `docs/rd_source_map.tsv`. Do not recreate it.

### A — RD core, snapshots, topology/culling
Complete context ownership, deterministic destruction/rebuild/failure state, immutable publication, topology/patch ABI proof, CPU reference culler, GPU/CPU comparison mode, indirect bounds/stride checks and diagnostics. Add focused CPU tests now.

Gate: no normal render-path mutex; topology/patch/culling/indirect tests pass; no stable traversal VB/IB/pipeline/uniform-set creation.

### B — direct RD terrain textures/caches/compute
Convert generated height/control/color/region/cache/displacement/edit resources to direct RD ownership. Explicit formats/usage/byte counts, dirty updates, deterministic recreation and synchronization. Visible compute uses the correct global RD; local RDs only for justified isolated/readback work.

Gate: visible terrain resources have no RS duplicate/bridge and survive streaming replacement/recreation.

### C — material ABI and terrain shader parity
Separate authored resource state from GPU publication. Implement fixed material tables and full near/middle/far production behavior using the reference material/shader path as authority. Compile every RD shader variant under Godot 4.7.2.

Gate: material changes update RD tables/resources; visual/reference feature matrix for terrain material path is complete.

### D — RD mesh cache and regular instancing
Ingest meshes once, allocate static geometry arenas, implement handles/generations/material extraction, packed instance records, residency/dirty uploads, culling, LOD, grouping, indirect commands, depth/shadow packets and debug direct-draw path.

Gate: no runtime MultiMesh renderer; transforms/colors/custom data/visibility/LOD/streaming/editor updates match reference behavior.

### E — HLOD
Resolve HLOD packages to the shared mesh/material cache. Implement descriptors, transforms/state, CPU residency, GPU visibility, distance/LOD rules, indirect submission, shadows and deterministic unload/reload.

Gate: zero Terrain3D native HLOD render instances and no stale handles after residency churn.

### F — Forward+ bridge and native feature parity
Use exact Godot `4.7.2-stable` source through `RD_GODOT_SOURCE`. Maintain the small ABI-10 native bridge in that source tree; it accepts Terrain3D callbacks and borrowed frame/resource/pass data while Terrain3D inserts its owned RD draw data into the active Forward+ draw lists.

Bridge requirements: depth prepass, clustered/local lights, selected native shadow lists, opaque ordering, velocity/motion vectors, required GI, decals, reflection probes; handle reversed-Z, MSAA, render scaling, multiview, resize and viewport recreation. Keep the exact ABI and failure contract documented under `engine_patch/`; do not embed or vendor an engine patch in this repository.

Gate: the ABI-10-enabled 4.7.2 editor build executes terrain + instance + HLOD scenes with required Forward+ features. Native shadow proxy/fallback renderer is gone.

### G — editor, preview and API cleanup
Port editor overlays/brush/paint/region/debug/selection/visibility feedback and RD preview framebuffer/readback. Remove Direct/Hybrid/native-proxy backend UX and stale native assumptions. Update bindings, GDScript, C#, solution/project metadata and plugin registration.

Gate: cacheless editor startup, plugin initialization, GDScript parse/import, managed build and editor tools succeed without native terrain proxy.

### H — complete regression and validation
Regression-test collision/navigation/sampling/editing/save/load/import/export/residency/baking. Finish RD test suite. Traverse supplied 176-region data enough to force load/unload/cache replacement/instance/HLOD residency and clipmap snaps. Capture resource/memory/performance counters before warmup, after warmup and after long traversal. Run original-feature parity matrix, second independent source audit, debug/release builds, shader validation, C#/GDScript validation, Git integrity, package extraction/rebuild and SHA-256.

Gate: every mandatory contract entry is complete or has explicit verified evidence. No mandatory item remains pending/temporary/fallback/TODO.

## 5. Forward+ external engine boundary

Stock Godot does not expose this internal Forward+ submission hook. Batch F therefore builds an exact Godot 4.7.2 source editor containing the separately maintained ABI-10 producer and validates Terrain3D against that binary.

Keep the external engine bridge narrow:
- no Terrain3D resource ownership in engine code;
- no Terrain3D scene-node implementation in engine code;
- no second terrain renderer;
- no general renderer rewrite;
- expose only the minimum submission/lifetime interface required for externally owned RD draw packets;
- maintain explicit bounds/lifetime/generation validation at the boundary.

## 6. Windows/GPU validation strategy

Do not use a full interactive D3D12 editor launch for routine compile/parse work.

Fast/non-GPU lane:
- incremental MSVC/SCons extension build;
- headless import/editor checks;
- GDScript/C# build checks;
- CPU tests/static audits.

GPU lane:
1. Vulkan Forward+ short runtime smoke first. It exercises RenderingDevice on the Arc 140V while avoiding D3D12-specific instability during ordinary iteration.
2. D3D12 Forward+ short runtime smoke at GPU-relevant batch gates and final validation.
3. Use `--render-thread safe`, `--single-window`, Dummy audio, a small window and finite `--quit-after` for automated GPU smoke.
4. Performance measurements use the intended production thread/renderer configuration, not the conservative automation flags.

All Godot/SCons output goes to `.rd_logs`; wrappers print only success or the failure tail.

## 7. Definition of done

Completion requires the contract's end state, not an implementation milestone. Final package must contain the complete Git repository on `experiment/rendering-device`, correct Noah Van Til attribution, clean worktree, required source/project/C#/GDScript/shaders/build files, ABI-10 external bridge contract, tests and validation evidence. Extract the produced ZIP into a separate directory and verify branch/SHA/history/files/build/Git integrity and archive SHA-256 before handing it off.
