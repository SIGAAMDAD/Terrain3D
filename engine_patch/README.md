# External Godot 4.7.2 Forward+ bridge requirement

Terrain3D no longer owns or distributes a Godot source patch. Production RenderingDevice rendering requires a separately maintained `4.7.2-stable` Forward+ editor/template exposing the Engine singleton `Terrain3DForwardPlusBridge`.

Required native contract:

- ABI version: `10`
- capabilities: direct POD, depth, opaque, shadow, motion, transparent, clustered lighting, decals, reflections, resource generations, and split shadow packet
- packet sizes: frame `472`, resources `120`, ordinary pass `48`, shadow `200` bytes
- cold methods: `get_abi_version`, `get_native_capabilities`, `register_native_callback`, `unregister_native_callback`, `has_native_callback`, `collect_retired_callbacks`
- callbacks: ordinary PREPARE, split shadow PREPARE, and serial-only SUBMIT

Terrain3D rejects missing methods, any ABI/size/capability mismatch, or failed registration. There is no stock-engine, Dictionary/Callable, RenderingServer geometry, or reduced-feature fallback route.

Set `RD_GODOT_SOURCE` to the external patched Godot 4.7.2 source tree when running deep cross-repository validation. Its absence must not block Terrain3D-only static/build checks, but the patched engine is required for release runtime validation.
