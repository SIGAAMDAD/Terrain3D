# Wave A (CS00–CS02 / SA00–SA02) — implementation record

## Source changes performed

- **CS00/SA00:** Static manifest covering the Godot near/middle/far/world shader version graph; a measured-capture matrix and CSV p50/p95/p99 summarizer; source color-space/alpha/PBR/normal semantics baseline and parity matrix.
- **CS01:** New independent `.t3sc` fixed-width file and record contracts, 128+4+4 BC page payloads, CRC32 channel checks and unaligned-safe byte reader. Independent page mapping includes physical slot, composition revision and device generation; validity requires all three committed channels. No Sparse VT V3 version/package/binding change.
- **CS01 memory:** New `COMPOSED_SURFACE` memory-account domain and editor/runtime **cold** profile planner with grant/deny and two-kind transactional reservation/rollback. Feature is disabled by default; no physical GPU resources created in this wave. GPU reservations are not made until resource creation at CS05.
- **CS02:** Extracted shared packed-control/height-blend/weight-selection/material-accumulation math from the shipping clipmap raster into a no-derivatives `.glslinc`, consumed by both color and DNR. Added a separate compute fixture using explicit gradients to write albedo/normal/ORM scratch for pre-resolved material sources. No shipping raster replacement.

## Verification actually performed

- Standalone C++17 `test_cs01_contract.cpp` in runtime and editor native build modes, with `-Wall -Wextra -Werror -pthread`.
- 10,000 deterministic reference scenarios for height/control weighting (`test_cs02_math_reference.py`).
- Include topology, extracted-symbol parity, camera-independent shared-math checks, explicit compute gradients, and 294 discovered shader variants (`wave_a_shader_checks.py`).

## Unmet acceptance gates / deliberately not activated

- **CS00 GPU capture:** No Vulkan/engine binary or scene benchmark capture was supplied with this archive. Capture matrix exists; GPU timings, frame attribution, p50/p95/p99 are still unmeasured.
- **CS02 full sampling parity:** The standalone compositor is a *fixture*, not a shipping producer. Triplanar/auto-material control, VT V3 address/residency/filtering, authoring tint/coverage, dual-scale and other source-specific layers are not yet shared with the producer. The clipmap-vs-instance encoded sRGB filtering mismatch remains a documented baseline behavior; no automatic conversion was imposed.
- **Production shader compilation:** glslangValidator / spirv-val / Godot were unavailable in this execution environment; the GLSL changes were checked statically, not compiled to SPIR-V or executed on Vulkan.
- **Later stages:** No sidecar cooker, runtime residency/cache, RD resources, descriptors, shader substitution, editor live recomposition or performance claims (CS03–CS08) are made.

Wave A infrastructure has no effect on runtime page uploads/residency until the acceptance gate and downstream stages are implemented.
