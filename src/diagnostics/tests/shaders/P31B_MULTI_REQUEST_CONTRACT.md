# P31b experimental: complete multi-request single-writer feedback

**Not activated by the renderer.** This implementation establishes a version-3 compute
producer, a strict CPU reader, and an overflow-safe residency-admission gate.
It is not a replacement for the version-2 production coarse feedback pipeline.

## Input ownership (required for subsequent GPU integration)

Binding | Type | Producer obligation
---|---|---
0 | `usampler2DArray`, R32_UINT, viewport dimensions, 1-8 layers | Every valid virtual-page demand contributed by the depth-tested visible surface at each pixel, including base/overlay and mip demands, with deterministic per-pixel order
1 | `usampler2D`, R32_UINT, viewport dimensions | Exact count of requests recorded for that pixel (0 = no page requests); a count exceeding the allocated layers marks the capture incomplete
2 | `std430` storage buffer | Separately allocated version-3 coarse-feedback grid, **not** the version-2 four-slot GPU feedback buffer

- A producer must publish both input textures after depth/visibility and material
  resolution, with barriers before compute. This is not implemented in multi-source VT feedback.
- Every fragment-producing family (terrain near/middle/far, world meshes,
  instances, and any transparent contributors required by the residency policy)
  must participate. Recording only the dominant material is insufficient.
- Each compute invocation owns one coarse grid cell (8x8 dispatch topology).
  It traverses every pixel in that cell and all declared request layers.
- Cells are accepted only if they cover at most **256 pixels**, contain at most
  **4 distinct page IDs**, have no invalid declared page, and no pixel asks for
  more layers than the source provides. Any violated condition sets overflow.
- CPU preflights **all** cells. One overflow invalidates the complete version-3
  capture before any requests are admitted. Continue using version-2 feedback
  and the existing prediction/residency pipeline. Never merge a partial v3
  capture into the streaming reducer.
- Retain the existing feedback header identity: slot, frame, feedback epoch,
  database generation, device generation, and viewport key. Version is **3**.
  Slots in version 3 are temporal slots, not additional page identities.

## Output format

The 40-byte header is followed by `grid_width * grid_height` records, each
`9 * sizeof(uint32_t)`: four virtual-page IDs, their four exact occurrence
counts, and an overflow word (0 = complete, nonzero = invalid). The slot size
must be exactly `40 + grid_width * grid_height * 36` bytes. The four-slot
storage budget is four times this amount. Unlike version-2, the header's
`word_count` describes the **data words**, i.e. `grid_width * grid_height * 9`.

The `Terrain3DVTFeedbackReducer::reduceMulti()` method consumes the validated
records, preserving page frequency and the existing parent-promotion/priority
policy. `t3d_vt_feedback_multi_reduce_capture()` preflights the header and slot
length, then invokes the strict reducer without hot-path allocations.

## Resource cost and activation conditions

At 240x136: **1,175,080 bytes per readback slot**, 4,700,320 bytes for the
four-slot buffer. At 1920x1080 with eight source layers: page-ID input consumes
66,355,200 bytes and count input another 8,294,400 bytes before GPU readback
or transitions. At 128x72 the v3 readback slot is 331,816 bytes.

The existing per-frame readback limit and available GPU memory must admit a
version-3 layout. The grid must be dense enough that the largest pixel footprint
is <=256; 32x24 at 1920x1080 is **ineligible**, even though its smaller output
buffer fits a low readback budget. Source allocations and capture/shader
variants must remain cold-owned and explicit.

### Required remaining work

1. Build the depth-tested request-count and page-ID raster output for *every*
   active material-producing family, including alpha/transparent cases.
2. Integrate source RID lifetime, shader catalog, descriptors, dispatch,
   barriers, slot readback, and publication epochs through the VT GPU owner.
3. On a complete capture, compare version-3 page demand and residency against
   version-2 during motion, teleports, dense material transitions, alpha test,
   cold cache, viewport resizing, and device restart. Reject partial captures.
4. Require improved median/p95/p99 total GPU+readback+CPU cost and no loss of
   required page demand or visual detail before replacing raster feedback.

No new runtime flag is defined here because this implementation has no complete
live source producer. The shipping path remains unchanged by design.
