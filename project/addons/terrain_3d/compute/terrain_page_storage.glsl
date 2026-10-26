#[versions]
r32f = "#define T3D_PAGE_R32F";
rgba8 = "#define T3D_PAGE_RGBA8";
r16ui = "#define T3D_PAGE_R16UI";
r32ui = "#define T3D_PAGE_R32UI";
r32f_reduce = "#define T3D_PAGE_R32F\n#define T3D_PAGE_REDUCE";
rgba8_reduce = "#define T3D_PAGE_RGBA8\n#define T3D_PAGE_REDUCE";
r16ui_reduce = "#define T3D_PAGE_R16UI\n#define T3D_PAGE_REDUCE";
r32ui_reduce = "#define T3D_PAGE_R32UI\n#define T3D_PAGE_REDUCE";
r32f_partial = "#define T3D_PAGE_R32F\n#define T3D_PAGE_REDUCE_PARTIAL";
rgba8_partial = "#define T3D_PAGE_RGBA8\n#define T3D_PAGE_REDUCE_PARTIAL";
r16ui_partial = "#define T3D_PAGE_R16UI\n#define T3D_PAGE_REDUCE_PARTIAL";
r32ui_partial = "#define T3D_PAGE_R32UI\n#define T3D_PAGE_REDUCE_PARTIAL";
reduce_final = "#define T3D_PAGE_REDUCE_FINAL";

