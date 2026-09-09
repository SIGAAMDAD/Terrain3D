# Terrain3D RenderingDevice Git History — P09 through Current

Date: 2026-09-09
Branch: `experiment/rendering-device`
Author identity preserved from the supplied repository: `Noah Van Til <nyvantil@gmail.com>`

## Reconstruction policy

The supplied `.git` history ended cleanly at:

- `f34ac65 perf(render-world): implement P08 prepare-submit visibility reuse`

The supplied project snapshots provide exact source states for the authoritative-store milestone and for every subsequent P0/P1/P2/Forward+/visible-index fix. Those exact states are committed individually below.

The repository does **not** contain exact intermediate source snapshots for every P09-P12 / HLOD / Forward+ / instancer bring-up sub-patch between P08 and the authoritative-store milestone. Rather than fabricate non-verifiable intermediate commits, that cumulative source state is represented by one truthful integration commit (`e05c131`). The detailed patch documents and verifiers for those sub-stages remain in-tree.

## New post-P08 history

### `e05c131 feat(rd): integrate post-P08 render-world and instancer runtime`

Establishes the exact authoritative-store snapshot on top of P08.

Includes the cumulative verified work for:

- P09 GPU-indirect visibility and VT prediction;
- P10 VT streaming index / worker handoff / retire batching;
- P11 instance GPU culling and dirty-range publication;
- P12 HLOD transition lists and node migration cadence;
- HLOD authoring/runtime hardening;
- Forward+ viewport lifecycle, lane diagnostics, pass prerequisites, and separate render-thread bridge work;
- runtime-page header/transient grant repairs;
- lightmap shader atomic and shadowmask finalization repairs;
- RenderingDevice instancer port integration;
- minimal instance bring-up and direct-surface path;
- indirect addressing, texture fallback, transparency, preview, and pipeline recovery work;
- authoritative `Terrain3DInstanceStore` RD publication.

This is intentionally one integration boundary because the exact intermediate working trees are not present in the supplied repository.

### `03a7a7f fix(instancer): publish ArrayMesh surface metadata through RD cache`

Fixes the first hard RD publication blocker after authoritative instance records became visible to the renderer.

- replaces the incorrect `Mesh::_surface_get_*()` extension virtual calls;
- uses engine-bound `ArrayMesh` surface metadata accessors;
- repairs mesh-source fingerprinting;
- rejects non-empty sources that produce zero usable triangle surfaces;
- adds focused regression coverage.

### `dd3b2c1 fix(memory): stop double charging runtime-zone-backed capacity`

Corrects central memory-budget accounting.

- tracks allocator-contained fixed bytes separately;
- avoids charging resident maps / instancer containers / node-streamer retained storage both as logical fixed memory and again as part of the runtime allocator reserve;
- caps containment by the real allocator reserve so editor heap overflow remains accountable.

### `c853883 refactor(memory): make active sampling storage Terrain3D-local`

Removes the process-global active terrain storage singleton.

- physical active pages are instance-owned;
- logical-to-physical mapping is instance-owned;
- writer synchronization and publication generations are instance-owned;
- leases retain the specific storage instance they pin;
- overlapping Terrain3D objects can no longer fight over one global owner token.

### `17c2c63 fix(forward): match blend state to Forward+ color attachment layout`

Repairs RenderingDevice pipeline creation for transparent Forward+ passes.

- provides the full three logical Forward+ color slots;
- enables alpha blending only on slot zero;
- retains disabled blend entries for unused specular/motion slots;
- applies the same contract to the ocean transparent renderer.

### `1528f85 fix(instancer): materialize visible indices for authoritative RD draws`

Fixes the partial-instance rendering failure where only a handful of records appeared and deleting one record exposed another.

- separates "skip CPU frustum" from "defer visible indices to GPU compute";
- materializes visible indices for the authoritative RD port where GPU visibility is disabled;
- prevents `gl_InstanceIndex` from indexing stale/aliased records;
- treats successful pass-specific PREPARE calls with zero eligible packets as valid no-ops instead of errors;
- adds visible-index diagnostics and regression coverage.

### `a6b3b31 fix(instancer): orient two-sided alpha-scissor lighting per face`

Fixes foliage/card materials that rasterized both sides but shaded only one side correctly.

- uses `gl_FrontFacing` to orient the fragment normal;
- corrects tangent-space handedness on backfaces;
- preserves the existing double-sided/no-cull raster path;
- fixes normal-mapped and regular alpha-scissor card lighting without duplicating geometry.

## Resulting branch tip

Functional renderer tip before this documentation commit:

- `a6b3b31 fix(instancer): orient two-sided alpha-scissor lighting per face`

The current source therefore contains, cumulatively:

1. P08 prepare/submit visibility reuse;
2. post-P08 P09-P12 + HLOD + Forward+ + instancer integration;
3. ArrayMesh mesh-source publication fix;
4. runtime-zone budget containment fix;
5. instance-local active storage;
6. Forward+ blend attachment contract fix;
7. authoritative visible-index materialization;
8. two-sided alpha-scissor lighting correction.

