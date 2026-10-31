# ST00 — Frozen `ST_REGION_V1` contract

Status: **frozen Wave 1 proposal** for the 2026-10-25 `Terrain3D_FAR_Regional_V1_Contract_Cleanup` source baseline. Production integration belongs **only to ST05**. This document and `src/diagnostics/tests/superterrain/st_region_v1_contract.{h,glslinc}` are the matching normative definitions. They are not included by production sources in ST00.

## Source truth

The existing source defines `T3D_REGION_SIZE=128`, `T3D_REGION_MAP_SIZE=32`, `T3D_REGION_SLOTS=1024`, V1 128-texel material-page interiors, four-texel gutters, 136x136 physical tiles, and **three physical channels per logical V1 allocation**. The existing C++/GLSL GPU region record is **48 bytes**; its original fields are `packed_physical_flags`, MegaVT allocation/generation/flags and `world_to_uv`, followed by FAR and COMPOSED allocation/generation. The existing FAR consumer is numeric value 3; COMPOSED is 2. `t3d_vt_composed_region_stable_key(index)` already reserves `0x5443535245470000 | index` and must be retained.

`T3D_VT_FILE_VERSION_V1=1`, `T3D_VT_V1_ABI_VERSION=2`, and `T3D_VT_REGIONAL_SOURCE_RECIPE_VERSION=3` are **current source facts**, not new version numbers. ST00 introduces **`T3D_ST_RECIPE_VERSION=1`** for the new coherent height/mask extension. The V1 COMPOSED allocation stays BC1 albedo (slot 0), BC5 normal (slot 1), BC7 PBR (slot 2). No new material slot and no extra composed runtime cache.

### Source representations to preserve

| Source | Exact source semantics | Owner/source |
| --- | --- | --- |
| GPU/CPU region height | 128x128 R16_UNORM, normalized over global **[-384, 768] meters**, `round(clamp((height + 384)/1152,0,1)*65535)` | `src/data/persistence/terrain_3d_region_codec.h`, `src/Terrain3DConstants.h` |
| Control | 128x128 `FORMAT_RF`: uint32 bit pattern carried through 32-bit float, not a numeric float conversion. Bit 2 is hole; bits 27–31 base material, 22–26 overlay, 14–21 blend, 10–13 rotation, 7–9 UV scale, bit 1 navigation, bit 0 autoshader | `src/core/utility/terrain_3d_util.h` |
| Color | Authored `FORMAT_RGBA8`; original far cook packs quantized alpha into the old index byte; canonical composed output is a separate signal | `src/rendering/surface/terrain_3d_far_regional_cooker.cpp` |
| Old FAR byte | `[7] solid`, `[6:2] dominant material (0..31)`, `[1:0] quantized averaged authored alpha`; where sampled, `alpha=bits[1:0]/3` if solid, else 0.5 | `t3dFarRegionalCookFarSurfaceAlpha`, GLSL `decode_far_surface_bits` / `far_surface_solid` / `far_surface_material` / `far_surface_alpha` |
| FAR coverage downsample | Old producer samples four control probes in each coarse source cell and treats `coverage < 0.5` as empty; the replacement uses full-resolution authored bit 2 exactly | `Terrain3DFarRegionalCooker::patchColorRegionAt` |
| Height physics/tools | Existing CPU `terrain_stream.t3sp` / `.t3pm` stream and editor height maps retain authority | `src/streaming/runtime/terrain_3d_runtime_page_store.cpp` |

**Important migration behavior:** Old FAR coverage is **coarsened**, whereas the new exact cutout is **per authored texel**. ST03 must preserve authored source holes and alpha exactly, then ST05 validate raster equivalence across depth/color/shadow. Testing old coarse alpha bytes alone is insufficient to validate replacement masks.

## ST_REGION_V1 GPU record — 64 bytes, std430

There is exactly one GPU region array/bank per publication generation. The replacement preserves the existing first **32 bytes** verbatim and replaces the final FAR fields with COMPOSED, VHM and MASK handles and a shared publication tag.