#[compute]
#version 450
#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#ifndef T3D_PAGE_REDUCE_FINAL
#ifdef T3D_PAGE_R32F
layout(r32f, set = 0, binding = 0) uniform image2D target_page;
layout(r32f, set = 0, binding = 1) readonly uniform image2D reference_page;
#elif defined(T3D_PAGE_R16UI)
layout(r16ui, set = 0, binding = 0) uniform uimage2D target_page;
layout(r16ui, set = 0, binding = 1) readonly uniform uimage2D reference_page;
#elif defined(T3D_PAGE_R32UI)
layout(r32ui, set = 0, binding = 0) uniform uimage2D target_page;
layout(r32ui, set = 0, binding = 1) readonly uniform uimage2D reference_page;
#else
layout(rgba8, set = 0, binding = 0) uniform image2D target_page;
layout(rgba8, set = 0, binding = 1) readonly uniform image2D reference_page;
#endif
#endif // !T3D_PAGE_REDUCE_FINAL
layout(set = 0, binding = 2, std430) buffer Result
{
	uint hash_xor; uint differs;
}
result;
layout(push_constant, std430) uniform Params
{
	uint width;
	uint height;
	uint operation; // 0: clear/fill, 1: add, 2: hash, 3: compare
	uint reserved;
	vec4 value;
}
pc;
// P30b: 16x16 tiles write disjoint exact partials; one final workgroup
// reduces those partials into the original 8-byte result ABI.
#if defined(T3D_PAGE_REDUCE_PARTIAL) || defined(T3D_PAGE_REDUCE_FINAL)
layout(set = 0, binding = 3, std430) buffer PagePartials
{
	uint values[];
}
page_partials;
shared uint s_large_partials[64];
#endif
#if defined(T3D_PAGE_REDUCE_FINAL)
void main()
{
	if (pc.operation != 2u && pc.operation != 3u)
		return;
	uint group_x = (pc.width + 15u) / 16u;
	uint group_y = (pc.height + 15u) / 16u;
	uint count = group_x * group_y;
	uint lane = gl_LocalInvocationIndex;
	uint value = 0u;
	for (uint i = lane; i < count; i += 64u)
	{
		if (pc.operation == 2u)
			value ^= page_partials.values[i];
		else value |= page_partials.values[i];
	}
	s_large_partials[lane] = value;
	barrier();
	for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (lane < stride)
		{
			if (pc.operation == 2u)
				s_large_partials[lane] ^= s_large_partials[lane + stride];
			else s_large_partials[lane] |= s_large_partials[lane + stride];
		}
		barrier();
	}
	if (lane == 0u)
	{
		result.hash_xor = pc.operation == 2u ? s_large_partials[0] : 0u;
		result.differs = pc.operation == 3u ? s_large_partials[0] : 0u;
	}
}
#elif defined(T3D_PAGE_REDUCE_PARTIAL)
void main()
{
	if (pc.operation != 2u && pc.operation != 3u)
		return;
	uint partial = 0u;
	uvec2 base = gl_WorkGroupID.xy * 16u + gl_LocalInvocationID.xy;
	for (uint dy = 0u; dy < 16u; dy += 8u)
	{
		for (uint dx = 0u; dx < 16u; dx += 8u)
		{
			uvec2 pixel = base + uvec2(dx, dy);
			if (pixel.x >= pc.width || pixel.y >= pc.height)
				continue;
			ivec2 pos = ivec2(pixel);
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
			uvec4 existing = imageLoad(target_page, pos);
#else
			vec4 existing = imageLoad(target_page, pos);
#endif
			if (pc.operation == 2u)
			{
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
				uvec4 bits = existing;
#else
				uvec4 bits = floatBitsToUint(existing);
#endif
				uint key = pixel.x * 0x9E3779B9u ^ pixel.y * 0x85EBCA6Bu;
				partial ^= key ^ bits.x ^ bits.y ^ bits.z ^ bits.w;
			}
			else
			{
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
				uvec4 other = imageLoad(reference_page, pos);
				partial |= uint(any(notEqual(existing, other)));
#else
				vec4 other = imageLoad(reference_page, pos);
				partial |= uint(any(notEqual(floatBitsToUint(existing), floatBitsToUint(other))));
#endif
			}
		}
	}
	uint lane = gl_LocalInvocationIndex;
	s_large_partials[lane] = partial;
	barrier();
	for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (lane < stride)
		{
			if (pc.operation == 2u)
				s_large_partials[lane] ^= s_large_partials[lane + stride];
			else s_large_partials[lane] |= s_large_partials[lane + stride];
		}
		barrier();
	}
	if (lane == 0u)
	{
		uint group_x = (pc.width + 15u) / 16u;
		uint index = gl_WorkGroupID.y * group_x + gl_WorkGroupID.x;
		page_partials.values[index] = s_large_partials[0];
	}
}
#elif defined(T3D_PAGE_REDUCE)
// P30: one workgroup owns the entire small page. Each lane accumulates its
// exact bits over a disjoint 8x8 grid-stride partition; the reduction uses
// integer XOR for hashing or OR for comparisons. Only lane zero writes the
// two-word result. No inter-workgroup sync, scratch buffer or global atomics.
shared uint s_page_partials[64];
void main()
{
	if (pc.operation != 2u && pc.operation != 3u)
		return;
	uint partial = 0u;
	for (uint y = gl_LocalInvocationID.y; y < pc.height; y += 8u)
	{
		for (uint x = gl_LocalInvocationID.x; x < pc.width; x += 8u)
		{
			ivec2 pos = ivec2(x, y);
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
			uvec4 existing = imageLoad(target_page, pos);
#else
			vec4 existing = imageLoad(target_page, pos);
#endif
			if (pc.operation == 2u)
			{
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
				uvec4 bits = existing;
#else
				uvec4 bits = floatBitsToUint(existing);
#endif
				uint key = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu;
				partial ^= key ^ bits.x ^ bits.y ^ bits.z ^ bits.w;
			}
			else
			{
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
				uvec4 other = imageLoad(reference_page, pos);
				partial |= uint(any(notEqual(existing, other)));
#else
				vec4 other = imageLoad(reference_page, pos);
				partial |= uint(any(notEqual(floatBitsToUint(existing), floatBitsToUint(other))));
#endif
			}
		}
	}
	uint lane = gl_LocalInvocationIndex;
	s_page_partials[lane] = partial;
	barrier();
	for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (lane < stride)
		{
			if (pc.operation == 2u)
			{
				s_page_partials[lane] ^= s_page_partials[lane + stride];
			}
			else
			{
				s_page_partials[lane] |= s_page_partials[lane + stride];
			}
		}
		barrier();
	}
	if (lane == 0u)
	{
		result.hash_xor = pc.operation == 2u ? s_page_partials[0] : 0u;
		result.differs = pc.operation == 3u ? s_page_partials[0] : 0u;
	}
}
#else
void main()
{
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (pos.x >= int(pc.width) || pos.y >= int(pc.height))
		return;
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
	uvec4 existing = imageLoad(target_page, pos);
#else
	vec4 existing = imageLoad(target_page, pos);
#endif
	if (pc.operation == 4u || pc.operation == 5u)
	{
		uint x = pc.reserved & 255u;
		uint y = (pc.reserved >> 8u) & 255u;
		uint w = (pc.reserved >> 16u) & 255u;
		uint h = (pc.reserved >> 24u) & 255u;
		if (uint(pos.x) < x || uint(pos.y) < y || uint(pos.x) - x >= w || uint(pos.y) - y >= h)
			return;
	}
	if (pc.operation == 0u || pc.operation == 4u)
	{
#ifdef T3D_PAGE_R16UI
		imageStore(target_page, pos, uvec4(clamp(pc.value.r, 0.0, 65535.0), 0u, 0u, 0u));
#elif defined(T3D_PAGE_R32UI)
		imageStore(target_page, pos, uvec4(pc.operation == 4u
		? floatBitsToUint(pc.value.r) : uint(max(pc.value.r, 0.0)), 0u, 0u, 0u));
#else
		imageStore(target_page, pos, pc.value);
#endif
	}
	else if (pc.operation == 1u || pc.operation == 5u)
	{
#ifdef T3D_PAGE_R16UI
		imageStore(target_page, pos, uvec4(min(existing.r + uint(max(pc.value.r, 0.0)), 65535u), 0u, 0u, 0u));
#elif defined(T3D_PAGE_R32UI)
		imageStore(target_page, pos, uvec4(existing.r + uint(max(pc.value.r, 0.0)), 0u, 0u, 0u));
#else
		imageStore(target_page, pos, existing + pc.value);
#endif
	}
	else if (pc.operation == 2u)
	{
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
		uvec4 bits = existing;
#else
		uvec4 bits = floatBitsToUint(existing);
#endif
		uint key = uint(pos.x) * 0x9E3779B9u ^ uint(pos.y) * 0x85EBCA6Bu;
		atomicXor(result.hash_xor, key ^ bits.x ^ bits.y ^ bits.z ^ bits.w);
	}
	else if (pc.operation == 3u)
	{
#if defined(T3D_PAGE_R16UI) || defined(T3D_PAGE_R32UI)
		uvec4 other = imageLoad(reference_page, pos);
		if (any(notEqual(existing, other)))
			atomicOr(result.differs, 1u);
#else
		vec4 other = imageLoad(reference_page, pos);
		if (any(notEqual(floatBitsToUint(existing), floatBitsToUint(other))))
			atomicOr(result.differs, 1u);
#endif
	}
}

#endif // T3D_PAGE_REDUCE / T3D_PAGE_REDUCE_PARTIAL / T3D_PAGE_REDUCE_FINAL
