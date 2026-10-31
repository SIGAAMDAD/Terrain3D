# Wave I / CS13 — Source V3 to composed sidecar cold orchestration

**Scope:** editor/cook-time only. This stage binds the Wave H V3 source pack sampler, Wave G restricted static material evaluator, Wave B incremental BC sidecar cooker and existing atomic `.t3sc` writer through one bounded function. It does not modify the shipping VT V3 ABI or activate the composed Forward+ raster path.

## Entry point

`authoring/vt/build/terrain_3d_composed_surface_extended_cold_pipeline.{h,cpp}` implements `t3d_comp_surf_cold_cook_source_pack_cold(request, result)`.

The caller provides the complete mapped `.t3sp` bytes, sorted composed page specifications, authored material scalar layers in logical terrain material-ID order (0..count-1), existing control/page-footprint callbacks, a BC1/BC5/BC7 encoder (e.g. `t3d_comp_surf_reference_encode`), cold BC decoder (e.g. `t3d_comp_surf_decode_godot_image`), and all fixed-size scratch/output buffers. The function opens the V3 pack through the frozen pack reader, resolves stable terrain material keys to V3 allocations, preserves original callback contexts, and performs a full or incremental `.t3sc` cook.

All producer pointers and V3 pack views are kept alive through evaluation. No worker pool, periodic timer, heap-owned cache, per-frame compression or render-thread resource creation is introduced.

### Memory contract

- `m_pDecodedCache`: at least `m_nCacheSlots * T3D_COMP_SURF_DECODED_TILE_BYTES` bytes, 1–32 slots.
- `m_pCookScratch`: at least `T3D_COMP_SURF_COOK_SCRATCH_BYTES` bytes.
- `m_pOutput`: at least `t3d_comp_surf_cook_total_bytes(m_nPageCount)` bytes.
- `m_pResolvedLayers`: at least `m_nLayerCount` records; the source pack's stable-key index, not caller-supplied allocation IDs, determines their resolved allocations.
- Optional `m_pPreviousSidecar` remains immutable during incremental cooking. Output/source/previous/cache/scratch spans must not overlap.

### Revision and identity

`source_fingerprint_cold` hashes the *entire* immutable source package bytes, composition ABI, logical material parameters, filtering/mip/blend modes and the caller's nonzero terrain/material/topology revisions. Structure padding, pointers and physical allocation IDs are excluded. This is an FNV-1a content fingerprint for deterministic invalidation, **not** an adversarial authentication hash.

`m_pCurrent` must confirm all three revisions before and after the cook. Invalid or stale input fails without atomic publication. The authoring owner must serialize final file replacement and journal acknowledgment with edits. The sidecar may be published only after all channels are complete and CRC-valid. The existing journal ticket must be acknowledged **after** this function reports successful publication.

### Parity and material restrictions

The existing `m_bCanonicalMaterialParityAccepted` remains mandatory; this entrypoint **never sets it**. Only the previously supported static V3 subset is admitted. Hardware-anisotropic filtering, triplanar/auto/dual-scale/detiling and adaptive modes remain rejected by the producer. Existing detailed terrain shading remains the runtime default.

`m_pDestination` is optional. With null destination the result remains in caller-owned memory. With a path it is written through `t3d_comp_surf_write_sidecar_cold` and the existing validated atomic replace protocol.

## Validation performed

- `run_cold_pipeline.sh`: Clang C++17 `-Wall -Wextra -Werror`, ASan and UBSan on real CS11/CS12/CS03 writer/cooker source units using a deterministic test V3 pack opener and block decoder. Tests material stable-key resolution, three-channel composition, `.t3sc` CRC verification, memory output and atomic file commit, fingerprint invalidation on material changes, unchanged-page incremental reuse (zero additional material samples), stale revision rejection after evaluation, corrupted mip-tail CRC rejection, missing material, disallowed parity and aliased output spans.
- Existing CS12 sanitizer fixture and CS06/CS11 shader-static checks passed; CS00/CS02's 294-shader variant topology and 10,000 seeded numerical-reference cases passed. The earlier CS01/CS03/CS04/CS05/CS07/CS08 standalone tests also passed.

## Open gates

No real authoring `.t3sp` package or Vulkan capture was supplied; the CS13 test substitutes the source pack opener and block decoder. Real BC decoding, exact raster-to-cooker material parity, native Godot/editor SCons registration, live editor cook invocation and enabled Forward+ GPU correctness/performance remain unvalidated. The source snapshot still lacks the root build/source-sets configuration. This stage supplies the cold invocation boundary but does not auto-enable composed shading or retire the legacy composite.
