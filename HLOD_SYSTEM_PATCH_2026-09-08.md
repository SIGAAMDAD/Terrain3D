# Terrain3D HLOD System Patch — 2026-09-08

## Scope

This patch applies the full HLOD repair pass requested after `HLOD_SYSTEM_AUDIT_2026-09-08.md`. It updates the HLOD editor window, authoring/bake path, native instancer source builder, manifest validation, dirty-build signatures, and geometric-error floor. The runtime selection/streaming architecture is preserved.

## Fixed defects

### 1. HLOD window no longer appears during editor-plugin initialization

The plugin now initializes `hlod_panel.visible = false`, and `Terrain3DHLODPanel._ready()` calls `hide()` before constructing the rest of the UI. The window is shown only by the explicit `open()`/`popup()` path.

### 2. Analyze Sources no longer performs hidden HLOD geometry baking

The old `analyze()` path called `_collect_sources()`, which called `build_instancer_source()`. That method expanded every placed instance into `SurfaceTool` geometry. Merely opening the HLOD window therefore performed much of the expensive instancer bake before the user pressed Bake.

A new native `analyze_instancer_source()` API now performs only:

- source eligibility validation;
- instance counting;
- exact transformed AABB accumulation;
- source-triangle counting;
- deterministic packed-transform content hashing.

Instancer geometry is represented as a deferred source descriptor until a cluster actually needs rebuilding.

### 3. Bake Dirty now avoids rebuilding unchanged instancer geometry

Instancer source descriptors carry a deterministic signature derived from:

- packed instance transforms and cell locations;
- source mesh content;
- effective material content;
- region/mesh identity.

`_build_cluster()` evaluates the package reuse signature before materializing deferred instancer geometry. Unchanged L1 packages can therefore be reused without duplicating/simplifying all placed meshes first.

### 4. Dense instancer baking is bounded and pre-reduced

The previous native path accumulated all instances of one mesh in a region into a single `SurfaceTool` before simplification.

The patched builder uses bounded cell-oriented chunks:

- target temporary chunk budget: 131,072 source triangles;
- hard cap: 128 instances per chunk;
- dense-source preproxy budget: 262,144 triangles before L1 cluster simplification;
- minimum simplification ratio lowered to 1% for extreme dense-source prepasses.

Each bounded chunk is optionally simplified before it is returned to GDScript. L1 then merges already-reduced chunks rather than the entire fully-expanded instance population.

This converts peak temporary geometry from approximately `source_mesh * all_instances` to roughly `source_mesh * bounded_chunk_instances`, while also keeping the aggregate dense preproxy representation bounded for ordinary region populations.

### 5. HLOD manifest path is now authoritative everywhere

`terrain.hlod_manifest_path` is resolved through one helper and is used consistently by:

- validation;
- dirty-bake reuse;
- final publication;
- clear-generated.

Absolute paths are localized when possible. Relative configured paths are resolved under the Terrain3D data directory. Transaction staging/backup files now preserve the configured manifest extension.

### 6. Packed runtime hierarchy is validated before publication and by Validate HLOD

`Terrain3DHLODManifest` now exposes `validate_runtime_blob()`, which decodes the packed runtime hierarchy with the production codec and confirms its entry count.

The baker now rejects a compiled blob that cannot immediately decode. Transactional staging also verifies the saved/reloaded blob before replacing the previous manifest.

`Validate HLOD` now checks:

- manifest format;
- active `hlod_max_level`;
- active builder/reduction build signature;
- packed runtime blob presence/decodability;
- all package metadata already checked previously.

### 7. Dirty signatures now detect pathless embedded Resource mutations

Pathless Resources are recursively content-signed from storage properties rather than represented only by `class + instance_id`.

The recursion handles:

- nested Resources;
- Arrays;
- Dictionaries with deterministic sorted entry signatures;
- ordinary Variant values through byte-content hashes;
- resource cycles;
- a recursion-depth safety limit.

This prevents in-session edits to embedded textures/shaders/material subresources from silently reusing stale HLOD packages.

### 8. Coplanar simplification no longer reports misleading zero error

The measured source-vertex-to-proxy-surface deviation remains authoritative. When topology was actually reduced, the simplifier also applies a small scale-aware error floor:

`bounds_diagonal * removed_triangle_fraction * 0.0025`

This prevents aggressive planar/coplanar reductions from producing a zero geometric-error policy solely because all original points still lie on the same mathematical plane.

### 9. Bake UI gives visible state before synchronous authoring work

The HLOD panel now updates the status label to `HLOD bake in progress…` and yields one process frame before entering the synchronous authoring bake. This does not make Godot mesh/resource APIs artificially worker-threaded, but it makes the operation explicit instead of appearing dead before the first editor redraw.

## Runtime architecture intentionally preserved

No redesign was made to the production HLOD selection/streaming model. Existing P12 behavior remains intact:

- packed hierarchy decoding;
- parent/child topology;
- generation/list-driven transitions;
- active/loading/retiring state;
- package streaming;
- instance/node detail suppression policy;
- direct-RD publication.

## Validation performed

- `clang++ -fsyntax-only`, debug/tools-style defines:
  - `terrain_3d_hlod_builder.cpp`
  - `terrain_3d_hlod_manifest.cpp`
- `clang++ -fsyntax-only`, release-style defines for the same files.
- Existing full `tools/verify_*.py` suite: all scripts pass.
- New `tools/verify_hlod_system_patch.py`: PASS.
- P12 HLOD/node-streaming verifier: PASS.
- Systems-C style verifier: PASS.
- Systems-C ownership verifier: PASS.

The only C++ diagnostics are the pre-existing generated `godot-cpp` `INT64_MIN` literal warnings.

A runnable Godot 4.7.2 editor binary is not included in the supplied project archive, so an actual editor HLOD bake/preview smoke test could not be executed in this environment.
