# BLD-01/02: stable PCH and subsystem header ownership

The supplied parent `SConstruct` is now included in the integrated source and calls `build_flavor_pch()` after native target defines, shader flags and include paths are configured. The source set remains the authoritative recursive `src/` discovery in that same SConstruct. **Install this updated SConstruct and the BLD-01 source files together.** The external `godot-cpp` checkout is still required for a real Windows build.

## SCons integration (implemented)

- On MSVC the default is `pch=stable`. Pass `pch=off` to restore the former build behavior; on non-MSVC the default is `pch=off`.
- `pch` is hidden from `godot-cpp/SConstruct` argument validation, then restored for Terrain3D processing.
- Each native target produces a PCH under `build/obj/<target>/<platform>/<arch>/<editor|runtime>/pch/`. The adapter uses MSVC `/Yc` on the producer and `/Yu` plus `/FI` on eligible translation units.
- The PCH producer (`src/core/build/terrain_3d_pch.cpp`) is excluded from recursive source discovery.
- Test, registration, generated and ISA backend translation units compile without PCH. `register_types.cpp` keeps its existing `T3D_TESTS_ENABLED` define in test builds.
- The PCH producer shares the same native macros/optimization and CRT settings as the ordinary objects. Each native target gets its own PCH artifact, without a second source manifest.
- For a PCH-only build run `scons terrain3d-pch target=editor ... pch=stable`. The ordinary target builds it as a dependency of eligible objects.

**Windows acceptance:** run `scons target=editor platform=windows arch=x86_64 pch=stable use_static_cpp=yes debug_crt=no`, repeat `pch=off`, and verify both link. Repeat for `target=template_debug`, then inspect `/Yc` versus `/Yu` compile commands and benchmark clean/warm builds.

## Ownership rule

Each subsystem (`assets`, `authoring`, `core`, `data`, `hlod`, `instances`, `integration`, `lighting`, `memory`, `physics`, `rendering`, `simd`, `streaming`) owns its public contracts. Consumers include the exact owning header. Cross-subsystem umbrella imports should be removed as each contract is extracted in BLD-03/04, not replaced with new all-in-one headers. BLD-02 establishes this in the VT feedback path without changing layouts or runtime code.

## Acceptance

- MSVC `pch=off` builds both native flavors; `pch=stable` builds both and links the unchanged exported ABI.
- `Terrain3DTypes.h` no longer transitively includes VT feedback-grid; every direct user includes it itself; its 40-byte header and 32-byte params assertions remain intact.
- PCH header and its dedicated TU compile with real Godot 4.7 bindings and use no Terrain3D project contract.
- All affected VT feedback native tests and production files compile in both configurations without forced PCH (`pch=off`, `unity=off`).
