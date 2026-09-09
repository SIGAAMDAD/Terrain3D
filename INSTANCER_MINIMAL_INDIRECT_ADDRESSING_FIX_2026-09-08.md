# Terrain3D Instancer Minimal Indirect Addressing Fix

Date: 2026-09-08
Build ID: `instancer-minimal-indirect-addressing-fix-2026-09-08`

## Runtime evidence from the supplied log

The supplied editor log proves that authored instance state is present and mutating correctly. Region `(8, 5)` reaches 418 resident instances, including mesh asset ID 1 (`grass_fountain_tall`) and the asset-level instance count continues increasing during painting.

The log also shows repeated RD editor-preview upload failures for mesh assets 0 and 1, but no live `Forward+ PREPARE failed` message. This means the logical instance store is not the point where the live render lane disappears.

## Verified minimal-path regression

The minimal bring-up configuration deliberately enables CPU/reference visibility packet construction by setting the instance renderer's direct-debug flag. That flag had two unrelated effects:

1. select CPU/reference visible-index and packet construction;
2. replace indexed-indirect raster submission with `RenderingDevice::draw_list_draw()`.

Effect 2 is invalid for the current mesh cache architecture.

`Terrain3DRdMeshCache` packs mesh surfaces into shared vertex/index arenas. The CPU packet builder correctly creates an indexed-indirect command containing:

- `index_count`;
- `instance_count`;
- `first_index`;
- `vertex_offset`;
- `first_instance`.

`uploadVisibility()` correctly uploads that command to the indirect buffer.

The old minimal draw path then discarded the command and called:

```cpp
pRd->draw_list_draw( nDrawList, true, rPacket.m_nVisibleCount, 0u );
```

That API has no packet-specific `first_index` or `vertex_offset`. With a shared arena it cannot address the selected cached surface correctly. The minimal path therefore broke the exact addressing information it was meant to preserve.

## Fix

Minimal mode now keeps the CPU/reference visibility builder but always submits the already-built indexed-indirect command.

The instance shader's visible-index addressing is also kept coherent:

- plain direct debug: push `visible_base = packet.first_visible`, draw base instance = 0;
- indexed-indirect: push `visible_base = 0`, command `first_instance = packet.first_visible`.

This avoids double-offsetting `gl_InstanceIndex` while preserving per-surface arena offsets.

Production/non-minimal behavior is unchanged.

## Log-noise cleanup

The supplied log contained 980 `Terrain3DUtil::printArr()` lines from full undo/redo instance-array dumps. Those dumps now require `EXTREME` logging rather than normal DEBUG level.

The minimal lane intentionally disables instance depth submission, so the repeated native-depth-prepass fallback warning is also suppressed while the minimal compile switch is enabled.

The Jolt/editor `Space state is inaccessible right now` / `Invalid PhysicsDirectSpaceState3D` messages are a separate editor raycast issue and are not part of the instance raster failure.

## Verification

C++17 syntax/front-end compile passed for:

- `terrain_3d_rd_instance_renderer.cpp`;
- `terrain_3d_render_coordinator.cpp`;
- `terrain_3d_editor.cpp`.

Only the existing generated godot-cpp `INT64_MIN` literal warning remains.

Focused cumulative verification passed for:

- minimal instancer path;
- indirect buffer usage;
- Forward+ instance submission/transparency;
- first-population rendering;
- pipeline recovery/depth fallback;
- preview/lazy Forward resources;
- Forward+ viewport lifecycle;
- lane diagnostics;
- pass prerequisites;
- P09 GPU indirect visibility;
- P11 GPU instance culling;
- native/GDScript contract;
- GPU page non-destructive layout;
- P10 VT streaming cumulative contract;
- systems-C style;
- systems-C ownership.

A real Godot viewport smoke test is still required to establish whether this is the final runtime blocker. The supplied log and source do establish that the previous minimal submission path was structurally invalid for shared mesh arenas.
