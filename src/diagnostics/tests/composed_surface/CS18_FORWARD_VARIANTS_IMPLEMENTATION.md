# CS18 / CS18 - Forward+ optional composed-surface variants

The existing shader names, per-pass shader IDs, Sparse VT V3 format, and normal world/clipmap bindings remain unchanged. Ten new `cs_terrain_strip_*` variants extend the world raster catalog after its existing IDs: eight color combinations and two DNR combinations. Depth-only, mesh and shadow programs remain unchanged.

The World raster and World runtime expose a default-off **cold-only** `configureComposedExperimentalCold(bool)` switch. The owning coordinator must deliberately configure it before World runtime initialization, after preparing a validated composed page pool. When the switch is on, the Forward+ preparer borrows composed GPU resources from `Terrain3DSharedVTRuntime`; failure to acquire a coherent pool fails the world PREPARE transaction. Binding 13 receives the composed lookup SSBO, while bindings 14-16 receive the BC1/BC5/BC7 physical arrays in world set 1. The descriptor signature includes pool RID identity and device generation, preventing reuse across cold resource replacement. The original path builds no extra descriptors.

**Acceptance remains open:** No code enables the experimental switch by default. The complete cold source cook and runtime population lifecycle, Godot world shader/SPIR-V compilation, actual Forward+ descriptor reflection and Vulkan parity captures are required before enabling it in normal use. No performance gain is claimed. Existing source VT feedback and runtime material fallbacks remain authoritative.

Run `python3 src/diagnostics/tests/composed_surface/test_cs18_world_activation.py` for source-level invariants. It verifies shader naming and binding construction only, not runtime compilation.
