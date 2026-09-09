# Runtime-page prepared-byte grant zero fix — 2026-09-08

## Symptom

Runtime startup can report:

```text
Terrain3D: Runtime-page prepared-byte grant cannot cover one worker scratch plus one fixed payload slot.
grant=0, minimum=612751, worker_scratch=174836, payload_slot=437915, ...
```

The cooked page itself is valid. The reported minimum is only about 598 KiB, but the central memory policy has supplied a zero-byte prepared-page grant.

## Root cause

The default transient reserve is 32 MiB. The default far-surface atlas can be 4096 x 4096 RGBA8, which is 64 MiB before the height atlas is counted.

The far-atlas CPU images are retained whole-world cache residency. They were already represented in fixed memory through `Terrain3D::getGpuFarAtlasBytes()`, but `Terrain3DData::getMemoryNonPageTransientStagingBytes()` also included `m_farSurfaceAtlasCache.getImageBytes()`.

After `ensureFarAtlases()` materialized those retained images, the second startup memory rebalance performed:

```text
runtime_page_grant = transient_reserve - non_page_transient
                   = 32 MiB - >64 MiB
                   = 0
```

`setPreparedByteBudget(0)` then dirtied the runtime-page capacity image and the next source rebuild correctly rejected the impossible zero grant.

This is a memory-classification/accounting bug, not a runtime-page serialization failure. `largest_channel=7` is the min/max-height blob; it is only the largest scratch requirement, not the failing consumer. Because capacity is established for the whole prepared page, a zero grant also prevents static-instance/instance-metadata/fast-start channels from becoming available to the instance residency lane.

## Fix

`Terrain3DData::getMemoryNonPageTransientStagingBytes()` now contains only genuine non-page transient staging owned by the map-edit service. Retained far-atlas images are not charged against the transient reserve a second time.

The existing fixed-memory accounting remains authoritative for the far-atlas footprint:

```cpp
const uint64_t fixed_far_atlas_bytes = getGpuFarAtlasBytes();
```

True transient competitors such as map-edit scratch, clipmap streaming buffers and collision scratch still reduce the runtime-page prepared-byte grant.

No runtime-page disk format, page version, channel layout, GDScript API or rendering API changes are involved.

## Regression gate

`tools/verify_runtime_page_transient_grant_accounting.py` verifies that:

- far-atlas retained images are absent from non-page transient accounting;
- far-atlas residency remains represented by fixed memory;
- the central memory policy still owns the runtime-page grant;
- genuine non-page transient staging still competes for that grant;
- the default 4096 RGBA8 far-surface footprint remains larger than the 32 MiB transient reserve, preserving the regression fixture that exposed the bug.
