# Terrain3D API and RenderingDevice Shader Repair

Target: Godot 4.7.1

Branch: `experiment/rendering-device-editor`

Native API revision: `2026082202`

Native build ID: `a2.9.1-api-shader-repair`

## Scope

This pass repairs the public Terrain3D naming/API surface and the first direct-RenderingDevice clipmap shader before the remaining RD alpha implementation continues. It does not attempt to complete the later RD material/lighting/streaming work in the same patch.

## Public class naming

All declared repository classes and structs using the `T3D*` type prefix were renamed to the canonical `Terrain3D*` prefix. The root terrain type is `Terrain3D`, not `Terrain3DTerrain`.

The rename covers native C++ declarations/definitions and ClassDB registration, GDScript `class_name` declarations and type annotations, C# wrapper classes and `NativeName` bindings, editor metadata, demo resources/scripts, and the RenderingDevice experiment classes. The C# wrapper filenames were restored to their canonical `Terrain3D*.cs` names.

Examples:

- `T3DTerrain` -> `Terrain3D`
- `T3DData` -> `Terrain3DData`
- `T3DMaterial` -> `Terrain3DMaterial`
- `T3DRegion` -> `Terrain3DRegion`
- `T3DMesher` -> `Terrain3DMesher`
- `T3DRdClipmapRenderer` -> `Terrain3DRdClipmapRenderer`
- `T3DRenderCoordinator` -> `Terrain3DRenderCoordinator`
- `T3DRdCompositorEffect` -> `Terrain3DRdCompositorEffect`

Lowercase/C-style ABI symbols, binary magic identifiers, macro prefixes, and non-class callback typedefs were left unchanged because they are not class names.

This is an intentional breaking API rename. No `T3D*` class compatibility aliases were retained. The native/script API revision was bumped so an old native library cannot silently bind to the renamed addon.

## Older API repair

The addon/editor GDScript remains on the 4.7-era APIs introduced by the previous modernization pass. A stale live dependency remained in `project/Terrain3D.csproj`: it targeted `Godot.NET.Sdk/4.6.3` while the project and GDExtension require Godot 4.7. The project now targets `Godot.NET.Sdk/4.7.1`.

Static searches were rerun for the previously removed Godot 3/4.x patterns, including zero-argument `EditorInterface.get_editor_viewport_3d()`, old mesh primitive access, old asset-limit names, `yield`, `setget`, `Pool*Array`, `File.new()`, `Directory.new()`, and `.instance()`. No live source use of those audited patterns remains.

## Direct RD shader defects repaired

### Fragment-accurate holes

The original direct shader sampled the control-map hole bit in the vertex stage and passed it as a flat triangle value. That made hole rejection depend on the provoking vertex instead of the fragment's terrain texel. Control lookup and hole discard now happen in the fragment stage.

### Missing-region phantom floor

A missing region previously returned the terrain minimum height while the fragment shader continued drawing, creating a flat fallback floor. The direct fragment path now discards fragments whose page-table lookup has no resident region. Middle/far fallback for nonresident data belongs to the remaining RD streaming alpha; drawing fabricated geometry does not.

### Height interpolation below vertex spacing

The previous `draw_data.scale < vertex_spacing` path still sampled nearest height texels. It now performs bilinear height sampling across four texels, including cross-region page-table lookup. Normal sampling remains a deliberate nearest-height finite difference at `max(vertex_spacing, draw_scale)`.

### Control-map addressing

Fragment control lookup now uses floor-based world-texel addressing, matching the established terrain fragment path instead of rounding to a neighboring texel.

### Material UV decode

The RD shader's control rotation sign and scale operation did not match Terrain3D's canonical material shaders. The direct path now uses the same negative 1/16-turn rotation convention and multiplies the decoded control scale by the texture UV scale instead of using its reciprocal.

The current alpha path uses the default Terrain3D texture asset UV scale of `0.1`. Per-asset scale/tint and runtime logical-to-physical layer remapping remain part of the next RD material-state implementation.

### Material sampler addressing

Terrain data textures and material textures no longer share one clamp sampler. Height/control/color use a clamp-to-edge sampler; albedo uses a repeat sampler on U/V, matching Terrain3D material tiling semantics.

### Painted color sampling

Painted terrain color was sampled at vertices and interpolated over the clipmap triangles. It is now sampled in the fragment stage with the canonical half-texel convention and explicit cross-region bilinear reads, which tracks the authored color map rather than clipmap tessellation density and avoids clamp seams at region edges.

