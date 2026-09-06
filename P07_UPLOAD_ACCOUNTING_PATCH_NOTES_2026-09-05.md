# Terrain3D P07 Upload Accounting / Throttling Patch

**Date:** 2026-09-05
**Base:** P01 -> P06 regression-fixed package
**Scope:** P07 only. P08 RenderWorld prepare/reuse is intentionally absent.

## Integration rule

P07 was adapted onto the repaired P06 ownership/capacity tree rather than copied wholesale from the older cumulative P06/P07/P08 archive. The repaired P06 generation-gating semantics and exact physical GPU page-grant logic are preserved.

The P06 regression invariants remain binding:

- semantic logical-material -> physical-layer lookup;
- material-state generation coverage of runtime publication state;
- dynamic tier changes advance RD material generation;
- regional-lighting resource replacement advances its global generation;
- preview/nonpersistent mesh changes advance mesh source generation;
- startup page/material state converges without requiring an edit.

## P07 changes

1. **Exact finite mip accounting**
   - RGBA8 mip chains use the exact finite sum rather than a `4/3` approximation.
   - R8 terrain-shadow mip chains use the exact finite sum.
   - The repaired P06 resident-layer budget continues to include Height, Control, optional Color, optional Derived, Middle composite, and terrain-shadow resources.

2. **Complete-page upload accounting**
   - Middle composite updates are counted as texture update calls.
   - Middle composite bytes are tracked separately.
   - Terrain-shadow bytes are tracked separately.
   - Full-page admission includes Height + Middle composite + terrain shadow plus enabled/required optional channels.

3. **Elapsed-time upload throttling**
   - Byte and texture-update-call budgets are represented as retained token buckets.
   - Tokens refill from elapsed steady-clock time at the configured 60 Hz budget rate.
   - Admission checks use available tokens before publication.
   - Actual bytes/calls are debited after the attempted upload; the first page-table publication call is reserved/debited explicitly.

4. **Prepared-page authority**
   - With cooked-page residency enabled, prepared pages are authoritative for full-page publication in runtime and editor.
   - Missing, malformed, or incompatible prepared full pages fail closed for that service attempt.
   - The hot GPU publication path does not recursively fall back into synchronous live control/composite synthesis.

5. **Telemetry**
   - Added Middle-composite update/byte counters.
   - Added terrain-shadow byte counters.
   - Added current upload byte/call token counters to detailed GPU-cache stats.

## Fixed 128m complete-page reference

For a 128 x 128 full-detail page with Height R16, Derived RG, Control RF, Color RGBA8 mip chain, Middle RGBA8 mip chain, and Shadow R8 mip chain:

```text
Height:             32,768 bytes
Derived:            32,768 bytes
Control:            65,536 bytes
Color mip chain:    87,380 bytes
Middle mip chain:   87,380 bytes
Shadow mip chain:   21,845 bytes
--------------------------------
Total:             327,677 bytes
```

## Validation targets

- `tools/verify_p06_generation_gates.py`
- `tools/verify_p07_upload_accounting.py`
- `tools/verify_p03_p04_p05_performance_contracts.py`
- owner-controlled allocation A/B/C, D/E/F, and G gates
- READY/cold-phase boundary gate
- initialization/residency ownership gate
- RD startup/capacity/shader-bound gate
- syntax compilation of the modified GPU-cache translation unit

The decisive runtime smoke test remains startup without editing: Near/control material state must be correct on first convergence, pending cooked pages must publish without waking only after an edit, and byte/call throttling must converge residency without per-frame burst-reset behavior.

## Validation performed

- P06 generation-gate regression verifier: PASS.
- P07 upload-accounting verifier: PASS.
- P03/P04/P05 contract verifier: PASS.
- P01/P02 shell contract gate: PASS.
- Owner-controlled allocation A/B/C, D/E/F, and G gates: PASS.
- READY/cold-phase boundary verifier: PASS.
- Initialization/residency ownership verifier: PASS.
- RD startup/capacity/shader-bound verifier: PASS.
- Systems-C ownership/style gates: PASS.
- Standalone Terrain3D memory-budget contract: PASS after removing the P07 mip helper's accidental dependency on the Godot `MAX` macro.
- Direct syntax compilation of `src/streaming/terrain_3d_gpu_region_cache.cpp`: PASS; only the pre-existing generated godot-cpp `INT64_MIN` warning is emitted.
- Broad source syntax sweep was attempted; the environment stopped it after 85 translation-unit log files had been created. No error-bearing log was produced before the timeout. This is not reported as a completed full build.
- P08-only RenderWorld prepare/reuse markers: absent.
