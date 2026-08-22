# Terrain3D GDScript Modernization Audit

Target: Godot 4.7.1
Branch: `experiment/rendering-device-editor`

## Scope

This pass updates the Terrain3D addon/editor GDScript and demo scripts to match the current native Terrain3D API and the Godot 4.7 editor/runtime API used by the RenderingDevice experiment. It deliberately removes permissive compatibility behavior where the native API is now part of the repository contract, while preserving dynamic access only where the target object is editor-only or Godot does not expose the required lightmap payload publicly.

The project contains 74 GDScript files. The audit covered all of them, including addon/editor tooling, regional lightmapping, HLOD tooling, Region Workbench, utility scripts, and the demo runtime scripts.

## Changes

### Typed editor contracts

The lazy-loaded editor base scripts are now actual abstract contracts instead of inert implementations whose methods silently returned defaults or `pass`ed:

- `Terrain3DEditorPluginBase`
- `Terrain3DExecutionModeMenuBase`
- `Terrain3DRuntimeProfilerWindowBase`

The concrete implementations were checked against the abstract methods and argument counts. The editor plugin contract now includes the HLOD conversion entry point used by `hlod_panel.gd`.

### Native Terrain3D contract

The editor no longer probes guaranteed native Terrain3D methods with `has_method()` and string-based `call()` before binding the UI. It calls the current typed native methods directly:

- `get_native_api_revision()`
- `get_native_build_id()`

The current lightmap/HLOD/editor paths likewise use typed Terrain3D and `Terrain3DRegionLighting` properties where those properties are part of the native binding surface.

String-based deferred self-calls were replaced with typed `Callable.call_deferred()` calls throughout the addon.

### Godot 4.7 editor API

- Explicitly selects editor viewport index `0` with `EditorInterface.get_editor_viewport_3d(0)`.
- Retains current `EditorDock`-based Workbench docking from the previous modernization pass.
- Audited the 4.7 input-device-ID change; no script compares mouse/keyboard device IDs against `0`.
- Audited the 4.7 inherited typed-return behavior; every function in the project now has an explicit return annotation, including editor callbacks and overrides.
- Audited the 4.7 RichTextLabel image-unit API changes; no removed `UPDATE_WIDTH_IN_PERCENT`, `width_in_percent`, or `height_in_percent` usage remains.

### Regional lightmap tooling

`region_lightmap_panel.gd` and `region_lightmap_baker.gd` now use typed Terrain3D/Terrain3DRegionLighting access instead of dynamic property names for the repository-owned API.

Three Godot-internal calls remain deliberately isolated behind `StringName` constants:

- `LightmapGI` editor `_bake`
- `LightmapGIData._get_user_data`
- `LightmapGIData._get_probe_data`

Terrain3D needs the baked user's UV scale, texture slice, sub-instance data, and probe payload to serialize regional lightmap packages. The public `LightmapGIData` API exposes user count/path and the atlas textures but does not expose the complete serialized user/probe payload. The baker now checks those internal methods before use and fails with an explicit error if they disappear or return malformed data.

### Region mover

The old region mover was replaced with a transactional implementation.

The previous script assumed a fixed `-16..15` region grid and directly renamed `terrain3d*.res` files. It did not account for runtime pages, manifests, HLOD, or world-position-dependent regional lighting.

The new mover:

1. validates every source and destination using `Terrain3DData.get_region_map_index()`;
2. preflights target collisions;
3. unloads the active data directory before filesystem mutation;
4. performs a two-phase source -> temporary -> destination rename;
5. rolls back staged/published files after rename failure;
6. invalidates old/new `.t3dpage` files;
7. invalidates far-atlas, runtime-world, cook, and HLOD manifests whose keys embed the old locations;
8. reloads the data directory;
9. clears baked `Terrain3DRegionLighting` from moved regions because the world-space bake is no longer valid;
10. saves the normalized moved regions and reloads the HLOD manifest.

### Demo scripts

`RuntimeNavigationBaker.gd` contained two real type-contract defects:

- `enter_cost: float` used a `bool` setter argument;
- `travel_cost: float` used a `bool` setter argument.

Both setters now take `float`, `_ready()` has an explicit `void` return annotation, and object/null tests are explicit.

`DemoScene.gd` now guards the optional Sky3D script with `ResourceLoader.exists()`. Its `EditorInterface` access remains through `Engine.has_singleton()` / `Engine.get_singleton()` intentionally because the demo script also exists in runtime/exported builds while `EditorInterface` is editor-only.

## Validation

### Script structure

- 74 GDScript files scanned.
- 0 functions without explicit return annotations.
- 0 duplicate `class_name` declarations.
- 0 lightweight delimiter/string structural errors.
- 0 exported property/setter type mismatches.
- 0 `@export_tool_button` references to missing functions.
- Abstract implementation checks:
  - `Terrain3DEditorPluginBase`: 19 methods, 0 missing/arity mismatches.
  - `Terrain3DExecutionModeMenuBase`: 1 method, 0 missing/arity mismatches.
  - `Terrain3DRuntimeProfilerWindowBase`: 3 methods, 0 missing/arity mismatches.

### Resource references

- 73 `.tscn`/`.tres` external resource paths checked; all resolve.
- 52 literal GDScript `load()`/`preload()` references checked.
- The only absent literal path is the optional demo dependency `res://addons/sky_3d/src/Sky3D.gd`; the demo now checks `ResourceLoader.exists()` before loading it.

### Native binding checks

The native source contains bindings for every newly typed Terrain3D/Terrain3DRegionLighting method/property used by this pass, including:

- native API revision/build ID;
- region-map indexing and HLOD reload;
- all regional lightmap configuration properties;
- all `Terrain3DRegionLighting` package metadata fields touched by the baker/Workbench.

### Stale API search

No remaining occurrences were found for the audited obsolete/breaking patterns:

- `yield()`;
- `setget`;
- Godot 3 `Pool*Array` types;
- `File.new()` / `Directory.new()`;
- `.instance()`;
- string-based deferred self-calls;
- old editor dock APIs;
- zero-valued mouse/keyboard device assumptions;
- removed RichTextLabel image-unit names;
- zero-argument `EditorInterface.get_editor_viewport_3d()`.

`setting_metadata.json` parses successfully and `git diff --check` is clean.

## Runtime/editor validation boundary

The repository declares Godot 4.7 in `project.godot`. Current Godot 4.7 documentation was used for the migration/API checks.

This environment does not contain a Godot 4.7.1 editor executable, and its normal shell cannot resolve external hosts, so the updated scripts could not be imported by the real 4.7.1 GDScript parser here. The remaining target-machine validation is therefore:

- project import under Godot 4.7.1;
- plugin enable/disable;
- asset/editor dock smoke test;
- Region Workbench create/cook/commit/abandon;
- regional lightmap bake/finalize;
- HLOD bake/panel;
- region-mover test on a disposable copied data directory;
- RenderingServer / RD Direct / RD Hybrid switching;
- runtime navigation demo bake.
