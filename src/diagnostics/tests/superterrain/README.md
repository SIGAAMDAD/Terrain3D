# SuperTerrain ST00 test-only contracts

This directory is the accepted Wave 1 contract baseline. It is not imported by SConstruct or production shaders; the ST05 integrator alone owns promotion into the runtime.

Run `python src/diagnostics/tests/superterrain/run_baseline.py` from the repository root. The runner checks four immutable JSON/binary snapshots, runs the Python semantic tests, and compiles/runs a standalone C++17 contract test with warnings as errors. It writes no build products into the source tree.

The `--write` switches in the four generator scripts are only for deliberate re-freezing of a changed agreed contract. Normal verification omits `--write`; it fails on input drift.

Corpus files `*.st00region` are synthetic source inputs, not cooked `.t3dvt` material pages. Source R16 height, raw RF control words, authored RGBA8 and representative derived RG8 values are stored as a fixed LE test packet; see `docs/superterrain/REFERENCE_CORPUS.md` for offsets. Material/height/coverage integration is not part of Wave 1.

Source contract and group handoff: `docs/superterrain/ST_REGION_FROZEN_CONTRACT.md` and `docs/superterrain/HANDOFF.md`.
