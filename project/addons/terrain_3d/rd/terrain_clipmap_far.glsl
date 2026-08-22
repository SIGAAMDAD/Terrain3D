#[vertex]
#version 450
#define T3D_TIER 2

layout(location = 0) in ivec2 in_grid;
layout(location = 1) in uvec2 in_meta;

layout(set = 0, binding = 0, std140) uniform FrameData {
	mat4 view_projection;
	vec4 light_direction_ambient;
	vec4 light_color;
	vec4 terrain_params;
	vec4 mesh_params;
	vec4 target_params;
	vec4 camera_params;
	vec4 fog_params;
	vec4 fog_depth_params;
	vec4 displacement_params;
} frame_data;

layout(set = 0, binding = 1, std430) readonly buffer RegionMapData { int region_map[]; };
layout(set = 0, binding = 2) uniform sampler2DArray height_maps;
layout(set = 0, binding = 12) uniform sampler2D displacement_buffer;

struct PatchData {
	vec2 origin;
	float scale;
	uint lod;
	uint tier;
	uint stitch_x;
	uint stitch_z;
	uint flags;
};
layout(set = 0, binding = 9, std430) readonly buffer PatchBuffer { PatchData patches[]; };
layout(set = 0, binding = 10, std430) readonly buffer VisibleBuffer { uint visible_indices[]; };

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) flat out uint patch_tier;

int floor_div_i(int value, int divisor) {
	int q = value / divisor;
	int r = value - q * divisor;
	return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}

int floor_mod_i(int value, int divisor) { return value - floor_div_i(value, divisor) * divisor; }

int region_layer(ivec2 region_cell) {
	int map_size = int(frame_data.terrain_params.y + 0.5);
	ivec2 map_pos = region_cell + ivec2(map_size / 2);
	if (map_pos.x < 0 || map_pos.y < 0 || map_pos.x >= map_size || map_pos.y >= map_size) return -1;
	return region_map[map_pos.y * map_size + map_pos.x] - 1;
}

float decode_height(float encoded) { return fma(encoded, frame_data.terrain_params.w, frame_data.terrain_params.z); }

float sample_height_texel(ivec2 texel) {
	int region_size = int(frame_data.terrain_params.x + 0.5);
	ivec2 region_cell = ivec2(floor_div_i(texel.x, region_size), floor_div_i(texel.y, region_size));
	int layer = region_layer(region_cell);
	if (layer < 0) return frame_data.terrain_params.z;
	ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
	ivec3 size3 = textureSize(height_maps, 0);
	local = clamp(local, ivec2(0), size3.xy - ivec2(1));
	return decode_height(texelFetch(height_maps, ivec3(local, layer), 0).r);
}

float sample_height_nearest(vec2 world_xz) {
	vec2 world_texel = world_xz / frame_data.mesh_params.x;
	return sample_height_texel(ivec2(round(world_texel)));
}

float sample_height_interpolated(vec2 world_xz) {
	vec2 world_texel = world_xz / frame_data.mesh_params.x;
	ivec2 base = ivec2(floor(world_texel));
	vec2 f = fract(world_texel);
	float h00 = sample_height_texel(base);
	float h10 = sample_height_texel(base + ivec2(1, 0));
	float h01 = sample_height_texel(base + ivec2(0, 1));
	float h11 = sample_height_texel(base + ivec2(1, 1));
	return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
}

float edge_axis_offset(int edge_index, int test_axis) {
	float mesh_size = frame_data.mesh_params.y;
	if (edge_index == 0) return test_axis == 2 ? -(4.0 * mesh_size + 6.0) : 0.0;
	if (test_axis == 0) return 4.0 * mesh_size + 8.0;
	return test_axis == 2 ? -2.0 : 0.0;
}

