# SuperTerrain Wave 1: ST00 contract/reference corpus handoff

**Result: ST00 implemented as an isolated, test-only contract baseline.**

The source base is `Terrain3D_FAR_Regional_V1_Contract_Cleanup_2026-10-25_Full_Source.zip` (SHA-256 `37bae8fec1033265de005a37cb174828c4de60494cba1ce9afc7010b3fc2a005`). This patch adds files only inside `src/diagnostics/tests/superterrain/` and `docs/superterrain/`; it modifies **zero existing production files**. That boundary is required by `WG_FREEZE_ST00` and is deliberate. FAR cook, residency, shader and publication paths remain enabled until ST01–ST05 integration has been accepted.

## Frozen output

- **One 64-byte replacement region record**, std430-mirrored C++17/GLSL structs and a tested all-or-nothing generation gate. Old bank is 48 bytes; additional record payload at 1,024 slots is 16 KiB per bank. Source of truth: `ST_REGION_V1_FROZEN_CONTRACT.md`, `st_region_v1_contract.h` and `.glslinc`.
- **COMPOSED** retains the current three V1 BC1/BC5/BC7 channels, same stable per-region keys and shared cache residency. No fourth V1 material channel, new renderer or separate FAR atlas.
- **VHM** contract: 128x128 source R16 height over global [-384,768] meters; mip0 four 64x64 pages, mip1..7 one page each; 11 page IDs per region; 68x68 physical R16; 2-texel gutter; 5 coarsest mip levels in a 52x20 compact pinned tail slot. Frozen parent lookup, page flags, 16-byte entry and 32-byte tail metadata.
- **MASK** contract: exact authored solidity plus two quantized alpha bits in three 4,096-bit planes per mixed page; 4 x 64x64 pages per region; eight uniform sentinels; immutable exact fallback for each mixed page, no lossy filtering. A 16-byte mask page entry.
- **Publication** proposal: all three resource families must share one nonzero committed generation; a present/ready region without composed allocation, VHM root, exact mask root, or matching generations is rejected. Both new sidecar families have the same frozen 48-byte little-endian header and CRC32 payload semantics. Only ST05 may change production C++/GLSL region layouts.
- **Frozen FAR owner inventory**: `far_inventory_v1.json` contains per-file SHA-256, line, matching source symbol and owner. It includes remaining authoring and editor bindings, material settings, dirty ledger, VT reader/page contracts, WORLD publication, shaders, addon metadata and retirement tests. It is a source census, not a declaration that these paths have been retired.
- **Frozen shader pass matrix**: `shader_pass_matrix_v1.json` enumerates both production RDShaderFile roots and all 216 variant definitions with pass, geometry owner, editor and feedback tags.
- **Deterministic source corpus**: 13 `.st00region` files and a SHA-256 index. The 12 present regions cover 196,608 authored texels, R16 negative and discontinuous height cases, matching neighbor edges, hole/alpha extremes, sparse controls, rotation/scale, wetness input and sampling-distance metadata. These are input fixtures, not canonical-render screenshots.

## Source evidence and check commands

Run from repo root:

```sh
python src/diagnostics/tests/superterrain/run_st00.py
```

Or execute the individual scripts without `--write` (which check frozen snapshot drift), `python -m unittest discover -s src/diagnostics/tests/superterrain -p test_st00.py -v`, and `clang++ -std=c++17 -Wall -Wextra -Werror -pedantic -O2 src/diagnostics/tests/superterrain/test_st00_contract.cpp -o st00_contract` followed by `./st00_contract`. Compiled executables should live outside the source tree, as in `run_st00.py`.

The externally generated **Wave 1 validation log** lists exact completed commands. The original source digest and all baseline file digests are checked against the handoff's `SHA256_MANIFEST.json`; the changed-files ZIP contains its own file SHA-256 manifest. The dependency archive and MSVC runtime are unnecessary for ST00's independent C++17 contract test.

