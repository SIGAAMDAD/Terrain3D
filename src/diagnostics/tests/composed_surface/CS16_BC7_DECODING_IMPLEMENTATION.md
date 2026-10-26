# CS16 / CS16 - Deterministic BC7 mode-6 source decoding

## Implemented

- The cold scalar decoder now accepts **BC7 mode 6** (128-bit RGBA blocks) using the same field order and 16-entry interpolation weights as `t3d_vt_reference_encode_bc7_mode6_block()`.
- The decoder checks **every block's mode prefix before any output writes**, rejecting unsupported modes and invalid byte spans. The output is unchanged for a rejected BC7 tile.
- `t3d_cs12_decode_godot_image()` routes all-mode-6 tiles directly to the deterministic scalar decoder and retains Godot's BPTC decompression path for other BC7 modes.
- The native V3 source-package fixture now uses actual BC7 mode-6 payloads and the scalar decoder for all three packed channels, replacing its previous synthetic fixed-PBR callback.
- A 136x136 mode-6 regression fixture validates endpoint p-bits, 3-bit anchor, 4-bit texel indices, RGBA interpolation, mixed-mode rejection and invalid-size rejection under AddressSanitizer and UndefinedBehaviorSanitizer.

## Run locally

```
sh src/diagnostics/tests/composed_surface/run_cs16_bc7_mode6.sh
T3D_GODOT_CPP_ROOT=/path/to/godot-cpp sh src/diagnostics/tests/composed_surface/run_cs15_native_pack.sh
```

The first command is self-contained. The second command runs scalar decoding tests and Godot 4.7 syntax checks if the generated bindings exist. Linking/executing the native V3 fixture still requires a linked godot-cpp library or a Godot host test target; the supplied dependency snapshot contains generated headers but no built library.

## Explicit boundaries

- Other BC7 modes are still decoded by Godot during cold authoring.
- This implementation **does not establish visual parity** with the GPU's BC7 sampler. Such parity requires a matched Vulkan capture on the target GPU/driver.
- No changes to `.t3sp` V3 pack ABI, `.t3sc` sidecar ABI, descriptor bindings, runtime resource ownership, render-pass variants or activation flags.
- Composed sampling remains disabled pending Forward+ integration and material-equivalence testing.
