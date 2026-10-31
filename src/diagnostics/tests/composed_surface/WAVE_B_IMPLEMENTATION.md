# Wave B — CS03/CS04 implementation and integration contract

## Source delivered

- CS03 `terrain_3d_composed_surface_cooker.{h,cpp}`: deterministic cold page-set cooker of 136x136 RGBA8 scratch pages, BC1/BC5/BC7 page-set channels, CRC32, per-page fingerprints and fixed-size `.t3sc` layout. Supports incremental rewrites, retaining validated untouched page payloads when the identity, generation, revision and source fingerprint match.
- CS03 `terrain_3d_composed_surface_encoder_adapter.h`: adapter to the existing V3 reference BC encoder, preserving the frozen source-VT pack. The BC encoder is invoked at cook-time only.
- CS03 `terrain_3d_composed_surface_sidecar_writer.{h,cpp}`: validate, write and atomically replace offline sidecars; never publish malformed/partially written files.
- CS03 `terrain_3d_composed_surface_dirty.h`: snapshot the existing MegaVT dirty mip0 masks and ancestor closure, and check source generation/fingerprint again before publication. Does not duplicate the existing MegaVT claim.
- CS04 `terrain_3d_composed_surface_mapped_file.{h,cpp}`: open an independently versioned sidecar via OS read-only demand paging without bringing the whole compressed file into a heap buffer; reject wrong source fingerprints or unsorted page directories.
- CS04 `terrain_3d_composed_surface_cache.{h,cpp}`: read by compact page ordinal with CRC verification, bounded per-owner queue, request deduplication and priority, shared-transfer-budget admission hook, generation-tagged jobs, complete-channel GPU submission gate, explicit retirement acknowledgment before physical-slot reuse. All persistent CPU slot/queue arrays are supplied by a cold owner.

## Integration status and CS02 gate

CS02's full raster/source VT semantic equivalence is NOT complete in the uploaded Wave A implementation. The Wave B composer therefore accepts a canonical material-evaluator callback and requires `m_bCanonicalMaterialParityAccepted`; it **does not implement that evaluator**. A consumer must supply real source-VT sampling, control/slope/rotation/height evaluation, gutter/neighbor and mip outputs, and pass the CS02 parity gate. Mock test pixels are never shipping terrain material data. Runtime source VT paths, physical GPU RIDs and shader descriptors are unchanged.

The renderer's full native build registration root (`build/source_sets.py`, `SConstruct`) is not present in the supplied `project(3).zip` baseline. Register new `.cpp` files in the editor/runtime source sets when applying to the full repository. A render-owner integration adapter to the shared VT transfer budget and physical page arrays belongs to CS05; `claimNextHot` currently accepts the caller's transfer-budget admission result and owns no GPU RIDs.

The OS mapping owner must outlive every sidecar view and cache. Calls that mutate `Terrain3DComposedSurfaceCache` state must be serialized by its owner; no built-in thread-safe producer queue or worker pool is introduced. The editor performs incremental recomposition on cold/debounced jobs only. `confirmEvictionHot` is callable only after GPU mapping retirement has been confirmed; `completeUploadHot` only after all three BC channels have been uploaded and the submission is confirmed. No per-frame rebuild/BC encoding or expensive global page-table scan.

## Tests

Run from the repository root:

```
clang++ -std=c++17 -Isrc -Wall -Wextra -Werror -pthread \
  src/diagnostics/tests/composed_surface/test.cpp \
  src/authoring/vt/build/terrain_3d_composed_surface_cooker.cpp \
  src/authoring/vt/io/terrain_3d_composed_surface_sidecar_writer.cpp \
  src/rendering/rd/vt/runtime/terrain_3d_composed_surface_cache.cpp \
  src/rendering/rd/vt/runtime/terrain_3d_composed_surface_mapped_file.cpp \
  -o /tmp/test && /tmp/test
```

Covers deterministic full/incremental output, immutable reuse, CRC/readback corruption, file replacement and reopening, wrong fingerprints, lookup, priority and dedup, insufficient transfer budget, partial uploads, retired-slot reuse, canceled/stale device generations, and failures that leave no published file. Compression callback in the portable test is a transport stub, so standalone test success is not evidence of BC encoder image quality or Vulkan correctness.

## Pending acceptance

- Full CS02 material equivalence, including editor tint, holes, normal/ORM decoding and control material selection, before any authoritative sidecar is exported.
- Full existing BC encoder compilation and quality validation in the native editor build.
- Full SCons build registration, owner-thread wiring and shared budget integration against the complete native build root.
- Vulkan submission/fence validation and live RD descriptor integration (CS05–CS06).
- Live authoring dirty-event wiring (CS07); dirty planner exists, but no new paint-event subscription is attached.
