// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// Shared compile-time constants and decode helpers used by all terrain render tiers.
#define COLOR_MAP_DEF vec4(1.0, 1.0, 1.0, 0.5)
#define DIV_255 0.003921568627450
#define DIV_1024 0.0009765625
#define TAU_16TH -0.392699081698724

#define DECODE_BLEND(control) float((control) >> 14u & 0xFFu) * DIV_255
#define DECODE_AUTO(control) bool((control) & 0x1u)
#define DECODE_BASE(control) int((control) >> 27u & 0x1Fu)
#define DECODE_OVER(control) int((control) >> 22u & 0x1Fu)
#define DECODE_ANGLE(control) float((control) >> 10u & 0xFu) * TAU_16TH
#define DECODE_SCALE(control) (0.9 - float((((control) >> 7u & 0x7u) + 3u) % 8u + 1u) * 0.1)
#define DECODE_HOLE(control) bool((control) >> 2u & 0x1u)
#define CONTROL_NON_MATERIAL_MASK 0x3FFFu

// Some low-end/mobile shader compilers reject the fma() builtin even when the
// equivalent arithmetic is valid, so keep this portable expansion centralized.
#define fma(a, b, c) ((a) * (b) + (c))

// Compatibility renderer derivative fallback is applied by Terrain3DMaterial
// after specialization. Forward+/Mobile retain their native coarse derivatives.
