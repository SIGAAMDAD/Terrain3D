# P10 VT Streaming Index, Worker Handoff, and Retire Batching

Date: 2026-09-07

## Lineage

This patch is a selective forward-port of the P10 portion of `Terrain3D_P09_P10_P11_Performance_128m_Memory_Patched_2026-09-04.zip` onto the current repaired P01-P09 cumulative tree. P11 changes were intentionally excluded. Current P06/P07/P08/P09 fixes remain authoritative where the older combined archive differs.

## Scope

P10 removes several O(capacity) CPU scans from the virtual-texture streaming path and replaces them with fixed owner-allocated indices/lists. It also adds fixed-context worker handoff and batches page-table invalidations around the actual retired slots rather than the complete physical cache.

### O(1) stream-job indexing

`T3DVTStreamingBuffers` now owns fixed arrays for:

- virtual page -> stream job (`pPageToJob`)
- free job stack (`pFreeJobs`)
- priority-ordered queued jobs (`pQueuedJobs`)
- queue position per job (`pJobQueuePosition`)
- ready/completed jobs (`pReadyJobs`)

The streaming state tracks the corresponding live counts. Request lookup no longer scans all jobs, and job allocation pops directly from the fixed free stack. Completion discovery is driven by fixed scratch lanes, then ready uploads iterate only the ready list.

### Explicit retired-slot lists

Each VT channel now owns a fixed `pRetiredSlots` array and a live retired count. Retirement appends the slot explicitly. Finalization removes it from that list. The GPU service path therefore does not scan every physical slot to discover retirements.

Before publication mutation, VT commit now verifies that a victim can be represented in the retired list. This preserves commit atomicity when the fixed retirement capacity is exhausted.

### Contiguous page-table invalidation batching

The GPU retirement service copies only live retired slots into preallocated scratch storage, orders them by virtual page, coalesces adjacent pages, and performs one page-table `buffer_update` per contiguous run. Telemetry tracks update bytes, update calls, and retire retries.

### Readback cadence

VT request readback scheduling is decoupled from raster frequency with a four-frame cadence. Frames suppressed by this cadence are counted separately. The schedule-frame sentinel is reset on cold initialization and shutdown so a previous lifetime cannot suppress the first readback of a new runtime.

### Production worker handoff

`Terrain3DRenderWorldRuntime` now has fixed worker contexts matching its bounded stream scratch lanes. Work is handed to Godot's `WorkerThreadPool::add_native_task` without allocating a per-job owner object. Workers execute the asynchronous VT decode and publish completion through the existing scratch-lane atomics. Shutdown joins outstanding tasks before releasing runtime storage.

The WorkerThreadPool singleton is refreshed during cold initialization rather than relying only on constructor-time singleton availability.

### Cold memory image

The owner-published CPU image now reserves fixed storage for:

- page-to-job map
- free-job stack
- queued-job list
- job queue-position map
- ready-job list
- retired-slot lists for all VT channels

No hot-path dynamic container was introduced.

## ABI / capacity guards

The new stream-job and retired-slot indices are 16-bit. Contract validation therefore rejects configurations whose stream-job capacity or per-channel physical-page capacity exceeds `0xffff`. This prevents silent truncation of owner-published indices.

The P09 108-byte indirect-argument ABI remains intact through `T3D_RW_INDIRECT_BUFFER_BYTES`; the older combined P09/P10/P11 archive's stale draw-only memory accounting was not imported.

## P11 exclusion

This patch does not import P11. In particular, it does not add owner-published instance dirty ranges, P11 production instance GPU culling, or P11 motion-history range processing.

## Verification performed

- all repository `tools/verify_*.py` gates passed, including a dedicated P10 contract gate;
- standalone P10 streaming state-transition test passed;
- all 22 RenderWorld translation units passed direct C++17 syntax compilation;
- focused Clang static analysis completed without new diagnostics on streaming, VT residency, VT GPU service, and RenderWorld runtime units;
- current P06-P09 regression gates remained green.

The only compiler diagnostic observed is the pre-existing generated godot-cpp `INT64_MIN` literal warning.

## Environment limits

A full SCons build was not run because SCons is unavailable in the execution environment. The actual Godot editor/runtime scene cannot be launched here, so the implementation has not been runtime smoke-tested against the user's project scene and no performance claim is presented as benchmarked.
