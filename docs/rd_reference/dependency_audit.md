# RenderingDevice Dependency Audit

## Target engine

- Supplied executable: `Godot_v4.7.1-stable_linux.x86_64`
- Executable reports: `4.7.1.stable.official.a13da4feb`
- Project GDExtension minimum: `4.7`
- Repository API override: `thirdparty/godot_api/extension_api-4.7.1.json`
- `SConstruct` explicitly assigns that file to `godot_cpp_env["custom_api_file"]` before loading the vendored godot-cpp SConstruct.

## godot-cpp comparison

The vendored `godot-cpp` tree was compared against the supplied `godot-cpp-10.0.0-rc1` archive by relative path and SHA-256, excluding generated/build directories (`bin/`, `gen/`, `.git/`) and `.sconsign.dblite`.

- Vendored files compared: **187**
- External archive files compared: **194**
- Common files with different contents: **0**
- Files present only in vendored copy: **0**
- Files present only in supplied archive: **7**

The seven archive-only files are test/generated metadata:

- `test/project/bin/libgdexample.macos.template_debug.framework/Resources/Info.plist`
- `test/project/bin/libgdexample.macos.template_release.framework/Resources/Info.plist`
- `test/project/example.gd.uid`
- `test/project/example.gdextension.uid`
- `test/project/icon.png.import`
- `test/project/main.gd.uid`
- `test/project/test_base.gd.uid`

There is no detected godot-cpp source mismatch between the repository and the supplied dependency package.

## Build-environment status for this reconstruction pass

The supplied Godot executable is runnable and version-verified. The current execution environment does **not** provide `scons`, and the repository archive does not contain a prebuilt Terrain3D extension under the ignored `project/addons/terrain_3d/bin/` directory. Therefore this pass cannot honestly upgrade the existing branch documentation's historical build claims into a fresh build result.

A reproducible build against the supplied binary/API remains an implementation gate before renderer work is described as freshly compiled.

## Supplied runtime dataset

`terrain3d_00_02.zip` contains **354** validation files:

- 176 `.res`
- 176 `.t3dpage`
- 1 `.t3dfar`
- 1 `.t3dmat`

It is a runtime/streaming validation dataset, not an alternate source-tree reference. Its complete deterministic manifest is stored in `terrain3d_00_02_manifest.tsv`.