## Gate results and limits

| ST00 gate | Status |
| --- | --- |
| Original source archive hash and per-file baseline integrity | PASS (unchanged original source files) |
| FAR semantic enumeration including indirect settings/validation clients | PASS (source census frozen; see JSON for exact list) |
| C++17 region/sidecar std430 size and field offsets | PASS (standalone compile + assertions) |
| C++ vs GLSL frozen field/macro parity | PASS (source parser cross-check) |
| Material/height/mask page IDs and parent identities | PASS (all 1,024 possible regions) |
| Exact 3-bit mask encode/decode, corners and uniform classes | PASS (12 present region fixtures + standalone C++ test) |
| Negative/flat/positive R16 and paired region seam vectors | PASS (source vectors) |
| Shader variant and pass coverage | PASS (216 parsed variants; no GPU compilation claim) |
| Static GPU payload budget projections | RECORDED (not device measurements) |
| Real authored material parity and runtime visual/generation eviction tests | RESERVED FOR ST01/ST07 (requires game material sources and renderer) |
| Engine/editor/runtime DLL integration and Vulkan benchmarks | RESERVED FOR ST05/ST07 (not changed by ST00) |

The synthetic corpus exercises deterministic **source semantics**. It does not establish that the current editor material evaluator produces bit-identical BC1/BC5/BC7 material pages; ST01 must capture actual evaluator reference images and all enabled shader feature combinations before accepting production material baking.

## Wave 2 starting interface

The four Wave 2 workgroups each take this **same** accepted ST00 patch and frozen `ST_REGION_V1` contract as their baseline:

| Group | Stage | Required binding |
| --- | --- | --- |
| WG-MAT | ST01 | Production canonical material sampling, true editor cook source, deterministic COMPOSED BC1/BC5/BC7+mips, parity/fingerprint tests |
| WG-RES | ST02 | Shared V1 demand, guaranteed coarse composed ancestor/root and bounded resident/tail capacity |
| WG-MASK | ST03 | Contract three-bit exact mask pages, uniform sentinels, 1,536-byte pinned mixed fallback, same cook generation |
| WG-VHM | ST04 | Contract 11-page geometry ID map, 68x68 R16 pages, packed 52x20 tails, coarsest rooted samples, source-relative morph continuity |

**Do not** let ST01–ST04 edit `src/Terrain3DTypes.h`, `src/rendering/rd/vt/core/terrain_3d_vt_contract.h`, common GLSL bindings, WORLD publication or SConstruct. ST05 is the exclusive merge integrator and promotes these test-only layouts once producer and runtime resource contracts compile together. ST06 retires FAR logical consumer/cooker/GLSL paths only after ST05 activation. ST07 performs editor/runtime builds and device acceptance.

### Frozen assumptions / items for integration to verify

- The **three-plane page packing** uses three separate 4,096-bit planes (3 bits/texel); each mixed tile occupies exactly 1,536 bytes of pinned raw fallback. This allocation is subject to a full-grid maximum of 6 MiB plus tables.
- Generation is a 32-bit, nonzero GPU ticket; full source/recipe fingerprints remain 64-bit cold-side metadata. Wraparound and in-flight reader lifetime must be handled by ST05.
- The proposed VHM pinned atlas of 2,129,920 raw bytes and proposed 144 live pages of 1,331,712 bytes are **pre-allocator** capacities, not observed Vulkan bytes. Region count, tail reuse and residency need on-device profiling in ST07.
- Authoring coverage is control bit2 at authored resolution. The old FAR coarse coverage classification loses information; production ST03/ST05 must use the exact field in shadow/depth/DNR/color passes and verify existing authored-alpha use (including wetness and edited colors).
- Existing native static CRT/PCH/linker behavior is unchanged by Wave 1. ST05/ST07 must still build both editor and runtime DLL variants on the Windows toolchain.
