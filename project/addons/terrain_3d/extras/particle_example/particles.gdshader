// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// This is an example particle shader designed to procedurally place
// particles like grass, small rocks, and other ground effects, on the terrain
// surface by reading the Terrain3D data maps. It works in tandem with the
// provided GDScript.

shader_type particles;
render_mode disable_velocity, disable_force;

group_uniforms options;
uniform sampler2D main_noise;
uniform float main_noise_scale = 0.01;
uniform vec3 position_offset = vec3(0.);
uniform bool align_to_normal = true;
uniform float normal_strength : hint_range(0.01, 1.0, 0.01) = 0.3;
uniform bool random_rotation = true;
uniform float random_spacing : hint_range(0.0, 1.0, 0.01) = 0.5;
uniform vec3 min_scale = vec3(1.0);
uniform vec3 max_scale = vec3(1.0);

group_uniforms wind;
uniform float noise_scale = 0.0041;
uniform float wind_speed = 0.025;
uniform float wind_strength : hint_range(0.0, 1.0, 0.01) = 1.0;
uniform float wind_dithering = 4.0;
uniform vec2 wind_direction = vec2(1.0,1.0);

group_uniforms shaping;
uniform float clod_scale_boost = 3.0;
uniform float clod_min_threshold : hint_range(0.0, 1.0, 0.001) = 0.2;
uniform float clod_max_threshold : hint_range(0.0, 1.0, 0.001) = 0.5;
uniform float patch_min_threshold : hint_range(0.0, 1.0, 0.001) = 0.025;
uniform float patch_max_threshold : hint_range(0.0, 1.0, 0.001) = 0.2;

group_uniforms filtering;
uniform int texture_filter : hint_range(-1, 31) = -1; // Specify a texture ID or -1 for all
uniform float condition_dither_range : hint_range(0.0, 1.0, 0.01) = 0.15;
uniform float surface_slope_min : hint_range(0.0, 1.0, 0.01) = 0.87;
uniform float distance_fade_ammount : hint_range(0.0, 1.0, 0.01) = 0.5;

group_uniforms private;
uniform float ground_level : hint_range(-1000., 1000.) = -20.0;
uniform float region_blend : hint_range(.001, 1., 0.001) = 0.25;
uniform float max_dist = 1.;
uniform vec3 camera_position = vec3(0.);
uniform uint instance_rows = 1;
uniform float instance_spacing = 0.5;
uniform uint _background_mode = 0u;
uniform float _vertex_spacing = 1.0;
uniform float _vertex_density = 1.0; // = 1/_vertex_spacing
const int T3D_REGION_SIZE = 128;
const int T3D_REGION_MASK = 127;
const float T3D_REGION_SIZE_F = 128.0;
const float T3D_REGION_INV_SIZE = 0.0078125;
const int T3D_REGION_MAP_SIZE = 32;
const int T3D_REGION_MAP_HALF = 16;

ivec2 t3d_region_cell_from_grid(const ivec2 grid) {
	return ivec2(floor(vec2(grid) * T3D_REGION_INV_SIZE));
}

ivec2 t3d_region_local_from_grid(const ivec2 grid, const ivec2 region_cell) {
	return grid - region_cell * T3D_REGION_SIZE;
}
uniform sampler2D _region_map_texture : filter_nearest, repeat_disable;
uniform vec2 _region_locations[1024];
uniform vec2 _height_decode = vec2(-384.0, 1152.0);
uniform highp sampler2DArray _height_maps : repeat_disable;

#define fma(a, b, c) ((a) * (b) + (c))

float decode_terrain_height(float encoded) {
	return fma(encoded, _height_decode.y, _height_decode.x);
}
uniform highp sampler2DArray _control_maps : repeat_disable;
uniform highp sampler2DArray _color_maps : repeat_disable;
uniform bool _control_maps_implicit = false;
uniform bool _color_maps_implicit = false;

uint read_terrain_control(ivec3 index) {
	return _control_maps_implicit ? 1u :
		(index.z >= 0 ? floatBitsToUint(texelFetch(_control_maps, index, 0).r) : 1u);
}

