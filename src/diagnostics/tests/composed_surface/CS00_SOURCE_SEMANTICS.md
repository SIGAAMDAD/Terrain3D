# CS00/SA01 — composed surface source-equivalence contract

This is the **pre-bake reference**, recorded from `project/addons/terrain_3d/rd/` at CS00–CS02. Shader validation does not constitute a Vulkan timing capture or visual-parity pass.

## Shipping reference and known differences

- **Clipmap source VT albedo:** `terrain_clipmap_fragment_inc.glslinc` samples BC1 UNORM with `textureGrad` and then invokes `t3d_vt_decode_srgb_albedo`. Hardware filtering therefore occurs in encoded-UNORM space. Freeze this as the *current clipmap reference*.
- **World/instance source VT albedo:** `vt/terrain_vt_consumer_sample_inc.glslinc` decodes individual fetched texels before custom interpolation in its filtered path. Its midpoint differs from clipmap: e.g., black/white midpoint is about 0.5 linear for decode-before-filter versus ~0.214 linear for filter-before-decode. Track each consumer independently; do not silently change source V3 resources.
- **Packed control:** base ID bits 27-31, overlay ID bits 22-26, blend bits 14-21, rotation bits 10-13, scale bits 7-9. `control_sample_key` deliberately excludes blend and metadata, retaining ID/rotation/scale identity. `select_top_two_control_weights` uses strict greater-than tie ordering, and active corner weights below 0.00001 are discarded before normalization.
- **Height blend:** `sharpness=fma(60,blend_sharpness,4)`, base weight = `pow(max(1-blend+base.height,1e-8),sharpness)`, overlay weight = `pow(max(blend+overlay.height,1e-8),sharpness)`; denominator clamped to `1e-5`. Both color and DNR now call the same pure GLSL helper.
- **Normal convention:** `SampledMaterial.tangent_normal` uses XYZ with up in Y; BC5 proposal stores tangent X,Z (reconstruct positive Y). `apply_surface_normal` handles the projected tangent basis; do not bake camera/view-space normals.
- **PBR:** source samples carry roughness, ambient occlusion, metallic and AO influence separately, not the old middle-composite defaults. Proposed composed BC7 is R=AO, G=roughness, B=metallic and A=AO influence. This covers all four output scalars without reusing the coverage/alpha contract; authored alpha/wetness stays live.
- **Authored color / wetness:** `sample_color_exact` interpolates authored RGBA using explicit neighboring texels; `authored_wetness_offset` interprets color alpha as a signed roughness offset around 0.5. `sample_color_material_shared` applies the authored RGB contribution downstream. Do not bake these until invalidation covers their full dependencies. Hole coverage and solid/alpha bits must remain in the live depth, DNR and color passes.
- **Camera-only / dynamic:** lighting/shadows, forward reflection, fog, dynamic weather, decal projection, editor overlays and dynamic material detail/mip transitions stay outside the page payload.

## Camera-independent evaluator coverage in CS00–CS02

`terrain_composed_material_math.glslinc` is source-common to the full raster color and DNR paths. It implements packed-control decode, four-corner weighting/top-two selection, the exact small-weight threshold, height blending and sampled-material accumulation. The separate compute **parity fixture** resolves prepared direct texture arrays using *explicit* gradients into uncompressed RGBA16F intermediates. It provides a controlled GPU test case for these operations.

The compute fixture does **not** currently implement full canonical VT V3 source-residency lookup, page filtering, runtime autoshader, triplanar/side projection, dual scaling, macros or every authoring overlay. It must not produce shipping `.t3sc` content; a CS02 completion requires extending the producer to use the same full sampling semantics and passing capture-based parity.

## Capture gate (not yet measured)

1. Record same-view Vulkan GPU timings for idle-down, stationary horizon, occluded horizon, camera rotation, camera movement and editor brush; full and half resolution, cold startup and steady state separately.
2. Record per-pass depth/DNR/color, visibility and GPU transfer times, upload bytes/calls, feedback readback bytes, VT page hits/misses and resident pages when available. CPU and GPU units remain separate.
3. Capture canonical/compat/editor variants with identical material inputs and GPU options. Render independent golden frames for clipmap and world-raster/instances to preserve their respective sRGB sampling semantics.
4. Parity scenes: single material, homogeneous pair, four corner material IDs, duplicate sampling keys with distinct blends, top-two ties, steep auto-slope, rotation+scale, tri-planar, two region boundaries, authored color alpha, holes, dual scale, all normal/PBR channels, BC seams and mips. Compare linear albedo, tangent normal, ORM and final Forward+ color/DNR images separately.
5. Establish numerical tolerances from uncompressed reference-vs-fixture comparisons, then independently establish BC1/BC5/BC7 encoding tolerances. No fabricated thresholds or latency savings are assigned to this source-only baseline.

See `capture_matrix.json` and `shader_variants.json` for capture targets. `analyze_composed_surface_captures.py` summarizes recorded per-frame CSV metrics into actual p50/p95/p99 values; no data rows are included in CS00–CS02. This state is **baseline infrastructure**, not completed source-equivalence acceptance.
