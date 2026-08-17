// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// T3D_OCEAN_VARIANT_SOURCE
// Terrain3D replaces the technical constants below when an ocean quality profile changes.

shader_type spatial;
render_mode cull_back, depth_draw_always, diffuse_burley, specular_schlick_ggx, shadows_disabled, skip_vertex_transform;

#define T3D_OCEAN_VERTEX_ITERATIONS 1
#define T3D_OCEAN_FRAGMENT_ITERATIONS 3
#define T3D_OCEAN_REFRACTION_MODE 2
#define T3D_OCEAN_FOAM_MODE 2
#define T3D_OCEAN_SCATTERING_MODE 2
#define T3D_OCEAN_SHADING_MODE 2

//////////////////////////////
// Uniforms
//////////////////////////////
group_uniforms water;
uniform float sea_level : hint_range(-50.0, 50.0, 0.001) = 1.0;
uniform vec3 water_color : source_color = vec3(.1058, .1568, .2117);
uniform float visible_depth : hint_range(0.001, 10000., .1) = 256.;
uniform float depth_curve : hint_range(0.001, 5.0) = .1;
uniform float shore_blend : hint_range(0.0, 10.0, 0.001) = 1.;
#if T3D_OCEAN_REFRACTION_MODE == 2
uniform float refraction_strength : hint_range(0.0, .2, 0.001) = 0.02;
#endif
group_uniforms;

group_uniforms waves;
uniform float time_scale : hint_range(0.0, 64.0) = 8.0;
uniform float height_scale : hint_range(0.0, 16.0) = 3.0;
uniform float ocean_scale : hint_range(0.1, 16.0) = 2.0;
group_uniforms;

group_uniforms reflections;
uniform float fresnel_scale : hint_range(0., 1.) = .5;
uniform float fresnel_power : hint_range(0., 10.) = 1.;
uniform float specular : hint_range(0., 10.) = 1.;
group_uniforms;

#if T3D_OCEAN_FOAM_MODE > 0
group_uniforms foam;
uniform float foam_height : hint_range(0., 5.0) = 2.5;
uniform float foam_top_intensity : hint_range(.1, 100.0) = 2.0;
uniform float foam_distance : hint_range(0.1, 16384.0) = 2048.;
uniform float foam_shore_intensity : hint_range(0.1, 100.0) = 2.;
uniform sampler2D foam_noise_texture;
uniform float foam_noise_scale : hint_range(0.1, 5.0) = 1.5;
group_uniforms;
#endif

#if T3D_OCEAN_SCATTERING_MODE > 0
group_uniforms light_scattering;
uniform float height_scattering : hint_range(0.0, 15.0) = 3.0;
uniform float height_threshold : hint_range(0.0, 1.0) = 0.1;
uniform float height_angle_threshold : hint_range(0.0, 1.0) = 0.21;
uniform float fersnel_scattering : hint_range(0.0, 70.0) = 20.0;
uniform float direct_scattering : hint_range(0.0, 70.0) = 10.0;
group_uniforms;
#endif

group_uniforms private_set_by_terrain;
uniform float _mesh_size = 32.;
uniform float _subdiv = 1.;
uniform float _vertex_spacing = 1.;
uniform float _vertex_density = 1.;
uniform vec3 _target_pos = vec3(0.,0.,0.);
uniform vec3 _light_direction = vec3(0.0, 0.0, 0.0);
uniform vec3 _light_color : source_color = vec3(1.0, 1.0, 1.0);
group_uniforms;

#if T3D_OCEAN_REFRACTION_MODE > 0 || T3D_OCEAN_FOAM_MODE > 0
uniform sampler2D depth_texture : hint_depth_texture, repeat_disable, filter_nearest;
#endif
#if T3D_OCEAN_REFRACTION_MODE == 2
uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_nearest;
#endif

varying vec3 v_vertex;
varying float v_vertex_distance;

float hash(vec2 p) {
	uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
	uint n = (q.x ^ q.y) * 1597334673u;
	return float(n) * (1.0 / float(0xffffffffu));
}

float noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = smoothstep(0., 1., f);
	vec2 i1 = i + 1.0;
	float a = hash(i);
	float b = hash(vec2(i1.x, i.y));
	float c = hash(vec2(i.x, i1.y));
	float d = hash(i1);
	return 2.0 * mix(mix(a, b, u.x), mix(c, d, u.x), u.y) - 1.0;
}

