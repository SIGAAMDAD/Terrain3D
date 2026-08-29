#[compute]
#version 450
#include "render_world_common_inc.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(constant_id = 0) const uint HIZ_MIP = 0u;
layout(set = 0, binding = 0, std430) readonly buffer FrameBuffer {
    Frame f;
};
layout(set = 0, binding = 1) uniform sampler2D sourceDepth;
layout(set = 0, binding = 2, r32f) uniform writeonly image2D destinationDepth;
void main() {
    uvec2 activeBase = uvec2(max(f.capacities1.y, 1u), max(f.capacities1.z, 1u));
    uint divisor = 1u << HIZ_MIP;
    uvec2 dstActive = max((activeBase + uvec2(divisor - 1u)) / divisor, uvec2(1u));
    uvec2 dst = gl_GlobalInvocationID.xy;
    if (any(greaterThanEqual(dst, dstActive)))
        return;
    if (HIZ_MIP == 0u) {
        uvec2 sourceActive = activeBase;
        ivec2 p = ivec2(min(dst, sourceActive - uvec2(1u)));
        float d = texelFetch(sourceDepth, p, 0).r;
        imageStore(destinationDepth, ivec2(dst), vec4(d));
        return;
    }
    uint sourceDivisor = 1u << (HIZ_MIP - 1u);
    uvec2 sourceActiveU = max((activeBase + uvec2(sourceDivisor - 1u)) / sourceDivisor, uvec2(1u));
    ivec2 sourceMax = ivec2(sourceActiveU) - ivec2(1);
    ivec2 base = ivec2(dst * 2u);
    ivec2 p00 = clamp(base, ivec2(0), sourceMax);
    ivec2 p10 = clamp(base + ivec2(1, 0), ivec2(0), sourceMax);
    ivec2 p01 = clamp(base + ivec2(0, 1), ivec2(0), sourceMax);
    ivec2 p11 = clamp(base + ivec2(1, 1), ivec2(0), sourceMax);
    float d = min(min(texelFetch(sourceDepth, p00, 0).r, texelFetch(sourceDepth, p10, 0).r),
        min(texelFetch(sourceDepth, p01, 0).r, texelFetch(sourceDepth, p11, 0).r));
    imageStore(destinationDepth, ivec2(dst), vec4(d));
}
