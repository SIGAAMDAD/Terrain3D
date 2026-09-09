# Terrain3D Instancer Authoritative Visible-Index Fix

Date: 2026-09-09
Build ID: `instancer-authoritative-visible-index-fix-2026-09-09`

## Observed runtime state

The earlier mesh-source and Forward+ pipeline fixes moved the RD instance lane into real raster submission:

- 18 published instance records
- 1 published cell
- 8,736 published vertex bytes
- 2 published LODs
- 2 published surfaces
- 1 resolved draw packet
- zero mesh upload failures

Instances rendered, but only roughly 4-6 unique placements were visible. Removing existing instances caused previously missing placements to appear. Shadow PREPARE callbacks also emitted repeated `zero draws for a non-empty published scene` errors.

## Root cause

`T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT` intentionally bypasses the CPU camera/frustum gate so the authoritative resident store can prove the RD rendering path independently of cached render cells and stale bounds.

That path calls `buildPackets(..., bSkipFrustum=true)`.

`buildPacketsForPass()` incorrectly gave `bSkipFrustum` two unrelated meanings:

1. skip CPU frustum rejection; and
2. defer visible-index/indirect output because GPU culling will populate it later.

The production GPU-cull topology legitimately uses both meanings together. The integrated RD direct-surface port does not:

```cpp
#if T3D_INSTANCER_RD_PORT
const bool gpu_visibility = false;
#endif
```

As a result, the direct path produced packet instance counts for all 18 records but skipped writing the corresponding `m_visibleIndices` entries. `uploadVisibility()` then uploaded stale/default backing and the vertex shader indexed instance records through those invalid logical indices:

```glsl
uint record_index = visible_indices[push_data.visible_base + uint(gl_InstanceIndex)];
InstanceRecord record = records[record_index];
```

This caused multiple submitted instances to alias the same small subset of records. Editing/deleting instances changed the stale index contents/order, explaining why different placements appeared afterward.

## Fix

Packet construction now distinguishes frustum policy from visibility-output ownership.

For the direct RD port and minimal direct path:

```cpp
const bool defer_visibility_output = false;
```

Therefore authoritative no-frustum packet construction still materializes the exact visible-index sequence and direct command instance count.

For the normal shared-arena GPU-cull path:

```cpp
const bool defer_visibility_output = bSkipFrustum;
```

so compute continues to own visible-index/indirect population when that path is active.

The resulting direct authoritative path for one 18-instance cell is now logically:

```text
packet visible count: 18
visible indices:       0,1,2,...,17
direct draw instances: 18
```

## Forward+ zero-draw diagnostic correction

Forward+ pass 3 is the shadow pass. A non-empty published scene does not imply every native pass has an eligible draw. Shadow, depth, opaque, motion and transparent passes each apply their own eligibility rules.

A successful `prepareForwardPass()` returning zero eligible packets is therefore a valid no-op, not a renderer failure.

The coordinator now:

- retains `m_nInstancePreparedWithoutDraws` telemetry;
- does not emit an ERROR for successful zero-draw PREPARE;
- continues emitting the detailed `Forward+ PREPARE failed` diagnostic when `prepareForwardPass()` actually fails.

## Added diagnostics

Instance renderer diagnostics now include:

- `visible_index_count`
- `shadow_visible_index_count`
- existing `shadow_packets` remains the pass-specific shadow packet count

These make publication/index/pass-filtering failures separable at runtime.

## Verification

Verified in the supplied source tree:

- affected instance renderer and render coordinator syntax-compile as C++17 against supplied godot-cpp/EASTL headers;
- authoritative visible-index regression gate passes;
- authoritative-store publication gate passes;
- integrated RD-port gate passes;
- direct-surface bring-up gate passes;
- ArrayMesh mesh-source binding P0 gate passes;
- Forward+ submission/transparency gate passes;
- Forward+ blend attachment contract gate passes;
- instance pipeline recovery gate passes;
- preview/lazy Forward gate passes;
- indirect-buffer usage gate passes;
- P11 GPU instance culling lineage gate passes;
- P1/P2 Forward+ diagnostic/resource gates pass;
- memory P1 and active-storage P2 gates pass;
- owner-controlled allocation A-G gates pass;
- native/GDScript API contract passes;
- systems-C style and ownership gates pass;
- GPU tier non-destructive capacity gate passes.

A full linked DLL build was not performed in this environment.
