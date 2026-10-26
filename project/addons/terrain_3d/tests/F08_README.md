# Terrain3D foliage integration (F08)

## FG00 panel regression baseline

`foliage_panel_integration_test.gd` exercises the actual panel through a test-only
subclass. It records desired-invariant PASS/FAIL results for FOL-01 through FOL-10
(including cancel/owner reservation), FOL-19 and FOL-22. Existing F08 assertions
are unchanged. Expected regressions remain FAIL; they are not counted as passes.
No runtime tests were executed for FG00: validation was restricted to compilation.

For a future explicit editor run, prepare a disposable scene with exactly two
Terrain3D nodes marked `terrain3d_foliage_fg00_fixture = true`. Give both regions
`(-1, 0)` and `(0, 0)`, flat height and material-0 control maps, two registered
texture assets and one registered mesh asset. Assign presets with distinct stable
UIDs, one enabled material-0 definition and one enabled mesh-0 rule; use density
0.001, spacing 0, seed 43721 and instance limits of at least 1000. Give each terrain
a distinct directory under `user://terrain3d_fg00/`. Ensure these directories exist
and no pending foliage recovery journal exists. Use disposable resource copies.

Run the integration EditorScript explicitly with File > Run. It restores original
instance pages, presets, height sample and texture slots, intercepts successful
publication to avoid registering undo actions, and injects staging/restore errors
only in its subclass. It refuses to touch a pre-existing pending journal. The JSON
report at `user://terrain3d_fg00_baseline.json` includes ledger/owner/page hashes,
raster statistics, synchronous call time and journal bytes. Unavailable full-frame
timings and total native I/O are null, not invented measurements. This is an editor
baseline harness, not whole-pipeline acceptance or crash/disk-full coverage.

Compilation only (does not run either EditorScript):

```sh
python tools/compile_foliage_fg00.py --godot /path/to/matching/custom/godot
```

Without `--godot`, only the Python runner is compiled; GDScript is reported
`NOT_RUN`. The initial unmeasured baseline is `qa/foliage_fg00/baseline.json`.

## Executable editor acceptance fixture

Open a **disposable** Godot 4.7.2 test project with this extension compiled and loaded. Create a scene with a Terrain3D node and two horizontally adjacent 128 m regions; populate representative height/control maps with a terrain hole, slopes, and blended material IDs. Add three registered terrain texture assets and foliage mesh assets, then assign a `Terrain3DFoliagePreset` with three enabled material definitions to the Terrain3D `foliage_profile` property. Give the mesh rules nonzero densities and distinct stable UIDs. Add at least one **manually placed** instance and save the scene. Set Terrain3D node metadata `terrain3d_foliage_f08_fixture = true`. Work only on this disposable fixture, never production terrain.

Open `res://addons/terrain_3d/tests/foliage_f08_editor_test.gd` and select **File > Run** in Godot's script editor. The `EditorScript` will compile preset rules into the native generator and check:

- Repeatable output for a fixed seed and reversed region order.
- Nonempty ownership-tagged batch transfer (covers the Godot Array copy bug corrected in F08).
- Cancellation does not publish instances.
- Invalid owner replacement is rejected without mutating instance pages.
- Existing manually placed transforms, colors, and behavior flags survive regeneration.
- Region snapshots reload with byte-identical encoded instance pages.
- Generated-owner removal still works with a removed mesh slot.
- The original instance pages are restored on completion, including failure paths.

Results appear in Godot's Output/Errors panels as `Terrain3D F08: N passed, M failed`.

## Additional acceptance runs in Godot

1. Use the Foliage Generation tool to Generate, Regenerate, Clear and Undo/Redo, then save the scene and reload it. Verify owners and placements persist and manual instances survive.
2. Change terrain blend coverage, slopes, holes and material exclusions independently; regenerate and check placements follow the expected constraints. Check positions crossing the common edge have no duplicate placements, and spacing persists across both regions.
3. Cancel midway through a large generation and verify the before-page bytes have not changed.
4. For failure recovery, run on a copy of the fixture, deliberately deny writes to the configured data directory before committing. Restore permission, reopen the Tooling window, and verify the recovery journal is retained until successful restoration. Do not simulate failures by killing the editor while working on important data.
5. Observe the editor process' peak working set during a large bake, and the runtime process' resident memory after loading the saved fixture. Record these **separately** against the editor scratch budget and runtime 512 MiB target.
6. Verify generated instances appear in the usual Terrain3D GPU-driven rendering, frustum/occlusion culling and instance streaming; compare draw counts and instance visibility to manually placed meshes.
7. Check narrow and wide Tooling window layouts, resource picker types, progress and cancellation, and editor plugin reload.

## Required commands

Run Godot GDScript/editor import with the compiled native extension in the project:

```sh
godot --headless --editor --path project --quit --verbose
```

The C++ source inventory is checked with `python tools/verify_source_tree.py`. The C++ source files are compiled with the project's Clang/MSVC C++17 flags and dependency revisions. The native verification in the patch report is **syntax compilation**, not a linked extension build.

## Environment restriction in the supplied workspace

The supplied workspace does not include an executable Godot editor or a linked GDExtension for Linux; the editor acceptance tests therefore ship as executable-in-Godot code and have **not** been executed here. They must not be represented as passed until they are run in the actual editor and renderer. The additional visual, GPU-streaming and memory checks also require a live Godot instance.
