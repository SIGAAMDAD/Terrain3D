# Terrain3D Lightmap, Shader Include, and 16-bit Atomic Fix — 2026-09-08

## Scope

This patch addresses three independent defects in the P0–P3 render-thread-optimized Terrain3D tree:

1. Regional LightmapGI preparation could invoke Godot's editor bake with a `LightmapGIData` resource that had no usable `resource_path`, causing Godot to request a save path instead of beginning the intended prepared bake.
2. `none_functions.gdshaderinc` contained an invalid GLSL constructor token.
3. The MSVC atomic implementation excluded 16-bit atomic values even though the runtime already uses `t3dAtomic_t<int16_t>`.

HLOD behavior is deliberately **not** modified by this patch. See `HLOD_SYSTEM_AUDIT_2026-09-08.md`.

## Regional LightmapGI root cause

`prepare_region_bake()` created a new `LightmapGIData`, saved it to the per-region working `.lmbake` path, then assigned the original in-memory resource to `LightmapGI.light_data`.

The prepared editor workflow invokes Godot's normal LightmapGI editor bake action without supplying a second output path. That path expects the assigned `LightmapGIData` to already have a usable resource path. The generated Godot API exposes `BAKE_ERROR_NO_SAVE_PATH`, and the observed editor behavior—asking the user where to save the lightmap—is the corresponding failure mode.

Saving a newly created resource is not a sufficient ownership contract for the in-memory object. The patch now reloads the just-saved `.lmbake` using `ResourceLoader.CACHE_MODE_IGNORE` and assigns that path-backed resource to `LightmapGI.light_data`.

The editor bake path also validates before invoking Godot's LightmapGI editor action that:

- `light_data` exists;
- its path is non-empty;
- the path is under `res://`;
- the path exists;
- the path matches Terrain3D's prepared working bake path.

This changes the workflow from "prepare an anonymous resource and hope the editor infers the output" to "prepare a path-backed resource and invoke the editor baker against that exact working artifact."

## Output path correction

Regional LightmapGI output is normalized with `ProjectSettings.localize_path()` and must resolve inside `res://`. This is required for the project-resource/import pipeline used by generated lightmap textures.

`prepare_region_bake()` now normalizes and creates the output directory before deriving region-specific output paths. This prevents the region path table from being built from an unnormalized absolute or stale configured path.

## Shader fix

Changed:

```glsl
ivec2 half_map = ivec2T3D_REGION_MAP_HALF;
```

to:

```glsl
ivec2 half_map = ivec2(T3D_REGION_MAP_HALF);
```

The former is parsed as an identifier, not an `ivec2` constructor invocation.

## 16-bit atomic fix

The MSVC implementation in `terrain_3d_sync.h` now admits two-byte atomic types and supplies explicit 16-bit intrinsic paths for:

- load / compare-exchange: `_InterlockedCompareExchange16`;
- exchange: `_InterlockedExchange16`;
- fetch-add/sub: `_InterlockedExchangeAdd16`;
- fetch-or: `_InterlockedOr16`;
- fetch-and: `_InterlockedAnd16`.

`t3dAtomicU16_t` is also provided alongside the existing U8/U32/U64 aliases.

This is not merely future API completeness: `terrain_3d_resident_pool.h` already uses `t3dAtomic_t<int16_t>`, so the old MSVC size assertion was incompatible with an existing runtime type.

## Validation

The patch includes `tools/verify_lightmap_shader_atomic_fix.py` to lock the three repairs in place. A standalone C++ atomic exercise covers unsigned and signed 16-bit operations through the generic non-MSVC implementation. The MSVC-specific intrinsic selection is statically verified in this environment because a Windows MSVC toolchain is not available here.

No Godot editor executable is present in the supplied workspace, so the final LightmapGI bake must still be exercised in the actual Godot 4.7.2 editor after rebuilding the extension.
