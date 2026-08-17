// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// T3D_STANDALONE_DEFAULTS_BEGIN
// ShaderInclude resources are parsed independently by the editor. Runtime-generated
// shaders inject the selected values before these guards, so local defaults only
// apply when this module is parsed without its Terrain3D parent shader.
#ifndef T3D_FAST_BLEND_CURVE
#define T3D_FAST_BLEND_CURVE 0
#endif
// T3D_STANDALONE_DEFAULTS_END


#ifdef T3D_ENABLE_BLEND_WEIGHT_FUNCTIONS
// Height-blend weighting is called for every sampled material. Performance
// presets quantize the dynamic exponent to coherent powers of two, replacing
// log2/exp2 with multiplies. Quality/custom variants retain the exact curve.
float terrain_blend_power(float value, float sharpness) {
	value = max(value, 1e-8);
#if T3D_FAST_BLEND_CURVE
	float value_2 = value * value;
	float value_4 = value_2 * value_2;
	if (sharpness < 6.0) {
		return value_4;
	}
	float value_8 = value_4 * value_4;
	if (sharpness < 12.0) {
		return value_8;
	}
	float value_16 = value_8 * value_8;
	if (sharpness < 24.0) {
		return value_16;
	}
	float value_32 = value_16 * value_16;
	if (sharpness < 48.0) {
		return value_32;
	}
	return value_32 * value_32;
#else
	return exp2(sharpness * log2(value));
#endif
}
#endif
