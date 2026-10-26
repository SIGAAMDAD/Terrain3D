# SuperTerrain ST03 + ST04 — implementation handoff

**Baseline:** ST00 followed by ST01/ST02 changed-file overlay.
**Scope:** independent exact virtual cutout and virtual height-map resource families, ready for ST05 integration. Existing active WORLD, FAR and common RD publication are unchanged.

## ST03: exact cutout + authored alpha

Production entry points:

- `Terrain3DCutoutCooker::cookRegionCold` accepts 128x128 control RF **bit-pattern** bytes (little-endian u32), authored RGBA8 bytes, nonzero transaction signature, recipe and generation. Source hole is bit 2; original authored RGBA8 alpha becomes `alpha2=(alpha8*3+127)/255`. The output value is `solid | (alpha2 << 1)` (three **exact** bits).
- One `SMK1` sidecar per present region, header 48 bytes: 4 x 16-byte descriptors, then 0–4 tightly packed 1,536-byte payloads. Each mixed payload is three 128-word little-endian bitplanes. Uniform pages consume zero words and support all eight uniform states. Mixed pages include a mandatory immutable pinned fallback; an optional physical cache cannot change the cutout result.
- `Terrain3DCutoutRegionView::open` rejects incorrect revision/recipe/generation, CRC, payload length, entry offsets, invalid state bits, and malformed sentinel references. It does not change a previously valid view on failure. `appendGpuPayload` relocates word offsets for one shared pinned SSBO while retaining the 16-byte GPU entry format.
- `terrain_clipmap_cutout.glslinc` has a generation-checked integer-only shader sampler. ST05 selects the two bindings in the shared RD descriptor layout, installs it in color/DNR/depth/shadow, and ensures all passes share an identical published snapshot.

## ST04: R16 virtual height map

Production entry points:

- `Terrain3DVhmCooker::cookRegionCold` consumes the authoritative raw **R16_UNORM** 128x128 field and matching-generation neighbor fields for gutters. Mips 0–7 reduce decoded signed heights and re-encode to the global `[-384,768]`-meter R16 interval. Mip0 has four pages; mips 1–7 have one page each (11 stable global IDs). Only mips 0–2 need optional physical fine pages; mip 3–7 live in the mandatory pinned tail.
- One `VMH1` sidecar per present region, 48-byte header and exactly **57,776 bytes payload**: 11 x 16-byte page entries, 32-byte tail entry, 6 x 9,248-byte 68x68 R16 fine pages, plus one 2,080-byte (52x20 R16) compact tail slot. Each sidecar contains only its own slot; ST05 packs 1,024 potential slots into the 1,664x640 shared R16 atlas. Tail starts at mip3; X offsets are 0,20,32,40,46.
- `Terrain3DVhmRegionView::open` checks fixed page IDs/parents/mip flags, recipe and generation, root coordinates, reserved bytes, payload byte count and CRC. `sampleR16` resolves missing fine pages through the nearest parent until the pinned tail; the source R16 bits are exact at resident mip0.
- `Terrain3DVhmRootAtlas` builds an isolated candidate root atlas for ST05, with no mutations to the current published GPU texture. `Terrain3DVhmFinePagePool` initializes an explicitly bounded CPU upload staging pool; hot page staging is allocation-free, map lookup is O(1), and physical page reuse requires an explicit fence-safe retirement call. The pool generates generation-checked page-table entries after upload completion. It **does not** create or own a competing GPU texture cache.
- `terrain_clipmap_height_vhm.glslinc` supports explicit page/ancestor/tail lookup, manual bilinear fetch across two-texel gutters and supplied LOD morph weight. ST05 binds its buffers/images and replaces existing near, middle and FAR GPU height samplers as one integration change.

## Shared sidecar framing / generation

`terrain_3d_st_sidecar_wire.h` writes/reads the frozen **48-byte LE header** with IEEE CRC32 over payload only. Readers require exact, nonzero `sourceSignature`, `recipeSignature` and `publicationGeneration` from the **same outer cook transaction**. A producer may not independently authorize its own generation. The previous valid bank is preserved while ST05 prepares and validates every resource.

The cold owner must recompute fingerprints from authored source and neighbor generations (and all canonical material features) before committing. These two stages accept fingerprints provided by the authoring transaction; they do not provide a replacement global dirty/revision registry. ST05 controls the final package manifest, disk offsets, GPU RIDs, descriptor sets and atomic publication.

## Remaining integration boundaries

1. **ST05** hooks these cold producers into `Terrain3DRegionRepository`/cooked-page jobs, accepts `SMK1` and `VMH1` in a single versioned manifest, and validates source fingerprints before exposing GPU resources.
2. **ST05** binds the 64-byte GPU region contract, VHM page table, pinned tail atlas, exact cutout entries/words and material COMPOSED allocation in one versioned publication bank. No half-updated GPU snapshot and no page slot reuse until GPU fences have retired the old bank.
3. **ST05** replaces common WORLD shader calls in color, DNR, depth and shadow and runs actual shader compilation; ST03/ST04 GLSL includes are stand-alone until then. The current engine has no modified shader descriptor ABI in this overlay.
4. **ST06/ST07** remove the legacy FAR consumer and validate edited materials, GPU rendering, eviction/morph/border behavior, runtime memory, MSVC PCH/linking and on-device metrics.

## Validation

Run from the source root:

```sh
sh src/diagnostics/tests/superterrain/run_st03_st04.sh
CXX=g++ sh src/diagnostics/tests/superterrain/run_st03_st04.sh
CXX=clang++ T3D_ST_SANITIZE=1 sh src/diagnostics/tests/superterrain/run_st03_st04.sh
```

Tests cook all 12 populated ST00 synthetic regions. They exercise all 196,608 exact mask texels, all mip0 R16 texels, signed reduction at every mip, alpha extremes, holes, region-border gutters, generation mismatch, CRC/offset corruption, mixed-page pin offsets, forced VHM evictions, fixed-capacity page staging, physical-slot reuse and no unexpected mutation on rejected cooks. GLSL visual/GPU behavior and true canonical material parity remain explicit ST05/ST07 checks.
