#[compute]
#version 450

// P09: build the same normal-XZ RG8 derived map as terrain_derived.glsl,
// directly from the authoritative R16_UINT page. The right and down edge
// pages supply the +1 sample at region boundaries. No CPU Image staging.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(r16ui, set = 0, binding = 0) readonly uniform uimage2D center_height;
layout(r16ui, set = 0, binding = 1) readonly uniform uimage2D right_height;
layout(r16ui, set = 0, binding = 2) readonly uniform uimage2D down_height;
layout(rg8, set = 0, binding = 3) writeonly uniform image2D destination_derived;
layout(push_constant, std430) uniform Params
{
	uint width;
	uint height;
	uint use_right;
	uint use_down;
	float spacing;
	float height_min;
	float height_range;
	float reserved;
}
pc;
float height_m(uint encoded)
{
	return pc.height_min + (float(encoded) / 65535.0) * pc.height_range;
}
void main()
{
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= int(pc.width) || p.y >= int(pc.height))
		return;
	uint c = imageLoad(center_height, p).r;
	uint r = p.x + 1 < int(pc.width) ? imageLoad(center_height, p + ivec2(1, 0)).r
	: (pc.use_right != 0u ? imageLoad(right_height, ivec2(0, p.y)).r : c);
	uint d = p.y + 1 < int(pc.height) ? imageLoad(center_height, p + ivec2(0, 1)).r
	: (pc.use_down != 0u ? imageLoad(down_height, ivec2(p.x, 0)).r : c);
	float h = height_m(c);
	vec3 normal = normalize(vec3(h - height_m(r), pc.spacing, h - height_m(d)));
	uint packed = packUnorm4x8(vec4(normal * 0.5 + 0.5, 1.0));
	vec2 rg = vec2(float(packed & 255u), float((packed >> 16u) & 255u)) * (1.0 / 255.0);
	imageStore(destination_derived, p, vec4(rg, 0.0, 1.0));
}
