# Wave H / CS12 — Source V3 pack sampling bridge

**Scope:** cold/editor-only, source VT V3 package reader adapter for the static composed-surface producer introduced in CS11. The shipping Sparse VT V3 ABI, renderer descriptors, runtime streamer and composed result sidecar are unchanged.

## Added

- `terrain_3d_composed_surface_extended_pack_sampler.{h,cpp}` consumes the existing **validated** `t3dVTPackExtendedView_t` (from `t3d_vt_pack_open_extended_view`). It resolves stable terrain material IDs via sorted package keys, allocation-specific ALBEDO/PBR/HEIGHT channel directories, regular physical pages, and horizontally packed tail mips. It reconstructs mip0 pixel extents from the tail metadata, preserving 64x64 tail-only profiles.
- Bounded cold decode cache: caller reserves `nSlots * 136 * 136 * 4` bytes, maximum 32 slots (~2.26 MiB). Per-payload CRC checks, cluster/offset bounds and invalid channel/coordinate failure. No per-fragment heap allocation. Cache entries become valid only after successful decoding. ZSTD outer-compressed clusters fail closed; source packet must remain mapped for the entire cook.
- `terrain_3d_composed_surface_extended_godot_decode.{h,cpp}` is a cold `Image` adapter for BC1, BC7, BC5, R8, returning 136x136 RGBA8. It uses Godot's available texture decompressors and can return failure if a codec is unavailable. Authoring only; never create/decompress images on the render thread.
- `t3dCompSurfProducerBridge_t` preserves the original page-footprint and control-map reader contexts while replacing the CS11 source-sample callback. Only filter modes **1** (linear/trilinear) and **3** (nearest) are admitted. Filter modes 0/2 (anisotropic) are not accepted by this producer. Float UVs, explicit derivative footprints and mip bias are passed into the source sampler; albedo is decoded from sRGB **after** filtering.

## Cold integration sequence

1. Open the complete frozen source `.t3sp` package with `t3d_comp_surf_open_source_pack`, which calls `t3d_vt_pack_open_extended_view` (not metadata-only); keep backing bytes and the output view valid for the entire cook.
2. Resolve each logical terrain material's stable key using `t3d_comp_surf_find_material_allocation`, then set the CS11 material layer's `m_nAllocation`. Reject missing keys/materials.
3. Supply a caller-owned decoded page cache to `t3d_comp_surf_open_source_pack` with `t3d_comp_surf_decode_godot_image` (or an equivalent validated BC decoder). Its sampler remains valid while the view and source bytes remain alive.
4. Configure CS11's existing page/control callbacks, layer data and supported filtering; call `t3d_comp_surf_bind_static_source` with a persistent bridge object.
5. After external material reference comparisons have accepted the subset, the existing CS11 cook-source/cooker entrypoints can consume this bridge. **CS12 does not set `m_bCanonicalMaterialParityAccepted`.** The source package content signature, terrain edit revision and material-definition fingerprints must be included in the composition revision by the authoring controller.

## Validation performed

- Standalone C++17 `-Wall -Wextra -Werror` and AddressSanitizer/UndefinedBehaviorSanitizer on CS12 pack-sample fixture: regular pages, low-resolution tail-only and regular-to-tail traversal, a decoded tile cache, CRC corruption rejection, stable-key lookup, repeat UVs, unsupported filtering, malformed float UVs, and preservation of the source producer's page/control callback context. The test intentionally uses **minimal POD stubs** for Godot-dependent Terrain3D headers and a deterministic test block decoder. It does **not** establish compiled Godot image codec coverage or real V3 package open/validation.
- CS11/CS06 dormant shader checks and CS00/CS02 294 variant topology/10,000 numerical reference cases remain green.

## Open gates

- No bundled real `.t3sp` sample package was supplied for live decode comparison; source-format decoder fidelity and Vulkan BC sampling parity remain unverified.
- Hardware anisotropic filtering and exact GPU virtual-trilinear residency/fallback semantics are outside CS12's supported subset. The offline source sampler has all pages available, but shader fallback decisions must be compared against captured runtime behavior.
- Native editor SCons source-set registration and integration into the authoring controller require the consuming full build root (not present in the uploaded source archive). Godot 4.7 generated headers and Vulkan tools were not available for a full native/engine compile.
- Composed raster stays default-disabled; the legacy middle composite remains intact.