| Offset | C++ `uint32_t`/`float[4]` | GLSL std430 | Meaning |
| ---: | --- | --- | --- |
| 0 | `m_nPackedPhysicalFlags` | `packed_physical_flags` | Existing low physical slot and presence bit 31 (unchanged) |
| 4 | `m_nMegaVTAllocation` | `megavt_allocation` | Existing nonregional MegaVT binding (unchanged) |
| 8 | `m_nMegaVTGeneration` | `megavt_generation` | Existing MegaVT generation (unchanged) |
| 12 | `m_nMegaVTFlags` | `megavt_flags` | Existing MegaVT flags (unchanged) |
| 16–31 | `m_vMegaVTWorldToUv[4]` | `megavt_world_to_uv` | Existing vec4 (unchanged) |
| 32 | `m_nComposedVTAllocation` | `composed_vt_allocation` | Existing V1 COMPOSED runtime slot base; INVALID when absent |
| 36 | `m_nComposedVTGeneration` | `composed_vt_generation` | Must equal shared published tag |
| 40 | `m_nVhmRootIndex` | `vhm_root_index` | Index of pinned coarse tail metadata (by logical region) |
| 44 | `m_nVhmGeneration` | `vhm_generation` | Must equal shared published tag |
| 48 | `m_nMaskRootIndex` | `mask_root_index` | Base of 4 exact-mask page entries for region |
| 52 | `m_nMaskGeneration` | `mask_generation` | Must equal shared published tag |
| 56 | `m_nPublicationGeneration` | `publication_generation` | Committed, nonzero u32 generation |
| 60 | `m_nRegionStateFlags` | `region_state_flags` | bit0 READY; bit1 COMPOSED; bit2 VHM; bit3 MASK |

`sizeof`=64, `alignof`=16, vec4 begins at byte 16, each region is a single 64-byte aligned record. `T3D_ST_INVALID_ID=0xffffffff` for absent allocations/roots; generation zero invalidates the record. All **present** regions must have bits 0..3 set and matching COMPOSED/VHM/MASK/record tags **before** publication. Absent regions keep presence bit31 clear and all new resource IDs invalid. A present-but-dirty region retains its **previous complete bank**, not an empty/new partial region.

At the 32x32 maximum region grid, old records require **49,152 bytes per bank**, proposed 64-byte records **65,536**, an increase of **16,384 bytes per bank**. ST05 must adjust all C++ bank reservations, GPU binds and GLSL layout in the same integration change. This record does **not** replace or expand existing instance-VT allocation layouts.

The old `far_vt_allocation`/`far_vt_generation` fields disappear **only at ST05**; until then this specification is a test-only type. The region map's source-location indexing and preexisting physical slot/shadowmask meanings do not change.

## VHM page and coarse-tail contract (ST04)

- Authored input: R16_UNORM using the existing **global height range**, no per-region renormalization. Separate derived regional min/max is permitted for culling; it must not reinterpret the saved R16 bits.
- Mip0 side 128 texels. Mips 0..7 are 128,64,32,16,8,4,2,1. Mip pages use **64x64 interior + two-texel gutter on each edge = 68x68 R16** (9,248 raw bytes per physical page).
- **Stable page ID** = `regionIndex * 11 + mipPagePrefix + pageY*widthPages + pageX`; prefixes: mip0=0 (2x2), mip1=4, mip2=5, mip3=6, ... mip7=10. Out-of-range IDs are 0xffffffff; never treat them as valid zero pages.
- For any page at mip0..6, the parent is its unique region-local mip+1 page (`x>>1,y>>1`); mip7 has no parent. `t3dStVhmPageEntryV1_t` is 16 bytes: physical slot u32, parent page ID u32, generation u32, packed mip/flags u32. The low 16 bits encode mip (`0x0000ffff`); bit16 VALID (`0x00010000`), bit17 RESIDENT (`0x00020000`) and bit18 PINNED (`0x00040000`). These flags are frozen in matching C++/GLSL macros and do not share IDs with the material V1 page table.
- Mips 3..7 always use a **pinned compact R16 coarse-tail atlas**. Every logical region has a reserved 52x20 atlas slot, with mip interiors 16,8,4,2,1, each with 2-texel gutters. Widths 20+12+8+6+5 = 51 <= 52; maximum height 20. Tail slot atlas position = `(regionIndex % 32 * 52, regionIndex / 32 * 20)`; mip X offsets = 0,20,32,40,46, Y offset always zero. Full atlas 1664x640 R16 is **2,129,920 raw bytes max** (not a measured RD allocation). The unused slot pixel is reserved.
- `t3dStVhmTailV1_t` is 32 bytes (atlas x/y, first mip, mip count, generation, flags (bit0 VALID), two reserved). VHM root index is the logical region index. Two-texel gutters read **authoritative neighbor height samples** when a matching neighbor is present/current. A neighbor edit dirties affected gutter pages. Mip reductions must use signed decoded elevation before re-encoding to R16; agreed LOD morph thresholds and neighboring border heights are ST04 acceptance fixtures.
- A missing resident fine page resolves through its nearest valid parent or committed pinned coarse tail **of the same generation**. It never resolves to zero elevation for a present region. The clipmap vertex shader eventually reads VHM for near, middle and far heights; CPU/tool/physics data remains untouched.
- The default candidate physical pool is 144 pages (**1,331,712 raw page bytes**) plus the pinned tail. This is a **proposed budget**, not a measured current allocation; size must be configurable and validated on-device.

