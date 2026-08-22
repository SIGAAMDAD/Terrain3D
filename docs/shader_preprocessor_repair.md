# Terrain3D Shader Preprocessor Repair

Target: Godot 4.7.1

Branch: `experiment/rendering-device-editor`

Native API revision: `2026082203`

Native build ID: `a2.9.2-shader-preprocessor-repair`

## Failure

The built-in Near/Middle/Far/Shadow and displacement shader templates are first expanded and specialized by Terrain3D, then passed to `Shader::set_code()`.

The previous pipeline deliberately left renderer and local feature conditionals for Godot's shader preprocessor. A Godot preprocessor condition failure prevented preprocessing from producing valid shader-language source. Rendering then received source that still contained preprocessor directives, so the downstream shader tokenizer stopped at the first `#define`. This made the reported `Unknown character '#35'` line look like the root failure even though it was only the first raw directive encountered after preprocessing had already failed.

## Repair

Built-in Terrain3D shader assembly now has one conditional owner: Terrain3D.

- Renderer-dependent `CURRENT_RENDERER` conditions were removed from the built-in terrain and displacement templates.
- Forward+/Mobile keep native `dFdxCoarse`/`dFdyCoarse` calls.
- Compatibility rewrites those two calls to `dFdx`/`dFdy` after Terrain3D specialization and before `Shader::set_code()`.
- The old `AUTO_SHADER`, `DISPLACEMENT`, and `SURFACE_DISPLACEMENT_ENABLED` second-stage flags were replaced by existing `T3D_*` static/feature state.
- Displacement projection now has its own projection snippet, removing the `IS_DISPLACEMENT_BUFFER` second-stage conditional.
- Unused `FILTER_LINEAR` / `FILTER_NEAREST` marker macros were removed. `FILTER_METHOD` remains because the selected sampler hint is a straightforward macro substitution rather than a conditional branch.
- Final built-in shader validation now rejects any conditional preprocessor directive that survives Terrain3D specialization. This turns a future regression into a Terrain3D assembly error before Godot shader compilation.

`#define` decode helpers remain intentionally. They do not evaluate conditions and are consumed normally by Godot's preprocessor.

## Validation

The repair was validated against a clean copy of the preceding API/shader-repair patch.

- 860 assembled variant combinations were statically generated across all five built-in roots and all four texture-filter selections.
- 0 conditional directives escaped specialization.
- 0 duplicate macro definitions were generated.
- Every surviving `FILTER_METHOD` use had exactly one selected definition.
- All `T3D_*` static expression identifiers used by the shader sources are known by the Terrain3D expression evaluator.
- `terrain_3d_shader_specializer.cpp` passes `g++ -std=c++17 -DGDEXTENSION -fsyntax-only` against generated bindings from the supplied `godot-cpp-10.0.0-rc1` package.
- `terrain_3d_material.cpp` passes the same syntax-only compile.
- Added C++ lines contain no `auto`, C++ cast syntax, lambdas, ternaries, standard memory algorithms, or lines over 120 columns.
- `git diff --check` is clean.

A real Godot 4.7.1 editor executable is not present in this environment, so the final runtime validation remains opening the project in 4.7.1 and forcing Near/Middle/Far shader candidate compilation on the target renderer.