vec2 edge_offset(uint part_code, PatchData patch_data) {
	int part = int(part_code);
	int sx = clamp(int(patch_data.stitch_x), 0, 2);
	int sz = clamp(int(patch_data.stitch_z), 0, 2);
	if (part == 1) return vec2(edge_axis_offset(0, sx), -2.0 * float(sz - 1));
	if (part == 2) return vec2(edge_axis_offset(1, sx), -2.0 * float(sz - 1));
	if (part == 3) return vec2(0.0, edge_axis_offset(0, sz));
	if (part == 4) return vec2(0.0, edge_axis_offset(1, sz));
	return vec2(0.0);
}

vec3 clipmap_morph(vec2 world_xz, vec2 grid_xz, float scale) {
	float inv_scale = 1.0 / max(scale, 1e-6);
	vec2 target_xz = frame_data.target_params.xy;
	float max_xz = max(abs(world_xz.x - target_xz.x), abs(world_xz.y - target_xz.y));
	float denominator = max(frame_data.mesh_params.y - 4.0, 1.0);
	float morph = smoothstep(0.0, 1.0, (max_xz * inv_scale - frame_data.mesh_params.y - 4.0) / denominator);
	vec2 vertex_fract = fract(grid_xz * 0.5) * 2.0;
	vec2 shift;
	float subdiv = max(frame_data.target_params.z, 1.0);
	if (scale < frame_data.mesh_params.x / subdiv + 1e-6) {
		float alternate = round(fract(round(mod(world_xz.y * inv_scale, 4.0)) * round(mod(world_xz.x * inv_scale, 4.0)) * 0.25));
		shift = mix(vertex_fract, vec2(vertex_fract.x, -vertex_fract.y), alternate);
	} else {
		shift = vertex_fract * round((fract(world_xz * 0.25 * inv_scale) - 0.5) * 4.0);
	}
	return vec3(shift, morph);
}

vec3 sample_clipmap_displacement(vec2 world_xz, float clipmap_scale) {
	if (frame_data.displacement_params.y < 0.5 || frame_data.target_params.z <= 1.0) return vec3(0.0);
	float vertex_density = 1.0 / max(frame_data.mesh_params.x, 1e-6);
	float lod_scale = floor(log2(1.0 / max(clipmap_scale * vertex_density, 1e-6)));
	float sample_scale = exp2(lod_scale) * 0.5;
	float subdivision_levels = max(log2(frame_data.target_params.z), 1.0);
	vec2 position_texel = world_xz * vertex_density;
	vec2 displacement_uv = (sample_scale * position_texel - round(frame_data.target_params.xy * vertex_density * sample_scale))
		/ max(frame_data.mesh_params.y * 2.0, 1.0) + vec2(0.5);
	displacement_uv.x += lod_scale - 1.0;
	displacement_uv.x /= subdivision_levels;
	if (any(lessThan(displacement_uv, vec2(0.0))) || any(greaterThan(displacement_uv, vec2(1.0)))) return vec3(0.0);
	return (textureLod(displacement_buffer, displacement_uv, 0.0).rgb * 2.0 - 1.0) * frame_data.displacement_params.x;
}

void main() {
	uint patch_index = visible_indices[gl_InstanceIndex];
	PatchData patch_data = patches[patch_index];
	vec2 grid = vec2(in_grid) + edge_offset(in_meta.x, patch_data);
	vec2 start_world_xz = grid * patch_data.scale + patch_data.origin;
	vec3 morph = clipmap_morph(start_world_xz, grid, patch_data.scale);
	vec2 end_world_xz = start_world_xz - morph.xy * patch_data.scale;
	vec2 world_xz = mix(start_world_xz, end_world_xz, morph.z);
	float height = patch_data.scale < frame_data.mesh_params.x ? sample_height_interpolated(world_xz)
		: mix(sample_height_nearest(start_world_xz), sample_height_nearest(end_world_xz), morph.z);
	float normal_step = max(frame_data.mesh_params.x, patch_data.scale);
	float h_l = sample_height_nearest(world_xz - vec2(normal_step, 0.0));
	float h_r = sample_height_nearest(world_xz + vec2(normal_step, 0.0));
	float h_d = sample_height_nearest(world_xz - vec2(0.0, normal_step));
	float h_u = sample_height_nearest(world_xz + vec2(0.0, normal_step));
	world_normal = normalize(vec3(h_l - h_r, 2.0 * normal_step, h_d - h_u));
	vec3 displacement_start = sample_clipmap_displacement(start_world_xz, patch_data.scale);
	vec3 displacement_end = sample_clipmap_displacement(end_world_xz, patch_data.scale * 2.0);
	vec3 displacement = mix(displacement_start, displacement_end, morph.z);
	world_position = vec3(world_xz.x, height, world_xz.y) + displacement;
	patch_tier = patch_data.tier;
	gl_Position = frame_data.view_projection * vec4(world_position, 1.0);
}