### Texture-array bounds

The direct shader clamps the usable material layer count to both the authored count and the physical albedo-array depth. This prevents out-of-range physical layer sampling. It does not substitute for the pending runtime material residency/remapping table.

### Blend curve

Base/overlay blending now uses the established middle-tier transition (`smoothstep(0.12, 0.88, blend)`) rather than the separate experimental curve.

## RD sampler/resource ownership changes

`Terrain3DRdClipmapRenderer` now owns two persistent sampler RIDs:

- terrain sampler: linear filtering, clamp-to-edge;
- material sampler: linear filtering, repeat U/V and clamp W.

Creation is gap-safe: each missing sampler is allocated independently, so a partial initialization retry does not replace or leak an already valid sampler. Shutdown frees both. RD diagnostic/resource labels were also normalized to `Terrain3D.RDExperiment.*`.

## How the repaired direct path works

1. `Terrain3DRenderCoordinator` snapshots the current clipmap patch state produced by `Terrain3DMesher` and publishes the current terrain texture RIDs/page table to the RD renderer.
2. `Terrain3DRdCompositorEffect` receives the PRE_OPAQUE compositor callback and asks the coordinator to submit the direct terrain pass.
3. `Terrain3DRdClipmapRenderer` reuses persistent LOD0/outer topology, frame UBO, page-table SSBO, sampler RIDs, and uniform sets until their dependencies change.
4. The vertex shader snaps/morphs the clipmap, resolves height through the region page table, interpolates sub-spacing height where required, and derives a finite-difference world normal.
5. The fragment shader resolves the exact control texel/region, discards missing regions and holes, samples painted color, decodes base/overlay material selection/rotation/scale, samples the repeating albedo array, and applies the experiment's simple managed directional-light/ambient model.
6. The RD pass writes the active scene color/depth attachments during PRE_OPAQUE. Later opaque geometry can depth-test against the terrain. The direct surface still does not become a normal Godot Forward+ render-list item.

## Validation completed in this pass

Static validation completed successfully for:

- all 111 renamed declared `T3D*` type identifiers absent from the repository;
- zero remaining `class`, `struct`, or GDScript `class_name` declarations beginning with `T3D`;
- 30 GDScript global classes with no duplicate `class_name` declarations;
- native/addon API revision and build-ID agreement;
- C# wrapper `NativeName` values matching the renamed native types;
- JSON parsing of `setting_metadata.json`;
- 82 literal `res://` references checked for existence, excluding the documented optional Sky3D dependency;
- generated Visual Studio project metadata regenerated after the C# wrapper moves;
- RD shader stage/delimiter structure and repair invariants;
- `git diff --check`.

This environment does not contain a Godot 4.7.1 editor, `dotnet`, a GLSL/SPIR-V compiler, or the repository's `godot-cpp` checkout. The current patch therefore has not been upgraded from static validation to a fresh native/GDScript/shader build or editor smoke test. The earlier repository history contains a compile pass against older supplied 4.6-generated bindings; that is historical evidence, not compile validation of this renamed 4.7.1 API patch.

## Remaining work for the RD alpha

The next implementation pass should complete the RD experiment rather than reopen the repaired public API:

1. Build/import/smoke-test this API/shader patch under the actual Godot 4.7.1 editor and 4.7.1-compatible `godot-cpp` bindings.
2. Add persistent RD material state: per-asset UV scale, tint, average/fallback data, material residency bits, and logical material ID -> physical texture-array layer mapping.
3. Add the remaining material channels required by the fork's intended shading path: packed normal/roughness/AO/metallic and the chosen displacement path.
4. Integrate middle/far resident-height/material data so a missing near-region page can transition to real lower-detail data instead of being discarded.
5. Finish explicit RD culling/submission work and reduce CPU draw/setup cost where the current patch list remains per-draw.
6. Complete multiview handling; the compositor experiment currently reports single-view support.
7. Decide/implement the native-engine submission bridge required for pure-RD terrain to participate in Godot's native depth prepass, clustered Forward+ lights, shadow receiving, decals, probes, GI, and motion vectors without a RenderingServer proxy. Hybrid/full-native proxy modes remain the current feature-parity bridge.
8. Profile the completed alpha on the target hardware and separate the cost of native proxy participation from explicit RD terrain submission.

The public API repair and current shader fixes are intended to be the stable base for that work.
