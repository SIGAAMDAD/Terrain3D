# CS10 (CS10): shared VT owner and GPU-completion publication

## Added integration

- `Terrain3DSharedVTRuntime` now retains the composed page GPU owner and
  fixed-capacity CPU cache. Cold opt-in is explicit; the caller supplies a
  validated `.t3sc` view, *expected* source fingerprint, source lifetime,
  page slots, request queue, pending transactions and hash-table backing.
  No live composed pool is allocated by ordinary VT startup.
- Composed descriptors are acquired from the shared VT owner; source VT V3
  descriptors are unchanged. `beginVtResourceReplacementCold` shuts down
  composed resources first, invalidating their RID dependencies in the global
  descriptor cache before releasing GPU resources. Resource-class and device
  replacement therefore cannot leave the composed owner published.
- `submitComposedPageSetHot` obtains the currently open submission epoch
  from `Terrain3DRdGlobalDescriptorCache` and refuses work without an
  *external shared-VT transfer admission*. The caller must read page bytes
  outside the render thread and retain the input until all three channels
  have been staged. All three channels share the same pending transaction.
- `Terrain3DComposedSurfaceGpu` records the submission epoch for each slot.
  `confirmAndPublishHot` and `serviceCompletedUploadsHot` refuse to publish
  until the descriptor cache's GPU-readback-completed epoch covers it.
  Retirement likewise requires a completed epoch covering the last raster
  use, supplied by the raster descriptor owner. This replaces the earlier
  caller-provided completion booleans.
- The render coordinator services completed composed uploads at the start of
  Forward+ PREPARE, after advancing the global descriptor submission epoch.
  The default-disabled state remains a cheap conditional.
- Cold page-table storage is created with a single zero-filled initial buffer
  rather than one buffer update per hash slot.

## Validated

- Clang C++17 `-Wall -Wextra -Werror`: CS01, CS03/CS04, CS05, CS07 and CS08
  standalone suites, plus new CS10 epoch/table tests.
- AddressSanitizer and UndefinedBehaviorSanitizer: CS03/CS04 and CS10.
- CS02 10,000-case reference math; existing 294 shader variant static checks;
  CS06 binding/filter contract and CS10 owner/topology static checks.
- Byte-exact Sparse VT V3 header and existing shipping shader variants.

## Still required for production activation

- A canonical source-material composer that passes CS02 full material parity;
  the current compute shader remains a fixture consuming pre-resolved inputs.
- A consuming SCons `build/source_sets.py` registration and full Godot native
  builds. This uploaded project contains only `src/` and a Godot sample project,
  not the extension build root or generated Godot C++ class headers.
- Actual composition source/job scheduling, integration with the **single**
  shared VT transfer-budget allocator, worker-thread sidecar reads, and GPU
  time-ordered raster epoch publication across Forward+ color/DNR/depth.
  The new owner API requires caller-supplied transfer admission and the last
  raster-use epoch; these are not fabricated by the owner.
- Optional shader variant authoring and descriptor recipes for 39-42 (clipmap)
  / 13-16 (world), Godot/Vulkan/spirv validation, visual parity, GPU capture
  and measured performance. Until those steps pass, keep composed shading
  disabled and retain the legacy middle composite.
