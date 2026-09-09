# Terrain3D Instance Indirect Rendering Fix

**Date:** 2026-09-07  
**Base:** P01-P12 plus runtime-page v7 header-padding repair  
**Scope:** Direct RenderingDevice instance/HLOD rendering only. No GDScript API changes.

## Symptom

Terrain instance data can exist and publish normally, while regular Terrain3D instances do not appear in the native Forward+ path. The defect survives the earlier first-population, target-synchronization, dirty-range, and GPU-culling repairs because it is below those systems at the RenderingDevice resource-creation boundary.

## Root cause

`Terrain3DRdInstanceRenderer::ensureBuffer()` classified the main and shadow command buffers as `RESOURCE_INDIRECT_BUFFER`, but created them as ordinary storage buffers:

```cpp
rRid = rd->storage_buffer_create( capacity );
```

Both buffers are later consumed by `RenderingDevice::draw_list_draw_indirect()`. Godot's RenderingDevice requires storage buffers used as native indirect argument buffers to be created with `STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT`, which maps the resource to the backend indirect-buffer usage capability.

The terrain clipmap renderer and RenderWorld resource owner already create their indirect buffers with this usage bit. The instance renderer was the inconsistent path.

P11 makes the defect especially destructive: the GPU culling compute pass writes `instance_count` and visible-instance ranges successfully into storage-visible memory, but the subsequent native indirect draw consumes a buffer that was never created for indirect command use. Static source checks and ordinary C++ compilation cannot detect that resource-usage mismatch.

The same incorrect allocation was already present in the P10 instance renderer, so this is not a P12 HLOD/node-streaming regression.

## Fix

The typed indirect-buffer branch now creates the resource as:

```cpp
rRid = rd->storage_buffer_create(
        capacity, PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT );
```

Because both `m_hIndirectBuffer` and `m_hShadowIndirectBuffer` already route through the typed `RESOURCE_INDIRECT_BUFFER` branch, the correction applies to both production instance draws and shadow instance draws without duplicating allocation policy.

No new backing store, hot-path allocation, readback, fallback renderer, or per-frame branch was added.

## Audit findings

The publication path was traced from `Terrain3DInstancer::updateMmiCell()` through `buildRdInstanceSnapshot()`, `Terrain3DRenderCoordinator::publishSnapshot()`, `Terrain3DRdInstanceRenderer::uploadScene()`, P11 GPU culling, and `drawForwardView()`.

The following previously repaired contracts remain present:

- first-population resident-page promotion;
- single-LOD target synchronization;
- owner-published P11 dirty ranges/cells;
- fixed-capacity instance publication banks;
- P11 GPU visibility descriptor construction and compute dispatch;
- main/shadow indirect command generation;
- terrain-primary failure isolation.

A separate existing limitation remains in the native Forward+ path: alpha-blended instance packets are excluded from the transparent pass. Generated Terrain3D mesh materials default to alpha scissor, so that limitation does not account for the blanket instance disappearance addressed here.

## Verification

- `tools/verify_instance_indirect_buffer_usage.py`: PASS.
- Full repository verifier sweep: 31/31 PASS.
- `terrain_3d_rd_instance_renderer.cpp` accepted by `clang++ -std=c++17 -fsyntax-only` with the repository GDExtension/EASTL include surface; one pre-existing generated godot-cpp integer-literal warning remains.
- `verify_script_native_contract.py`: PASS; no GDScript API/editor contract changes were made.

No Godot executable or patched engine binary is present in this environment, so the final Forward+ runtime draw must be smoke-tested in the user's Godot build.

## Runtime confirmation target

Rebuild the GDExtension and open the same scene without forcing an instance update. Existing alpha-scissor/opaque instance populations should become visible through the normal indirect path. `TERRAIN3D_RD_INSTANCE_DIRECT_DRAW=1` remains useful as a comparison lane because it bypasses GPU visibility/indirect submission; it should no longer be required for ordinary rendering after this fix.