## MASK page contract (ST03)

- Canonical source is `!t3dIsHole(controlWord)` from control bit 2 and **two authored-alpha bits** computed from full-resolution `RGBA8.a`: `alpha2 = (alpha8*3 + 127)/255`. Keeping both bits is a conservative frozen choice; a later proven baked-only optimization may reduce the number of planes, but **ST03 must implement the three-plane exact form**.
- Texel value layout: bit0 SOLID, bit1 ALPHA_LOW, bit2 ALPHA_HIGH. `value = solid | (alpha2 << 1)`, range 0..7. Four 64x64 mask pages per 128x128 region; nearest integer indexing only, no bilinear filtering, lossy BC or mip averaging of semantic bits.
- Uniform pages have no physical payload. `t3dStMaskPageEntryV1_t` (16 bytes): physical slot, pinned exact-word offset, generation, state/3-bit value. `packed_state` bit30 means VALID, bit31 means UNIFORM; low three bits encode any of eight uniform states (including EMPTY or FULL solidity). The caller still checks `generation == region.publication_generation`. The physical slot is invalid on a uniform page.
- Mixed pages have exactly 3 bitplanes, each **128 little-endian uint32 words** (64x64 / 32), ordered SOLID, ALPHA_LOW, ALPHA_HIGH. Each uses 512 bytes, total **1,536 bytes** per mixed page. Word index is `texelIndex >> 5`, bit index `texelIndex & 31`, x-major row order. Each mixed page has a pinned **immutable exact fallback bitplane** indexed by `pinned_word_offset`. A GPU-friendly mixed-page cache may mirror these data but can be evicted; fallback stays valid so cutouts never change with residency. Do not instantiate a second competing region owner.
- `mask_root_index = regionIndex * 4`. Full 32x32 grid worst-case pinned fallback **6,291,456 bytes** plus `4096 * 16 = 65,536` bytes of index entries; uniform tiles consume no pinned payload. Pin count scales with the actual mixed-tile population; reserve/capacity fails closed if insufficient.
- Editor changes to color alpha or hole control invalidate relevant page and producer transaction together. All render passes (color, DNR, depth, shadow) must sample identical bits. The old FAR material ID is **not** a mask input.

## Package, endianness and publication transaction

