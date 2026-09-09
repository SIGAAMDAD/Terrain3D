# Terrain3D HLOD System Audit — 2026-09-08

**Status:** investigation only. No HLOD implementation file was modified as part of this work.

## Executive result

The HLOD subsystem is not one single broken function. The runtime hierarchy/streaming path is substantially implemented and the P12 structural verifier passes, but the authoring/bake layer has several concrete defects. The most severe bake scalability problem is the instancer-source construction strategy: it expands every source mesh once per placed instance into a merged `SurfaceTool` before simplification. Dense foliage/rock populations can therefore turn a modest source mesh into a huge temporary geometry image before any reduction occurs.

The startup window bug is also direct and unambiguous: the editor plugin creates and adds the `HLODPanel` without first hiding it, while the regional lightmap panel explicitly initializes hidden. `HLODPanel._ready()` builds the window but never calls `hide()`, so the `Window` can become visible during plugin initialization instead of only when `open()` calls `popup()`.

## Findings

### H0 — HIGH — HLOD window is not initialized hidden

`editor_plugin.gd` constructs `HLODPanel`, assigns its plugin/baker, and immediately `add_child()`s it. There is no `hlod_panel.visible = false` assignment.

`hlod_panel.gd::_ready()` configures the `Window` and connects `close_requested` to `hide`, but does not call `hide()`.

The adjacent regional lightmap panel explicitly sets `visible = false` before adding it and also calls `hide()` from `_ready()`. HLOD does neither. This directly explains the unwanted initialization popup.

### H1 — HIGH — Dense instancer baking materializes full per-instance geometry before simplification

`Terrain3DHLODBuilder::buildInstancerSource()` walks every supported mesh surface, every instance cell, and every instance transform, then calls:

```cpp
tool->append_from( mesh, surface, cell.getTransform( transform_index ) );
```

for each individual instance. It indexes and commits the fully expanded geometry only after that duplication. `source_triangles` correspondingly scales by source triangles × instance count.

The GDScript hierarchy builder then merges those source meshes again at cluster level before calling the native simplifier.

There is no chunked simplification, spatial batching limit, geometry-count ceiling, or streaming reduction stage in this path. For dense procedural instance populations, temporary CPU memory and bake time scale directly with the expanded geometry rather than the final proxy. This is the strongest source-level explanation for HLOD baking hanging, exhausting memory, or appearing to make no progress on instance-heavy regions.

### H2 — MEDIUM/HIGH — Configured manifest path is ignored by bake and clear

`validate_generated()` honors `terrain.hlod_manifest_path` and falls back to the default only when it is empty.

`bake()` does not. It always publishes to:

```text
<data_directory>/terrain3d_hlod_manifest.tres
```

and then overwrites `terrain.hlod_manifest_path` with that location.

`clear_generated()` likewise removes only the default manifest path. A configured custom manifest can therefore be ignored during a bake and can survive a clear operation.

### H3 — MEDIUM/HIGH — Validation does not prove the packed runtime hierarchy is usable

The baker compiles `manifest.runtime_blob`, and the production streamer first requires that packed blob and decodes it through `Terrain3DHLODRuntimeCodec`.

`validate_generated()` checks manifest format and individual package metadata, but it does not check that `runtime_blob` is present or decodable.

In editor-authoring mode the native streamer can fall back to legacy `entries` when packed-manifest compilation fails. Outside authoring mode it returns failure. The current validation UI can therefore report a manifest/package set as valid while the shipping runtime representation is missing or corrupt.

### H4 — MEDIUM — Validation message claims an active build-contract check it does not perform

The validation path marks the manifest invalid only when `manifest.format != 3`, yet its message says the terrain/build geometry contract does not match the active Terrain3D node.

It does not compare the manifest's `max_level` or `build_signature` against the active HLOD settings/current builder signature. Package metadata is compared to the manifest entries, so a stale but internally self-consistent old bake can pass validation after the active HLOD configuration changes.

### H5 — MEDIUM — Dirty-build signatures miss mutations inside unsaved embedded resources

Saved external resources are content-hashed using file MD5. For a pathless `Resource`, `_resource_content_signature()` falls back to:

```text
<class>:<instance_id>
```

If an embedded/pathless texture, shader, or other resource is edited in place without its instance identity changing, the HLOD source signature does not see the content mutation. Dirty-only baking can therefore reuse a stale package during the same editor session.

### H6 — LOW/QUALITY RISK — Geometric-error metric can collapse to zero on coplanar simplification

The proxy geometric-error calculation measures source-vertex distance to proxy triangles. A planar/coplanar simplification can preserve all source points on the proxy surface and measure zero error despite a major topology reduction. The runtime codec clamps serialized error to at least `0.001`, so exact zero does not survive into the packed hierarchy, but the metric can still materially underestimate perceptual/silhouette error. This is a quality/LOD-policy risk rather than a demonstrated bake failure.

## What does *not* look broken in the audited path

The fixed `REGION_SIZE = 128` assumption is consistent with this fork's current 128 m / 1 m specialization and native static limits; it is not treated as a defect here.

`SurfaceTool.generate_lod(1.0, target_indices)` matches the generated Godot API's parameter order. There is no swapped-argument simplifier bug in this code.

The simplifier indexes source geometry, generates a reduced index stream, falls back when the generated stream is invalid, compacts supported vertex attributes, and computes geometric error. No source-level unconditional simplification failure was found there.

The runtime streamer has a packed hierarchy decode path, parent/child topology, source policies/detail suppression, asynchronous package loading, active/retiring package state, and direct-RD snapshot publication. The existing P12 structural verifier passes.

The exclusions for scripts/behavior, collision, animation/skeletons, audio/particles, navigation, lights, MultiMesh, blend shapes, material overlays, non-white per-instance tint, and custom/skinned attributes are explicit eligibility constraints rather than accidental failures.

## Recommended HLOD patch sequence for the next pass

1. **H0:** initialize `HLODPanel` hidden in both plugin construction and `_ready()` for lifecycle safety.
2. **H1:** centralize manifest-path resolution and use it consistently for validate/bake/clear.
3. **H2:** make validation decode/verify the packed `runtime_blob` and compare the active build signature/max level.
4. **H3:** replace full instancer geometry explosion with bounded spatial batches/chunk simplification, then merge already-reduced chunks into hierarchy clusters.
5. **H4:** recursively content-sign pathless embedded Resources instead of hashing only their instance IDs.
6. **H5:** harden geometric-error/coverage metrics so aggressive planar reduction cannot report a misleading zero-error proxy.

The first three are small correctness patches. The instancer bake redesign is the substantial one and should be implemented separately with explicit peak-memory/triangle-count instrumentation rather than hidden behind another ad hoc limit.
