# Terrain3D Compile and Validation Audit

## Validation basis

This audit uses the supplied `godot-cpp-10.0.0-rc1` source package. Its bundled `gdextension/extension_api.json` identifies itself as **Godot Engine v4.6.stable.official**, and `tools/godotcpp.py` advertises API selections 4.3 through 4.6. The repository therefore no longer hard-codes `api_version="4.7"`; it uses the bundled `godot-cpp` API in the same way the original SConstruct did.

The project target remains Godot 4.7.1. Exact 4.7.1 API verification requires a 4.7-compatible `godot-cpp` package. The supplied package is still sufficient to validate the C++/EASTL/type/member/declaration structure exercised below.

## Compiler-derived fixes in this pass

The supplied MSVC error log and full translation-unit compilation exposed and fixed:

- move-only `t3dInstanceCell_t` / `Terrain3DTransformArray` EASTL compaction failures;
- missing `t3dMapType_t` visibility in `Terrain3DFarSurfaceAtlasCache`;
- stale `t3dRuntimePageChannelInfo_t::type` accesses;
- missing logger class markers and logger use from captureless callbacks;
- malformed `EA_FORCE_INLINE` placement left by the one-line pass;
- Godot `Vector2`/`Vector2i` component names damaged by bulk native-member renaming;
- stale unqualified native member names after the naming migration;
- missing `Terrain3DData` forward declaration in the instancer;
- missing direct `GeometryInstance3D`/`RenderingServer` dependencies in the mesher header;
- stale `Terrain3D::EXTREME` qualification;
- stale shader/HLOD/residency/memory-stat member names;
- `V2I_DIVIDE_CEIL/FLOOR` macro precedence, which incorrectly applied `.x/.y` to only the final subexpression for compound vector arguments;
- missing CPU-cache helper routing after the residency-cache extraction;
- stale `Terrain3DRegion::TYPE_CONTROL` qualification;
- obsolete `CHANNEL_HEIGHT` use in cooked runtime map adoption;
- stale resident-pool channel constants;
- invalid two-argument `UtilityFunctions::bytes_to_var()` call for the supplied bindings;
- default lightmap output helper stranded in `terrain_3d.cpp` after implementation splitting;
- invalid hard-coded `api_version="4.7"` for the supplied `godot-cpp` package.

## Compiler passes

All 69 Terrain3D translation units were compiled against generated `godot-cpp` headers in four independent frontend configurations:

| Pass | Compiler/configuration | Result |
| --- | --- | --- |
| 1 | GCC 14, C++17, debug/dev defines | 69 / 69 pass |
| 2 | GCC 14, C++17, release-style defines | 69 / 69 pass |
| 3 | Clang 17, C++17, debug/dev defines | 69 / 69 pass |
| 4 | Clang 17, C++17, release-style defines | 69 / 69 pass |

A fifth pass compiled all 69 translation units to real GCC object files rather than using only `-fsyntax-only`:

- objects produced: 69 / 69;
- compiler failures: 0.

## Cross-translation-unit symbol audit

The 69 generated objects were scanned with demangled `nm` output. Undefined Terrain3D-owned symbols were compared against symbols defined by the complete Terrain3D object set.

- unresolved `T3D*` / `t3d*` project symbols: **0**.

This catches missing project function definitions that a per-file syntax-only pass can miss.

## Static verification

The final tree also passes:

- `git diff --check`;
- MSVC project regeneration through `tools/generate_msvc_project.py`;
- Python syntax compilation of the MSVC project generator;
- zero `EA_FORCE_INLINE if/for/while/switch` placements;
- zero stale `Terrain3DRegion::TYPE_HEIGHT/CONTROL/COLOR` qualifications;
- zero known stale `nodePolicyOffsets`, `cpuSourceRequests`, shader `entry.key`, and unprefixed memory-zone statistic accesses;
- zero obsolete two-argument `bytes_to_var(..., false)` calls;
- zero Terrain3D-owned unresolved symbols in the object-level symbol audit.

## Remaining validation boundary

The supplied `godot-cpp` package is a 4.6 API package, not a 4.7/4.7.1 package. A Windows MSVC build against the user's exact 4.7.1 bindings remains the final platform/API-specific check. The C++ source tree itself passes both GCC and Clang compilation and object generation against the supplied bindings.
