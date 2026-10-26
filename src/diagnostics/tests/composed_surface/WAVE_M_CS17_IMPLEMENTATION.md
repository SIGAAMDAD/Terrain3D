# Wave M / CS17 — Fail-closed composed page-table publication

## Implemented

- The render-owner GPU lookup now retains the exact **pre-insertion** slot state when adding a composed page mapping. If a tracked GPU buffer update is rejected, the CPU-side insertion is rolled back to the original empty or tombstone entry rather than leaving a phantom resident mapping.
- The rollback operation is identity-checked against the *staged* 48-byte entry; delayed rollback cannot erase a different or newer resident entry.
- If the GPU table update fails, the composed GPU owner becomes incoherent and will refuse new submissions, completions, retirements, and binding acquisition until a cold resource replacement reinitializes it. This avoids treating partially published mapping state as safe.
- If CS04 publication acknowledgment fails after the GPU table write, the previous hash slot state is restored and uploaded back to the GPU. Failure of that restoration also invalidates acquisition.
- Shared VT runtime readiness now incorporates the GPU owner's mapping-coherency gate instead of merely remembering that cold initialization succeeded.
- Standalone and world-raster descriptors retain their existing **dormant** layout: clipmap set 0 / bindings 39–42 and embedded world set 1 / bindings 13–16. No compiled shader variant is activated in this wave.

## Validation performed

`sh src/diagnostics/tests/composed_surface/run_cs17_publication.sh` runs the CPU GPU-table transaction fixture with ASan/UBSan and verifies the descriptor and fail-closed ownership contract. The fixture covers empty-slot restore, tombstone restore, stale rollback rejection and hash collision continuity.

The previous CS05 table and CS12–CS16 source/codec/editor test suites are still required to pass. The actual Godot/Vulkan buffer write and descriptor publication cannot be exercised headlessly with the uploaded dependencies: the archive lacks an EASTL header dependency, a linked godot-cpp library, and the full SCons build root. The CS15 native V3 linked package test is still unexecuted. The existing tests with full Godot 4.7 headers can perform syntax checks for the V3 pack reader/writer/sampler.

## Activation status

Material parity, complete Forward+ composed descriptors, enabled SPIR-V compilation, GPU memory barriers, and Vulkan image/performance validation are still unverified. The shipping composed rendering path remains disabled and the frozen V3 disk ABI is untouched.