- Existing COMPOSED V1 `.t3dvt` file/ABI stays intact. Add versioned `VMH1` and `SMK1` sidecar chunks, **mandatory together** for new packages. Their source-derived bodies (VHM page R16+tail+indirection; mask page descriptors+mixed bitplanes) are packed little-endian; never memcpy compiler-native structs to disk without explicit LE encoding.
- Both sidecar chunks start with the **same 48-byte LE header**, C++ type `t3dStSidecarHeaderV1_t`: `{magic:u32@0, version:u32@4, regionIndex:u32@8, generation:u32@12, sourceSignature:u64@16, recipeSignature:u64@24, entryCount:u32@32, payloadBytes:u32@36, payloadCrc32:u32@40, flags:u32@44}`. Version=1; magic `VMH1` = `0x31484d56`, `SMK1` = `0x314b4d53`; payload CRC32 uses IEEE/zlib semantics over the **body only**. The header does not decide chunk byte offsets inside the new final pack: ST04/ST03 must emit explicit byte-counted framing plus these headers; ST05 integrates framing once with the format manifest.
- Source signature is a 64-bit deterministic digest of canonical source pixels + their revisions, neighbor-dependent gutters, material/assets (including projection, rotation, detiling, wetness, blend, texture variants), region index, and the build recipe. `recipeSignature` is a separately recorded 64-bit fingerprint of all output layout/compression/filter parameters. No source may authorize a new generation by presenting only old FAR fingerprints.
- Each region candidate collects **COMPOSED page completeness+resident tail**, VHM valid root, MASK valid exact root and three matching nonzero generation tags. Cold cook validates source signatures again before commit; all new records and tables are prepared offscreen in a writable bank. Only after all resources validate does the owner atomically swap the region publication snapshot. On any cook/IO/CRC/source change/upload failure, keep the **last valid complete snapshot**, including page tables and textures still owned by its generation. Never reuse live resources before readers have released the old snapshot.
- The 32-bit GPU generation is an opaque nonzero publication ticket; the transaction owner retains the full 64-bit authored revision/recipe in the manifest and tracks wraparound. Producer generations must never be independently promoted. `flags READY | HAS_MATERIAL | HAS_HEIGHT | HAS_MASK` indicate a complete committed record; flags do **not** authorize partially loaded resources.
- Fresh cooked-package manifest replaces `far_surface_vt=` with composed/VHM/mask entries and their fingerprints at ST06. Old manifest versions fail with an explicit recook diagnostic. ST00 does **not** alter the existing reader or cooked outputs.

## Frozen ownership and acceptance constraints

| Stage | Frozen input it must consume | Scope restrictions |
| --- | --- | --- |
| ST01 | existing V1 composed slot order; source signatures and parity reference corpus | Canonical evaluator, deterministic cold material cook; **no global shader ABI** |
| ST02 | composed region stable keys, epoch, matching pinned coarse root guarantee | Shared V1 demand/streaming only; **no FAR deletion yet** |
| ST03 | exact 64x64 three-bitplane mask, uniform sentinels, pinned fallback, 16-byte entry | New mask source and runtime only; **no common region shader edits** |
| ST04 | 64+2 R16 paged VHM, 8 mips/11 page IDs, 52x20 pinned slot, 32-byte tail metadata | New height source and runtime only; **no common region shader edits** |
| ST05 | 64-byte std430 GPU region ABI, shared generation and all family roots | **Sole owner** of global C++/GLSL ABI, bindings, atlas upload and atomic publication |
| ST06 | accepted ST05 production output | Delete all legacy FAR logical consumer/cooker/fragment/vertex paths, not geometric LOD |
| ST07 | compiled and verified ST06 | Shader variants, editor/runtime native build, on-device output/residency/memory acceptance |

### Source inventory and shader pass matrix

The machine-readable files `src/diagnostics/tests/superterrain/far_inventory.json` and `shader_pass_matrix.json` are **frozen source census snapshots**. The former lists file SHA-256 plus every occurrence and line of the enumerated FAR-specific anchors; the latter records all shader-mode names, source line, pass, defines and geometry owner found in the two Godot RDShaderFile roots. The active call sites cover direct editor source/cooker paths, pack format validation, material/runtime VT residency, publication, vertex/fragment consumers, indirect settings, dirty ledger and diagnostics. Geometry-only `far` LOD names are intentionally not treated as FAR material-cache ownership.

`src/diagnostics/tests/superterrain/inventory.py --write` and `shader_matrix.py --write` require **deliberate re-freezing** when follow-on waves change these inputs; no silent automatic update. At Wave 2 integration compare the new current census against frozen Wave 1 JSON, verifying every removed FAR path has a new owner. Editor variants and material feature correctness require actual renderer reference images at ST01/ST07; the synthetic ST00 source corpus cannot prove them.

### Required checks and context

Run `python -m unittest discover -s src/diagnostics/tests/superterrain -p test_baseline.py -v`, then compile `test_baseline_contract.cpp` as standalone C++17 with warnings-as-errors. `baseline.py` reports **static** bank/payload counts; `static_baseline.json.live_metrics` is intentionally null until an on-device camera trace exists. For native builds, the baseline `SConstruct` uses `target=editor` / `template_release`, MSVC `use_static_cpp=yes debug_crt=no`, `pch=stable` only with MSVC, and links the PCH companion object. Record results separately for shader-source checks, editor C++ compile, runtime C++ compile, MSVC link, Vulkan timing, memory and pixel parity.

See `REFERENCE_CORPUS.md` for the input binary schema and the manually pinned semantic probes.
