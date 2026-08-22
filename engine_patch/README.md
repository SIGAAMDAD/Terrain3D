# Godot 4.7.1 Forward+ bridge patch

Batch F owns this directory. Do not vendor a Godot source checkout here.

Target upstream tag: `4.7.1-stable` (`a13da4f...`). Source checkout belongs under `.rd_tools/godot-4.7.1-src` or `RD_GODOT_SOURCE`.

Commit only the minimal patch/source additions, an application/build note, and any bridge-specific validation harness required to reproduce the patched editor. Terrain3D must retain ownership of its RenderingDevice resources; engine changes are limited to explicit submission/integration into the native Forward+ stages required by the completion contract.
