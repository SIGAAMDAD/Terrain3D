// ENV-P07: camera-centered weather state, transient force/ripple and two height-aware contact layers.
#[versions]
state = "#define T3D_ENV_STATE";
force = "#define T3D_ENV_FORCE";
height = "#define T3D_ENV_HEIGHT";

#[compute]
#version 450
#VERSION_DEFINES
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#ifdef T3D_ENV_STATE
layout(set = 0, binding = 0, rgba8) readonly uniform image2D source_field;
layout(set = 0, binding = 1, rgba8) writeonly uniform image2D destination_field;
layout(set = 0, binding = 2, std430) readonly buffer RegionClimateBuffer
{
	uint climate_cells[];
};
const float TEXEL_SIZE = 1.0;
const ivec2 GRID_DIMENSION = ivec2(512);
#elif defined(T3D_ENV_HEIGHT)
layout(set = 0, binding = 0, rgba16f) readonly uniform image2DArray source_field;
layout(set = 0, binding = 1, rgba16f) writeonly uniform image2DArray destination_field;
const float TEXEL_SIZE = 0.5;
const ivec2 GRID_DIMENSION = ivec2(256);
#else
layout(set = 0, binding = 0, rgba16f) readonly uniform image2D source_field;
layout(set = 0, binding = 1, rgba16f) writeonly uniform image2D destination_field;
const float TEXEL_SIZE = 0.5;
const ivec2 GRID_DIMENSION = ivec2(256);
#endif
#if defined(T3D_ENV_FORCE) || defined(T3D_ENV_HEIGHT)
struct EnvironmentImpulse
{
	vec4 position_radius; // xyz world-space position, w radius
	vec2 lateral_force;
	float magnitude;
	uint type;
};
layout(set = 0, binding = 2, std430) readonly buffer EnvironmentImpulseBuffer
{
	EnvironmentImpulse impulses[];
};
#endif
layout(push_constant, std430) uniform EnvironmentPush
{
	vec4 origin_current_previous; // current XZ, previous XZ
	vec4 rain_snow_dry_dt;
	uvec4 flags; // history valid, impulse count, environment enabled, reserved
}
params;

#ifdef T3D_ENV_HEIGHT
void t3d_integrate_height(ivec2 at, ivec2 previous, bool inside_previous, vec2 world, float dt)
{
	vec4 a = inside_previous ? imageLoad(source_field, ivec3(previous, 0)) : vec4(0.0);
	vec4 b = inside_previous ? imageLoad(source_field, ivec3(previous, 1)) : vec4(0.0);
	float decay = exp2(-dt * 5.5);
	a.xy *= decay;
	a.w *= decay;
	b.xy *= decay;
	b.w *= decay;
	if (a.w < 0.01)
		a = vec4(0.0);
	if (b.w < 0.01)
		b = vec4(0.0);
	if (params.flags.z != 0u)
	{
		for (uint i = 0u; i < min(params.flags.y, 128u); ++i)
		{
			EnvironmentImpulse impulse = impulses[i];
			vec2 offset = world - impulse.position_radius.xz;
			float radius = max(impulse.position_radius.w, 0.001);
			float square_distance = dot(offset, offset);
			if (square_distance >= radius * radius)
				continue;
			float falloff = 1.0 - square_distance / (radius * radius);
			float amplitude = abs(impulse.magnitude) * falloff;
			float height = impulse.position_radius.y;
			bool same_a = a.w > 0.0 && abs(a.z - height) <= 2.5;
			bool same_b = b.w > 0.0 && abs(b.z - height) <= 2.5;
			bool use_a = same_a || (!same_b && a.w <= 0.0);
			bool use_b = !use_a && (same_b || b.w <= 0.0);
			// Only the two nearest occupied height bands are representable.
			// When both are full, discard unrelated impulses rather than causing
			// contact displacement on the wrong floor of an overpass.
			if (use_a)
			{
				if (!same_a)
					a = vec4(0.0);
				a.xy += impulse.lateral_force * (impulse.magnitude * falloff);
				a.z = height;
				a.w = clamp(a.w + amplitude, 0.0, 1.0);
			}
			else if (use_b)
			{
				if (!same_b)
					b = vec4(0.0);
				b.xy += impulse.lateral_force * (impulse.magnitude * falloff);
				b.z = height;
				b.w = clamp(b.w + amplitude, 0.0, 1.0);
			}
		}
	}
	a.xy = clamp(a.xy, vec2(-8.0), vec2(8.0));
	b.xy = clamp(b.xy, vec2(-8.0), vec2(8.0));
	imageStore(destination_field, ivec3(at, 0), a);
	imageStore(destination_field, ivec3(at, 1), b);
}
#endif

