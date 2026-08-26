# Terrain3D RD clean-break — Codex Ultra instructions

## Mission
Finish `experiment/rendering-device` to the full completion contract and frozen-reference feature parity. Do not stop because terrain renders, C++ builds, shaders compile, or one batch passes.

## Start — do not rediscover the repo
On Windows run:

```bat
tools\rd.cmd start
```

Then read only the paths printed for the active batch. Use:

```bat
tools\rd.cmd contract <BATCH>
```

only when exact acceptance text is needed. The full contract and large manifests are reference material, not startup context.

Canonical compact sources:
- `CODEX_ULTRA_IMPLEMENTATION.md`
- `docs/rd_codex_batches.tsv`
- `docs/rd_contract_ledger.tsv`
- `docs/rd_source_map.tsv`
- `docs/rd_reference/reference_feature_matrix.md` only for parity questions

Frozen reference: tag `rd-alpha-pre-cleanbreak-reference` at `5d9e8b1c264867e147d5b9d1992acb883f82adb5`.
Required branch: `experiment/rendering-device`.
Never move `main` or the frozen tag.

## Windows environment
- Target: Windows x86-64, Godot 4.7.2 Mono/Forward+.
- Default Godot source root: `D:\godot-4.7.2-stable` (override with `RD_GODOT_SOURCE`).
- Hardware validation target: Intel Core Ultra 7 258V / Arc 140V.
- Prefer the Godot `_console.exe` binary for automation when present.
- Use the repository SCons cache and incremental builds. Do not clean/rebuild unless dependency state requires it.
- Batch F uses an exact Godot `4.7.2-stable` source tree supplied through `RD_GODOT_SOURCE`.

## Locked architecture
- Terrain3D production rendering is RenderingDevice-owned.
- RenderingServer may remain only as the minimum gateway to the global RenderingDevice where Godot exposes no direct GDExtension equivalent.
- No Terrain3D RS geometry, instance, texture, material, shadow proxy, Hybrid path, or hidden fallback.
- No MultiMesh runtime renderer at completion.
- `Terrain3DRdContext` is the explicit GPU ownership/co-ordination boundary.
- Steady-state render consumption uses immutable published snapshots; no render-thread mutex.
- No steady-state VB/IB/shader/pipeline/uniform-set/mesh allocation after warmup.
- Regular instances and HLOD share the RD mesh/material cache unless measured lifetime constraints justify separate arenas.
- Preserve reference functionality and non-rendering behavior. Never delete a feature because its reference implementation is RS-backed.
- Exact Forward+ parity requires the independently maintained Godot 4.7.2 ABI-10 submission bridge specified in Batch F. This repository owns the matching consumer and external ABI contract, not an embedded engine patch.

## Token discipline — mandatory
- Never `cat`/read the full completion contract, public API inventory, file manifests, or historical reports as routine context.
- Never rescan the whole repo to learn architecture already present in `docs/rd_source_map.tsv`.
- Use targeted `rg -n`, `git diff --name-only`, `git show <reference>:<path>`, and narrow file ranges.
- Do not paste source, diffs, build logs, or long reasoning into chat. Logs belong in `.rd_logs/`.
- Build/test wrappers print success tersely and only tail logs on failure.
- Reuse the current workspace so cached context remains useful.
- Work through a coherent batch before reporting. Commit at batch gates, not every tiny edit.
- Final reports: <=250 words unless a hard blocker requires details. Report only commit, batch/state, checks, blocker, next batch.
- Do not ask for confirmation when code/reference evidence resolves the question.

## Validation
One-time:

```bat
tools\rd.cmd bootstrap
```

Edit loop:

```bat
tools\rd.cmd fast
```

Batch gate:

```bat
tools\rd.cmd full
```

GPU smoke, normally Vulkan first on this Intel GPU:

```bat
tools\rd.cmd gpu-smoke vulkan
```

Target D3D12 validation when the batch is GPU-relevant:

```bat
tools\rd.cmd gpu-smoke d3d12
```

Final validation fixture (Batch H only):

```bat
tools\rd.cmd fixture --verify
```

Final strict legacy-symbol audit:

```bat
tools\rd.cmd audit --final
```

`docs/rd_work/final_symbol_allowlist.tsv` is for genuinely justified final occurrences only. Do not use it to excuse old rendering architecture.

If Godot crashes, do not repeatedly relaunch the full editor. Use `tools\rd.cmd diagnose-godot`; inspect `.rd_logs`; isolate headless/import vs Vulkan vs D3D12. Continue compiler/static work while a GPU-specific failure is being isolated.
Detailed Windows crash lane: `docs/windows_godot_cli_stability.md`.

All implementation commits: `Noah Van Til <nyvantil@gmail.com>`.