## Complete branch chronology

```text
9bc792a 2026-08-22 Reconstructed repaired RenderingDevice baseline
5d9e8b1 2026-08-22 Complete RenderingDevice terrain alpha
fbba751 2026-08-22 Establish RenderingDevice reference baseline
bc1ed6d 2026-08-22 Add Codex-efficient RD workflow
ba90812 2026-08-22 Prepare Windows Codex Ultra RD handoff
239783e 2026-08-23 RenderingDevice: complete batches A-C implementation
754a7ab 2026-08-23 RenderingDevice: complete Batch D instancing
1c259a2 2026-08-23 Complete Batch E HLOD RenderingDevice conversion
63bd108 2026-08-23 RenderingDevice: complete Batch F1 Forward+ bridge
fd7345a 2026-08-23 Implement Batch F2 native depth and opaque submission
451d10d 2026-08-23 Implement Batch F3 native shadow submission
26e36d3 2026-08-23 Implement Batch F4 native Forward+ lighting
2db46dc 2026-08-23 Implement Batch F5 motion vectors
d95c7e6 2026-08-23 Implement Batch F6 scene integrations
d4dfd49 2026-08-23 Implement Batch F7 multiview lifecycle
1188dcc 2026-08-23 Complete Batch F8 consolidation
5fef328 2026-08-23 Complete Batch G editor RD integration
29fa224 2026-08-24 Complete Batch H final validation
71702d2 2026-08-26 finishing up upload bridge to godot
171fd98 2026-08-26 style(core): remove anonymous namespace linkage
81f5355 2026-08-26 refactor(shaders): share RD clipmap shader implementation
6d2818f 2026-08-26 fix(rd): restore terrain material sampling parity
440036f 2026-08-26 fix(rd): restore Forward+ terrain lighting parity
0fdf958 2026-08-26 fix(streaming): synchronize physical GPU page residency
e232e67 2026-08-27 fix(settings): preserve serialized terrain configuration
d1cc038 2026-08-27 fix(memory): make runtime allocator lifetimes safe
04d129b 2026-08-27 refactor(runtime): seal hot storage and immutable config
42df06b 2026-08-27 perf(rd): make frame publication and instance packets fixed-capacity
538e1d8 2026-08-27 perf(streaming): make runtime page service allocation-free
379cebe 2026-08-27 perf(streaming): preallocate terrain activation state
3ef5d79 2026-08-27 fix(rd): bound resource retention and memory accounting
2c4a085 2026-08-27 perf(runtime): remove redundant compute and enforce zero-allocation hot paths
4567338 2026-08-27 fix(shaders): repair far clipmap material sampling
9470cbf 2026-08-27 fix(shaders): bypass far-tier control wrapper
3f55079 2026-08-27 fix(shaders): invalidate clipmap shader imports
7c2ae6d 2026-08-27 fix(shaders): avoid struct-array material sampling
2cac133 2026-08-27 fix(rd): rebuild topology after mesh size changes
a350ff3 2026-08-28 feat(renderworld): add fixed-capacity RenderingDevice world runtime
bfe755e 2026-08-30 feat(runtime): integrate cooked streaming and regional authoring workflows
5e808b3 2026-09-01 refactor(memory): make runtime ownership explicit and bounded
b8e888c 2026-09-02 refactor(core): enforce ANSI memory operations and EASTL systems-C rules
acdb0ee 2026-09-03 style(repo): normalize systems-C formatting and source readability
2c1b30c 2026-09-03 chore(repo): enforce source and ownership consistency gates
cded9b8 2026-09-05 perf(terrain3d): integrate P01-P06 fixed-128m runtime architecture
30cbb73 2026-09-05 perf(gpu-cache): implement P07 exact upload accounting and admission
cf54fa2 2026-09-06 fix(terrain3d): repair P07 startup residency and authoring regressions
f34ac65 2026-09-06 perf(render-world): implement P08 prepare-submit visibility reuse
e05c131 2026-09-09 feat(rd): integrate post-P08 render-world and instancer runtime
03a7a7f 2026-09-09 fix(instancer): publish ArrayMesh surface metadata through RD cache
dd3b2c1 2026-09-09 fix(memory): stop double charging runtime-zone-backed capacity
c853883 2026-09-09 refactor(memory): make active sampling storage Terrain3D-local
17c2c63 2026-09-09 fix(forward): match blend state to Forward+ color attachment layout
1528f85 2026-09-09 fix(instancer): materialize visible indices for authoritative RD draws
a6b3b31 2026-09-09 fix(instancer): orient two-sided alpha-scissor lighting per face
```

## Verification of current tip

Targeted current-state regression checks pass for:

- authoritative visible-index publication;
- authoritative store publication;
- ArrayMesh RD surface binding;
- Forward+ three-slot blend state;
- P1 memory budget zone containment;
- P2 instance-local active storage;
- Forward+ instance submission/transparency;
- alpha-scissor material pipeline.

The source also retains the direct RD-port visible-index materialization and the pass-specific zero-draw no-op behavior while applying the two-sided shader correction.
