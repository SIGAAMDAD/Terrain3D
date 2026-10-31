# HOT Dirty-Range Publisher — P05/P06 Integration and Acceptance

Scope: Terrain3D C++17 HOT arena and editor diagnostics on the source baseline
`Terrain3D_HOT_Dirty_Ranges_Terrain_Side_Source.zip`.

## P05 lifecycle

- **Forward+ ABI 2:** The supplied C++ uses the engine-assigned physical slot for comparisons. The engine callback validates frame, device frame and slot, then commits only after successful publication. Failed/abandoned callbacks invalidate comparison state. The matching custom Godot engine is **not included**; this path needs live integration tests.
- **Standalone ordinary ABI 2:** `beginStandaloneHotFrame()` now activates `Terrain3DHotDirtyImage`. `publishStandaloneHotFrame()` finalizes the logical image, issues public `RenderingDevice::buffer_update()` calls only for changed byte spans, commits only on success, and rejects draws on failure. This occurs before standalone draw-list creation. The Godot public API requires a `PackedByteArray` argument, so standalone publication creates one temporary subarray per range; native Forward+ writes retain their preallocated memory path.
- **Standalone persistent ABI 2:** Uses the same dirty comparison and does not clear records. Only changed bytes are copied to the mapped frame slot. It must still perform **one whole-buffer compatibility flush when any range changed**, since the standalone synchronous path has no supplied custom engine range-flush call. No-op frames skip the flush.
- **ABI 1:** Preserved as a separately selected compatibility path. It retains record clears and whole-frame publication. A persistent-map failure no longer leaves a deferred callback pending before the map is ready.
- **Rebuild/device loss:** Context shutdown releases all mirrors. Device invalidation marks them unknown and causes pending dirty callbacks to reject dependent draws. New buffer creation reinitializes all committed-state tables against the new physical allocation.
- **Partial failure:** Any partially written GPU physical slot is marked unknown; the next successful use republishes the entire *allocated logical record* including zero/default fields. No earlier partial publication is accepted as a valid baseline.

## P06 diagnostics

`Terrain3DRdContext::getSharedArenaDiagnostics()` exports the last completed publication's `hot_dirty_blocks`, `hot_dirty_ranges`, `hot_dirty_bytes`, `hot_bytes_uploaded`, `hot_flush_bytes`, `hot_initialized_blocks`, `hot_physical_slot`, plus the current frame's producer `hot_bytes_written_cpu`, and cumulative `hot_noop_frames`, `hot_full_initializations`, `hot_recoveries`, `hot_failed_publications` and lifetime totals.

The editor's existing **Diagnostics > RenderingDevice Experiment** tree includes the HOT metrics. Byte measurements are *last transaction* or *lifetime counts*, never a cumulative duration masquerading as `ms/frame`. `hot_full_initializations` tracks whole-arena clears and is zero for the new ABI-2 implementation; `hot_initialized_blocks` reports cold physical-slot blocks.

### Verification achieved without the engine

Run `python project/addons/terrain_3d/tests/native_hot/run_hot_native_tests.py` and
`python project/addons/terrain_3d/tests/verify_hot.py`.

- C++ dirty-image unit test: cold slot, stable slot, slot rotation, failure, default transitions.
- Deterministic fuzz: 650 frames, 3 slots, overlapping writes, 14 injected failed publications.
- P05 lifecycle mock: 3 slots, no-op, partial failure followed by full logical replay, device invalidation, no allocated records, record-capacity limit.
- CPU-only mock benchmark: 1,200 trials, 8 records, p50/p95/p99 for dirty-range publication vs full-buffer clearing and copying. This is not GPU time and not a measured Godot FPS delta.
- Clang C++17 syntax checks of context and integration translation units using the supplied Godot C++ dependencies and the compile-only compatibility definition for the pinned engine enum mismatch.

### Remaining live acceptance (requires matching patched Godot 4.7.2 engine)

| Workload | Required result |
|---|---|
| ABI 2 ordinary native Forward+ | Deferred range callback advances the correct physical slot, writes only changed ranges, rejects dependent draws on failure |
| ABI 2 persistent native Forward+ | Slot identity matches map advance; only dirty mapped bytes/flush ranges become visible |
| 3 frames in flight, shadow/depth/opaque/motion | No stale or cross-pass HOT data; correct GPU draw and shadow output |
| 0 changed data after slot warmup | Zero ranges, zero transferred/flush bytes |
| Buffer regeneration or device loss | Full *active record* replay after recovery; no missing renderer data |
| Fault injection at map advance/prepare/copy/flush/complete | Either complete success or draw rejection and slot invalidation |
| Fixed vs moving camera, CPU/GPU culling, multiple quality modes | Compare baseline and candidate p50/p95/p99 bridge CPU and whole-frame time, GPU correctness; no quality regressions |
| Persistent standalone path | Whole-buffer compatibility flush only on changed frames until synchronous range-flush engine support is available |

**Not yet validated:** Full MSVC build, custom engine ABI 2 compilation, editor/headless runtime, actual GPU timestamps or FPS, or GPU image-diff correctness. Terrain3D cannot implement or measure these without the matching engine build. P03/P04 engine implementation remains outstanding.
