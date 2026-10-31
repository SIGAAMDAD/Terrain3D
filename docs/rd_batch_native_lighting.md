# Batch F4 - Native Forward+ lighting and shadow receiving

## Status

F4 makes the production RenderingDevice terrain, regular-instance, and HLOD color paths consume Godot 4.7.1 Forward+ lighting state directly. It builds on F1-F3 native pass submission and does not add motion vectors, GI/lightmap parity, or lifecycle convergence owned by F5-F7.

## Bridge ABI 4

ABI 4 retains the synchronous PREPARE/SUBMIT contract and adds the exact clustered-lighting metadata needed by Terrain3D shaders:

- `cluster_shift`, `cluster_width`, `cluster_type_size`, and `cluster_max_elements_div_32`;
- `directional_light_count` and `directional_light_soft_shadows`;
- camera near/far depth values;
- the previously exposed directional, omni, spot, area, cluster, shadow-atlas, directional-shadow-atlas, and native shadow-sampler RIDs.

The engine packet remains typed once at the GDExtension boundary. No main-thread light enumeration is introduced.

## Resource ownership and descriptor policy

`Terrain3DRdForwardLighting` owns only a small frame metadata UBO, fallback shadow resources used when a native RID is absent, and shader-specific Terrain3D descriptor sets. Godot owns all light buffers, cluster storage, shadow atlases, and the comparison sampler. Borrowed native RIDs are never registered in or freed by Terrain3D.

The set-2 descriptor key contains the borrowed native RIDs. Descriptor sets are rebuilt only when that key or shader RID changes. PREPARE updates the compact frame metadata UBO and ensures the descriptor exists before Godot opens the native draw list; SUBMIT only retrieves and binds the cached set.

## Exact Godot 4.7.1 cluster addressing

The shared GLSL include follows Forward+ 4.7.1 indexing rather than constructing a parallel light list:

```text
cluster_xy   = gl_FragCoord.xy >> cluster_shift
cluster_base = (cluster_width * y + x) * (max_cluster_element_count_div_32 + 32)
cluster_z    = clamp((-view_z / z_far) * 32, 0, 31)
```

Type offsets use `cluster_type_size * 0` for omni lights, `* 1` for spot lights, and `* 2` for area lights. The packed range word, 32-bit membership words, and range clipping mask use the same low/high-16-bit and bitfield rules as Godot 4.7.1. Bounds are checked against the bridge-provided render dimensions and cluster metadata before dereferencing membership words.

## Shared native lighting shader

`project/addons/terrain_3d/rd/terrain_forward_lighting_inc.glsl` is shared by near/middle/far terrain and regular-instance/HLOD color shaders. The production `color_native` variants consume:

- dynamic directional lights from Godot's directional-light UBO;
- arbitrary clustered omni and spot lights from native storage buffers;
- clustered area lights using Godot's native one-sided rectangle orientation, closest-point distance, range data, and the fork's simplified direct-light material response rather than duplicating Godot's full LTC shading model;
- native per-light layer masks;
- native directional and local shadow atlases through Godot's comparison sampler.

The fork's existing simplified terrain material model remains authoritative. F4 replaces hardcoded production light direction with native Forward+ light data; the old static direction remains only in the explicit diagnostic/non-native shader route.

## Shadow receiving

Directional receiving selects native cascade matrices from Forward+ split metadata and applies native bias, normal-bias, opacity, atlas coordinates, and comparison sampling. Omni uses the native dual-paraboloid atlas representation. Spot uses its native shadow matrix and atlas rect. Area lights consume the native dual-paraboloid shadow representation used by Godot 4.7.1.

The same receiver code is used for terrain, regular instances, and HLOD. Instance/HLOD light acceptance uses each published packet's visibility-layer mask; terrain uses the render snapshot's terrain layer mask.

## Preserved material/environment inputs

F4 leaves the existing ambient/environment contribution, terrain baked material response, AO/cavity, fog, normal, roughness, metallic, alpha-scissor, and emission paths in place. Native direct light is added at the direct-light stage rather than replacing those authored inputs. Section 18 remains marked partial in the contract ledger because F6 still owns the required lightmap/GI integration; that later work is not part of the F4 completion gate.

## Validation performed

- affected C++ translation units compile against generated Godot 4.7.1 GDExtension bindings;
- near/middle/far terrain plus instance shader variants compile through the supplied Godot 4.7.1 RDShaderFile importer, including `color_native`;
- the F4 static contract checks ABI 4 metadata, borrowed-resource ownership, descriptor caching, exact cluster indexing, directional/omni/spot/area paths, native shadow receiving, masks, and HLOD shader sharing;
- the engine patch parses with `git apply --stat`.

A patched Godot engine binary is still required for runtime Forward+ scene/GPU validation. F4 does not claim that runtime evidence from the stock supplied binary.

## Next chunk

F5 owns motion-vector/TAA correctness. F6 owns GI/lightmaps/reflections/decals parity. F7 owns multiview/render-scale/MSAA/device lifecycle convergence.