float octave(vec2 uv) {
	uv += noise(uv * 0.5) * 2.;
	vec2 wave = 1.0 - abs(sin(uv));
	float value = wave.x * wave.y + 1e-6;
	return 1.0 - 2.0 * (value * inversesqrt(value)) + value;
}

float ocean_height(vec2 pos, int iterations) {
	float freq = 0.03 * ocean_scale;
	float amp = 4.0;
	vec2 uv = pos;
	float height = 0.0;
	float time = TIME * time_scale;
	for (int i = 0; i < iterations; i++) {
		height += (octave((uv + time) * freq) + octave((uv - time) * freq)) * amp;
		uv *= mat2(vec2(1.8, 0.8), vec2(-0.8, 1.8));
		freq *= 1.9;
		amp *= 0.2;
	}
	return height * 0.1 * height_scale;
}

void vertex() {
	v_vertex = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float scale = MODEL_MATRIX[0][0];
	float inv_scale = 1.0 / scale;
	float max_xz = max(abs(v_vertex.x - _target_pos.x), abs(v_vertex.z - _target_pos.z));
	float vertex_lerp = smoothstep(0.0, 1.0, (max_xz * inv_scale - _mesh_size - 4.0) / (_mesh_size - 4.0));
	vec2 vertex_fract = fract(VERTEX.xz * 0.5) * 2.0;
	vec2 shift = (scale < _vertex_spacing / _subdiv + 1e-6) ?
		mix(vertex_fract, vec2(vertex_fract.x, -vertex_fract.y),
			round(fract(round(mod(v_vertex.z * inv_scale, 4.0)) * round(mod(v_vertex.x * inv_scale, 4.0)) * 0.25))) :
		vertex_fract * round((fract(v_vertex.xz * 0.25 * inv_scale) - 0.5) * 4.0);
	v_vertex.xz -= shift * scale * vertex_lerp;
	UV = v_vertex.xz;
	v_vertex.y = sea_level + ocean_height(UV, T3D_OCEAN_VERTEX_ITERATIONS);
	VERTEX = (VIEW_MATRIX * vec4(v_vertex, 1.0)).xyz;
	v_vertex_distance = length(v_vertex - _target_pos);
}

void fragment() {
	float wave_height = 0.0;
	vec3 world_normal = vec3(0.0, 1.0, 0.0);
#if T3D_OCEAN_FRAGMENT_ITERATIONS > 0
	float dv = max(0.25, length(fwidth(VERTEX)));
	vec2 n_uv = UV + dv * -0.66;
	wave_height = ocean_height(n_uv, T3D_OCEAN_FRAGMENT_ITERATIONS);
	float wave_u = ocean_height(n_uv + vec2(dv, 0.0), T3D_OCEAN_FRAGMENT_ITERATIONS);
	float wave_v = ocean_height(n_uv + vec2(0.0, dv), T3D_OCEAN_FRAGMENT_ITERATIONS);
	world_normal = normalize(vec3(wave_height - wave_u, max(.1, v_vertex_distance / 512.0), wave_height - wave_v));
#endif
#if T3D_OCEAN_SHADING_MODE != 3
	NORMAL = mat3(VIEW_MATRIX) * world_normal;
#endif

#if T3D_OCEAN_SHADING_MODE == 1 || T3D_OCEAN_SHADING_MODE == 2 || T3D_OCEAN_SCATTERING_MODE > 0
	float daytime_mask = smoothstep(0., .1, _light_direction.y);
#endif
#if T3D_OCEAN_SHADING_MODE == 1 || T3D_OCEAN_SHADING_MODE == 2
	float cos_theta = max(0.0, dot(NORMAL, VIEW));
	float fresnel = clamp(fresnel_scale * pow(1.0 - cos_theta, fresnel_power), 0.0, 1.0);
	ROUGHNESS = mix(0.02, 0.8, fresnel) * daytime_mask;
	SPECULAR = clamp(specular * fresnel - .5 * (1. - daytime_mask), 0.15, 1.);
#else
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
#endif
	ALBEDO = water_color;
	ALPHA = 1.0;
	float shore_mask = 0.0;
	float depth_fade = 1.0;

#if T3D_OCEAN_REFRACTION_MODE > 0 || T3D_OCEAN_FOAM_MODE > 0
	float depth_raw = textureLod(depth_texture, SCREEN_UV, 0.0).r;
	vec4 clip_pos = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth_raw, 1.0);
	vec3 view_pos = clip_pos.xyz / clip_pos.w;
	vec4 world_pos_hom = INV_VIEW_MATRIX * vec4(view_pos, 1.0);
	vec3 terrain_position = world_pos_hom.xyz / world_pos_hom.w;
	shore_mask = 1.0 - clamp(1.0 - smoothstep(view_pos.z + 2. * shore_blend, view_pos.z, VERTEX.z), 0.0, 1.0);
	ALPHA = 1.0 - shore_mask;
	float water_depth = max(20., sea_level + v_vertex.y - terrain_position.y);
	depth_fade = pow(clamp(water_depth / visible_depth, 0.0, 1.0), depth_curve);
