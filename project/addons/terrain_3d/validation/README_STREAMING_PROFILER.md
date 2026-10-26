# Terrain3D streaming stage profiler (P12)

### Inspect in the Terrain3D editor profiling window

Select a Terrain3D node, open its **Execution** dropdown, and choose
**Open Live Profiler...**. The **Streaming** tab displays all native streaming
stages in one table (calls, timed samples, mean, histogram p50/p95/p99 upper
bounds, peak, recent estimated CPU milliseconds per rendered frame and bytes).
The estimate uses call-count changes between UI refreshes and the sampled stage mean,
normalized by rendered frames over the same interval. The first refresh after
opening, resetting, or changing sampling rates shows `—` until an interval exists.
It ranks stages by this recent per-frame cost rather than cumulative lifetime calls.
Nested scopes overlap.

The editor window enables sampled instrumentation automatically while open,
and restores the prior enabled state on close or Terrain3D selection change.
The tab allows live enable/disable, sampling rates of 1/4/8/16/32 calls,
and reset. The window's normal **Reset** and **Export...** actions include
streaming statistics. No separate runtime overlay is needed. The native
profiler is global to the extension and aggregates all live Terrain3D nodes.

Manual scripting remains supported:

```gdscript
$Terrain3D.reset_streaming_profiler()
$Terrain3D.set_streaming_profiler_enabled(true, 8)
var stats: Dictionary = $Terrain3D.get_streaming_profiler_stats()
print(stats["stages"]["vt_io_read_decode"])
$Terrain3D.set_streaming_profiler_enabled(false)
```

### Repeatable capture

```text
godot --headless --path <project> -s res://addons/terrain_3d/validation/streaming_profile_capture.gd -- --scene res://world.tscn --frames 900 --warmup-frames 120 --sample-every 8 --output user://streaming_profile.json
```

The capture script warms up for 120 frames, then moves the camera across 128 m regions and teleports mid-capture. Use `--stationary` to capture the warm-residency control. It records periodic cumulative snapshots and CPU frame intervals; replay the same route / scene to compare builds. `sample_every=1` measures every invocation at higher overhead; the default is 8. The profiler is initially disabled and has no thread synchronization, heap allocation, engine calls, or clock reads when disabled; when enabled it uses relaxed atomic accumulators and a sampled monotonic clock.

### Interpreting the numbers

All stages measure **CPU elapsed time**, including waiting for disk, locks, or Godot RD submissions. The `*upload*`/`*publication*`/`*readback_submit*` stages **do not** report hardware GPU execution time. Use a Vulkan GPU timestamp/timeline capture (Intel GPA, RenderDoc or the engine's GPU profiler) to time GPU-side texture copies, buffer hazards and fragment shader atomics. `vt_feedback_service` is a CPU service time, not fragment shader atomic duration.

- `calls`: full stage invocation count, even when unsampled.
- `timed_samples`: number of samples used in time/histogram statistics.
- `total_usec_sampled`: **only timed invocations**, not a full-workload total.
- `avg_usec_sampled`: mean of sampled stage durations.
- `peak_usec_sampled`: sampled maximum.
- `p50/p95/p99_usec_upper`: approximate histogram upper bounds; bins double in size; last bin saturates at 2^23 us.
- `bytes`: instrumentation-specific accumulated physical read or submitted payload bytes (not a global sum: nested stages overlap; some stages have no byte counter).
- `Est CPU ms/frame` (editor only): recent interval estimate of CPU work per rendered frame. Cumulative native counts and exports are unchanged. Infrequently sampled stages can use their last known sampled mean until a new timed call is recorded.

Scopes include VT prediction and admission, VT worker I/O and decode, VT upload service, physical layer upload, page-table publication, feedback submission/readback/reset, region file cache read (including waits), region worker preparation, region image preparation and tracked/direct texture uploads/copies, world publication/buffer updates, aggregate forward pass preparation and global transfer budget lock admission.

The sampled percentile bins are approximate and snapshot loads are concurrent relaxed atomics. Do not add nested `total_usec_sampled` values together to compute GPU or frame time. Compare stage timing distributions and throughput across identical workloads.

### Standalone native smoke test

```text
clang++ -std=c++17 -O2 -pthread -Isrc src/diagnostics/tests/test_streaming_profiler.cpp -o /tmp/t3d_stream_profile_test
/tmp/t3d_stream_profile_test
```
