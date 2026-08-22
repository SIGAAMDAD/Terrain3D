# Terrain3D RenderingDevice clean-break instructions

This repository is the implementation workspace for the clean-break RenderingDevice branch.

## Start here

Do not rediscover the project from scratch on every task.

1. Run `python3 tools/rd.py next`.
2. Run `python3 tools/rd.py context <batch>` for the batch it reports.
3. Read only the files listed for that batch plus directly relevant callers.
4. Run `python3 tools/rd.py contract <batch>` when exact acceptance criteria are needed. Open the full contract only if an ambiguity remains.

Canonical short context:

- `docs/rd_codex_handoff.md`
- `docs/rd_codex_batches.tsv`
- `docs/rd_contract_ledger.tsv`
- `docs/rd_source_map.tsv`
- `docs/rd_reference/reference_feature_matrix.md` only when parity details are needed

The frozen native/RD-alpha reference is tag `rd-alpha-pre-cleanbreak-reference` at commit `5d9e8b1c264867e147d5b9d1992acb883f82adb5`.
The implementation branch must remain `experiment/rendering-device`.
Never modify or move `main` or the frozen reference tag.

## Non-negotiable architecture

- Terrain3D owns its production rendering through RenderingDevice.
- RenderingServer may remain only as the minimal Godot gateway needed to obtain/schedule access to the global RenderingDevice when no direct GDExtension equivalent exists.
- Do not create Terrain3D geometry, instances, textures, materials, shadow proxies, or fallback rendering through RenderingServer.
- No MultiMesh runtime renderer remains in the completed branch.
- No Direct/Hybrid/native-proxy compatibility renderer remains in the completed branch.
- `Terrain3DRdContext` is the explicit GPU ownership/co-ordination boundary.
- Ordinary frame consumption uses immutable published render state; no render-thread mutex in the steady-state frame path.
- Do not create vertex buffers, index buffers, shaders, pipelines, uniform sets, or mesh allocations during stable camera traversal after warmup.
- Preserve the reference branch's public/non-rendering behavior unless the completion contract explicitly removes an obsolete backend/proxy API.
- Never delete a feature merely because its old implementation uses RenderingServer.
- Do not introduce a second parallel RD abstraction when an existing RD owner can be extended.

## Source and style

- Follow `.clang-format` and the existing subsystem layout in `src/README.md`.
- New native implementation files go in the subsystem that owns them; do not repopulate the `src` root.
- Infer behavior from the frozen reference and current callers before deleting or renaming APIs.
- Preserve unrelated user changes.
- Do not add compatibility shims for the removed renderer. Update callers to the clean-break path.

## Validation loop

One-time workspace setup:

`python3 tools/rd.py bootstrap`

During implementation:

`python3 tools/rd.py fast`

At a completed Codex batch:

`python3 tools/rd.py full`

Use `python3 tools/rd.py audit` for the concise forbidden-symbol report. Counts may decrease during migration; they must never increase without an explicit reason. The final clean-break audit is stricter than the intermediate baseline.

If an external dependency prevents one validation step, report that exact step and continue with independent checks. Do not convert static inspection into a claim that code compiled or ran.

## State tracking

`docs/rd_codex_batches.tsv` is the high-level execution ledger. Update its state only after the batch gate is satisfied. `docs/rd_contract_ledger.tsv` maps all 47 completion-contract sections to those batches.

Do not declare the branch complete until the final batch passes the full contract, runtime/resource plateau checks, independent archive extraction, `git diff --check`, and `git fsck --full --strict`.

## Codex response economy

Work in the repository rather than narrating intermediate reasoning. Final task reports should be compact: commit SHA, batch/state, validations run, exact blockers, and the next batch. Do not paste large diffs, source files, or repeated architecture summaries unless explicitly requested.

All implementation commits must use:

- author/committer: `Noah Van Til <nyvantil@gmail.com>`