vec4 read_terrain_color(ivec3 index) {
	const vec4 default_color = vec4(1.0, 1.0, 1.0, 0.5);
	return _color_maps_implicit ? default_color :
		(index.z >= 0 ? texelFetch(_color_maps, index, 0) : default_color);
}

// Takes in world space XZ (UV) coordinates
// Returns ivec3 with:
// XY: (0 to T3D_REGION_SIZE_F - 1) coordinates within a region
// Z: layer index used for texturearrays, -1 if not in a region
ivec3 get_index_coord(const vec2 uv) {
	vec2 r_uv = round(uv);
	ivec2 pos = ivec2(floor(r_uv * T3D_REGION_INV_SIZE)) + T3D_REGION_MAP_HALF;
	int bounds = int(uint(pos.x | pos.y) < uint(T3D_REGION_MAP_SIZE));
	int layer_index = (int(round(texelFetch(_region_map_texture, pos, 0).r * 65535.0)) * bounds) - 1;
	return ivec3(t3d_region_local_from_grid(ivec2(r_uv), t3d_region_cell_from_grid(ivec2(r_uv))), layer_index);
}


// Takes in UV2 region space coordinates, returns 1.0 or 0.0 if a region is present or not.
float check_region(const vec2 uv2) {
	ivec2 pos = ivec2(floor(uv2)) + T3D_REGION_MAP_HALF;
	int layer_index = 0;
	if (uint(pos.x | pos.y) < uint(T3D_REGION_MAP_SIZE)) {
		layer_index = clamp(int(round(texelFetch(_region_map_texture, pos, 0).r * 65535.0)) - 1, -1, 0) + 1;
	}
	return float(layer_index);
}

// Takes in UV2 region space coordinates, returns a blend value (0 - 1 range) between empty, and valid regions
float get_region_blend(vec2 uv2) {
	uv2 -= 0.5011; // correct for floating point error
	const vec2 offset = vec2(0.0, 1.0);
	float a = check_region(uv2 + offset.xy);
	float b = check_region(uv2 + offset.yy);
	float c = check_region(uv2 + offset.yx);
	float d = check_region(uv2 + offset.xx);
	vec2 blend_factor = vec2(2.0 + 126.0 * (1.0 - region_blend));
	vec2 f = fract(uv2);
	vec2 w = 1.0 / (1.0 + exp(blend_factor * log((1.0 - f) / f)));
	float blend = mix(mix(d, c, w.x), mix(a, b, w.x), w.y);
	return (1.0 - blend) * 2.0;
}

float get_height(vec2 index_id, vec2 offset) {
	float height = decode_terrain_height(texelFetch(_height_maps, get_index_coord(index_id + offset), 0).r);
	if (_background_mode != 0u) {
		float blend = get_region_blend(index_id * T3D_REGION_INV_SIZE + offset * T3D_REGION_INV_SIZE);
		height = mix(height, ground_level, smoothstep(0., 1., blend));
	}

	return height;
}

float random(vec2 v) {
	return fract(1e4 * sin(fma(17.0, v.x, v.y * 0.1)) * (0.1 + abs(sin(fma(v.y, 13.0, v.x)))));
}

mat3 rotation_matrix(vec3 axis, float angle) {
	float c = cos(angle);
	float s = sin(angle);
	float t = 1.0 - c;
	vec3 n = normalize(axis);
	float x = n.x;
	float y = n.y;
	float z = n.z;

	return mat3(
		vec3(t * x * x + c, t * x * y - z * s, t * x * z + y * s),
		vec3(t * x * y + z * s, t * y * y + c, t * y * z - x * s),
		vec3(t * x * z - y * s, t * y * z + x * s, t * z * z + c));
}

mat3 align_to_vector(vec3 normal) {
	vec3 up = vec3(0.0, 1.0, 0.0);
	if (abs(dot(normal, up)) > 0.9999) { // Avoid singularity
		up = vec3(1.0, 0.0, 0.0);
	}
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = normalize(cross(tangent, normal));
	return mat3(tangent, normal, bitangent);
}