#[fragment]
#version 450
#define T3D_TIER 2
#define T3D_DIV_255 0.0039215686274509803921568627451
#define T3D_TAU_16 0.39269908169872415480783042290994

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) flat in uint patch_tier;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform FrameData {
	mat4 view_projection;
	vec4 light_direction_ambient;
	vec4 light_color;
	vec4 terrain_params;
	vec4 mesh_params;
	vec4 target_params;
	vec4 camera_params;
	vec4 fog_params;
	vec4 fog_depth_params;
	vec4 displacement_params;
} frame_data;
layout(set = 0, binding = 1, std430) readonly buffer RegionMapData { int region_map[]; };
layout(set = 0, binding = 3) uniform sampler2DArray control_maps;
layout(set = 0, binding = 4) uniform sampler2DArray color_maps;
layout(set = 0, binding = 5) uniform sampler2DArray albedo_maps;
layout(set = 0, binding = 6) uniform sampler2DArray normal_maps;
layout(set = 0, binding = 7) uniform sampler2DArray surface_maps;

struct MaterialLayer {
	vec4 color;
	vec4 pbr0;
	vec4 pbr1;
	vec4 displacement;
};
layout(set = 0, binding = 8, std430) readonly buffer MaterialBuffer { MaterialLayer materials[]; };

int floor_div_i(int value, int divisor) {
	int q = value / divisor;
	int r = value - q * divisor;
	return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}
int floor_mod_i(int value, int divisor) { return value - floor_div_i(value, divisor) * divisor; }
int region_layer(ivec2 region_cell) {
	int map_size = int(frame_data.terrain_params.y + 0.5);
	ivec2 map_pos = region_cell + ivec2(map_size / 2);
	if (map_pos.x < 0 || map_pos.y < 0 || map_pos.x >= map_size || map_pos.y >= map_size) return -1;
	return region_map[map_pos.y * map_size + map_pos.x] - 1;
}

uint read_control(ivec2 texel, int layer) {
	if (layer < 0) return 0u;
	int region_size = int(frame_data.terrain_params.x + 0.5);
	ivec2 local = ivec2(floor_mod_i(texel.x, region_size), floor_mod_i(texel.y, region_size));
	ivec3 size3 = textureSize(control_maps, 0);
	local = clamp(local, ivec2(0), size3.xy - ivec2(1));
	return floatBitsToUint(texelFetch(control_maps, ivec3(local, layer), 0).r);
}

vec4 color_texel(ivec2 local_coord, ivec2 region_cell, ivec2 map_size) {
	ivec2 region_delta = ivec2(0);
	if (local_coord.x < 0) { local_coord.x += map_size.x; region_delta.x = -1; }
	else if (local_coord.x >= map_size.x) { local_coord.x -= map_size.x; region_delta.x = 1; }
	if (local_coord.y < 0) { local_coord.y += map_size.y; region_delta.y = -1; }
	else if (local_coord.y >= map_size.y) { local_coord.y -= map_size.y; region_delta.y = 1; }
	int layer = region_layer(region_cell + region_delta);
	return layer >= 0 ? texelFetch(color_maps, ivec3(local_coord, layer), 0) : vec4(1.0);
}

