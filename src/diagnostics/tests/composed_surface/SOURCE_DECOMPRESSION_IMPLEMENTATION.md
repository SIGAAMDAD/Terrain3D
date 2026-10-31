# CS15 / CS15 - Native V3 source validation and decompression preparation

## Scope

CS15 introduces a deterministic **cold** BC1/BC5/R8 tile decoder and routes
`terrain_3d_composed_surface_extended_godot_decode.cpp` through it. BC7 continues to
use Godot `Image::decompress` at authoring time. This avoids per-tile Godot
Image construction for three of the physical families and provides an exact
scalar reference for validating compressed source pages. It does not change
Sparse VT V3 pack bytes, the `.t3sc` sidecar ABI, live GPU shaders or memory
ownership. Runtime decoding is unchanged.

The new `terrain_3d_composed_surface_extended_reference_decode.{h,cpp}` reads frozen
V3 family IDs, requires exactly a 136x136 physical tile, and validates input
and output sizes before writes. BC1 supports both endpoint modes and the
transparent selector; BC5 uses unsigned BC4 blocks for independent red/green
components, leaving blue at 0 and alpha at 255; R8 expands to RGBA with red
carrying the height value. BC7 is explicitly refused by the reference path.

## Acceptance coverage

`run_native_pack.sh` executes scalar BC1/BC5/R8 known-vector checks under
ASan/UBSan and, when generated Godot 4.7 C++ headers are available, syntax
checks the **actual production** native V3 reader, V3 pack writer, source
sampler, reference decoder, Godot decoder and the native V3 fixture source.
The native fixture constructs an on-disk V3 source package, roundtrips it
through the production writer/opener and samples three logical channels.
It still requires an actual linked Godot C++ library to run; the BC7 fixture
uses an explicit synthetic callback, so running it also would not establish
BC7 rendering parity. No such library/runtime was included in the supplied
native snapshot.

The existing CS12, CS13, CS14, and shader-contract tests must continue to pass.

## Editor-only build registration

Register this **new** translation unit with the editor build's explicit source
list (not the runtime DLL):

`src/authoring/vt/build/terrain_3d_composed_surface_extended_reference_decode.cpp`

The existing decoder unit
`src/authoring/vt/build/terrain_3d_composed_surface_extended_godot_decode.cpp`
now links against it. The source archive has no `build/source_sets.py` or
`SConstruct`, so no build-list file can be edited here.

## Still required for live activation

- Decode real BC7 payloads through the active Godot Image backend and compare
  against the raster path using captured source packs.
- Validate canonical blend, normals, ORM, color space and compressed-page
  filtering with matched Vulkan captures, including gutters and near/middle.
- Produce a complete editor-native build and linked source-pack test.
- Link composed page upload-budget admission, descriptor generation, shader
  reflection and Forward+ color/DNR/depth coherence, then establish GPU-time
  uplift with p50/p95/p99 captures.

`T3D_COMP_SURF_ACTIVE` remains disabled and no parity flag is promoted by this patch.
