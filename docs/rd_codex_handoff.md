# RD clean-break: compact Codex handoff

Use this file instead of rereading the full completion contract on every task.

## Repository identity

- Required branch: `experiment/rendering-device`
- Frozen reference tag: `rd-alpha-pre-cleanbreak-reference`
- Frozen reference commit: `5d9e8b1c264867e147d5b9d1992acb883f82adb5`
- Response-1 baseline commit: `fbba7511ade7bfbe43e6532bb1e8fe073f88732b`
- Target engine: Godot 4.7.1 stable
- C++ binding source: vendored godot-cpp 10.0.0-rc1 seeded with `thirdparty/godot_api/extension_api-4.7.1.json`
- Commit identity: `Noah Van Til <nyvantil@gmail.com>`

## What already exists

The current alpha already contains useful RD terrain work:

- canonical packed clipmap topology;
- 8-byte clipmap vertices;
- 16/32-bit topology-index selection;
- near/middle/far raw RD terrain shaders;
- RD frame/material/patch buffers;
- compute terrain frustum culling;
- indexed indirect terrain drawing;
- compositor integration and partial framebuffer/view lifecycle;
- partial RD diagnostics/accounting.

Keep and harden those systems rather than restarting the terrain renderer.

## What still makes the alpha non-compliant

- `Terrain3DRdContext` is not yet the complete GPU ownership layer.
- frame snapshots still use a mutex-protected mutable publication model;
- Terrain3D-generated textures/caches still cross RenderingServer ownership/bridging;
- `Terrain3DMaterial` still publishes native Shader/ShaderMaterial state;
- regular instances still use MultiMesh/native render instances;
- HLOD still uses native RenderingServer instances;
- native shadow proxies/fallback backend concepts still exist;
- direct RD geometry does not yet have exact Forward+ depth/light/shadow/motion/GI/decal/probe participation;
- editor overlays/previews still depend on native material/rendering paths;
- C#/GDScript still expose stale backend/proxy assumptions;
- RD-specific automated tests and final runtime/resource plateau validation remain incomplete.

## Execution order

The detailed 19-response roadmap is grouped into 8 Codex batches to avoid context resets:

A. RD ownership + immutable snapshots + terrain topology/culling hardening.
B. Direct RD terrain textures/caches + compute ownership.
C. Material GPU ABI + full near/middle/far material/shader parity.
D. RD mesh cache/arenas + regular instance storage/culling/indirect rendering.
E. HLOD RD conversion using the same mesh cache/material system.
F. Godot 4.7.1 Forward+ submission bridge + shadows/lighting/depth/motion + viewport lifecycle.
G. Editor/preview RD rendering + public API/GDScript/C# obsolete-backend cleanup.
H. Non-rendering regression, tests/builds, runtime dataset traversal, memory/performance plateau, final static/Git/archive audit.

The exact mapping and gates are in `docs/rd_codex_batches.tsv`.

## Commands

Before editing a batch:

```text
python3 tools/rd.py next
python3 tools/rd.py context A
```

One-time environment bootstrap:

```text
python3 tools/rd.py bootstrap
```

Fast edit/compile loop:

```text
python3 tools/rd.py fast
```

Batch completion gate:

```text
python3 tools/rd.py full
```

Exact contract text for only the active batch:

```text
python3 tools/rd.py contract A
```

Static migration view:

```text
python3 tools/rd.py audit
```

The wrapper locates SCons and Godot when possible. `RD_GODOT=/absolute/path/to/Godot_v4.7.1-stable_linux.x86_64` overrides Godot discovery.

## Context discipline

Prefer the batch context output and source map. Read the full 1,197-line completion contract only for the active contract sections. Do not repeatedly ingest historical audit documents unless the current code contradicts the compact state above.

At the end of a batch, update only the state/evidence columns in the ledgers, run the gate, commit, and move to the next batch. Keep the user-facing report short.