vec4 sample_color_map(vec2 world_xz) {
	float region_size = frame_data.terrain_params.x;
	vec2 world_texel = world_xz / frame_data.mesh_params.x;
	vec2 world_region_uv = (world_texel + vec2(0.5)) / region_size;
	vec2 region_pos = floor(world_region_uv);
	vec2 local_uv = world_region_uv - region_pos;
	ivec2 map_size = textureSize(color_maps, 0).xy;
	vec2 color_texel_pos = local_uv * vec2(map_size) - vec2(0.5);
	ivec2 base = ivec2(floor(color_texel_pos));
	vec2 f = fract(color_texel_pos);
	ivec2 region_cell = ivec2(region_pos);
	vec4 c00 = color_texel(base, region_cell, map_size);
	vec4 c10 = color_texel(base + ivec2(1, 0), region_cell, map_size);
	vec4 c01 = color_texel(base + ivec2(0, 1), region_cell, map_size);
	vec4 c11 = color_texel(base + ivec2(1, 1), region_cell, map_size);
	return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

vec2 rotate_uv(vec2 uv, float angle) {
	float s = sin(angle);
	float c = cos(angle);
	return mat2(c, -s, s, c) * uv;
}

vec3 decode_surface_normal(vec2 encoded_xy) {
	vec2 normal_xy = fma(encoded_xy, vec2(2.0), vec2(-1.0));
	float len_sq = dot(normal_xy, normal_xy);
	if (len_sq > 1.0) { normal_xy *= inversesqrt(len_sq); len_sq = 1.0; }
	return vec3(normal_xy.x, sqrt(max(1.0 - len_sq, 0.0)), normal_xy.y);
}

struct SampledMaterial {
	vec3 albedo;
	vec3 tangent_normal;
	float roughness;
	float ao;
	float metallic;
};

SampledMaterial sample_material(int logical_id, vec2 world_xz, float control_scale, float control_angle) {
	int count = max(int(frame_data.mesh_params.w + 0.5), 1);
	int id = clamp(logical_id, 0, count - 1);
	MaterialLayer layer = materials[id];
	int texture_layers = max(textureSize(albedo_maps, 0).z, 1);
	int physical = clamp(int(layer.displacement.z + 0.5), 0, texture_layers - 1);
	float uv_scale = max(layer.pbr0.x, 0.000001) * control_scale;
	vec2 uv = rotate_uv(world_xz * uv_scale, control_angle);
	float lod_bias = T3D_TIER == 0 ? 0.0 : (T3D_TIER == 1 ? 1.0 : 2.0);
	bool resident = layer.displacement.w >= 2.0;
	bool packed_surface = mod(layer.displacement.w, 2.0) > 0.5;
	vec4 albedo_texel = resident ? textureLod(albedo_maps, vec3(uv, float(physical)), lod_bias) : vec4(1.0);
	SampledMaterial result;
	result.albedo = albedo_texel.rgb * layer.color.rgb;
	result.tangent_normal = vec3(0.0, 1.0, 0.0);
	result.roughness = 1.0;
	result.ao = 1.0;
	result.metallic = layer.pbr0.w;
	if (resident && T3D_TIER <= 1) {
		if (packed_surface && textureSize(surface_maps, 0).z > physical) {
			vec4 surface = textureLod(surface_maps, vec3(uv, float(physical)), lod_bias);
			result.roughness = clamp(surface.b + layer.pbr0.z, 0.0, 1.0);
			result.ao = mix(1.0, surface.a, clamp(layer.pbr1.x, 0.0, 1.0));
			if (T3D_TIER == 0) result.tangent_normal = decode_surface_normal(surface.rg);
		} else if (textureSize(normal_maps, 0).z > physical) {
			vec4 legacy = textureLod(normal_maps, vec3(uv, float(physical)), lod_bias);
			result.roughness = clamp(legacy.a + layer.pbr0.z, 0.0, 1.0);
			vec3 decoded = fma(legacy.xzy, vec3(2.0), vec3(-1.0));
			float length_value = length(decoded);
			result.ao = mix(1.0, max(length_value * 2.0 - 1.0, 0.0), clamp(layer.pbr1.x, 0.0, 1.0));
			if (T3D_TIER == 0 && length_value > 0.00001) result.tangent_normal = decoded / length_value;
		}
	}
	return result;
}

SampledMaterial blend_material(SampledMaterial a, SampledMaterial b, float t) {
	SampledMaterial r;
	r.albedo = mix(a.albedo, b.albedo, t);
	r.tangent_normal = normalize(mix(a.tangent_normal, b.tangent_normal, t));
	r.roughness = mix(a.roughness, b.roughness, t);
	r.ao = mix(a.ao, b.ao, t);
	r.metallic = mix(a.metallic, b.metallic, t);
	return r;
}

void main() {
	vec2 world_xz = world_position.xz;
	ivec2 control_texel = ivec2(floor(world_xz / frame_data.mesh_params.x));
	int region_size = int(frame_data.terrain_params.x + 0.5);
	ivec2 region_cell = ivec2(floor_div_i(control_texel.x, region_size), floor_div_i(control_texel.y, region_size));
	int region = region_layer(region_cell);
	uint control = read_control(control_texel, region);
	if (((control >> 2u) & 1u) != 0u) discard;
	int base_id = int((control >> 27u) & 0x1Fu);
	int overlay_id = int((control >> 22u) & 0x1Fu);
	float blend = float((control >> 14u) & 0xFFu) * T3D_DIV_255;
	float angle = float((control >> 10u) & 0xFu) * T3D_TAU_16;
	uint encoded_scale = (control >> 7u) & 0x7u;
	float control_scale = 0.9 - float(((encoded_scale + 3u) % 8u) + 1u) * 0.1;
	SampledMaterial material = sample_material(base_id, world_xz, control_scale, angle);
	if (overlay_id != base_id && blend > 0.0001) material = blend_material(material, sample_material(overlay_id, world_xz, control_scale, angle), blend);

	vec3 normal = normalize(world_normal);
	if (T3D_TIER == 0) {
		vec3 tangent = normalize(vec3(1.0, -normal.x / max(normal.y, 0.05), 0.0));
		vec3 bitangent = normalize(cross(normal, tangent));
		normal = normalize(tangent * material.tangent_normal.x + normal * material.tangent_normal.y + bitangent * material.tangent_normal.z);
	}
	vec4 color_map = sample_color_map(world_xz);
	vec3 albedo = material.albedo * color_map.rgb;
	vec3 light_dir = normalize(-frame_data.light_direction_ambient.xyz);
	float ndotl = max(dot(normal, light_dir), 0.0);
	vec3 view_dir = normalize(frame_data.camera_params.xyz - world_position);
	vec3 half_dir = normalize(light_dir + view_dir);
	float spec_power = mix(128.0, 4.0, material.roughness);
	float specular = pow(max(dot(normal, half_dir), 0.0), spec_power) * (1.0 - material.roughness);
	vec3 diffuse = albedo * frame_data.light_color.rgb * ndotl;
	vec3 ambient = albedo * frame_data.light_direction_ambient.w * material.ao;
	vec3 f0 = mix(vec3(0.04), albedo, clamp(material.metallic, 0.0, 1.0));
	vec3 lit = ambient + diffuse * mix(vec3(1.0), vec3(0.4), material.metallic) + f0 * specular * frame_data.light_color.rgb;
	float fog_density = max(frame_data.fog_params.w, 0.0);
	if (fog_density > 0.0 || frame_data.fog_depth_params.w > 0.5) {
		float distance_to_camera = length(frame_data.camera_params.xyz - world_position);
		float fog = 0.0;
		if (frame_data.fog_depth_params.w > 0.5) {
			float fog_range = max(frame_data.fog_depth_params.y - frame_data.fog_depth_params.x, 1e-4);
			fog = clamp((distance_to_camera - frame_data.fog_depth_params.x) / fog_range, 0.0, 1.0);
			fog = pow(fog, max(frame_data.fog_depth_params.z, 0.01));
		} else {
			fog = 1.0 - exp(-fog_density * distance_to_camera);
		}
		lit = mix(lit, frame_data.fog_params.rgb, clamp(fog, 0.0, 1.0));
	}
	out_color = vec4(lit, 1.0);
}
