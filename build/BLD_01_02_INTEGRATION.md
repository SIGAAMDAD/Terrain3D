# BLD-01/02: stable PCH and subsystem header ownership

This archive does not include the parent repository's `SConstruct` or `build/source_sets.py`. `build/pch.py` is the integration-ready, opt-in adapter for that actual build root. **It cannot take effect until the existing native builder calls `build_flavor_pch()`**. Do not create a second source-set manifest.

## SCons integration

1. Import `build_flavor_pch` and `pch_eligible` from `build.pch` in the authoritative outer `SConstruct`.
2. Register `pch=off|stable` in its existing command-line options (`off` by default). Call the adapter after creating each complete editor/runtime environment clone with its own `T3D_EDITOR_NATIVE` or `T3D_RUNTIME_NATIVE` and CRT/optimization flags.
3. Compile normal objects using the returned PCH-enabled environment when `pch_eligible(src)` and the source has no file-specific compiler flags. Compile tests, SIMD backends, generated code and registration with the ordinary PCH-free environment. Do not compile `src/core/build/terrain_3d_pch.cpp` as an ordinary source.
4. Keep runtime/editor objects/PCH files in separate output directories. The output PCH target directory is `build/obj/<flavor>/pch` by default; match the outer build's variant directory if it uses distinct targets for toolsets/flags.
5. Use the PCH builder's first returned node as `env['PCH']`. Let SCons manage the `/Yu` dependency and the PCH object/link behavior. Do not manually link a duplicate PCH object.
6. Confirm `/Yc` appears on PCH generation and `/Yu` plus `/FIterrain_3d_pch.h` on ordinary sources; run the PCH-off build before enabling by default.

The PCH deliberately excludes all Terrain3D types, renderer state, constants, enums, editor classes and ISA intrinsics. Build shared headers are not a way to combine subsystem-specific contracts. VT feedback's own header is now included directly by its consumers, never re-exported through `Terrain3DTypes.h`.

## Ownership rule

Each subsystem (`assets`, `authoring`, `core`, `data`, `hlod`, `instances`, `integration`, `lighting`, `memory`, `physics`, `rendering`, `simd`, `streaming`) owns its public contracts. Consumers include the exact owning header. Cross-subsystem umbrella imports should be removed as each contract is extracted in BLD-03/04, not replaced with new all-in-one headers. BLD-02 establishes this in the VT feedback path without changing layouts or runtime code.

## Acceptance

- MSVC `pch=off` builds both native flavors; `pch=stable` builds both and links the unchanged exported ABI.
- `Terrain3DTypes.h` no longer transitively includes VT feedback-grid; every direct user includes it itself; its 40-byte header and 32-byte params assertions remain intact.
- PCH header and its dedicated TU compile with real Godot 4.7 bindings and use no Terrain3D project contract.
- All affected VT feedback native tests and production files compile in both configurations without forced PCH (`pch=off`, `unity=off`).
