# RD clean-break: compact Codex Ultra handoff

Use this file only when AGENTS.md does not already answer the question.

## Identity
- Branch: `experiment/rendering-device`
- Frozen reference: `rd-alpha-pre-cleanbreak-reference`
- Target: Godot 4.7.2 Mono/Forward+, Windows x86-64
- Local Godot source root: `D:\godot-4.7.2-stable` (or `RD_GODOT_SOURCE`)
- Hardware: Core Ultra 7 258V / Arc 140V
- Commit identity: `Noah Van Til <nyvantil@gmail.com>`

## Resume with minimum context

```bat
tools\rd.cmd start
tools\rd.cmd context <BATCH>
```

Only if exact acceptance text is needed:

```bat
tools\rd.cmd contract <BATCH>
```

Do not routinely read the full contract, public API inventory, manifests, or historical audit reports.

## Current resume point
- Batches A-G complete; Batch F now uses the exact external Godot 4.7.2 ABI-10 bridge.
- Next work: Batch H validation and packaging.

## Remaining architecture
A: RD ownership + immutable snapshots + terrain topology/culling proof.
B: direct RD terrain textures/caches + compute ownership.
C: fixed material ABI + full near/middle/far shader/material parity.
D: RD mesh cache/arenas + MultiMesh-free regular instancing.
E: HLOD on the shared RD mesh/material path.
F: exact Godot 4.7.2 ABI-10 Forward+ bridge + shadows/lighting/depth/motion/view lifecycle.
G: editor/previews + native/GDScript/C# obsolete-backend cleanup.
H: non-rendering regression + complete builds/tests/runtime/memory/performance/final package audit.

Full compact implementation rules: `CODEX_ULTRA_IMPLEMENTATION.md`.

## Validation

```bat
tools\rd.cmd bootstrap
tools\rd.cmd fast
tools\rd.cmd full
```

GPU checks are separate from routine compile/import work:

```bat
tools\rd.cmd gpu-smoke vulkan
tools\rd.cmd gpu-smoke d3d12
```

For startup/crash isolation:

```bat
tools\rd.cmd diagnose-godot
```

Batch F only:

```bat
tools\rd.cmd engine-source --bootstrap
```

Batch H validation data:

```bat
tools\rd.cmd fixture --verify
```

Final:

```bat
tools\rd.cmd audit --final
tools\rd.cmd final
```

Build and Godot output is captured under `.rd_logs`; successful wrappers stay terse and failures print only a tail.
