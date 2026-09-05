# Terrain3D P01->P05 + ownership/capacity repair

## Composition

This tree is the cumulative P01/P02 + P03/P04/P05 performance/128 m patch lineage with the 2026-09-05 owner-controlled allocation, exact GPU terrain-page grant, and runtime-page prepared-byte capacity repairs applied on top.

P05 asynchronous runtime-material streaming is preserved. No post-P05 material-bootstrap, Near/control publication, Far-albedo, or later P06+ changes are included.

## Ownership repair

Runtime allocation phases are telemetry only. The raw allocation choke point no longer rejects HOT_FRAME or HOT_RENDER and no allocator derives permission from the current phase. Retained-capacity growth remains the responsibility of the subsystem owner, fixed/slab seal, or explicitly bound allocation domain.

## GPU terrain-page repair

GPU cache capacity uses the exact per-layer resources that rebuildTextures actually creates: height, control, optional color, optional derived data, Middle composite, and terrain-shadow mip chains. Mapped terrain pages take priority over optional retirement/spare capacity. The two-page safety minimum remains mandatory; the configured spare target is best-effort within the exact byte grant.

The physical layout is validated before RD texture creation and cannot knowingly exceed its managed grant.

## Runtime-page repair

Prepared-page capacity computes an effective worker count from the actual grant while reserving at least one payload slot. If two workers do not fit, the store degrades to one. If no legal one-worker layout fits, the failure is latched and reported once until a budget/topology/configuration/cook change can alter the result. A failed rebuild does not destroy a valid retained prepared image before replacement capacity is proven.

## P05 preservation

The P05 worker-side cooked material preparation contract remains present, including runtimeMaterialWorkerMain, request/completion rings, disk/GPU token buckets, and non-blocking initial priority seeding.

## Validation performed

- P03/P04/P05 combined contract updated for telemetry-only allocator ownership: PASS.
- Owner-controlled allocation A/B/C, D/E/F, G gates: PASS.
- READY/cold-phase boundary gate: PASS.
- Initialization/residency ownership gate: PASS.
- RD startup capacity/shader-bound gate: PASS.
- Systems-C ownership/style gates: PASS.
- P04 standalone allocator test: HOT_FRAME/HOT_RENDER allocations succeed and are recorded as telemetry: PASS.
- P01 standalone memory-budget test, including the observed 4,950,062-byte GPU grant regression: PASS.
