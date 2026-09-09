# Terrain3D Forward+ Color Blend Attachment Contract Fix — 2026-09-09

## Failure

Godot 4.7.2 rejected Terrain3D transparent instance pipelines with:

```text
Condition "p_blend_state.attachments.size() < pass.color_attachments.size()" is true.
```

The instance scene and mesh publication were already valid (`records > 0`, `vertex bytes > 0`, `LODs > 0`, `surfaces > 0`, `draw_packets > 0`). The failure was isolated to raster-pipeline creation.

## Root cause

Godot Forward+ creates its ordinary color framebuffer with three logical color slots:

1. color;
2. separate specular;
3. motion vectors.

When separate specular or motion vectors are disabled, those slots remain present in the framebuffer pass as `ATTACHMENT_UNUSED` entries.

`RenderingDevice::render_pipeline_create()` validates that `PipelineColorBlendState.attachments` is at least as large as the complete logical `FramebufferPass::color_attachments` array before checking which slots are unused.

Terrain3D's transparent instance path incorrectly reduced its blend-state array to one attachment because the transparent shader writes only location 0. This made the pipeline incompatible with Godot's three-slot Forward+ framebuffer contract.

The ocean transparent path had the same latent defect.

## Fix

### Instance renderer

Transparent Forward+ instance pipelines now provide three blend-state attachments. Only attachment 0 enables alpha blending. Attachments 1 and 2 remain non-blended placeholders matching the native Forward+ logical framebuffer layout.

### Ocean renderer

The transparent ocean pipeline now follows the same three-slot contract so the defect cannot reappear when ocean rendering enters the native transparent pass.

### Build provenance

Native/editor build ID:

```text
forward-color-blend-attachment-contract-fix-2026-09-09
```

## Validation

- Focused Forward+ blend attachment verifier: PASS.
- Instance Forward+ submission verifier: PASS.
- P0 ArrayMesh surface publication verifier: PASS.
- P1 memory-budget containment verifier: PASS.
- P2 instance-local active-storage verifier: PASS.
- GPU visibility / indirect submission verifiers: PASS.
- Runtime-page verifiers: PASS.
- Native/GDScript contract verifier: PASS.
- Systems-C ownership/style verifiers: PASS.
- Terrain-primary render-lane verifier: PASS.
- `clang++ -std=c++17 -DGDEXTENSION -DDEBUG_ENABLED -DTOOLS_ENABLED -fsyntax-only` passes both modified rendering translation units using the supplied godot-cpp, EASTL and EABase headers.

A complete SCons/MSVC DLL link and live Godot editor smoke test were not available in this environment.
