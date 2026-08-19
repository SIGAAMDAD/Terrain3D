#ifdef T3D_TEMPLATE_CONTEXT
shader_type spatial;
render_mode depth_draw_opaque, cull_back, unshaded, skip_vertex_transform;

uniform vec3 _target_pos = vec3(0.0);
uniform float _mesh_size = 48.0;
uniform float _subdiv = 1.0;
uniform float _vertex_spacing = 1.0;
uniform float _vertex_density = 1.0;
uniform float _region_size = 1024.0;
uniform float _region_texel_size = 0.0009765625;
uniform vec2 _height_decode = vec2(-384.0, 1152.0);
uniform int _region_map_size = 32;
uniform int _shadow_height_source = 0;
uniform sampler2D _region_map_texture : filter_nearest, repeat_disable;
uniform sampler2D _middle_height_region_map_texture : filter_nearest, repeat_disable;
uniform highp sampler2DArray _height_maps : filter_linear, repeat_disable;
uniform highp sampler2DArray _middle_height_maps : filter_linear, repeat_disable;
uniform highp sampler2D _far_height_map : filter_linear, repeat_disable;
uniform vec4 _clipmap_lod_states[16];

float decode_terrain_height(float encoded) {
	return encoded * _height_decode.y + _height_decode.x;
}

int shadow_page_layer(bool middle, ivec2 map_pos) {
	if (uint(map_pos.x | map_pos.y) >= uint(_region_map_size)) {
		return -1;
	}
	float encoded = middle ? texelFetch(_middle_height_region_map_texture, map_pos, 0).r :
		texelFetch(_region_map_texture, map_pos, 0).r;
	return int(round(encoded * 65535.0)) - 1;
}

bool shadow_far_contains(vec2 world_texel) {
	float half_world_size = _region_size * float(_region_map_size) * 0.5;
	return all(greaterThanEqual(world_texel, vec2(-half_world_size))) &&
		all(lessThan(world_texel, vec2(half_world_size)));
}

float shadow_far_height(vec2 world_texel) {
	if (!shadow_far_contains(world_texel)) {
		return 0.0;
	}
	float world_size = _region_size * float(_region_map_size);
	ivec2 atlas_size = textureSize(_far_height_map, 0);
	vec2 sample_coord = ((world_texel + vec2(world_size * 0.5)) / world_size) * vec2(atlas_size);
	vec2 half_texel = vec2(0.5) / vec2(atlas_size);
	vec2 atlas_uv = clamp((sample_coord + vec2(0.5)) / vec2(atlas_size), half_texel, vec2(1.0) - half_texel);
	return decode_terrain_height(textureLod(_far_height_map, atlas_uv, 0.0).r);
}

float shadow_page_height(vec2 world_texel, bool middle) {
	vec2 rounded_uv = round(world_texel);
	ivec2 map_pos = ivec2(floor(rounded_uv * _region_texel_size)) + (_region_map_size / 2);
	int layer = shadow_page_layer(middle, map_pos);
	if (layer < 0) {
		return middle ? shadow_far_height(world_texel) : 0.0;
	}
	vec2 local_texel = mod(rounded_uv, _region_size);
	ivec2 page_size = middle ? textureSize(_middle_height_maps, 0).xy : textureSize(_height_maps, 0).xy;
	vec2 page_uv = (local_texel * vec2(page_size) / _region_size + vec2(0.5)) / vec2(page_size);
	float encoded = middle ?
		textureLod(_middle_height_maps, vec3(page_uv, float(layer)), 0.0).r :
		textureLod(_height_maps, vec3(page_uv, float(layer)), 0.0).r;
	return decode_terrain_height(encoded);
}

vec4 shadow_lod_state(float lod_code) {
	return _clipmap_lod_states[clamp(int(round(lod_code)), 0, 15)];
}

vec2 shadow_stitch_state(float packed_code) {
	int packed = clamp(int(round(packed_code)), 0, 8);
	return vec2(float(packed % 3), float(packed / 3));
}

float shadow_edge_axis_offset(int edge_index, int test_axis) {
	if (edge_index == 0) {
		return test_axis == 2 ? -(4.0 * _mesh_size + 6.0) : 0.0;
	}
	if (test_axis == 0) {
		return 4.0 * _mesh_size + 8.0;
	}
	return test_axis == 2 ? -2.0 : 0.0;
}

vec2 shadow_edge_offset(float part_code, vec2 stitch_state) {
	int part = int(round(part_code));
	int test_x = clamp(int(round(stitch_state.x)), 0, 2);
	int test_z = clamp(int(round(stitch_state.y)), 0, 2);
	if (part == 1) return vec2(shadow_edge_axis_offset(0, test_x), -2.0 * float(test_z - 1));
	if (part == 2) return vec2(shadow_edge_axis_offset(1, test_x), -2.0 * float(test_z - 1));
	if (part == 3) return vec2(0.0, shadow_edge_axis_offset(0, test_z));
	if (part == 4) return vec2(0.0, shadow_edge_axis_offset(1, test_z));
	return vec2(0.0);
}

void vertex() {
	vec2 metadata = UV2;
	vec4 lod_state = shadow_lod_state(metadata.y);
	vec3 grid_vertex = VERTEX;
	grid_vertex.xz += shadow_edge_offset(metadata.x, shadow_stitch_state(lod_state.w));
	VERTEX.xz = grid_vertex.xz * lod_state.z + lod_state.xy;
	vec3 world_vertex = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float scale = lod_state.z;
	float inv_scale = 1.0 / scale;
	float max_xz = max(abs(world_vertex.x - _target_pos.x), abs(world_vertex.z - _target_pos.z));
	float vertex_lerp = smoothstep(0.0, 1.0, (max_xz * inv_scale - _mesh_size - 4.0) / (_mesh_size - 4.0));
	vec2 vertex_fract = fract(grid_vertex.xz * 0.5) * 2.0;
	vec2 shift = scale < _vertex_spacing / _subdiv + 1e-6 ?
		mix(vertex_fract, vec2(vertex_fract.x, -vertex_fract.y),
			round(fract(round(mod(world_vertex.z * inv_scale, 4.0)) * round(mod(world_vertex.x * inv_scale, 4.0)) * 0.25))) :
		vertex_fract * round((fract(world_vertex.xz * 0.25 * inv_scale) - 0.5) * 4.0);
	world_vertex.xz -= shift * scale * vertex_lerp;
	vec2 world_texel = world_vertex.xz * _vertex_density;
	world_vertex.y = _shadow_height_source == 0 ? shadow_page_height(world_texel, false) :
		(_shadow_height_source == 1 ? shadow_page_height(world_texel, true) : shadow_far_height(world_texel));
	VERTEX = (VIEW_MATRIX * vec4(world_vertex, 1.0)).xyz;
}

void fragment() {
}
#endif // T3D_TEMPLATE_CONTEXT
