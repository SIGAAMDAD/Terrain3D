# Wave G / CS11 — bounded canonical V3 cold-material producer

## Scope implemented

- `terrain_3d_composed_surface_static_extended.{h,cpp}` is a **cold** evaluation adapter for an explicitly eligible subset of the existing canonical VT V3 material backend.
- It evaluates each physical 136x136 page **including real neighboring gutters**, per mip, using a caller-provided page-to-world footprint and source control lookup. The source VT V3 callback must preserve the original clipmap's page-table, fallback, filtering and decode-after-filter semantics; any missing control or VT channel aborts the page, rather than baking a substitute.
- Implements packed material IDs, rotation, scale, base/overlay height blend, nearest/bilinear/top-two four-corner weighting, material-layer albedo tint, source PBR normal transformation/strength, AO, roughness, metallic and AO influence. Albedo is encoded back into sRGB for BC1; tangent X/Z are encoded for BC5; ORM/AO influence goes to BC7. Dynamic authored overlays, wetness and coverage remain live.
- Integrates via existing CS03 `t3dComposedSurfaceCookSource_t` evaluate/encode callbacks, allowing the CS03 incremental cooker and any existing compatible BC1/BC5/BC7 encoder. The reference encoder adapter can be supplied as its `m_pEncoder`.
- Rejects automatic controls, dual-scale, triplanar projection, detiling, adaptive camera-dependent control interpolation, implicit controls, camera-dependent mip bias, malformed source/material input and failed VT reads. An explicit externally verified material-parity acceptance flag is still required to cook.
- Adds an **optional shader-only eligibility predicate** to both color and DNR composed sampling paths; no existing shipping variant changes behavior without `T3D_COMP_SURF_ACTIVE`. The shader predicate excludes unsupported features and requires distance-invariant mip bias. The producer's sampling mode and mip bias must agree with the active tier; final acceptance is still source-dependent.

## Verification

- `test_static_v3.cpp`: 136x136 page evaluation, sampled albedo/PBR and height blend, scalar vs bilinear/top-two control modes, callback error rejection, unsupported feature rejection, and valid on-disk `.t3sc` output through CS03 cooker (mock BC payload bytes). Clang C++17 `-Wall -Wextra -Werror`, plus ASan/UBSan.
- `test_static_v3_shader.py`: dormant shader gating shared by color and DNR; checks cold eligibility contract and source operation coverage.
- Earlier wave contract and shader topology regression tests are run separately.

## Outstanding parity / live integration

1. **No real production source VT sampler is wired to `m_pSampleSource`.** The current test callback supplies exact linear albedo, RGBA PBR and height values but is not a decoder for disk V3 source pages. Only a fully parity-validated source reader may enable `m_bCanonicalMaterialParityAccepted`.
2. The producer only supports the static V3 subset described above; complete CS02 parity (dynamic dual-scale, detiling, side projection, automatic terrain control, derivatives/adaptive footprint, authored edges and different mip policies) is not implemented. The GPU comparator also requires Vulkan visual tests including BC compression and mip seams.
3. The original source archive lacks the extension build root / `build/source_sets.py`, generated Godot C++ includes and Godot/Vulkan tools. Register `terrain_3d_composed_surface_static_extended.cpp` in the editor/authoring build and run full engine/SPIR-V tests before enabling the specialization.
4. The three-channel baker currently calls the source evaluator separately per output channel; it is cold-only and needs measurement before choosing a page-local shared evaluation scratch. Runtime VT uploads remain independent of this wave.

**Status:** integrated cold producer contract and tests for a restricted static V3 material subset; composed GPU sampling still dormant. Source V3 package ABI unchanged; no live cache, descriptor or transfer-budget allocation changed.
