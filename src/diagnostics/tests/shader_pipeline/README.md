# Offline SPIR-V variant packaging

`build_spirv_package.py` compiles an explicit list of *accepted* RDShaderFile
versions with glslang using `-Od`, then optionally applies the tested
non-inlining SPIRV-Tools profile, and deduplicates identical modules. The
result is a deterministic content-addressed ZIP plus its SHA-256 manifest.
It does not load into Godot by itself or change any shader default.

Usage:

```text
python3 build_spirv_package.py --shader-root ./rd \
  --selection selection_example.json \
  --glslang /path/to/glslangValidator \
  --output ./ocean_reference.zip
```

Optional `--optimization intensive --spv-tool /path/to/spv_tool` invokes the
existing non-inlining optimization pipeline. `--strip-debug --spirv-opt
/path/to/spirv-opt` requires the Khronos `spirv-opt` command separately.
Do not substitute glslang `-O`, which greatly expanded some Terrain3D
fragments in the previous audit.

`selection_example.json` is an example, not a claim that additional
experiments are accepted for shipping. The five-attachment screen-space
pilot, prepared ocean trial, compact lighting and VT hint variants remain
experiments until GPU measurements and parity checks exist.

The bundle is a source-independent build/analysis artifact. GPU/driver
pipeline caches, Godot `RDShaderFile` resources, descriptor ownership, and
runtime material bindings remain managed by the native renderer.
