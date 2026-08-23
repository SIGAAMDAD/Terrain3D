# Batch F1 — Forward+ bridge ABI and engine hook

## Status

F1 establishes the synchronous integration boundary between Godot 4.7.1 Forward+ and Terrain3D-owned RenderingDevice resources. It is deliberately plumbing-only: F1 does not claim native terrain depth, final shadow rendering, clustered lighting, or motion-vector output.

## Ownership boundary

```text
Godot Forward+ owns native pass + draw list + scene data
                    |
                    v
        Terrain3DForwardPlusBridge ABI 2
                    |
                    v
 Terrain3DRdForwardPlusBridgeAdapter
                    |
                    v
      Terrain3DRenderCoordinator
                    |
                    v
      Terrain3D-owned RD resources
```

The engine bridge owns no Terrain3D GPU resource. The extension borrows native RIDs only for the synchronous callback lifetime.

## Current ABI 2 pass kinds

| Value | Kind | F1 hook |
|---:|---|---|
| 0 | invalid | no |
| 1 | depth | yes |
| 2 | opaque | yes |
| 3 | shadow | yes |
| 4 | motion | yes |
| 5 | material | reserved for later Batch F |
| 6 | SDF | reserved for later Batch F |
| 7 | GI | reserved for later Batch F |
| 8 | reflection | reserved for later Batch F |

## Current ABI 2 packet

The engine sends one `Dictionary`; the extension validates and converts it once into `t3dRdForwardPassContext_t`.

Required typed state includes:

- `bridge_abi`, callback `phase`, `pass_kind`, native `pass_mode`, native `draw_list`, `framebuffer_format`, native framebuffer RID;
- `view_count`, `scene_pass`, render region/size, color-pass flags, reverse-cull state, reflection-probe state;
- current/previous camera transform, per-view current/previous projection, eye offsets, visible-layer mask;
- cluster size/max-elements and cluster buffer;
- directional/omni/spot/area light buffers;
- native shadow atlas, directional shadow atlas, compatible shadow sampler;
- lightmap and lightmap-capture buffers;
- reflection-probe buffer, reflection atlas and radiance texture;
- decal buffer plus linear/SRGB decal atlases and area-light atlas;
- native base and render-pass uniform sets for diagnostics/compatible later integration.

Malformed packets, unsupported pass kinds, out-of-range view counts, bad matrix-array types, and ABI mismatch are rejected before the coordinator sees them.

## Callback lifetime

Engine registration is serialized off the render hot path. Normal pass invocation reads one immutable callback slot atomically and calls it synchronously. Unregistration atomically removes that slot, Terrain3D synchronizes the render server, then the engine deletes retired slots. This avoids a renderer-thread mutex and prevents an extension `Callable` from surviving library teardown.

The extension itself registers one engine callback and fans out to a bounded array of coordinator pointers. Attach/detach uses atomics; coordinator destruction performs a render-server sync after removing its pointer so an in-flight callback cannot dereference freed coordinator state.

## Production and diagnostic routes

Production RD routing requires `Terrain3DForwardPlusBridge` ABI 2. A missing/wrong bridge disables production RD submission and records the reason. No native Terrain3D surface or shadow proxy is resurrected to conceal that failure.

The legacy compositor callback is disabled by default. `TERRAIN3D_RD_DEBUG_COMPOSITOR=1` is the explicit diagnostic opt-in and is never treated as native Forward+ parity.

## Engine source target

`engine_patch/godot-4.7.1-terrain3d-forward-plus.patch` targets only:

```text
Godot tag:    4.7.1-stable
Godot commit: a13da4feb8d8aefc283c3763d33a2f170a18d541
Bridge ABI:   2
```

The patch hooks real Forward+ draw-list call sites and deliberately avoids compositor timing. The shadow hook is explicit rather than inferred from the generic shadow pass mode so particle-collision heightfield draws remain unrelated.

## F1 evidence

- extension ABI/context: `src/rendering/rd/terrain_3d_rd_types.h`;
- adapter/lifetime/packet validation: `src/rendering/rd/terrain_3d_rd_forward_plus_bridge.*`;
- coordinator entry point/routing/diagnostics: `src/rendering/rd/terrain_3d_render_coordinator.*`;
- diagnostic compositor default-off behavior: `src/rendering/rd/terrain_3d_rd_compositor_effect.cpp`;
- engine singleton + native hook package: `engine_patch/godot-4.7.1-terrain3d-forward-plus.patch`;
- application/build/ABI contract: `engine_patch/README.md`;
- regression gate: `tools/rd.py` (`rd_batch_f1_forward_bridge_check`).

F2 superseded the original F1 ABI with ABI 2 by adding synchronous PREPARE/SUBMIT phases required for legal buffer/culling work before the native list opens. F1 ownership and callback-lifetime guarantees remain unchanged.
