# Terrain3D Formatting Style

The repository uses one explicit formatting contract for Terrain3D-authored C++ and one for RD shader sources.

## C++

- C++17.
- Tabs for indentation, width 4.
- Attached opening braces.
- Braces on all control-flow bodies.
- No one-line function bodies or compressed statement chains.
- 140-column target; authored C++ is expected to remain within it.
- Spaces inside C/C++ parentheses, matching the existing systems-C convention: `if ( condition )`, `fn( value )`.
- Right-aligned pointers/references: `Type *pValue`, `const Type &rValue`.
- Include order is preserved rather than automatically sorted.
- Traditional include guards only; do not add `#pragma once`.
- Header guards use the `TERRAIN3D_*` family and wrap the complete header.
- Project-local includes are rooted at the `src/` include path rather than using `../` traversal.
- C-style casts are the project convention; `static_cast` and `reinterpret_cast` are rejected. `const_cast` remains explicit where const removal is required.
- Prefer C headers (`<string.h>`, `<stdint.h>`, `<stddef.h>`, `<math.h>`, etc.) over C++ wrapper headers. STL algorithm/utility headers are rejected unless required inside the synchronization facade.
- Direct `std::` usage is confined to `core/terrain_3d_sync.h`; runtime code consumes Terrain3D/EASTL/C interfaces instead.
- Runtime loops use semantic index names (`iMesh`, `iCell`, `iRegion`, `iPipeline`, etc.). Generated placeholder names such as `iT3dRangeN` are rejected.

Run:

```text
python tools/format_cpp.py
python tools/format_cpp.py --check
```

The formatter prefers `clang-format` and falls back to clangd's formatting implementation when only clangd is installed.

## RD GLSL

- Four spaces for indentation; never tabs.
- Attached braces.
- 140-column target for shader code.
- Version-manifest entries under `#[versions]` are preserved verbatim because their string syntax belongs to Godot's shader metadata parser, not C/C++.
- Embedded fallback GLSL strings in `src/rendering/rd/render_world/` are formatted by the same shader pass.

Run:

```text
python tools/format_shaders.py
python tools/format_shaders.py --check
```

## Static gates

```text
python tools/verify_systems_c_style.py
python tools/verify_systems_c_ownership.py
```

The formatting tools and static gates are intended to be run before committing broad native-source changes.
