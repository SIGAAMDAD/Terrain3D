# Terrain3D P01->P06 + ownership/capacity repair

## Composition

This tree is the cumulative P01/P02 + P03/P04/P05 lineage, retains the 2026-09-05 owner-controlled allocation / exact GPU page-grant / adaptive runtime-page repair, and adds P06 generation-gated RenderCoordinator producer work.

P07 region-page upload accounting and P08 RenderWorld prepare/reuse are intentionally excluded.

## P06

P06 replaces recurring coordinator producer scans/rebuilds with source-owned monotonic generations for terrain material state, texture publication/layer mapping, regional lighting, mesh/HLOD source state, and ocean static state. The coordinator caches material and ocean static publications and performs source rebuilds only when their generation changes.

The runtime material layer map is read from fixed native mapping storage rather than allocating a PackedInt32Array during every coordinator publication. Mesh-source fingerprinting is removed from the normal publishSnapshot hot path. Regional lighting's 32x32 scan is gated by a source/topology generation.

## Preserved ownership/capacity repair

- HOT_FRAME/HOT_RENDER are telemetry only at the raw allocator choke point.
- GPU terrain-page capacity uses exact physical-layer byte accounting and best-effort spare pages.
- Runtime-page prepared-byte capacity adapts effective worker count to its exact grant and latches impossible layouts instead of retry-spamming.

## Exclusions

- No P07 exact full-page upload accounting/token changes.
- No P08 prepare_camera_visibility_hot / submit_camera_pass_hot split or prepared-view reuse.