#endif

#if T3D_OCEAN_REFRACTION_MODE == 1
	// Depth tint keeps shoreline fading without a screen back-buffer sample.
	ALBEDO = mix(water_color * 0.45, water_color, depth_fade);
#elif T3D_OCEAN_REFRACTION_MODE == 2
	vec2 refract_offset = NORMAL.xz * refraction_strength;
	float vp_y_mask = 1.0 - smoothstep(0.0, max(length(refract_offset) * 2.0, 1e-5), 1.0 - SCREEN_UV.y);
	refract_offset = mix(refract_offset, vec2(0.0), max(shore_mask, vp_y_mask));
	vec2 refract_uv = clamp(SCREEN_UV + refract_offset, vec2(0.001), vec2(0.999));
	float refract_depth_raw = textureLod(depth_texture, refract_uv, 0.0).r;
	vec4 refract_clip_pos = INV_PROJECTION_MATRIX * vec4(refract_uv * 2.0 - 1.0, refract_depth_raw, 1.0);
	vec3 refract_view_pos = refract_clip_pos.xyz / refract_clip_pos.w;
	vec4 refract_world_hom = INV_VIEW_MATRIX * vec4(refract_view_pos, 1.0);
	vec3 refract_world_pos = refract_world_hom.xyz / refract_world_hom.w;
	bool use_refracted = refract_world_pos.y < v_vertex.y && VERTEX.z < refract_view_pos.z + 2. * visible_depth;
	vec3 background_color = texture(screen_texture, use_refracted ? refract_uv : SCREEN_UV).rgb;
	ALBEDO = mix(background_color, water_color, depth_fade);
#endif

	float foam_mask = 0.0;
#if T3D_OCEAN_FOAM_MODE > 0
	float foam_noise = texture(foam_noise_texture, UV * .1 * foam_noise_scale).r;
#if T3D_OCEAN_FOAM_MODE == 1
	foam_mask = clamp(shore_mask * foam_shore_intensity * foam_noise - foam_height * 0.1, 0.0, 1.0);
#else
	float detail_wave = wave_height * 0.005;
	foam_mask = clamp(wave_height * 0.5 + (detail_wave * foam_top_intensity + shore_mask * foam_shore_intensity) * foam_noise * 1.5 - foam_height, 0.0, 1.0);
#endif
	foam_mask *= 1.0 - smoothstep(0., 1., v_vertex_distance / foam_distance);
	ALBEDO = mix(ALBEDO, vec3(0.8), foam_mask);
#endif

#if T3D_OCEAN_SCATTERING_MODE == 1
	float facing = max(dot(_light_direction, world_normal), 0.0);
	BACKLIGHT = water_color * _light_color * facing * 2.0 * daytime_mask;
#elif T3D_OCEAN_SCATTERING_MODE == 2
	vec3 ray_dir = normalize(mat3(INV_VIEW_MATRIX) * VIEW);
	vec3 light_factor = height_scattering * max(wave_height, height_threshold)
		* max(pow(dot(_light_direction, -ray_dir), 4.0), height_angle_threshold)
		* _light_color * pow(0.5 - 0.5 * dot(_light_direction, world_normal), 3.0);
	light_factor += fersnel_scattering * pow(dot(ray_dir, world_normal), 2.0) * _light_color * water_color;
	light_factor += direct_scattering * dot(_light_direction, world_normal) * water_color * _light_color;
	BACKLIGHT = light_factor * 4.0 * clamp(depth_fade, 0.4, 1.0) * max(1.0 - foam_mask, 0.5) * daytime_mask;
#endif
}
