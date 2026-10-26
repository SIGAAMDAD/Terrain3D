# Terrain3D HLOD recipe ABI (U05 retirement)

- Keep v1 W1-W7 `Terrain3DHLODPackage` metadata and binary manifest decoding read-compatible. `T3D_HLOD_LAYER_LEGACY_SIMPLIFIED=0` is a persisted historical identifier; it is not an editor cook option.
- New editor recipe resources default to `SIMPLIFIED=3`. Serialized value 0 migrates to value 3 on recipe load. New packages use format v2 cooked-mesh or v3/v4 instancing groups, recipe version 1, recipe hash schema 2.
- Native `simplify_mesh` remains as a compatibility entrypoint but now delegates to meshoptimizer with zero world error; baking callers use `simplify_mesh_with_error` with an explicit error ceiling.
- Dense native instancer chunks receive a **separate, explicit world-error budget**, limited to half the available screen-error budget. The final HLOD cook deducts measured inherited error before calling meshoptimizer. With insufficient quality budget, baking fails before publishing the runtime manifest.
- `MERGED` preserves topology; `SIMPLIFIED` uses meshoptimizer v1.3 with attribute-preserving, border-locked simplification and independent vertex-to-surface error measurement.
- Runtime instance-group variants: v3 single-prototype and v4 multi-prototype; each prototype maps to WORLD source/cell publication. Runtime group visibility waits for a full publication generation ACK.
- Runtime resource/shader ABI and the W1-W7 physical geometry cache are unchanged.
