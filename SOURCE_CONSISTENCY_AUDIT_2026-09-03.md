# Terrain3D Source Consistency Audit — 2026-09-03

## Scope

Second-pass source audit of the formatting-normalized systems-C package.

Audited authored source:

- 267 C/C++ source/header files
- 153 headers
- 114 C++ translation units
- 23 RD GLSL source files
- embedded RenderWorld fallback shader strings
- formatter and systems-C verification tooling

Vendored `godot-cpp` source is excluded from authored-style judgments.

## Result

The repository is now mechanically consistent under the established systems-C / 2009-2012 formatting contract.

Final verification:

```text
C++ formatter check:                    PASS
RD shader formatter check:              PASS
Systems-C style gate:                   PASS
Explicit ownership/layout gate:         PASS
C++ translation units:                   114
Object files emitted:                    114
Compiler failures:                         0
Terrain3D-authored compiler warnings:      0
```

The only compiler warning is the known generated `godot-cpp` `INT64_MIN` literal warning in `global_constants.hpp`.

## Consistency defects found and corrected

### 1. Header-guard split

Before this audit, authored headers used several incompatible guard conventions:

```text
TERRAIN3D_*             115 headers
TERRAIN_3D_*             33 headers
legacy one-off guards     5 headers
```

`src/core/terrain_3d_compiler.h` did not actually have a whole-file include guard; its first `#ifndef` only guarded the `EA_FORCE_INLINE` definition.

Corrected state:

```text
Authored headers                         153
Whole-file guards                        153
TERRAIN3D_* guard family                 153
Duplicate guard macros                     0
Mismatched guard define/footer             0
#pragma once                               0
```

All guard footers now use:

```cpp
#endif // TERRAIN3D_...
```

The style gate now validates the full guard pair rather than merely checking for `#pragma once`.

### 2. Cast dialect drift

The tree still contained 33 `static_cast` / `reinterpret_cast` expressions despite the established project convention preferring explicit C-style casts except where `const_cast` semantics are required.

Corrected state:

```text
static_cast          0
reinterpret_cast     0
```

`const_cast` remains explicit where const removal is intentional.

### 3. Stale STL/header dependency residue

The implementation no longer used STL algorithms or owning containers, but many source files still included their headers.

Removed stale includes:

```text
<algorithm>          56 -> 0
<limits>             42 -> 0
<numeric>            11 -> 0
<type_traits>         5 -> 0
<utility>            25 -> 1
```

The sole remaining `<utility>` include is inside `core/terrain_3d_sync.h`, where the standard threading facade uses `std::forward`.

C++ wrappers around C facilities were normalized to the corresponding C headers:

```text
<cstdint>    -> <stdint.h>
<cstddef>    -> <stddef.h>
<cstring>    -> <string.h>
<cmath>      -> <math.h>
<cstdlib>    -> <stdlib.h>
<cstdio>     -> <stdio.h>
<cfloat>     -> <float.h>
<climits>    -> <limits.h>
<cassert>    -> <assert.h>
```

231 C++ wrapper-header inclusions were converted.

### 4. Standard-library confinement

Direct `std::` implementation usage is now confined to the synchronization facade:

```text
core/terrain_3d_sync.h
```

Outside that file, the only remaining `std::` text is a comment describing the C++17 replacement for `std::countr_zero`.

The style gate now rejects new `std::` implementation leakage outside the synchronization facade.

### 5. Project include paths

Two project-local `../` includes remained in `terrain_3d_integration_helpers.h`. They now use the configured `src/` include root.

Final state:

```text
Project-local ../ or ./ includes          0
```

`core/eastl_runtime.cpp` deliberately includes `../thirdparty/EASTL/source/hashtable.cpp`; this is a third-party implementation inclusion outside the `src/` tree and is the explicit verifier exception.

## Final formatting invariants

```text
Authored C++ lines > 140 columns           0
Trailing whitespace                        0
CRLF authored source files                 0
#pragma once                               0
Generated iT3dRangeN loop names            0
nullptr                                    0
static_cast                                0
reinterpret_cast                           0
Anonymous namespaces                       0
std owning containers                      0
TODO/FIXME/HACK/XXX markers                0
Relative project-local includes            0
```

RD shader sources:

```text
RD GLSL files                             23
Non-manifest lines > 140 columns           0
Tab-indented GLSL lines                    0
Dense multi-statement GLSL lines           0
```

Godot `#[versions]` manifest strings remain protected from formatting.

## Ownership/lifecycle vocabulary

The lifetime terminology is not fully single-word, but it is now semantically layered rather than random:

- `reset()` — clear logical state while retaining established backing/capacity.
- `shutdown()` — final teardown for fixed native/RD backing and synchronization state.
- `destroy()` — high-level Terrain3D/Godot subsystem teardown.
- `release()` — allocator, arena, or raw backing ownership release.
- `clear()` — logical/resource-content clear where the object remains valid.

The explicit ownership gate continues to verify the highest-risk parts of this contract: slab-backed instance payloads, fixed RD publication storage, fixed-buffer reset/shutdown behavior, deleted obsolete generic containers, and pointer/count render/placement APIs.

## Remaining intentional/non-mechanical variation

### Naming dialects

Naming is the main remaining source-level variation.

The primary Terrain3D codebase predominantly uses:

```text
Terrain3DClassName
camelCaseMethod
m_nCount / m_pData / m_bReady
nCount / pData / rState / iRegion
```

The standalone RenderWorld ABI intentionally retains its established C-style surface:

```text
T3DRenderWorld...
t3d_rw_...
T3D_RW_...
```

Older upstream-derived/editor code also retains some snake_case locals. These are naming/history issues, not formatting or ownership inconsistencies. They were not mass-renamed because doing so would produce a very large symbol-only diff, risk serialized/API/debug-name churn, and provide no runtime benefit.

If a future pass wants one naming dialect everywhere, it should be a dedicated rename-only patch with compiler and Godot binding validation rather than mixed into formatting work.

### High-level source size

Formatting consistency does not solve the existing large-file/function concentration. `terrain_3d_material.cpp`, `terrain_3d_data.cpp`, `terrain_3d_erosion_job.cpp`, and `terrain_3d_instancer.cpp` remain large subsystems. That is decomposition debt, not formatting drift, and should be addressed separately only along real ownership/responsibility boundaries.

## Compiler verification

Portable validation command family:

```text
clang++ 17
-std=c++17
-DGDEXTENSION
-Isrc
-Ithirdparty/EASTL/include
-Ithirdparty/EABase/include/Common
-Igodot-cpp/include
-Igodot-cpp/gen/include
-Igodot-cpp/gdextension
```

Final result after all second-pass corrections:

```text
114 / 114 object files emitted
0 error-bearing logs
0 Terrain3D-authored warning classes
```

No Godot executable or standalone GLSL compiler is installed in the current environment, so runtime/editor and shader-compiler smoke tests remain outside this audit's evidence level.
