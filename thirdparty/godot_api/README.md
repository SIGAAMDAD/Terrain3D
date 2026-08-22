# Godot 4.7.1 GDExtension API

`extension_api-4.7.1.json` was dumped from the supplied Godot 4.7.1 stable binary.

godot-cpp 10.0.0-rc1 predates Godot 4.7 and its generator emits the new integer limit globals (`UINT8_MAX`, `INT32_MIN`, and related names) as C++ constants, colliding with the standard `<stdint.h>` macros. Those eleven redundant integer-limit globals are removed from this copy of the API. Class, method, enum, and builtin API data is otherwise unchanged.