void main()
{
	ivec2 at = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(at, GRID_DIMENSION)))
		return;
	vec2 world = params.origin_current_previous.xy + (vec2(at) + vec2(0.5)) * TEXEL_SIZE;
	ivec2 previous = ivec2(round((world - params.origin_current_previous.zw) / TEXEL_SIZE - vec2(0.5)));
	bool inside_previous = params.flags.x != 0u &&
	all(greaterThanEqual(previous, ivec2(0))) && all(lessThan(previous, GRID_DIMENSION));
	float dt = clamp(params.rain_snow_dry_dt.w, 0.0, 0.1);
#ifdef T3D_ENV_HEIGHT
	t3d_integrate_height(at, previous, inside_previous, world, dt);
#else
	vec4 value = inside_previous ? imageLoad(source_field, previous) : vec4(0.0);
#ifdef T3D_ENV_STATE
	bool seeded = false;
	if (!inside_previous || params.flags.w != 0u)
	{
		// An explicit import/region edit invalidates every visible region tile.
		// Invalid (cleared) cells must not retain the previous world's state.
		if (params.flags.w != 0u)
		{
			value = vec4(0.0); seeded = true;
		}
		ivec2 region = ivec2(floor(world / 128.0)) + ivec2(16);
		if (all(greaterThanEqual(region, ivec2(0))) && all(lessThan(region, ivec2(32))))
		{
			uint packed = climate_cells[region.y * 32 + region.x];
			if ((packed & 0xff000000u) != 0u)
			{
				value = vec4(float(packed & 255u), float((packed >> 8u) & 255u),
				float((packed >> 16u) & 255u), 0.0) / 255.0;
				seeded = true;
			}
		}
	}
	if (params.flags.z != 0u && !seeded)
	{
		value.r = clamp(value.r + dt * (params.rain_snow_dry_dt.x - params.rain_snow_dry_dt.z), 0.0, 1.0);
		value.g = clamp(value.g + dt * (params.rain_snow_dry_dt.y - params.rain_snow_dry_dt.z * 0.3), 0.0, 1.0);
	}
	if (!seeded)
		value.b = max(value.b - dt * 0.15, 0.0);
	value.a = smoothstep(0.55, 0.95, value.r);
#else
	// P02's 2D field remains the water ripple and local force receiver.
	value.xy *= exp2(-dt * 5.5);
	value.zw *= exp2(-dt * 3.0);
	if (params.flags.z != 0u)
	{
		for (uint i = 0u; i < min(params.flags.y, 128u); ++i)
		{
			EnvironmentImpulse impulse = impulses[i];
			vec2 offset = world - impulse.position_radius.xz;
			float radius = max(impulse.position_radius.w, 0.001);
			float squared_distance = dot(offset, offset);
			if (squared_distance >= radius * radius)
				continue;
			float falloff = 1.0 - squared_distance / (radius * radius);
			value.xy += impulse.lateral_force * (impulse.magnitude * falloff);
			value.z += impulse.magnitude * falloff * 0.3;
			value.w += impulse.magnitude * falloff;
		}
	}
	value = clamp(value, vec4(-8.0), vec4(8.0));
#endif
	imageStore(destination_field, at, value);
#endif
}
