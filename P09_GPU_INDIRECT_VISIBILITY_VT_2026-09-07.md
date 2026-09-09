# P09 GPU-indirect visibility and VT prediction

## Scope

P09 is forward-ported from `Terrain3D_P09_P10_P11_Performance_128m_Memory_Patched_2026-09-04.zip` onto the repaired cumulative P01-P08 tree.

Only the P09 contract was imported. P10 and P11 changes were deliberately excluded because the supplied archive is cumulative and later patches modify some of the same RenderWorld files.

## Contract

P09 removes sealed-capacity compute launches from the variable visibility stages.

The first visibility stage still launches from the CPU-known live cell count:

```text
live cells -> direct cell dispatch
```

Every counter-dependent continuation is generated on GPU and launched indirectly:

```text
cell cull
  -> visibleCellCount
  -> args pass
  -> indirect instance cull
  -> visibleInstanceCount
  -> args pass
  -> indirect cluster cull
  -> visibleClusterCount[3]
  -> args pass
  -> indirect VT prediction for world/clutter/foliage
```

The fixed-domain VT hash reset/compaction passes remain direct because their work domain is the fixed request hash table, not a live visibility count.

## Shared indirect-buffer ABI

The existing three draw records occupy bytes 0-47. P09 appends five 12-byte compute-dispatch triplets:

| Offset | Purpose |
| ---: | --- |
| 0 | world draw args |
| 16 | clutter draw args |
| 32 | foliage draw args |
| 48 | instance visibility dispatch |
| 60 | cluster visibility dispatch |
| 72 | world VT prediction dispatch |
| 84 | clutter VT prediction dispatch |
| 96 | foliage VT prediction dispatch |
| total 108 | complete buffer size |

The buffer was already created with `STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT`, so P09 enlarges and reuses the existing Terrain3D-owned indirect resource rather than adding another allocation.

The minimum RenderWorld GPU visibility memory plan is also updated to use the complete 108-byte indirect ABI, so the planner does not retain the old 48-byte draw-only accounting.

## Hot-path effect

Before P09, instance, cluster, and VT prediction stages launched against sealed maximum capacities. This caused empty workgroups whenever the live scene population was substantially below capacity.

After P09:

- cell culling launches only for `T3DVisibilityFrame::nCellCount`;
- instance culling launches from the GPU-produced visible-cell count;
- cluster culling launches from the GPU-produced visible-instance count;
- each VT prediction tier launches from the GPU-produced visible-cluster count for that tier;
- barriers preserve producer/indirect-consumer ordering inside the compute list;
- no hot-path allocation is introduced.

## Telemetry

P09 adds:

- `T3DGpuVisibilityStats::nLaunchedVariableGroups`;
- `T3DGpuVisibilityStats::nRequiredVariableGroups`;
- `T3DGpuVisibilityStats::nIndirectDispatchCount`;
- `T3DVTGpuStats::nVariableTierDispatchCount`.

## Compatibility decisions

The current P01-P08 fixes remain authoritative. In particular, P09 does not replace or revert:

- P07 exact upload accounting and Editor Authoring page fallback;
- GPU page budget/residency fixes;
- runtime-page legacy/Authoring cook fixes;
- render-startup recovery;
- P08 RenderWorld prepare/submit reuse.

P10 O(1) VT streaming/readback changes and P11 instance dirty-range/GPU instance-cull changes are not included.

## Verification

Run:

```text
python tools/verify_p09_gpu_indirect_visibility.py
python tools/verify_p08_render_world_prepare_submit.py
python tools/verify_script_native_contract.py
```

The P09 verifier also checks that representative P10/P11 markers are absent.
