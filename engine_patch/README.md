# Terrain3D Forward+ bridge for Godot 4.7.1 stable

This directory contains the **minimal engine-side part of Terrain3D Batch F1**. It does not vendor Godot. The patch adds one versioned singleton and four synchronous Forward+ insertion points so the Terrain3D GDExtension can submit its own RenderingDevice commands while Godot owns the active native draw list.

## Exact upstream target

- Repository: `godotengine/godot`
- Tag: `4.7.1-stable`
- Commit: `a13da4feb8d8aefc283c3763d33a2f170a18d541`
- Patched base blobs: `render_forward_clustered.cpp` `370d3b66ce817d9accf17f6339625e7120a6f238`, `render_forward_clustered.h` `be97ee2d96af1e17fc9fb490cf94c91e9e2a4d31`, `register_server_types.cpp` `de403337857720babbb9f805166053b74ff4e42b`
- Terrain3D bridge singleton: `Terrain3DForwardPlusBridge`
- Bridge ABI: `5`

Do not apply this patch to another Godot revision without auditing the Forward+ call sites and bumping the bridge ABI if packet semantics change.

## Patch contents

`godot-4.7.1-terrain3d-forward-plus.patch` adds:

- `servers/rendering/renderer_rd/forward_clustered/terrain3d_forward_plus_bridge.h/.cpp`;
- singleton registration and deterministic teardown in `servers/register_server_types.cpp`;
- synchronous PREPARE callbacks immediately before selected native draw lists open and SUBMIT callbacks while those same draw lists are open;
- explicit F1 pass kinds for `depth`, `opaque`, `shadow`, and `motion`;
- reserved ABI pass kinds for `material`, `SDF`, `GI`, and `reflection` so F6 data can be carried without replacing the F1 ABI;
- native pass packet fields for draw-list/framebuffer identity, view state, current/previous camera state and jittered view-projection state, current/previous TAA jitter, native motion-history validity, exact cluster addressing parameters, directional-light count, camera depth range, cluster/light/shadow data, lightmap data, reflection data, decal data, and native base/render-pass uniform-set RIDs.

The bridge owns **no Terrain3D geometry, buffers, textures, materials, or instances**. It does not begin or end a Terrain3D draw list. Godot first invokes PREPARE so Terrain3D can update buffers, dispatch culling, and populate generation-driven pipeline caches. Godot then opens the native draw list and invokes SUBMIT while that same list is active; SUBMIT only binds/draws and never opens, closes, or mutates persistent RD resources. Godot closes the list after SUBMIT returns.
There is **no hidden or deferred callback queue**: invocation is synchronous on the renderer thread and returns before the native draw list is closed.

## Apply

From an exact Godot 4.7.1 stable checkout:

```sh
git checkout a13da4feb8d8aefc283c3763d33a2f170a18d541
git status --short
git apply --check /path/to/Terrain3D/engine_patch/godot-4.7.1-terrain3d-forward-plus.patch
git apply /path/to/Terrain3D/engine_patch/godot-4.7.1-terrain3d-forward-plus.patch
```

`git apply --check` must succeed cleanly. If it does not, stop: the source tree is not the target revision or the patch has drifted.

## Build patched Godot

Use the normal Godot 4.7.1 toolchain for the desired platform. A Linux editor build is, for example:

```sh
scons platform=linuxbsd target=editor
```

Use the project-specific build flags required by the local Godot build, but do not replace the patched renderer with Compatibility/Mobile when validating this bridge. Batch F targets Forward+.

## Runtime registration contract

At Terrain3D scene-module initialization the extension:

1. queries `Engine` for `Terrain3DForwardPlusBridge`;
2. checks `get_abi_version() == 5`;
3. verifies `register_callback`, `unregister_callback`, `has_callback`, and `collect_retired_callbacks`;
4. registers exactly one extension callback;
5. converts every accepted `Dictionary` packet into `t3dRdForwardPassContext_t` exactly once at the boundary;
6. fans the typed context out to active `Terrain3DRenderCoordinator` instances without a render-thread mutex.

Unregistration clears the engine's atomic callback pointer first. The extension then calls `RenderingServer::force_sync()` and `collect_retired_callbacks()` before releasing the `Callable`, preventing a renderer-thread callback from outliving the GDExtension library.

## F1 native hook points

The patch invokes the bridge only from the selected native `_render_list_with_draw_list()` calls for:

- depth prepass;
- opaque color pass;
- queued scene shadow pass;
- motion/velocity pass.

It does **not** infer bridge pass kinds from `PASS_MODE_SHADOW`; this avoids accidentally treating GPU-particle collision-heightfield rendering as a Terrain3D scene shadow pass.

F1 established these insertion points. F2 upgraded the bridge to ABI 2 with synchronous PREPARE/SUBMIT phases and native depth/opaque submission. F3 upgraded it to ABI 3 for native terrain/regular-instance/HLOD shadow submission and production shadow-proxy removal. F4 upgraded it to ABI 4 with exact Forward+ cluster addressing, native directional-light count/depth metadata, and native clustered directional/omni/spot/area lighting plus shadow receiving. F5 upgrades it to ABI 5 with corrected jittered current/previous view-projection state, current/previous TAA jitter, native motion-history validity, and motion-pass depth-prepass state. F6-F7 consume the remaining reserved GI/lifecycle state.

## Missing/wrong bridge behavior

Production RenderingDevice mode requires an ABI-compatible bridge. With an unpatched engine or wrong ABI Terrain3D:

- records a visible initialization/diagnostic error;
- disables the production RD submission route;
- does not silently re-enable native Terrain3D visual geometry or the shadow proxy to hide the bridge failure;
- does not claim the compositor callback provides native Forward+ parity.

The old compositor route is retained only as an explicit diagnostic lane. Set:

```text
TERRAIN3D_RD_DEBUG_COMPOSITOR=1
```

to opt into that lane for debugging. It is disabled by default and is not the production Batch F path.

## Validation notes from the F1 implementation run

The patch was authored against and source-audited against the exact `4.7.1-stable` files above. Its base `index` hashes match those upstream blob IDs, and every modified hunk site was checked against that revision's Forward+ source. `git apply --stat` accepts the patch grammar.

The implementation sandbox could not obtain a complete Godot source checkout because shell DNS access to GitHub was unavailable, so a full local `git apply --check` and patched-engine compilation were not executed there. Run the commands below on an exact checkout before treating a locally built engine binary as validated.

## Verification

From the Terrain3D repository:

```sh
python3 tools/rd.py fast --static-only
```

The F1 static contract verifies the patch package, ABI, engine hook markers, packet fields, adapter registration/lifetime behavior, coordinator entry point, and explicit compositor gating.

For full engine validation, use an exact source checkout and run `git apply --check`, build the patched Godot editor, then start Terrain3D under Forward+ and confirm diagnostics report:

- `forward_plus_bridge_present = true` (inside bridge diagnostics as `bridge_present`);
- `observed_abi = 5`;
- `callback_registered = true`;
- native pass packets arriving with non-negative draw-list IDs.

A stock/unpatched Godot 4.7.1 binary cannot execute the bridge; that failure is expected and deliberately visible.
