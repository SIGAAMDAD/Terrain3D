# RenderingDevice Alpha Test Snapshot

Branch: `experiment/rendering-device-alpha`
Target: Godot 4.7.1

This repository was reconstructed from the latest retained cumulative Terrain3D RenderingDevice source artifact in the conversation so it can be tested directly.

## Test focus

- Enable the Terrain3D addon in `project/`.
- Exercise `RenderingServer`, `RenderingDevice Direct`, and `RenderingDevice Hybrid` backends.
- The direct RD implementation lives under `src/rendering/rd/` and `project/addons/terrain_3d/rd/`.
- Shader-preprocessor repairs are included in the ordinary Terrain3D shader path.

## Known validation boundary

This snapshot has not been claimed as a completed GPU-frame-validated final alpha. Use it as the current testable source state and report exact compile/runtime diagnostics from Godot 4.7.1.
