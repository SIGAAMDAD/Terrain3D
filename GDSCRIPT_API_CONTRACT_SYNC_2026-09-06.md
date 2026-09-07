# Terrain3D GDScript API Contract Sync

**Date:** 2026-09-06

## Scope

The capacity-layout repair changed native runtime behavior and native build identity, but it did not add, remove, or rename any public `ClassDB` method/property. The native API revision therefore remains `2026082203`.

The GDScript/editor layer did require synchronization in three places.

### 1. Native/script build guard

`project/addons/terrain_3d/src/editor_plugin.gd` still expected the historical native build ID:

```text
a2.9.2-shader-preprocessor-repair
```

The native library now reports:

```text
p07-capacity-layout-nondestructive-2026-09-06
```

The expected GDScript build ID now matches the native constant. Without this update, the editor plugin can correctly reject the new DLL as a mixed native/script revision even though the public API revision is unchanged.

### 2. Capacity UI semantics

GPU page capacity is now residency state rather than render-tier authority. The runtime profiler action was renamed from `Clamp detail to capacity` to `Fit detail residency to capacity` and remains an explicit user action. Automatic capacity pressure does not rewrite Near/Middle/Far shader-tier counts.

### 3. Inspector/editor metadata

The GDScript metadata now states that:

- `gpu_region_detail_range_valid` is a residency-health diagnostic;
- `gpu_region_max_supported_detail_lods/range_m` are advisory residency-capacity values;
- `runtime_sync_detail_lods` synchronizes requested detailed residency with the Near tier but does not clamp render tiers to current mapped capacity;
- shipping/runtime cooked pages remain authoritative while Editor Authoring may use the restored live-map fallback for missing/stale/incompatible prepared pages.

## Verification

`tools/verify_script_native_contract.py` checks the native API revision/build ID against the GDScript contract and rejects the old destructive-capacity wording/API action.
