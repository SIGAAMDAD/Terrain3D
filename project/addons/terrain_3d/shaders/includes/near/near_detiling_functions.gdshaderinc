// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

float random(in vec2 xy) {
	return fract(sin(dot(xy, vec2(12.9898, 78.233))) * 43758.5453);
}

uint detile_hash(const ivec2 p) {
	uvec2 q = uvec2(p);
	uint h = q.x * 0x9E3779B9u ^ q.y * 0x85EBCA6Bu;
	h ^= h >> 16u;
	h *= 0x7FEB352Du;
	h ^= h >> 15u;
	h *= 0x846CA68Bu;
	return h ^ (h >> 16u);
}

// Returns cos(angle), sin(angle), and scalar UV shift. The selected implementation
// is compiled from the sidebar detiling mode; no runtime mode branch remains.
vec3 get_detile_transform(const vec2 uv_center, const int texture_id) {
	// Godot rejects a dynamically indexed uniform-array element when it is passed
	// directly as a custom-function argument on some shader backends. Resolve it
	// inside the function so the call site passes an ordinary local integer.
	vec2 strength = _texture_detile_array[texture_id];
#if T3D_DETILING_MODE == 2
	return vec3(1.0, 0.0, 0.0);
#elif T3D_DETILING_MODE == 3
	uint h = detile_hash(ivec2(uv_center));
	float shift = (float((h >> 8u) & 0xFFFFu) * (2.0 / 65535.0) - 1.0) * strength.y * TAU;
	return vec3(1.0, 0.0, shift);
#elif T3D_DETILING_MODE == 1
	uint h = detile_hash(ivec2(uv_center));
	float enabled = step(0.000001, strength.x);
	vec2 cs = mix(vec2(1.0, 0.0), _detile_rotation_cs[int((h & 7u) << 1u)], enabled);
	float shift = (float((h >> 8u) & 0xFFFFu) * (2.0 / 65535.0) - 1.0) * strength.y * TAU;
	return vec3(cs, shift);
#elif T3D_DETILING_MODE == 4
	uint h = detile_hash(ivec2(uv_center));
	float enabled = step(0.000001, strength.x);
	vec2 cs = mix(vec2(1.0, 0.0), _detile_rotation_cs[int((h & 3u) << 2u)], enabled);
	float shift = (float((h >> 8u) & 0xFFFFu) * (2.0 / 65535.0) - 1.0) * strength.y * TAU;
	return vec3(cs, shift);
#elif T3D_DETILING_MODE == 5
	uint h = detile_hash(ivec2(uv_center));
	float enabled = step(0.000001, strength.x);
	vec2 cs = mix(vec2(1.0, 0.0), _detile_rotation_cs[int(h & 15u)], enabled);
	float shift = (float((h >> 8u) & 0xFFFFu) * (2.0 / 65535.0) - 1.0) * strength.y * TAU;
	return vec3(cs, shift);
#else
	vec2 detile = fma(random(uv_center), 2.0, -1.0) * strength * TAU;
	return vec3(cos(detile.x), sin(detile.x), detile.y);
#endif
}

vec2 rotate_vec2(const vec2 v, const vec2 cs) {
	return vec2(fma(cs.x, v.x, cs.y * v.y), fma(cs.x, v.y, -cs.y * v.x));
}
