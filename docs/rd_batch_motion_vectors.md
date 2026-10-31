# Batch F5 - Native motion vectors and previous-frame state

## Status

F5 makes Terrain3D terrain, regular instances, and HLOD participate in Godot 4.7.1 Forward+'s native motion-vector path. The native velocity attachment remains Godot-owned; Terrain3D submits velocity during the bridge motion pass and does not create a private temporal buffer.

## Bridge ABI 5

ABI 5 retains the synchronous PREPARE/SUBMIT contract and adds the temporal state needed by the motion shaders:

- current and previous TAA jitter;
- the native `calculate_motion_vectors` history-valid state;
- current and previous per-view view-projection matrices after Godot's depth correction and frame-specific jitter have been applied;
- motion-pass depth-prepass state so the Terrain3D redraw matches native `EQUAL`/no-depth-write behavior when the prepass is active.

The extension converts this packet into the typed `t3dRdForwardPassContext_t` once. Shadow PREPARE callbacks do not advance or reset scene motion history.

## Velocity convention

Terrain3D matches Godot 4.7.1's Forward+ convention exactly:

```text
current_clip  = current_screen.xy / current_screen.w - current_taa_jitter
previous_clip = previous_screen.xy / previous_screen.w - previous_taa_jitter
velocity      = (previous_clip - current_clip) * 0.5
```

This is previous UV minus current UV. The output is written to the native motion attachment at fragment location 2.

## Terrain history and clipmap snapping

Terrain clipmap recentering is a representation change, not physical terrain motion. The previous screen position is therefore computed from the same stable current world-space terrain surface point transformed by the previous camera view-projection matrix. Previous clipmap ring origin/scale fields are not used to synthesize physical motion.

When camera history is invalid, the previous clip position is set equal to the current clip position, yielding neutral velocity for the reset frame.

## Regular-instance and HLOD history

`Terrain3DRdInstanceRenderer` keeps a persistent packed previous-record stream and a persistent GPU previous-record buffer. It reuses the existing cell identity rather than allocating a per-frame hash table.

Regular-instance correspondence requires matching region/cell identity, mesh asset identity, and instance count. A regular LOD change does not discard transform history. HLOD correspondence additionally requires the same source key and build revision; a replacement/residency transition is initialized from its current transforms and therefore emits neutral velocity rather than fabricated movement.

After a rendered history frame, stopped objects advance previous=current once so the same non-zero vector cannot repeat indefinitely. New cells, camera-history invalidation, and replacement HLOD cells reset locally to current transforms.

## Native motion submission

Near/middle/far terrain and regular-instance/HLOD renderers select the existing motion shader variants when `T3D_RD_FORWARD_PASS_MOTION` arrives. PREPARE performs any required frame/history upload before Godot opens the list. SUBMIT binds the already-created pipeline/resources and draws into the supplied native list.

The motion redraw retains the same material alpha-scissor/discard behavior and native Forward+ direct lighting state as the native color path. When depth prepass is active, the motion pipeline uses `COMPARE_OP_EQUAL` with depth writes disabled.

No Terrain3D motion draw path begins or ends its own draw list, and no private velocity texture is allocated.

## Deterministic debug mode

Set:

```text
TERRAIN3D_RD_DEBUG_MOTION=1
```

to encode the generated velocity into the motion shader's color output for deterministic inspection when a patched-engine runtime is available. Runtime diagnostics report native motion-pass callback count, whether native motion has actually been observed, instance-history reset/match counters, and the debug toggle.

## Validation performed

- affected C++ translation units compile against generated Godot 4.7.1 GDExtension bindings;
- near/middle/far terrain plus instance RD shader variants, including motion variants, compile through the supplied Godot 4.7.1 RDShaderFile importer;
- the F5 static contract verifies ABI 5 temporal fields, native motion hook ownership, Godot velocity convention, terrain clipmap-snap neutrality, persistent instance/HLOD history, reset behavior, native attachment use, depth-prepass compatibility, and explicit debug mode;
- the engine patch parses with `git apply --stat`.

A patched Godot 4.7.1 Forward+ binary is still required for scene-level TAA/upscaler and velocity-buffer visual validation. The supplied stock binary can compile the shaders but cannot execute the bridge.

## Next chunk

F6 owns the required GI/lightmap/decal/reflection-probe Forward+ integrations. F7 owns multiview, resize/render-scale/MSAA, multiple-camera history isolation, and native-RID lifecycle convergence.