void start() {
	// Create centered a grid
	vec3 pos = vec3(float(INDEX % instance_rows), 0.0, float(INDEX / instance_rows)) - float(instance_rows >>1u);

	// Apply spcaing
	pos *= instance_spacing;
	// Move the grid to the emitter, snapping is handled CPU side
	pos.xz += EMISSION_TRANSFORM[3].xz;

	// Create random values per-instance, incorporating the seed, mask bits to avoid NAN/INF
	float seed = fract(uintBitsToFloat(RANDOM_SEED & 0x7EFFFFFFu));
	vec3 r = fract(vec3(random(pos.xz), random(pos.xz + vec2(0.5)), random(pos.xz - vec2(0.5))) + seed);
	// Randomize instance spacing
	pos.x += ((r.x * 2.0) - 1.0) * random_spacing * instance_spacing;
	pos.z += ((r.z * 2.0) - 1.0) * random_spacing * instance_spacing;

	// Lookup offsets, ID and blend weight
	const vec3 offsets = vec3(0, 1, 2);
	vec2 index_id = floor(pos.xz * _vertex_density);
	vec2 weight = fract(pos.xz * _vertex_density);
	vec2 invert = 1.0 - weight;
	vec4 weights = vec4(
		invert.x * weight.y, // 0
		weight.x * weight.y, // 1
		weight.x * invert.y, // 2
		invert.x * invert.y  // 3
	);

	ivec3 index[4];
	// Map lookups
	index[0] = get_index_coord(index_id + offsets.xy);
	index[1] = get_index_coord(index_id + offsets.yy);
	index[2] = get_index_coord(index_id + offsets.yx);
	index[3] = get_index_coord(index_id + offsets.xx);

	highp float h[8];
	h[0] = get_height(index_id, offsets.xy); // 0 (0,1)
	h[1] = get_height(index_id, offsets.yy); // 1 (1,1)
	h[2] = get_height(index_id, offsets.yx); // 2 (1,0)
	h[3] = get_height(index_id, offsets.xx); // 3 (0,0)
	h[4] = get_height(index_id, offsets.yz); // 4 (1,2)
	h[5] = get_height(index_id, offsets.zy); // 5 (2,1)
	h[6] = get_height(index_id, offsets.zx); // 6 (2,0)
	h[7] = get_height(index_id, offsets.xz); // 7 (0,2)

	vec3 index_normal[4];
	index_normal[0] = vec3(h[0] - h[1], _vertex_spacing, h[0] - h[7]);
	index_normal[1] = vec3(h[1] - h[5], _vertex_spacing, h[1] - h[4]);
	index_normal[2] = vec3(h[2] - h[6], _vertex_spacing, h[2] - h[1]);
	index_normal[3] = vec3(h[3] - h[2], _vertex_spacing, h[3] - h[0]);
	vec3 w_normal = normalize(
		index_normal[0] * weights[0] +
		index_normal[1] * weights[1] +
		index_normal[2] * weights[2] +
		index_normal[3] * weights[3]);

	// Set the height according to the heightmap data
	pos.y =
		h[0] * weights[0] +
		h[1] * weights[1] +
		h[2] * weights[2] +
		h[3] * weights[3] ;

	// Offset, Rotation, Alignment.
	TRANSFORM = mat4(1.0);
	vec3 orientation = vec3(0., 1., 0.);

	vec2 uv = (pos.xz) * main_noise_scale;
	float noise = textureLod(main_noise, uv, 0.0).r;
	float clods = smoothstep(clod_min_threshold, clod_max_threshold, noise) * clod_scale_boost;
	float patch = smoothstep(patch_min_threshold, patch_max_threshold, noise);
	float width_modifier = 1.0 + 3.0 * smoothstep(0., max_dist, length(camera_position - pos));

	// Calculate scale
	vec3 scale = vec3(
		mix(min_scale.x, max_scale.x, r.x) * width_modifier,
		mix(min_scale.y, max_scale.y, r.y) + clods,
		mix(min_scale.z, max_scale.z, r.z) * width_modifier) * patch;

	// Apply scale to offset
	vec3 offset = position_offset * scale;

	// Apply normal orientation
	if (align_to_normal) {
		orientation = mix(orientation, w_normal, normal_strength);
		mat3 alignment = align_to_vector(orientation);
		offset = alignment * offset;
		TRANSFORM = mat4(alignment);
	}

	// Apply rotation around orientation
	if (random_rotation) {
		mat3 rotation = rotation_matrix(orientation, r.x * TAU);
		TRANSFORM = mat4(rotation) * TRANSFORM;
	}

	// Filtering - Causes some particles to be rendered as degenerate triangles
	// via 0./0. - Particles filtered this way are still processed by the GPU.
	// For compatibility it seems we must shift as well.
	// Surface slope filtering
	if (surface_slope_min > w_normal.y + (r.y - 0.5) * condition_dither_range) {
		pos.y = 0. / 0.;
		pos.xz = vec2(100000.0);
	}

	// Read color map. Fully implicit terrain binds one shared default layer,
	// so the external particle shader must bypass region-layer indexing.
	highp vec4 c[4];
	c[0] = read_terrain_color(index[0]); // 0 (0,1)
	c[1] = read_terrain_color(index[1]); // 1 (1,1)
	c[2] = read_terrain_color(index[2]); // 2 (1,0)
	c[3] = read_terrain_color(index[3]); // 3 (0,0)
	vec4 color_map =
		c[0] * weights[0] +
		c[1] * weights[1] +
		c[2] * weights[2] +
		c[3] * weights[3] ;

	COLOR = color_map;

	// Read control maps
	uvec4 control = uvec4(
		read_terrain_control(index[0]),
		read_terrain_control(index[1]),
		read_terrain_control(index[2]),
		read_terrain_control(index[3]));
	bool hole = any(bvec4(control >> uvec4(2u) & uvec4(0x1u)));
	bool auto = any(bvec4(control & uvec4(0x1u)));
	int base = int(control[3] >> 27u & 0x1Fu);
	int over = int(control[3] >> 22u & 0x1Fu);
	float blend = float(control[3] >> 14u & 0xFFu) * 0.003921568627450; // 1. / 255.

	// Filter out holes
	if (hole) {
		pos.y = 0. / 0.;
		pos.xz = vec2(100000.0);
	}

	// Apply only to specified texture id, or -1 for all. Adjust blend values as needed
	if (!auto  && texture_filter >= 0 &&
		((base != texture_filter && blend < 0.7) ||
		(over != texture_filter && blend >= 0.3))) {
			pos.y = 0. / 0.;
			pos.xz = vec2(100000.0);
	}

	if (length(camera_position - pos) > max_dist || get_index_coord(pos.xz).z < 0) {
		pos.y = 0. / 0.;
		pos.xz = vec2(100000.0);
	} else {
		float fade_factor = 1.0 - smoothstep(max_dist * distance_fade_ammount, max_dist + 0.0001, length(camera_position - pos)) + 0.001;
		scale.y *= fade_factor;
		offset *= fade_factor;
	}

	// Apply scale
	TRANSFORM[0] *= scale.x;
	TRANSFORM[1] *= scale.y;
	TRANSFORM[2] *= scale.z;

	// Apply the position
	TRANSFORM[3].xyz = pos.xyz + offset;

	// Save Fixed 2 Random values for Reg/Green color randomness
	CUSTOM.rg = r.rg;
	// Save Y component scale pre-rotation
	CUSTOM[3] = scale.y;
}

void process() {
	// Extract world space UV from Transform Matrix
	vec2 uv = (TRANSFORM[3].xz + CUSTOM.rg * wind_dithering) * noise_scale;
	// Scaled wind noise, updated per instance, at process FPS. Passed to Vertex()
	CUSTOM[2] = textureLod(main_noise, uv + TIME * wind_speed * normalize(wind_direction), 0.0).r * wind_strength;
}
