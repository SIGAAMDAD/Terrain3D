# Terrain3D Formatting Normalization Audit

Date: 2026-09-03
Baseline: `Terrain3D_systems_c_2009_complete.zip`
Scope: Terrain3D-authored native source plus the direct-RD GLSL source set. Vendored `godot-cpp`, EASTL, and EABase were not reformatted.

## Result

The repository has been normalized to a single C++ house style and a separate deterministic GLSL style. This pass is primarily presentation/readability work; it does not intentionally change runtime architecture or behavior.

### C++ normalization

- Root `.clang-format` now has a real 140-column limit instead of unlimited lines.
- Function/type/control braces use the attached convention consistently.
- Control bodies are braced.
- One-line function bodies and compressed statement chains are expanded.
- Tabs are the only indentation mechanism for authored C++ block indentation, width 4.
- Pointer/reference spacing follows the project systems-C convention.
- Long argument and parameter lists are wrapped consistently rather than hand-aligned differently by subsystem.
- All 153 authored headers now use conventional include guards only.
  - Before: 125 headers used both `#pragma once` and an include guard, 26 used guards only, 2 used `#pragma once` only.
  - After: 153 use include guards, 0 use `#pragma once`.
- All generated range-loop placeholder names from the previous mechanical conversion were removed.
  - 364 automatically detected loops plus two multiline loops were renamed to semantic indexes such as `iMesh`, `iCell`, `iRegion`, `iTexture`, `iPipeline`, `iDependency`, and `iBank`.
  - Remaining `iT3dRangeN` references: 0.
- Authored C++ lines over 140 columns after formatting: 0.

### Shader normalization

The 23 direct-RD `.glsl` files under `project/addons/terrain_3d/rd/` now use:

- four-space indentation;
- attached braces;
- consistent operator/call spacing;
- expanded function/control bodies;
- wrapped long expressions and resource declarations;
- no dense multi-statement shader lines.

Measured before/after for the RD shader source set:

```text
Lines over 140 columns: 56 -> 5
Dense multi-statement lines: 191 -> 0
```

The five remaining lines over 140 columns are `#[versions]` manifest strings. They are deliberately preserved verbatim because Godot's shader version metadata does not use C/C++ adjacent-string syntax.

Embedded RenderWorld fallback shader strings are formatted by the same shader formatter. The authored `.glsl` transformations preserve the non-whitespace character stream; the pass is formatting-only for those files.

## Tooling added

- `.editorconfig`
- deterministic root `.clang-format`
- shader-local `project/addons/terrain_3d/rd/.clang-format`
- `tools/format_cpp.py`
- `tools/format_shaders.py`
- expanded `tools/verify_systems_c_style.py` checks for:
  - generated `iT3dRangeN` names;
  - `#pragma once` drift;
  - the existing systems-C prohibitions.
- `FORMATTING_STYLE.md`

Both formatter scripts support `--check` and prefer standalone `clang-format`, falling back to clangd when necessary.

## Verification

Final static gates:

```text
PASS: Terrain3D systems-C style gates
PASS: Terrain3D explicit ownership/layout gates
PASS: C++ formatter idempotence
PASS: RD shader formatter idempotence
PASS: git diff --check
```

Final structural formatting checks:

```text
Authored headers:                         153
Headers using include guards only:        153
Headers using #pragma once:                 0
Authored C++ lines over 140 columns:         0
Generated iT3dRangeN references:             0
RD GLSL files:                               23
Non-manifest GLSL lines over 140 columns:     0
```

Final portable C++ object-emission gate:

```text
Terrain3D translation units: 114
Objects emitted:             114
Compiler failures:             0
Terrain3D warning lines:        0
```

Compiler: Clang 17, C++17, `GDEXTENSION`, source-matched generated Godot 4.7.2 API in the package.

The known generated `godot-cpp` `INT64_MIN` warning remains outside Terrain3D-authored source.

## Validation boundary

No Godot executable, `glslangValidator`, or `glslc` is installed in this environment, so runtime shader compilation was not executed. Shader-source changes were constrained to formatting, with version-manifest metadata protected verbatim and the non-whitespace source stream checked against the baseline.
