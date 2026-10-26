# Wave D (CS07-CS08) - editor invalidation and acceptance tooling

## Implemented in the editor owner

- `Terrain3DRenderCoordinator::onTerrainMapsEdited()` calls the existing MegaVT dirty path, then records an independent composed-surface dirty event. No second `claimIncrementalCookHot()` and no calls to RenderingDevice in the edit callback.
- `Terrain3DRenderCoordinator::serviceAuthoringUpdateCold()` watches the existing material-source and terrain-topology generation counters. A change invalidates every present region and forgets removed regions. It never recooks material pages during frame updates.
- `forceRebakeVirtualTexturesCold()` invalidates composed-surface region revisions for the explicit full-rebuild action.
- The bounded owner-thread journal maintains revision, fingerprint, topology generation, mip0 page mask and mip closure per region. Edits expand by **2 control texels + 4 composed-gutter texels**, including neighboring regions. Tickets reject out-of-order completions; the source MegaVT's cook completion cannot clear composed dirty state.
- `terrain_3d_composed_surface_recook_bridge.cpp` maps the claimed dirty closure to exact mip/page flags for Wave B's `t3d_cs_recook_incremental_cold()`. It refuses to invoke the cooker while canonical parity is unaccepted, before a ticket is valid, or when the requested page list fails to cover every affected page. The caller still owns the atomic sidecar commit before acknowledging an edit ticket.
- Both normal renderer diagnostics (`composed_surface`) and editor force-rebake status report `composed_dirty_regions`, `composed_dirty_edit_events`, `composed_source_invalidations`, `composed_stale_discarded`, `composed_accepted_recooks`, `composed_removed_regions`, and explicit disabled/parity/legacy-retirement states. Counters are actual journal counters; `gpu_counters_available=false` explicitly marks unavailable GPU telemetry rather than inventing values.

## CS08 implementation

- `analyze_wave_d_acceptance.py` compares matched real Vulkan captures using p50/p95/p99 for material, frame, uploads and requested optional counters. Its output distinguishes absent pairs, insufficient measurements, failing timing gates, and *timing-only* passes.
- `terrain_3d_composed_surface_acceptance.h` rejects retirement of the legacy middle composite until material parity, edit-to-page publication, all-middle coverage, pass consistency, low-budget fallback, Vulkan A/B and long-run resource stability have **all** been demonstrated. There is currently no path setting these evidence flags to true.
- `test_cs07_editor_journal.cpp` covers bordering regions, parent closure, repeated edits, material changes, stale callbacks, deletion and invalid requests. `test_cs07_recook_bridge.cpp` covers exact dirty-page flag generation and refusal to cook before parity.
- `test_cs08_retirement.cpp` covers default-disabled profiles and 256, 512, 1024 and 2048 MiB page capacities.
- `test_cs08_acceptance.py` covers CSV validation, unmatched configurations, incomplete data, minimum sample counts and the timing gate.

## Remaining blockers carried from prior waves

**This is a partial Wave D implementation, not final composed VT activation.** CS02 canonical compositor parity is still missing. Wave C's GPU owner is not wired to a live `Terrain3DSharedVTRuntime` GPU completion/descriptor authority; therefore there is not yet an accepted render-owner recook or invalidation publication path. The independent journal intentionally does not mark an existing composed page resident/valid or replace the middle atlas. The cold recook bridge is implemented but does not yet spawn an editor worker job because no canonical evaluation producer is available; this avoids packaging incorrect pages.

There is no full SCons source registration in the supplied snapshot. The journal and recook bridge are new translation units and **must be added to the editor-only source set** in the consuming repository before building; do not add to the runtime template. The new acceptance header and diagnostic scripts are portable and header/script-only. This environment does not have the Godot + Vulkan editor or glslang tools required for a full enabled build or measured GPU passes.

## Acceptance matrix: not run on Vulkan

- Editor paint/undo/save/reload/region delete visual tests: not run.
- Authoritative composed-page generation and validation: blocked by CS02 parity.
- 256/512/1024/2048 budget contract: standalone tests available.
- BC1/BC5/BC7 GPU sampling and Forward+ color/DNR/depth consistency: not run.
- 10,000 unchanged GPU frames and p50/p95/p99 performance uplift: no captures provided.
- Legacy middle atlas removal: intentionally not done.

## Test commands

From repo snapshot root:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -Isrc src/authoring/vt/integration/terrain_3d_composed_surface_edit_journal.cpp src/diagnostics/tests/composed_surface/test_cs07_editor_journal.cpp -o /tmp/cs07 && /tmp/cs07
clang++ -std=c++17 -DT3D_EDITOR_NATIVE -Wall -Wextra -Werror -Isrc src/authoring/vt/integration/terrain_3d_composed_surface_edit_journal.cpp src/authoring/vt/integration/terrain_3d_composed_surface_recook_bridge.cpp src/authoring/vt/build/terrain_3d_composed_surface_cooker.cpp src/diagnostics/tests/composed_surface/test_cs07_recook_bridge.cpp -o /tmp/cs07b && /tmp/cs07b
clang++ -std=c++17 -DT3D_RUNTIME_NATIVE -Wall -Wextra -Werror -Isrc src/diagnostics/tests/composed_surface/test_cs08_retirement.cpp -o /tmp/cs08 && /tmp/cs08
python src/diagnostics/tests/composed_surface/test_cs08_acceptance.py
python src/diagnostics/tests/composed_surface/analyze_wave_d_acceptance.py capture.csv --output acceptance.json
```

The acceptance script's `PASS_TIMING_GATES_ONLY` is *not* permission to enable composed shading or retire legacy resources: visual parity and GPU/descriptor correctness require separate verification.
